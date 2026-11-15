// standalone self-check for server-prefix-index
//
// This is a pure metadata test: it never loads a model, never touches a
// llama_context and never reads a disk file. Build it with the same toolchain as
// the server, for example (Windows, from the repository root, inside a shell that
// has vcvars64 loaded):
//
//   cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
//      /I tools/server /I common /I include /I ggml/include /I vendor/nlohmann ^
//      /I tools/mtmd ^
//      /Fe:test-run\prefix_index_test.exe ^
//      tools/server/tests/unit/test_cache_prefix_index.cpp tools/server/server-prefix-index.cpp
//
// It is deliberately not wired into the CMake test target: the server test
// directory is a pytest suite that needs a running server, and this check must be
// runnable without one.

#include "server-prefix-index.h"

#include <cstdio>
#include <cstdlib>
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

prefix_index_options small_opt() {
    prefix_index_options opt;
    opt.min_tokens       = 8;
    opt.min_observations = 2;
    opt.max_candidates   = 64;
    opt.max_index_bytes  = 1ull << 20;
    return opt;
}

prefix_observation make_obs(uint64_t id, const llama_tokens & tokens) {
    prefix_observation obs;
    obs.request_id = id;
    obs.tokens     = tokens;
    return obs;
}

prefix_candidate find_candidate(const std::vector<prefix_candidate> & list, size_t prefix) {
    for (const auto & c : list) {
        if (c.prefix_tokens == prefix) {
            return c;
        }
    }
    return prefix_candidate{};
}

size_t longest(const std::vector<prefix_candidate> & list) {
    return list.empty() ? 0 : list.front().prefix_tokens;
}

bool has_prefix(const std::vector<prefix_candidate> & list, size_t prefix) {
    for (const auto & c : list) {
        if (c.prefix_tokens == prefix) {
            return true;
        }
    }
    return false;
}

void test_fork_discovery() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens a(10);
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = (llama_token) (100 + i);
    }

    llama_tokens b = a;
    for (int i = 0; i < 5; ++i) { b.push_back((llama_token) (200 + i)); }
    llama_tokens c = a;
    for (int i = 0; i < 7; ++i) { c.push_back((llama_token) (300 + i)); }
    llama_tokens d = a;
    for (int i = 0; i < 6; ++i) { d.push_back((llama_token) (400 + i)); }

    auto r1 = idx.observe(make_obs(1, b));
    CHECK(r1.status == prefix_input_status::ok);
    CHECK_EQ(r1.candidates.size(), 0);

    // A+C sees A (10 tokens), observed once, below the observation gate
    auto r2 = idx.observe(make_obs(2, c));
    CHECK_EQ(longest(r2.candidates), 10);
    CHECK_EQ(find_candidate(r2.candidates, 10).observations, 1);
    CHECK(find_candidate(r2.candidates, 10).state == prefix_state::observed);

    // A+D: A now has two observations and passes the gates
    auto r3 = idx.observe(make_obs(3, d));
    CHECK_EQ(longest(r3.candidates), 10);
    CHECK_EQ(find_candidate(r3.candidates, 10).observations, 2);
    CHECK(find_candidate(r3.candidates, 10).state == prefix_state::eligible);

    // the fork point is reported once, and the full A+B is still its own node
    auto r4 = idx.observe(make_obs(4, b));
    CHECK(has_prefix(r4.candidates, 15));
    CHECK(has_prefix(r4.candidates, 10));
    CHECK(r4.candidates.front().prefix_tokens >= r4.candidates.back().prefix_tokens);

    // nothing shared at all
    llama_tokens other(12, 7);
    auto r5 = idx.observe(make_obs(5, other));
    CHECK_EQ(r5.candidates.size(), 0);

    // a strict prefix of a stored path
    auto r6 = idx.observe(make_obs(6, a));
    CHECK(has_prefix(r6.candidates, 10));
}

void test_request_id_dedup() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens a(12, 5);
    llama_tokens b = a;
    b.push_back(9);

    idx.observe(make_obs(77, a));

    // same request id at the output boundary: no second observation, no candidates
    auto out = idx.observe(make_obs(77, b));
    CHECK_EQ(out.candidates.size(), 0);
    CHECK_EQ(idx.stats().n_requests, 1);

    auto s = idx.stats();
    CHECK_EQ(s.n_nodes, 1); // one path only, the output boundary was ignored
}

