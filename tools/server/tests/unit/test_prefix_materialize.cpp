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

disk_cache_media_chunk chunk(size_t start, size_t end) {
    disk_cache_media_chunk c;
    c.start = start;
    c.end   = end;
    return c;
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
    r.media_chunks.push_back(chunk(1024, 2048));

    skip_t sk = skip_t::none;

    // 1500 ends inside the chunk: refused
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(1500) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::media);
    }

    // 1024 is exactly the chunk start: the chunk is entirely ahead, so the prefix
    // ends before it and is fine
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

    // 1023 and 2049 are outside it entirely
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(1023) }, sk);
        CHECK(plan.capture);
    }
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(4096) }, sk);
        CHECK(plan.capture);
    }
}

// the gap between two chunks is a legal boundary. treating the chunks as one span
// from the first to the last would refuse it for no reason
void test_media_gap_is_legal() {
    // the disk minimum is 512, so every candidate here has to be at least that
    // long for the media check to be the thing under test
    auto r = req(true, 512, 8192);
    r.media_chunks.push_back(chunk(600, 700));
    r.media_chunks.push_back(chunk(900, 1000));

    skip_t sk = skip_t::none;

    // k = 800 sits between the two chunks: allowed. one span from 600 to 1000
    // would have refused it
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(800) }, sk);
        CHECK(plan.capture);
        CHECK_EQ(plan.prefix_tokens, 800);
    }

    // k = 700 is the first chunk's end and k = 900 the second's start: both allowed
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(700) }, sk);
        CHECK(plan.capture);
    }
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(900) }, sk);
        CHECK(plan.capture);
    }

    // inside either chunk: refused
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(650) }, sk);
        CHECK(!plan.capture);
    }
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(950) }, sk);
        CHECK(!plan.capture);
    }

    // past every chunk: allowed
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(2048) }, sk);
        CHECK(plan.capture);
    }

    // the longest usable candidate still wins across the gap: a longer one past
    // the chunks beats the one between them
    {
        const auto plan = disk_cache_plan_materialize(r, { cand(800), cand(2048) }, sk);
        CHECK(plan.capture);
        CHECK_EQ(plan.prefix_tokens, 2048);
    }
}

