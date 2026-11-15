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

    // first request: nothing to share with
    auto r1 = idx.observe(make_obs(1, b));
    CHECK_EQ(r1.size(), 0);

    // A+C sees A (10 tokens), observed once, below the observation gate
    auto r2 = idx.observe(make_obs(2, c));
    CHECK_EQ(longest(r2), 10);
    CHECK_EQ(find_candidate(r2, 10).observations, 1);
    CHECK(find_candidate(r2, 10).state == prefix_state::observed);

    // A+D: A now has two observations and passes the gates
    auto r3 = idx.observe(make_obs(3, d));
    CHECK_EQ(longest(r3), 10);
    CHECK_EQ(find_candidate(r3, 10).observations, 2);
    CHECK(find_candidate(r3, 10).state == prefix_state::eligible);

    // the fork point is reported once, and the full A+B is still its own node
    auto r4 = idx.observe(make_obs(4, b));
    CHECK(has_prefix(r4, 15));
    CHECK(has_prefix(r4, 10));
    CHECK(r4.front().prefix_tokens >= r4.back().prefix_tokens);

    // nothing shared at all
    llama_tokens other(12, 7);
    auto r5 = idx.observe(make_obs(5, other));
    CHECK_EQ(r5.size(), 0);

    // a strict prefix of a stored path
    auto r6 = idx.observe(make_obs(6, a));
    CHECK(has_prefix(r6, 10));
}

void test_request_id_dedup() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens a(12, 5);
    llama_tokens b = a;
    b.push_back(9);

    idx.observe(make_obs(77, a));

    // same request id at the output boundary: no second observation, no candidates
    auto out = idx.observe(make_obs(77, b));
    CHECK_EQ(out.size(), 0);
    CHECK_EQ(idx.stats().n_requests, 1);

    auto s = idx.stats();
    CHECK_EQ(s.n_nodes, 1); // one path only, the output boundary was ignored
}

void test_media_identity() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens base(12, 3);
    // media chunk covers tokens [8, 11)
    for (size_t i = 8; i < 12; ++i) {
        base[i] = LLAMA_TOKEN_NULL;
    }

    prefix_observation img1;
    img1.request_id        = 10;
    img1.tokens            = base;
    img1.media_indices     = { 8 };
    img1.media_token_counts = { 4 };
    img1.media_ids         = { "img-A" };

    prefix_observation img2 = img1;
    img2.request_id = 11;
    img2.media_ids  = { "img-B" };

    idx.observe(img1);

    // the same token ids with a different image must not share the media chunk
    auto r = idx.observe(img2);
    CHECK_EQ(longest(r), 8);

    // same image again: the media boundary is shared, the whole prefix matches
    prefix_observation img1b = img1;
    img1b.request_id = 12;
    auto r2 = idx.observe(img1b);
    CHECK_EQ(longest(r2), 12);
}

void test_media_clamp() {
    server_prefix_index idx("sig", small_opt());

    // a request whose media chunk starts at 4 of 12 tokens: a candidate inside
    // the chunk must be clamped to its start
    llama_tokens tokens(12, 4);
    for (size_t i = 4; i < 8; ++i) {
        tokens[i] = LLAMA_TOKEN_NULL;
    }

    prefix_observation o;
    o.request_id         = 20;
    o.tokens             = tokens;
    o.media_indices      = { 4 };
    o.media_token_counts = { 4 };
    o.media_ids          = { "img" };

    idx.observe(o);

    // a longer request that shares tokens but has no media at all: the shared
    // token path must stop before the media chunk
    prefix_observation plain;
    plain.request_id = 21;
    plain.tokens     = tokens;
    plain.tokens.push_back(42);

    auto r = idx.observe(plain);
    for (const auto & c : r) {
        CHECK(c.prefix_tokens <= 4 || c.prefix_tokens >= 8);
    }
}

void test_snapshot_refs() {
    server_prefix_index idx("sig", small_opt());

    llama_tokens a(10, 1);
    llama_tokens ab = a;
    for (int i = 0; i < 4; ++i) { ab.push_back(2); }

    idx.observe(make_obs(1, ab));
    idx.observe(make_obs(2, a));

    // a snapshot of A+B must not mark A as persisted
    idx.on_snapshot_committed("pc-ab.bin", ab, {}, {}, {});

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
    idx.on_snapshot_invalidated("pc-ab.bin");
    auto rab2 = idx.candidates(ab, {}, {}, {});
    CHECK(find_candidate(rab2, 14).state == prefix_state::unavailable);
    CHECK(find_candidate(rab2, 14).snapshot_path.empty());
    CHECK_EQ(idx.stats().n_snapshots, 0);
    CHECK_EQ(idx.stats().n_invalidated, 1);

    // an unknown path is a no-op
    idx.on_snapshot_invalidated("does-not-exist");
    CHECK_EQ(idx.stats().n_invalidated, 1);
}

void test_capacity() {
    prefix_index_options opt = small_opt();
    opt.max_candidates = 6;

    server_prefix_index idx("sig", opt);

    llama_tokens keep(10, 9);
    idx.observe(make_obs(1000, keep));
    idx.on_snapshot_committed("pc-keep.bin", keep, {}, {}, {});

    // 40 distinct long prefixes, each its own path: the directory must stay
    // bounded and must not drop the node that carries a physical reference
    for (int i = 0; i < 40; ++i) {
        llama_tokens t(20, (llama_token) (1000 + i));
        idx.observe(make_obs((uint64_t) (2000 + i), t));
    }

    auto s = idx.stats();
    CHECK(s.n_nodes <= opt.max_candidates + 1); // +1 for the root being excluded
    CHECK(s.n_evicted > 0);
    CHECK_EQ(s.n_persisted, 1);

    auto r = idx.candidates(keep, {}, {}, {});
    CHECK(find_candidate(r, 10).state == prefix_state::persisted);
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
    CHECK_EQ(r.size(), 0);
    CHECK_EQ(idx.stats().n_persisted, 0);
}

} // namespace

int main() {
    test_fork_discovery();
    test_request_id_dedup();
    test_media_identity();
    test_media_clamp();
    test_snapshot_refs();
    test_capacity();
    test_disabled_noop();

    if (g_failures == 0) {
        std::printf("all prefix-index checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