void test_media_identity() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens base(12, 3);
    // media chunk covers tokens [8, 12)
    for (size_t i = 8; i < 12; ++i) {
        base[i] = LLAMA_TOKEN_NULL;
    }

    prefix_observation img1;
    img1.request_id         = 10;
    img1.tokens             = base;
    img1.media_indices      = { 8 };
    img1.media_token_counts = { 4 };
    img1.media_ids          = { "img-A" };

    prefix_observation img2 = img1;
    img2.request_id = 11;
    img2.media_ids  = { "img-B" };

    idx.observe(img1);

    // the same token ids with a different image must not share the media chunk
    auto r = idx.observe(img2);
    CHECK_EQ(longest(r.candidates), 8);

    // same image again: the media boundary is shared, the whole prefix matches
    prefix_observation img1b = img1;
    img1b.request_id = 12;
    auto r2 = idx.observe(img1b);
    CHECK_EQ(longest(r2.candidates), 12);

    // same id but a different chunk length is a different identity
    prefix_observation img1c = img1;
    img1c.request_id         = 13;
    img1c.media_token_counts = { 3 };
    auto r3 = idx.observe(img1c);
    CHECK_EQ(longest(r3.candidates), 8);
}

void test_readonly_media_query() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens tokens(12, 4);
    prefix_observation o;
    o.request_id         = 20;
    o.tokens             = tokens;
    o.media_indices      = { 8 };
    o.media_token_counts = { 4 };
    o.media_ids          = { "img-known" };
    idx.observe(o);

    const auto before = idx.stats();
    CHECK_EQ(before.n_media_ids, 1);

    // a query must not grow the intern table, and an unknown identity must stop
    // the match at the media chunk start (the plain tokens before it are shared)
    auto c1 = idx.candidates(tokens, { 8 }, { 4 }, { "img-unknown" });
    auto c2 = idx.candidates(tokens, { 8 }, { 4 }, { "img-unknown-2" });

    CHECK_EQ(longest(c1), 8);
    CHECK_EQ(longest(c2), 8);
    CHECK_EQ(idx.stats().n_media_ids, before.n_media_ids);

    // the known identity still matches through the query path
    auto c3 = idx.candidates(tokens, { 8 }, { 4 }, { "img-known" });
    CHECK_EQ(longest(c3), 12);
    CHECK_EQ(idx.stats().n_media_ids, before.n_media_ids);
}

void test_invalid_media() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens tokens(12, 6);

    // array sizes disagree
    {
        prefix_observation o = make_obs(30, tokens);
        o.media_indices      = { 4 };
        o.media_token_counts = { 4 }; // no id
        auto r = idx.observe(o);
        CHECK(r.status == prefix_input_status::invalid_media);
    }

    // overlapping chunks
    {
        prefix_observation o = make_obs(31, tokens);
        o.media_indices      = { 2, 5 };
        o.media_token_counts = { 4, 2 };
        o.media_ids          = { "a", "b" };
        auto r = idx.observe(o);
        CHECK(r.status == prefix_input_status::invalid_media);
    }

    // out of range
    {
        prefix_observation o = make_obs(32, tokens);
        o.media_indices      = { 10 };
        o.media_token_counts = { 5 };
        o.media_ids          = { "a" };
        auto r = idx.observe(o);
        CHECK(r.status == prefix_input_status::invalid_media);
    }

    // empty identity
    {
        prefix_observation o = make_obs(33, tokens);
        o.media_indices      = { 4 };
        o.media_token_counts = { 4 };
        o.media_ids          = { "" };
        auto r = idx.observe(o);
        CHECK(r.status == prefix_input_status::invalid_media);
    }

    // a refused observation must not create nodes or consume a request id
    const auto s = idx.stats();
    CHECK_EQ(s.n_nodes, 0);
    CHECK_EQ(s.n_requests, 0);
    CHECK_EQ(s.n_input_rejected, 4);

    // a malformed read-only query is refused as well
    auto c = idx.candidates(tokens, { 2, 5 }, { 4, 2 }, { "a", "b" });
    CHECK_EQ(c.size(), 0);
}