// a malformed chunk list makes the boundary unknown. it must refuse, not silently
// behave like a request without media
void test_media_chunks_malformed() {
    skip_t sk = skip_t::none;

    // overlapping
    {
        auto r = req(true, 512, 8192);
        r.media_chunks.push_back(chunk(10, 50));
        r.media_chunks.push_back(chunk(30, 60));
        const auto plan = disk_cache_plan_materialize(r, { cand(1024) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::media);
    }

    // out of order
    {
        auto r = req(true, 512, 8192);
        r.media_chunks.push_back(chunk(100, 200));
        r.media_chunks.push_back(chunk(10, 20));
        const auto plan = disk_cache_plan_materialize(r, { cand(1024) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::media);
    }

    // empty chunk
    {
        auto r = req(true, 512, 8192);
        r.media_chunks.push_back(chunk(20, 20));
        const auto plan = disk_cache_plan_materialize(r, { cand(1024) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::media);
    }

    // past the end of the request
    {
        auto r = req(true, 512, 1000);
        r.media_chunks.push_back(chunk(900, 1200));
        const auto plan = disk_cache_plan_materialize(r, { cand(1000) }, sk);
        CHECK(!plan.capture);
        CHECK(sk == skip_t::media);
    }

    // a well-formed list is accepted
    {
        auto r = req(true, 512, 8192);
        r.media_chunks.push_back(chunk(100, 200));
        r.media_chunks.push_back(chunk(300, 400));
        const auto plan = disk_cache_plan_materialize(r, { cand(1024) }, sk);
        CHECK(plan.capture);
    }
}

// the chunk predicate itself, at its boundaries
void test_media_chunk_predicate() {
    const std::vector<disk_cache_media_chunk> chunks = { chunk(10, 20), chunk(40, 50) };

    CHECK( disk_cache_media_chunks_valid(chunks, 8192));

    CHECK(!disk_cache_prefix_in_media( 9, chunks));
    CHECK(!disk_cache_prefix_in_media(10, chunks)); // chunk start
    CHECK( disk_cache_prefix_in_media(11, chunks));
    CHECK( disk_cache_prefix_in_media(19, chunks));
    CHECK(!disk_cache_prefix_in_media(20, chunks)); // chunk end
    CHECK(!disk_cache_prefix_in_media(30, chunks)); // between chunks
    CHECK(!disk_cache_prefix_in_media(40, chunks)); // second chunk start
    CHECK( disk_cache_prefix_in_media(49, chunks));
    CHECK(!disk_cache_prefix_in_media(50, chunks));
    CHECK(!disk_cache_prefix_in_media(51, chunks));
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

// ---------------------------------------------------------------------------
// the paired-state contract: target and draft must both end exactly on the prefix
// ---------------------------------------------------------------------------
void test_capture_bounds() {
    using b = disk_cache_capture_bounds;

    // target exactly on K, no draft configured: this is the target-only path
    CHECK(disk_cache_check_capture_bounds(100, 100, false, -1) == b::exact);

    // target exactly on K, draft exactly on K
    CHECK(disk_cache_check_capture_bounds(100, 100, true, 100) == b::exact);

    // target has not arrived: the plan waits, this is not a failure
    CHECK(disk_cache_check_capture_bounds(100,  99, true,  99) == b::target_short);

    // target past the prefix: the state no longer describes it
    CHECK(disk_cache_check_capture_bounds(100, 101, true, 101) == b::target_past);

    // the draft lags the target
    CHECK(disk_cache_check_capture_bounds(100, 100, true,  99) == b::draft_short);

    // the draft ran ahead: this is the branch that was missing. no rewinding, no
    // slicing of the draft's opaque state
    CHECK(disk_cache_check_capture_bounds(100, 100, true, 101) == b::draft_past);

    // target short and draft past: the target's own problem is reported first
    CHECK(disk_cache_check_capture_bounds(100,  99, true, 101) == b::target_short);

    // a draft context is configured but holds nothing
    CHECK(disk_cache_check_capture_bounds(100, 100, true,  -1) == b::draft_missing);

    // a negative target endpoint means the sequence holds nothing
    CHECK(disk_cache_check_capture_bounds(  1,  -1, false, -1) == b::target_short);

    // a target-only deployment never reports a draft problem
    CHECK(disk_cache_check_capture_bounds(100, 100, false, 101) == b::exact);
    CHECK(disk_cache_check_capture_bounds(100, 101, false, -1) == b::target_past);

    // every outcome has its own name
    const b all[] = { b::exact, b::target_short, b::target_past, b::draft_short, b::draft_past, b::draft_missing };
    for (const auto v : all) {
        CHECK(std::string(disk_cache_capture_bounds_name(v)) != "unknown");
    }
}

// ---------------------------------------------------------------------------
// a checkpoint must not describe KV beyond the snapshot's positions
// ---------------------------------------------------------------------------
void test_checkpoint_fits() {
    // real text-only relation, as observed in the server logs: a checkpoint with
    // n_tokens tokens has pos_max == n_tokens - 1
    //
    // the entry claims K = 100 tokens and its KV ends at position 99

    // a checkpoint covering [0, 98] with 99 tokens is inside
    CHECK(disk_cache_checkpoint_fits({ 0,  98,  99 }, 100, 99));

    // the last covered checkpoint: 100 tokens, pos_max == 99
    CHECK(disk_cache_checkpoint_fits({ 0,  99, 100 }, 100, 99));

    // 101 tokens reach 100, one position past the snapshot's KV. the old
    // `pos_max <= K` check admitted exactly this one
    CHECK(!disk_cache_checkpoint_fits({ 0, 100, 101 }, 100, 99));

    // n_tokens beyond the prefix is out even when the position looks small
    CHECK(!disk_cache_checkpoint_fits({ 0,  98, 101 }, 100, 99));

    // a position past the KV endpoint is out even with an acceptable token count
    CHECK(!disk_cache_checkpoint_fits({ 0, 100, 100 }, 100, 99));

    // a checkpoint starting past the endpoint cannot be inside
    CHECK(!disk_cache_checkpoint_fits({ 150, 160,  11 }, 100, 99));

    // degenerate inputs are refused rather than guessed at
    CHECK(!disk_cache_checkpoint_fits({ 0,  10,  11 },   0, -1)); // no prefix
    CHECK(!disk_cache_checkpoint_fits({ 5,   4,   1 }, 100, 99)); // pos_min > pos_max
    CHECK(!disk_cache_checkpoint_fits({ 0,  -1,   1 }, 100, 99)); // negative position
    CHECK(!disk_cache_checkpoint_fits({ 0,  10,  -1 }, 100, 99)); // negative count
    CHECK(!disk_cache_checkpoint_fits({ 0,   0,   1 }, 100, -1)); // no KV endpoint

    // the prefix boundary of 1 token: position 0
    CHECK(disk_cache_checkpoint_fits({ 0,   0,   1 },   1,  0));
    CHECK(!disk_cache_checkpoint_fits({ 0,   1,   1 },   1,  0));

    // with media the two bounds differ: 10 tokens covering positions [0, 25).
    // a checkpoint that fits the token count but not the position bound is out
    CHECK(disk_cache_checkpoint_fits({ 0,  20,   8 }, 10, 25));
    CHECK(!disk_cache_checkpoint_fits({ 0,  26,   8 }, 10, 25));
}

// the boundary arithmetic itself. it lives in the module so it is covered here
// instead of being recomputed at each call site - an off-by-one there would
// silently admit one extra position
void test_prefix_pos_max_text() {
    // N tokens occupy positions [0, N): the highest one is N - 1
    CHECK_EQ(disk_cache_prefix_pos_max_text(   1),    0);
    CHECK_EQ(disk_cache_prefix_pos_max_text(   2),    1);
    CHECK_EQ(disk_cache_prefix_pos_max_text( 100),   99);
    CHECK_EQ(disk_cache_prefix_pos_max_text(2102), 2101);

    // an empty prefix has no valid position, reported as -1. the predicate
    // refuses a negative bound, so this is a refusal rather than "everything fits"
    CHECK_EQ(disk_cache_prefix_pos_max_text(0), -1);
    CHECK(!disk_cache_checkpoint_fits({ 0, 0, 1 }, 0, disk_cache_prefix_pos_max_text(0)));

    // the two functions agree: a checkpoint of exactly N tokens fits, one of N+1
    // does not. the lower variant needs N >= 2 for a non-negative pos_max
    for (size_t n = 1; n <= 32; ++n) {
        const int64_t bound = disk_cache_prefix_pos_max_text(n);

        CHECK(disk_cache_checkpoint_fits({ 0, (int64_t) n - 1, (int64_t) n }, n, bound));

        if (n >= 2) {
            CHECK(disk_cache_checkpoint_fits({ 0, (int64_t) n - 2, (int64_t) n - 1 }, n, bound));
        }

        CHECK(!disk_cache_checkpoint_fits({ 0, (int64_t) n, (int64_t) n + 1 }, n, bound));
    }

    // passing the token count as the position bound is the historical mistake.
    // the shape below fits the token check (100 tokens in a 100-token prefix) and
    // is refused only because its position bound is off by one
    CHECK( disk_cache_checkpoint_fits({ 0,  99, 100 }, 100,  99)); // correct bound: inside
    CHECK(!disk_cache_checkpoint_fits({ 0, 100, 100 }, 100,  99)); // ... and the next position is out
    CHECK( disk_cache_checkpoint_fits({ 0, 100, 100 }, 100, 100)); // bound taken as K: wrongly admitted
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
    test_media_gap_is_legal();
    test_media_chunks_malformed();
    test_media_chunk_predicate();
    test_capture_bounds();
    test_checkpoint_fits();
    test_prefix_pos_max_text();
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
