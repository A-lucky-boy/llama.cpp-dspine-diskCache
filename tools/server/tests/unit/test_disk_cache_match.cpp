// standalone self-check for the disk-cache substitution helpers
//
// Pure metadata: no model, no llama_context, no file. Build it with the server
// toolchain (Windows, repository root, vcvars64 loaded):
//
//   cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
//      /I tools/server /I common /I include /I ggml/include /I vendor/nlohmann ^
//      /Fe:test-run\disk_cache_match_test.exe ^
//      tools/server/tests/unit/test_disk_cache_match.cpp tools/server/server-disk-cache-match.cpp

#include "server-disk-cache-match.h"

#include <cstdio>
#include <unordered_map>
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

// owns the buffers an identity points at
struct ident {
    llama_tokens             toks;
    std::vector<uint32_t>    idx;
    std::vector<uint32_t>    cnt;
    std::vector<std::string> ids;
    size_t                   bytes = 0;

    disk_cache_prompt_identity view() const {
        disk_cache_prompt_identity v;
        v.tokens             = &toks;
        v.media_indices      = &idx;
        v.media_token_counts = &cnt;
        v.media_ids          = &ids;
        v.file_bytes         = bytes;
        return v;
    }
};

ident make_ident(size_t n, llama_token base, size_t bytes = 0) {
    ident x;
    x.toks.resize(n);
    for (size_t i = 0; i < n; ++i) {
        x.toks[i] = base + (llama_token) i;
    }
    x.bytes = bytes;
    return x;
}

// shim so the view can be passed straight into the helpers
disk_cache_prompt_identity view_of(const ident & x) {
    return x.view();
}

constexpr size_t MIN_TOKENS = 8;
constexpr size_t LAG        = 4096;

void test_plain_prefix() {
    ident a = make_ident(20, 100);
    ident b = make_ident(12, 100); // strict token prefix

    const auto m = disk_cache_match_prefix(view_of(b), view_of(a));
    CHECK_EQ(m.token_lcp, 12);
    CHECK_EQ(m.usable_lcp, 12);
    CHECK(m.media_compatible);

    const auto rev = disk_cache_match_prefix(view_of(a), view_of(b));
    CHECK_EQ(rev.usable_lcp, 12);

    // unrelated
    ident c = make_ident(12, 900);
    CHECK_EQ(disk_cache_match_prefix(view_of(a), view_of(c)).usable_lcp, 0);
}

void test_media_identity() {
    // 20 tokens, media chunk [8, 12)
    ident a = make_ident(20, 100);
    for (size_t i = 8; i < 12; ++i) {
        a.toks[i] = LLAMA_TOKEN_NULL;
    }
    a.idx = { 8 };
    a.cnt = { 4 };
    a.ids = { "img-A" };

    // same tokens, different image
    ident b = a;
    b.ids = { "img-B" };
    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(b));
        CHECK_EQ(m.token_lcp, 20);   // placeholders are identical
        CHECK_EQ(m.usable_lcp, 8);   // the media chunk is not interchangeable
        CHECK(!m.media_compatible);
    }

    // same id, different chunk length: a different identity as well
    ident c = a;
    c.cnt = { 3 };
    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(c));
        CHECK_EQ(m.usable_lcp, 8);
        CHECK(!m.media_compatible);
    }

    // a chunk that only partially covers the other one is not a match either
    ident p = a;
    p.idx = { 6 };
    p.cnt = { 8 }; // [6, 14) versus A's [8, 12)
    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(p));
        CHECK_EQ(m.usable_lcp, 6);
        CHECK(!m.media_compatible);
    }

    // one side has no media metadata at all
    ident d = a;
    d.idx.clear();
    d.cnt.clear();
    d.ids.clear();
    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(d));
        CHECK_EQ(m.usable_lcp, 8);
        CHECK(!m.media_compatible);
    }

    // identical media: full match
    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(a));
        CHECK_EQ(m.usable_lcp, 20);
        CHECK(m.media_compatible);
    }
}