void test_media_budget() {
    prefix_index_options opt = small_opt();
    opt.max_index_bytes = 160; // too small for one more identity string

    server_prefix_index idx("sig", opt);

    llama_tokens tokens(12, 1);
    prefix_observation o = make_obs(40, tokens);
    o.media_indices      = { 4 };
    o.media_token_counts = { 4 };
    o.media_ids          = { std::string(200, 'm') };

    auto r = idx.observe(o);
    CHECK(r.status == prefix_input_status::capacity);
    CHECK_EQ(idx.stats().n_media_ids, 0);
    CHECK_EQ(idx.stats().n_nodes, 0);
}

void test_snapshot_refs() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens a(10, 1);
    llama_tokens ab = a;
    for (int i = 0; i < 4; ++i) { ab.push_back(2); }

    idx.observe(make_obs(1, ab));
    idx.observe(make_obs(2, a));

    // a snapshot of A+B must not mark A as persisted
    idx.on_snapshot_committed("pc-ab.bin", 1, ab, {}, {}, {});

    auto r = idx.candidates(a, {}, {}, {});
    CHECK(has_prefix(r, 10));
    CHECK(find_candidate(r, 10).state != prefix_state::persisted);
    CHECK(find_candidate(r, 10).snapshot_path.empty());

    auto rab = idx.candidates(ab, {}, {}, {});
    CHECK(find_candidate(rab, 14).state == prefix_state::persisted);
    CHECK(find_candidate(rab, 14).snapshot_path == "pc-ab.bin");

    auto s = idx.stats();
    CHECK_EQ(s.n_persisted, 1);
    CHECK_EQ(s.n_snapshots, 1);

    // invalidation withdraws the reference instead of keeping a dangling one
    idx.on_snapshot_invalidated("pc-ab.bin", 2);
    auto rab2 = idx.candidates(ab, {}, {}, {});
    CHECK(find_candidate(rab2, 14).state == prefix_state::unavailable);
    CHECK(find_candidate(rab2, 14).snapshot_path.empty());
    CHECK_EQ(idx.stats().n_snapshots, 0);
    CHECK_EQ(idx.stats().n_invalidated, 1);

    // an unknown path is a no-op
    idx.on_snapshot_invalidated("does-not-exist", 3);
    CHECK_EQ(idx.stats().n_invalidated, 1);
}

void test_event_order() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens tokens(12, 9);

    // a withdrawal is remembered even when the commit was never seen
    idx.on_snapshot_invalidated("pc-late.bin", 5);

    // the late commit (older epoch) must not resurrect it
    idx.on_snapshot_committed("pc-late.bin", 3, tokens, {}, {}, {});
    auto r1 = idx.candidates(tokens, {}, {}, {});
    CHECK(find_candidate(r1, 12).state != prefix_state::persisted);
    CHECK(find_candidate(r1, 12).snapshot_path.empty());
    CHECK_EQ(idx.stats().n_snapshots, 0);

    // a newer commit is accepted
    idx.on_snapshot_committed("pc-late.bin", 6, tokens, {}, {}, {});
    auto r2 = idx.candidates(tokens, {}, {}, {});
    CHECK(find_candidate(r2, 12).state == prefix_state::persisted);
    CHECK(find_candidate(r2, 12).snapshot_path == "pc-late.bin");
    CHECK_EQ(idx.stats().n_snapshots, 1);
}

void test_capacity_termination() {
    prefix_index_options opt = small_opt();
    opt.max_candidates = 1; // the root plus a single node

    server_prefix_index idx("sig", opt);

    llama_tokens keep(10, 9);
    idx.observe(make_obs(1000, keep));
    idx.on_snapshot_committed("pc-keep.bin", 1, keep, {}, {}, {});

    // a committed node is never evicted; every further observation is admitted
    // and then evicted again. the important part is that each call returns and
    // that the refusal counter does not grow without bound
    for (int i = 0; i < 20; ++i) {
        llama_tokens t(20, (llama_token) (1000 + i));
        auto r = idx.observe(make_obs((uint64_t) (2000 + i), t));
        CHECK(r.status == prefix_input_status::ok);
    }

    auto s = idx.stats();
    // at most one refusal per call that could not evict (21 observations plus the
    // commit registration above): the counter must not grow per loop iteration
    CHECK(s.n_rejected <= 25);
    CHECK_EQ(s.n_persisted, 1);
    CHECK(s.n_nodes <= 2);     // the protected node plus whatever is in flight

    // the protected prefix is still queryable
    auto r = idx.candidates(keep, {}, {}, {});
    CHECK(find_candidate(r, 10).state == prefix_state::persisted);
}

