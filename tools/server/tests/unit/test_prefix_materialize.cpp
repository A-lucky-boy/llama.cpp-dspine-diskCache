// standalone self-check for the prefix materialization planner
//
// Pure metadata: no model, no lock, no file. Build with the server toolchain
// (Windows, repository root, vcvars64 loaded) - see test-run/btmat.bat.
//
// What this pins down is the *selection* contract: which eligible prefix a
// request should try to capture, and why it captures nothing otherwise. The
// capture itself (stopping a prefill batch, exporting state) is not testable
// without a model and is covered by the end-to-end run instead.

#include "server-prefix-materialize.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    const long long va = (long long) (a); \
    const long long vb = (long long) (b); \
    if (va != vb) { \
        std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); \
        g_failures++; \
    } \
} while (0)

namespace {

using skip_t = disk_cache_materialize_skip;

disk_cache_materialize_candidate cand(size_t tokens, bool persisted = false, uint32_t obs = 3, bool eligible = true) {
    disk_cache_materialize_candidate c;
    c.prefix_tokens = tokens;
    c.persisted     = persisted;
    c.observations  = obs;
    c.eligible      = eligible;
    return c;
}

disk_cache_materialize_request req(bool enabled, size_t min_tokens, size_t n_input, int64_t n_past = 0) {
    disk_cache_materialize_request r;
    r.enabled        = enabled;
    r.min_tokens     = min_tokens;
    r.n_input_tokens = n_input;
    r.n_past         = n_past;
    return r;
}

// the feature is off: no plan, and it says so
void test_disabled() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(false, 512, 4096), { cand(1024) }, sk);

    CHECK(!plan.capture);
    CHECK(sk == skip_t::disabled);
}

// nothing known about this prompt
void test_no_candidates() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 4096), {}, sk);

    CHECK(!plan.capture);
    CHECK(sk == skip_t::no_candidates);
}

// the longest usable candidate wins
void test_longest_wins() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(600), cand(2048), cand(1024), cand(4096) }, sk);

    CHECK(plan.capture);
    CHECK_EQ(plan.prefix_tokens, 4096);
    CHECK(sk == skip_t::none);
}

// a candidate shorter than the disk tier's minimum is not a capture target,
// even though the directory considered it eligible
void test_disk_minimum_is_separate() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 1024, 8192),
            { cand(700), cand(900) }, sk);

    CHECK(!plan.capture);
    CHECK(sk == skip_t::too_short); // not "no_candidates": the directory did offer some
}

// a short candidate does not block a longer usable one in the same report
void test_short_does_not_block_long() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 1024, 8192),
            { cand(300), cand(4096) }, sk);

    CHECK(plan.capture);
    CHECK_EQ(plan.prefix_tokens, 4096);
}

// an already-persisted prefix has its snapshot: capturing it again is pointless
void test_already_persisted() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(1024, /*persisted=*/true) }, sk);

    CHECK(!plan.capture);
    CHECK(sk == skip_t::already_persisted);
}

// a persisted shorter prefix does not block an unpersisted longer one
void test_persisted_shorter_does_not_block() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(1024, true), cand(2048, false) }, sk);

    CHECK(plan.capture);
    CHECK_EQ(plan.prefix_tokens, 2048);
}

// a prefix longer than this request cannot be reached by it
void test_beyond_input() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 1000),
            { cand(2048) }, sk);

    CHECK(!plan.capture);
    CHECK(sk == skip_t::beyond_input);
}

// a prefix whose end lands inside a media chunk has no complete-state boundary
void test_media_boundary() {
    auto r = req(true, 512, 8192);
    r.has_media        = true;
    r.media_chunk_start = 1024;
    r.media_chunk_end   = 2048;

    skip_t sk = skip_t::none;

    // 1500 ends inside the chunk: refused
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(1500) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::media);
    }

    // 1024 is exactly the chunk start: a chunk begins there, so the prefix ends
    // before it and is fine
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(1024) }, sk);
        CHECK(plan.capture);
        CHECK_EQ(plan.prefix_tokens, 1024);
    }

    // 2048 is exactly one past the chunk: the chunk is fully contained
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(2048) }, sk);
        CHECK(plan.capture);
        CHECK_EQ(plan.prefix_tokens, 2048);
    }
}