void test_media_boundary_clamp() {
    // a prefix that would end inside the chunk is clamped to the chunk start
    ident a = make_ident(20, 100);
    for (size_t i = 6; i < 12; ++i) {
        a.toks[i] = LLAMA_TOKEN_NULL;
    }
    a.idx = { 6 };
    a.cnt = { 6 };
    a.ids = { "img" };

    // the request shares the placeholder tokens (as it would when it is a slice of
    // the same prompt) but carries no media metadata: the usable prefix stops at
    // the chunk start, not at the token divergence
    ident b = a;
    b.toks.resize(9);
    b.idx.clear();
    b.cnt.clear();
    b.ids.clear();
    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(b));
        CHECK_EQ(m.token_lcp, 9);
        CHECK_EQ(m.usable_lcp, 6);
        CHECK(!m.media_compatible);
    }
}

// problem A: both directions must be scanned. A's chunk sits later than B's, so
// the earlier difference is found only by the B->A direction. a short-circuit
// `||` would return A's later position and over-report the usable prefix
void test_bidirectional_earliest_difference() {
    const size_t n = 40;

    // both prompts carry placeholders at 4..8 and 12..16, so every token id is
    // identical and only the media metadata can diverge. A declares its chunk at
    // 12, B at 4: the earlier difference is visible only from the B->A direction
    auto two_chunk_tokens = [&]() {
        ident x = make_ident(n, 5);
        for (size_t i = 4; i < 8; ++i) {
            x.toks[i] = LLAMA_TOKEN_NULL;
        }
        for (size_t i = 12; i < 16; ++i) {
            x.toks[i] = LLAMA_TOKEN_NULL;
        }
        return x;
    };

    ident a = two_chunk_tokens();
    a.idx = { 12 };
    a.cnt = { 4 };
    a.ids = { "later" };

    ident b = two_chunk_tokens();
    b.idx = { 4 };
    b.cnt = { 4 };
    b.ids = { "earlier" };

    {
        const auto m = disk_cache_match_prefix(view_of(a), view_of(b));
        CHECK_EQ(m.token_lcp, n);
        CHECK_EQ(m.usable_lcp, 4);   // the earliest mismatch, discovered by B->A
        CHECK(!m.media_compatible);
    }
    {
        // argument order must not change the answer
        const auto m = disk_cache_match_prefix(view_of(b), view_of(a));
        CHECK_EQ(m.usable_lcp, 4);
        CHECK(!m.media_compatible);
    }

    // the mirror image: the earlier difference now lives in the first direction
    ident c = two_chunk_tokens();
    c.idx = { 4 };
    c.cnt = { 4 };
    c.ids = { "c" };

    ident d = two_chunk_tokens();
    d.idx = { 12 };
    d.cnt = { 4 };
    d.ids = { "d" };

    {
        const auto m = disk_cache_match_prefix(view_of(c), view_of(d));
        CHECK_EQ(m.token_lcp, n);
        CHECK_EQ(m.usable_lcp, 4);
        CHECK(!m.media_compatible);
    }

    // several chunks: the earliest disagreement wins over later agreements, in
    // both argument orders
    ident e = make_ident(n, 5);
    for (size_t i = 4; i < 8; ++i)  { e.toks[i] = LLAMA_TOKEN_NULL; }
    for (size_t i = 20; i < 24; ++i) { e.toks[i] = LLAMA_TOKEN_NULL; }
    e.idx = { 4, 20 };
    e.cnt = { 4, 4 };
    e.ids = { "x", "y" };

    ident f = e; // identical at 4, different at 20
    f.ids = { "x", "z" };
    {
        const auto m = disk_cache_match_prefix(view_of(e), view_of(f));
        CHECK_EQ(m.usable_lcp, 20);
        CHECK(!m.media_compatible);
    }

    ident g = e; // different at 4, identical at 20
    g.ids = { "w", "y" };
    {
        const auto m = disk_cache_match_prefix(view_of(e), view_of(g));
        CHECK_EQ(m.usable_lcp, 4);
        CHECK(!m.media_compatible);
    }

    // fully identical multi-chunk descriptions stay compatible
    {
        const auto m = disk_cache_match_prefix(view_of(e), view_of(e));
        CHECK_EQ(m.usable_lcp, m.token_lcp);
        CHECK(m.media_compatible);
    }
}

