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

} // namespace

int main() {
    test_plain_prefix();
    test_media_identity();
    test_media_boundary_clamp();
    test_cost_guard();
    test_can_replace();
    test_can_replace_with_media();
    test_identity_equal();
    test_classify_duplicate();
    test_suppresses_write();

    if (g_failures == 0) {
        std::printf("all disk-cache match checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