// the KV has already moved past the position: stopping there is impossible
void test_past_the_boundary() {
    skip_t sk = skip_t::none;

    // n_past == the boundary is fine: the state sits exactly there
    {
        const auto plan = disk_cache_plan_materialize(req(true, 512, 8192, /*n_past=*/1024),
                { cand(1024) }, sk);
        CHECK(plan.capture);
        CHECK_EQ(plan.prefix_tokens, 1024);
    }

    // n_past beyond it: refused, no rewinding
    {
        const auto plan = disk_cache_plan_materialize(req(true, 512, 8192, /*n_past=*/2000),
                { cand(1024) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::past);
    }

    // a longer candidate still reachable wins over a passed shorter one
    {
        const auto plan = disk_cache_plan_materialize(req(true, 512, 8192, /*n_past=*/2000),
                { cand(1024), cand(4096) }, sk);
        CHECK(plan.capture);
        CHECK_EQ(plan.prefix_tokens, 4096);
    }
}

// a zero-length candidate describes nothing
void test_zero_length_ignored() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(0), cand(2048) }, sk);

    CHECK(plan.capture);
    CHECK_EQ(plan.prefix_tokens, 2048);
}

// the real scenario: two requests share A, the third walks through it
void test_shared_prefix_scenario() {
    // the directory reported A (1024 tokens) as eligible while the request asks
    // for 8192; the disk tier requires 512
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(1024, false, /*obs=*/2) }, sk);

    CHECK(plan.capture);
    CHECK_EQ(plan.prefix_tokens, 1024);
    CHECK(sk == skip_t::none);

    // the second time around A already has a snapshot: nothing to do
    const auto again = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(1024, true, 3) }, sk);
    CHECK(!again.capture);
    CHECK(sk == skip_t::already_persisted);
}

// `observed` is not a capture target: only an eligible prefix is
void test_observed_is_not_enough() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(2048, /*persisted=*/false, /*obs=*/1, /*eligible=*/false) }, sk);

    CHECK(!plan.capture);
    CHECK(sk == skip_t::no_candidates); // nothing eligible was offered
}

// an eligible longer prefix wins over a shorter observed one
void test_eligible_beats_observed() {
    skip_t sk = skip_t::none;
    const auto plan = disk_cache_plan_materialize(req(true, 512, 8192),
            { cand(4096, false, 1, /*eligible=*/false), cand(2048, false, 3, /*eligible=*/true) }, sk);

    CHECK(plan.capture);
    CHECK_EQ(plan.prefix_tokens, 2048);
}

// every refusal reason has a printable name, distinct from "none"
void test_skip_names() {
    const skip_t all[] = {
        skip_t::none, skip_t::disabled, skip_t::no_candidates, skip_t::too_short,
        skip_t::already_persisted, skip_t::beyond_input, skip_t::media, skip_t::past,
        skip_t::duplicate,
    };

    for (const auto s : all) {
        const std::string name = disk_cache_materialize_skip_name(s);
        CHECK(!name.empty());
        if (s != skip_t::none) {
            CHECK(name != "none");
        }
    }
}

} // namespace

int main() {
    test_disabled();
    test_no_candidates();
    test_longest_wins();
    test_disk_minimum_is_separate();
    test_short_does_not_block_long();
    test_already_persisted();
    test_persisted_shorter_does_not_block();
    test_beyond_input();
    test_media_boundary();
    test_past_the_boundary();
    test_zero_length_ignored();
    test_shared_prefix_scenario();
    test_observed_is_not_enough();
    test_eligible_beats_observed();
    test_skip_names();

    if (g_failures == 0) {
        std::printf("all prefix-materialize checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