// the structural validator, exercised separately from the comparison
void test_media_structural_validity() {
    const size_t n = 20;

    ident ok = make_ident(n, 100);
    for (size_t i = 4; i < 8; ++i) { ok.toks[i] = LLAMA_TOKEN_NULL; }
    ok.idx = { 4 };
    ok.cnt = { 4 };
    ok.ids = { "a" };
    CHECK(disk_cache_media_valid(view_of(ok)));

    // no media at all is valid
    CHECK(disk_cache_media_valid(view_of(make_ident(n, 100))));

    ident bad = ok;
    bad.cnt.clear(); // array sizes disagree
    CHECK(!disk_cache_media_valid(view_of(bad)));

    bad = ok;
    bad.ids = { "" }; // empty id
    CHECK(!disk_cache_media_valid(view_of(bad)));

    bad = ok;
    bad.cnt = { 0 }; // empty chunk
    CHECK(!disk_cache_media_valid(view_of(bad)));

    bad = ok;
    bad.idx = { 18 };
    bad.cnt = { 5 }; // out of range
    CHECK(!disk_cache_media_valid(view_of(bad)));

    bad = ok;
    bad.idx = { 4, 6 };
    bad.cnt = { 6, 4 };
    bad.ids = { "a", "b" }; // overlapping
    CHECK(!disk_cache_media_valid(view_of(bad)));

    bad = ok;
    bad.idx = { 8, 4 };
    bad.cnt = { 4, 4 };
    bad.ids = { "a", "b" }; // out of order
    CHECK(!disk_cache_media_valid(view_of(bad)));

    bad = ok;
    bad.idx = { 4, 4 };
    bad.cnt = { 2, 2 };
    bad.ids = { "a", "b" }; // duplicate position
    CHECK(!disk_cache_media_valid(view_of(bad)));

    // an invalid description is never equal to anything, even to itself
    CHECK(!disk_cache_identity_equal(view_of(bad), view_of(bad)));

    // ... and it produces no usable prefix at all
    const auto m = disk_cache_match_prefix(view_of(bad), view_of(ok));
    CHECK_EQ(m.usable_lcp, 0);
}

void test_cost_guard() {
    const size_t bytes_per_token = DISK_CACHE_MAX_BYTES_PER_TOKEN;

    CHECK(disk_cache_cost_ok(bytes_per_token * 1000, 1000));
    CHECK(!disk_cache_cost_ok(bytes_per_token * 1000 + 1, 1000));
    CHECK(!disk_cache_cost_ok(1, 0));

    // a 1.2 GiB entry cannot serve a 1.4k-token prefix: 786 KiB/token > 768 KiB
    CHECK(!disk_cache_cost_ok(1226ull * 1024 * 1024, 1596));
    // ... but a 506 MiB entry can serve the same prefix: 374 KiB/token
    CHECK(disk_cache_cost_ok(506ull * 1024 * 1024, 1596));
}

// problem B: the estimate and the real file size agree at the boundary and differ
// by 1 byte / 1 KiB - the guard must treat all of them through the same expression
void test_cost_boundary() {
    const size_t lcp = 1000;
    const size_t limit = lcp * DISK_CACHE_MAX_BYTES_PER_TOKEN;

    CHECK(disk_cache_cost_ok(limit, lcp));
    CHECK(!disk_cache_cost_ok(limit + 1, lcp));
    CHECK(!disk_cache_cost_ok(limit + 1024, lcp));
    CHECK(disk_cache_cost_ok(limit - 1024, lcp));

    // a canonical replace decision at the boundary
    ident long_ok = make_ident(5000, 1);
    ident short_ok = make_ident(lcp, 1);

    long_ok.bytes = limit;
    CHECK(disk_cache_can_replace(view_of(long_ok), view_of(short_ok), MIN_TOKENS));

    long_ok.bytes = limit + 1;
    CHECK(!disk_cache_can_replace(view_of(long_ok), view_of(short_ok), MIN_TOKENS));

    // the pre-write estimate (small) must not be used while the real size (over the
    // limit) is what load() would see
    long_ok.bytes = limit + 1024;
    CHECK(!disk_cache_can_replace(view_of(long_ok), view_of(short_ok), MIN_TOKENS));
}