void test_capacity_evicts_unreferenced() {
    prefix_index_options opt = small_opt();
    opt.max_candidates = 8;

    server_prefix_index idx("sig", opt);

    for (int i = 0; i < 60; ++i) {
        llama_tokens t(20, (llama_token) (500 + i));
        idx.observe(make_obs((uint64_t) (3000 + i), t));
    }

    auto s = idx.stats();
    CHECK(s.n_nodes <= opt.max_candidates);
    CHECK(s.n_evicted > 0);
}

void test_reference_survives_compaction() {
    prefix_index_options opt = small_opt();
    opt.max_candidates = 4; // forces evictions, and the compaction that follows them

    server_prefix_index idx("sig", opt);

    llama_tokens keep(10, 77);
    idx.observe(make_obs(7000, keep));
    idx.on_snapshot_committed("pc-keep.bin", 1, keep, {}, {}, {});

    // enough dead nodes to trip the compaction threshold
    for (int i = 0; i < 120; ++i) {
        llama_tokens t(24, (llama_token) (20000 + i));
        idx.observe(make_obs((uint64_t) (7100 + i), t));
    }

    const auto s = idx.stats();
    CHECK(s.n_evicted > 60);
    CHECK_EQ(s.n_persisted, 1);
    CHECK_EQ(s.n_snapshots, 1);

    // the reference must still point at the surviving node after remapping
    auto r = idx.candidates(keep, {}, {}, {});
    const auto c = find_candidate(r, 10);
    CHECK(c.state == prefix_state::persisted);
    CHECK(c.snapshot_path == "pc-keep.bin");

    // and a withdrawal still finds it
    idx.on_snapshot_invalidated("pc-keep.bin", 2);
    CHECK_EQ(idx.stats().n_snapshots, 0);
    CHECK_EQ(idx.stats().n_persisted, 0);
}

void test_media_boundary_property() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens tokens(16, 4);
    for (size_t i = 6; i < 12; ++i) {
        tokens[i] = LLAMA_TOKEN_NULL;
    }

    prefix_observation o;
    o.request_id         = 50;
    o.tokens             = tokens;
    o.media_indices      = { 6 };
    o.media_token_counts = { 6 };
    o.media_ids          = { "img" };

    idx.observe(o);
    idx.on_snapshot_committed("pc-media.bin", 1, o.tokens, o.media_indices, o.media_token_counts, o.media_ids);

    // no candidate may end strictly inside a media chunk, and a candidate that is
    // not the exact snapshot end may not keep its physical reference
    auto r = idx.candidates(tokens, o.media_indices, o.media_token_counts, o.media_ids);
    for (const auto & c : r) {
        const bool inside = c.prefix_tokens > 6 && c.prefix_tokens < 12;
        CHECK(!inside);
        if (c.prefix_tokens != 16) {
            CHECK(c.snapshot_path.empty());
            CHECK(c.state != prefix_state::persisted);
        }
    }

    // a shorter query cuts before the chunk and must not inherit the reference
    llama_tokens shorter(tokens.begin(), tokens.begin() + 8);
    auto r2 = idx.candidates(shorter, {}, {}, {});
    for (const auto & c : r2) {
        CHECK(c.prefix_tokens != 16);
    }
}

void test_disabled_noop() {
    // the server does not construct the index when the switch is off; the point
    // of this check is that a default-constructed directory reports zeros and
    // never invents a hit
    server_prefix_index idx("sig", prefix_index_options{});

    auto s = idx.stats();
    CHECK_EQ(s.n_nodes, 0);
    CHECK_EQ(s.n_persisted, 0);

    llama_tokens t(4, 1);
    auto r = idx.observe(make_obs(1, t));
    CHECK_EQ(r.candidates.size(), 0);
    CHECK_EQ(idx.stats().n_persisted, 0);
}

} // namespace

int main() {
    test_fork_discovery();
    test_request_id_dedup();
    test_media_identity();
    test_readonly_media_query();
    test_invalid_media();
    test_media_budget();
    test_snapshot_refs();
    test_event_order();
    test_capacity_termination();
    test_capacity_evicts_unreferenced();
    test_reference_survives_compaction();
    test_media_boundary_property();
    test_disabled_noop();

    if (g_failures == 0) {
        std::printf("all prefix-index checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