void test_can_replace() {
    ident longer = make_ident(600, 100);
    ident shorter = make_ident(200, 100);

    // economical: 600 tokens stored in a small file
    longer.bytes = 200 * DISK_CACHE_MAX_BYTES_PER_TOKEN / 2;
    CHECK(disk_cache_can_replace(view_of(longer), view_of(shorter), MIN_TOKENS));

    // too expensive for the 200-token prefix it would serve
    longer.bytes = 200 * DISK_CACHE_MAX_BYTES_PER_TOKEN + 1;
    CHECK(!disk_cache_can_replace(view_of(longer), view_of(shorter), MIN_TOKENS));

    // not a prefix
    ident other = make_ident(200, 700);
    CHECK(!disk_cache_can_replace(view_of(longer), view_of(other), MIN_TOKENS));

    // equal lengths are not a replacement relationship
    longer.bytes = 1024;
    CHECK(!disk_cache_can_replace(view_of(shorter), view_of(shorter), MIN_TOKENS));

    // shorter than the minimum match length
    ident tiny = make_ident(3, 100);
    CHECK(!disk_cache_can_replace(view_of(longer), view_of(tiny), MIN_TOKENS));
}

void test_can_replace_with_media() {
    // longer carries a media chunk inside the range the shorter one covers
    ident longer = make_ident(600, 100);
    for (size_t i = 8; i < 12; ++i) {
        longer.toks[i] = LLAMA_TOKEN_NULL;
    }
    longer.idx = { 8 };
    longer.cnt = { 4 };
    longer.ids = { "img" };
    longer.bytes = 1024;

    // the shorter one has no media metadata there: not interchangeable
    ident plain = make_ident(200, 100);
    CHECK(!disk_cache_can_replace(view_of(longer), view_of(plain), MIN_TOKENS));

    // with the same media chunk it is
    ident with_media = make_ident(200, 100);
    for (size_t i = 8; i < 12; ++i) {
        with_media.toks[i] = LLAMA_TOKEN_NULL;
    }
    with_media.idx = { 8 };
    with_media.cnt = { 4 };
    with_media.ids = { "img" };
    CHECK(disk_cache_can_replace(view_of(longer), view_of(with_media), MIN_TOKENS));
}

void test_identity_equal() {
    ident a = make_ident(12, 100);
    ident b = a;
    CHECK(disk_cache_identity_equal(view_of(a), view_of(b)));

    b.toks[11] = 7;
    CHECK(!disk_cache_identity_equal(view_of(a), view_of(b)));

    // media differences matter even with identical tokens
    ident c = make_ident(12, 100);
    for (size_t i = 4; i < 8; ++i) {
        c.toks[i] = LLAMA_TOKEN_NULL;
    }
    c.idx = { 4 };
    c.cnt = { 4 };
    c.ids = { "img-A" };

    ident d = c;
    d.ids = { "img-B" };
    CHECK(!disk_cache_identity_equal(view_of(c), view_of(d)));

    ident e = c;
    e.cnt = { 3 };
    CHECK(!disk_cache_identity_equal(view_of(c), view_of(e)));

    CHECK(disk_cache_identity_equal(view_of(c), view_of(c)));
}

void test_classify_duplicate() {
    ident a = make_ident(12, 100);
    ident b = a;

    CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::identical);

    // hash and length agree but the identity does not: a collision, not a duplicate
    b.toks[5] = 42;
    CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::none);

    // different hash/length: nothing to classify
    CHECK(disk_cache_classify_duplicate(view_of(a), view_of(a), false) == disk_cache_duplicate::none);

    // same tokens, different media id: still a collision
    ident c = make_ident(12, 100);
    for (size_t i = 4; i < 8; ++i) {
        c.toks[i] = LLAMA_TOKEN_NULL;
    }
    c.idx = { 4 };
    c.cnt = { 4 };
    c.ids = { "img-A" };
    ident d = c;
    d.ids = { "img-B" };
    CHECK(disk_cache_classify_duplicate(view_of(c), view_of(d), true) == disk_cache_duplicate::none);

    // a differing token length shares the file name but is never a duplicate
    ident longer = c;
    longer.toks.push_back(7);
    CHECK(disk_cache_classify_duplicate(view_of(c), view_of(longer), true) == disk_cache_duplicate::none);
    CHECK(disk_cache_classify_duplicate(view_of(longer), view_of(c), true) == disk_cache_duplicate::none);

    // a shorter prefix of the same prompt is not a duplicate either
    ident shorter = c;
    shorter.toks.resize(8);
    shorter.idx.clear();
    shorter.cnt.clear();
    shorter.ids.clear();
    CHECK(disk_cache_classify_duplicate(view_of(c), view_of(shorter), true) == disk_cache_duplicate::none);

    // an unverifiable description can never be classified as identical, even
    // against itself: the chunk runs past the end of the prompt
    ident broken = c;
    broken.cnt = { 20 };
    CHECK(!disk_cache_media_valid(view_of(broken)));
    CHECK(disk_cache_classify_duplicate(view_of(broken), view_of(broken), true) == disk_cache_duplicate::none);
}

void test_suppresses_write() {
    // an entry that covers all but a short tail, economically
    ident existing = make_ident(600, 100);
    existing.bytes = 600 * 1024; // 1 KiB/token

    ident wanted = make_ident(500, 100);
    CHECK(disk_cache_suppresses_write(view_of(existing), view_of(wanted), MIN_TOKENS, LAG));

    // the tail is beyond the tolerance
    ident far = make_ident(600, 500); // shares nothing
    CHECK(!disk_cache_suppresses_write(view_of(existing), view_of(far), MIN_TOKENS, LAG));

    // economical only when the file is small enough for the shared prefix
    ident huge = make_ident(600, 100);
    huge.bytes = 500 * DISK_CACHE_MAX_BYTES_PER_TOKEN + 1;
    CHECK(!disk_cache_suppresses_write(view_of(huge), view_of(wanted), MIN_TOKENS, LAG));

    // media metadata disagreement over the shared prefix stops the suppression.
    // the prompts are long enough that the 4096-token tail tolerance cannot hide
    // the mismatch: the usable prefix is 8 tokens against a 6000-token request
    {
        ident with_media = make_ident(6100, 100);
        for (size_t i = 8; i < 12; ++i) {
            with_media.toks[i] = LLAMA_TOKEN_NULL;
        }
        with_media.idx = { 8 };
        with_media.cnt = { 4 };
        with_media.ids = { "img" };
        with_media.bytes = 1000 * 1024;

        ident plain_wanted = make_ident(6000, 100); // same ids, no media metadata
        CHECK(!disk_cache_suppresses_write(view_of(with_media), view_of(plain_wanted), MIN_TOKENS, LAG));

        // with the identical media chunk the whole 6000-token prefix is usable
        ident media_wanted = with_media;
        media_wanted.toks.resize(6000);
        CHECK(disk_cache_suppresses_write(view_of(with_media), view_of(media_wanted), MIN_TOKENS, LAG));
    }

    // below the minimum match length
    ident short_wanted = make_ident(4, 100);
    CHECK(!disk_cache_suppresses_write(view_of(existing), view_of(short_wanted), MIN_TOKENS, LAG));
}

// ---------------------------------------------------------------------------
// the in-flight table's verdict
//
// store() and mirror() both ask the same question about a hash that is already
// queued: is this the same prompt, or a different one competing for the same path?
// They ask it with disk_cache_classify_duplicate(queued, wanted, same_hash=true),
// so the four outcomes are pinned here. The hash itself is deliberately NOT varied:
// the point is that the hash only selects the candidate and the full identity
// decides, which is exactly what a constructed hash collision exercises without
// having to find one.
// ---------------------------------------------------------------------------
void test_inflight_identity_verdict() {
    // identical tokens and no media: the same prompt queued twice -> duplicate
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(64, 1000);
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::identical);
    }

    // same hash, different token length -> conflict, not duplicate
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(48, 1000);
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::none);
        CHECK(disk_cache_classify_duplicate(view_of(b), view_of(a), true) == disk_cache_duplicate::none);
    }

    // same hash, same length, different tokens -> conflict
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(64, 2000);
        CHECK_EQ(a.toks.size(), b.toks.size());
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::none);
    }

    // same tokens, different media id -> conflict
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(64, 1000);
        a.idx = { 10 }; a.cnt = { 4 }; a.ids = { "img-a" };
        b.idx = { 10 }; b.cnt = { 4 }; b.ids = { "img-b" };
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::none);
    }

    // same tokens and media ids, different chunk start/length -> conflict
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(64, 1000);
        a.idx = { 10 }; a.cnt = { 4 }; a.ids = { "img-a" };
        b.idx = { 20 }; b.cnt = { 4 }; b.ids = { "img-a" };
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::none);
    }

    // identical tokens AND identical media -> duplicate, even with media present
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(64, 1000);
        for (ident * x : { &a, &b }) {
            x->idx = { 10, 40 }; x->cnt = { 4, 6 }; x->ids = { "img-a", "img-b" };
        }
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::identical);
    }

    // a differing hash never matches, regardless of identity: unrelated writes are
    // not classified at all
    {
        ident a = make_ident(64, 1000);
        ident b = make_ident(64, 1000);
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), false) == disk_cache_duplicate::none);
    }

    // a structurally broken identity is never "identical", even against itself:
    // the queued entry cannot vouch for a prompt whose media description is invalid
    {
        ident broken = make_ident(64, 1000);
        broken.idx = { 10, 5 };  // out of order
        broken.cnt = { 4, 6 };
        broken.ids = { "img-a", "img-b" };
        CHECK(disk_cache_classify_duplicate(view_of(broken), view_of(broken), true) == disk_cache_duplicate::none);
    }
}

// ---------------------------------------------------------------------------
// the in-flight placeholder lifecycle
//
// This mirrors what server_disk_cache::inflight does, using the very same
// classify call the production helpers use, so the decision under test is the real
// one and not a second copy of the rule. The hash is injected directly: the
// scenario is "two prompts collide", which must not depend on finding a real
// 64-bit collision.
//
// What it pins down:
//   * a placeholder is visible from the moment the write is queued
//   * it stays visible after the worker has taken the element out of the queue
//   * a different identity is refused, and the first writer is untouched
//   * the same identity is merged
//   * the placeholder is released on every terminal path, so a later write of the
//     same prompt is queued again instead of being answered "already queued"
// ---------------------------------------------------------------------------
struct inflight_table {
    std::unordered_map<uint64_t, ident> live;

    enum class verdict { free, duplicate, conflict };

    verdict check(uint64_t hash, const ident & wanted) const {
        const auto it = live.find(hash);
        if (it == live.end()) {
            return verdict::free;
        }
        return disk_cache_classify_duplicate(view_of(it->second), view_of(wanted), true)
                == disk_cache_duplicate::identical ? verdict::duplicate : verdict::conflict;
    }

    void add(uint64_t hash, const ident & pending) { live[hash] = pending; }

    void release(uint64_t hash) { live.erase(hash); }

    size_t size() const { return live.size(); }
};

void test_inflight_lifecycle() {
    using v = inflight_table::verdict;

    const uint64_t colliding = 0xABCDEF0123456789ull; // deliberately shared by A and B

    ident a = make_ident(64, 1000);
    ident b = make_ident(48, 1000); // same hash, different length

    inflight_table t;

    // nothing queued yet
    CHECK(t.check(colliding, a) == v::free);

    // A is queued: its identity is visible right away
    t.add(colliding, a);
    CHECK_EQ(t.size(), 1);
    CHECK(t.check(colliding, a) == v::duplicate); // the same prompt again
    CHECK(t.check(colliding, b) == v::conflict);  // a different prompt for the same path

    // the worker pops A off the queue. The placeholder must survive that, otherwise
    // the very window in which a conflicting write is most likely would be blind
    // (here the pop is modelled by nothing happening to the table at all)
    CHECK(t.check(colliding, b) == v::conflict);

    // A is committed: the placeholder turns into a stored entry. From the table's
    // point of view that is a release; the entry side is a separate table
    t.release(colliding);
    CHECK_EQ(t.size(), 0);

    // the path is now free for B, which was refused a moment ago
    CHECK(t.check(colliding, b) == v::free);

    // and A may be queued again too: the placeholder was not left behind
    t.add(colliding, a);
    CHECK(t.check(colliding, a) == v::duplicate);
    t.release(colliding);
    CHECK_EQ(t.size(), 0);

    // a second, unrelated hash is tracked independently
    ident c = make_ident(64, 5000);
    t.add(colliding, a);
    t.add(colliding + 1, c);
    CHECK_EQ(t.size(), 2);
    CHECK(t.check(colliding, b) == v::conflict);
    CHECK(t.check(colliding + 1, c) == v::duplicate);
    t.release(colliding);
    CHECK(t.check(colliding, b) == v::free);
    CHECK(t.check(colliding + 1, c) == v::duplicate);
    t.release(colliding + 1);
    CHECK_EQ(t.size(), 0);

    // rejecting B must not have disturbed A's queued object: A is still exactly the
    // prompt that was queued, and still merges with itself
    t.add(colliding, a);
    (void) t.check(colliding, b);
    CHECK(t.check(colliding, a) == v::duplicate);
    t.release(colliding);

    // same tokens, different media: also a conflict, and also leaves the first
    // writer intact
    ident m1 = make_ident(64, 1000);
    ident m2 = make_ident(64, 1000);
    m1.idx = { 10 }; m1.cnt = { 4 }; m1.ids = { "img-a" };
    m2.idx = { 10 }; m2.cnt = { 4 }; m2.ids = { "img-b" };
    t.add(colliding, m1);
    CHECK(t.check(colliding, m2) == v::conflict);
    CHECK(t.check(colliding, m1) == v::duplicate);
    t.release(colliding);
    CHECK_EQ(t.size(), 0);
}

// ---------------------------------------------------------------------------
// the questions preflight_materialize() asks, in the order it asks them
//
// preflight calls committed_check_locked() (built on classify_duplicate), then
// inflight_check_locked() (the same call), then covered_cheaply() (built on
// suppresses_write). Each of those is exercised above; this pins the *combination*
// down so the preflight's conclusions follow from the predicates it reuses rather
// than from a second rule.
//
// The distinction that matters: "already have this exact prompt" is a different
// answer from "have something longer that covers it", and neither is "a different
// prompt wants this path".
// ---------------------------------------------------------------------------
void test_preflight_conclusions() {
    const size_t MIN_TOKENS = 256;
    const size_t LAG        = 16;   // DISK_CACHE_MAX_PREFIX_LAG_TOKENS in the tests' terms

    // the committed entry: 512 tokens, small enough to be cheap to restore
    ident committed = make_ident(512, 1000);
    committed.bytes = 512 * 4096;

    // 1. identical_committed: the same prompt is already there
    {
        ident wanted = make_ident(512, 1000);
        CHECK(disk_cache_classify_duplicate(view_of(committed), view_of(wanted), true) == disk_cache_duplicate::identical);
    }

    // 2. covered_by_committed: a longer committed entry covers this prefix, and its
    // real size_file passes the read-cost guard
    {
        ident wanted = make_ident(500, 1000); // a prefix of the committed one
        CHECK(disk_cache_classify_duplicate(view_of(committed), view_of(wanted), true) == disk_cache_duplicate::none);
        CHECK(disk_cache_suppresses_write(view_of(committed), view_of(wanted), MIN_TOKENS, LAG));
    }

    // 3. neither: the committed entry is too expensive to restore for this prefix,
    // so it must NOT be reported as coverage. the state has to be exported
    {
        ident wanted  = make_ident(500, 1000);
        ident costly  = make_ident(512, 1000);
        costly.bytes  = 512 * (1024 * 1024); // way over the per-token guard

        CHECK(disk_cache_classify_duplicate(view_of(costly), view_of(wanted), true) == disk_cache_duplicate::none);
        CHECK(!disk_cache_suppresses_write(view_of(costly), view_of(wanted), MIN_TOKENS, LAG));
    }

    // 4. neither: an unrelated prompt, so no coverage either
    {
        ident wanted = make_ident(500, 2000);
        CHECK(!disk_cache_suppresses_write(view_of(committed), view_of(wanted), MIN_TOKENS, LAG));
    }

    // 5. hash_path_conflict: a committed entry with the same hash (same_hash = true)
    // but a different identity. it is a refusal, and it must not be read as dedup.
    //
    // note the two questions are independent by construction: `suppresses_write`
    // only asks whether a longer entry could serve this prefix, and it has no hash
    // to compare. Here the entry genuinely is a longer prefix of `wanted`, so
    // coverage holds - and the identity check still says "not the same prompt".
    // preflight asks the identity question first, which is why the conflict is
    // reported instead of the coverage.
    {
        ident wanted = make_ident(400, 1000); // differs in length, shares the prefix
        CHECK(disk_cache_classify_duplicate(view_of(committed), view_of(wanted), true) == disk_cache_duplicate::none);
        CHECK(disk_cache_suppresses_write(view_of(committed), view_of(wanted), MIN_TOKENS, LAG));

        // an entry that is NOT a longer prefix of `wanted` is neither a duplicate
        // nor coverage: both questions answer no, and the export proceeds
        ident unrelated = make_ident(400, 3000);
        CHECK(disk_cache_classify_duplicate(view_of(committed), view_of(unrelated), true) == disk_cache_duplicate::none);
        CHECK(!disk_cache_suppresses_write(view_of(committed), view_of(unrelated), MIN_TOKENS, LAG));
    }

    // 6. a prefix below the tier's minimum is nothing to store, so preflight reports
    // it as invalid rather than as coverage
    {
        ident tiny = make_ident(64, 1000);
        // suppresses_write itself refuses below the minimum match length
        CHECK(!disk_cache_suppresses_write(view_of(committed), view_of(tiny), MIN_TOKENS, LAG));
    }

    // 7. media must match for any of this to hold: same tokens, different media id
    {
        ident a = make_ident(512, 1000);
        ident b = make_ident(512, 1000);
        a.idx = { 100 }; a.cnt = { 8 }; a.ids = { "img-a" };
        b.idx = { 100 }; b.cnt = { 8 }; b.ids = { "img-b" };
        CHECK(disk_cache_classify_duplicate(view_of(a), view_of(b), true) == disk_cache_duplicate::none);
        CHECK(!disk_cache_suppresses_write(view_of(a), view_of(b), MIN_TOKENS, LAG));
    }
}

} // namespace

int main() {
    test_plain_prefix();
    test_media_identity();
    test_media_boundary_clamp();
    test_bidirectional_earliest_difference();
    test_media_structural_validity();
    test_cost_guard();
    test_cost_boundary();
    test_can_replace();
    test_can_replace_with_media();
    test_identity_equal();
    test_classify_duplicate();
    test_inflight_identity_verdict();
    test_inflight_lifecycle();
    test_preflight_conclusions();
    test_suppresses_write();

    if (g_failures == 0) {
        std::printf("all disk-cache match checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
