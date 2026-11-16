// standalone self-check for the segmented snapshot codec
//
// Pure bytes: no model, no llama_context, no file. Build it with the server
// toolchain (Windows, repository root, vcvars64 loaded):
//
//   cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
//      /I tools/server /I common /I include /I ggml/include ^
//      /Fe:test-run\codec_test.exe ^
//      tools/server/tests/unit/test_disk_cache_codec.cpp tools/server/server-disk-cache-codec.cpp

#include "server-disk-cache-codec.h"

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

// encode `src` the way the file writer would: use the encoded form only when it is
// strictly smaller, otherwise a raw segment
struct stored_segment {
    std::vector<char>    bytes;
    disk_cache_segment_desc desc;
    bool                was_encoded = false;
};

stored_segment store_segment(const std::vector<char> & src, bool allow_codec) {
    stored_segment out;

    if (!allow_codec) {
        out.bytes = src;
        out.desc.raw_len    = src.size();
        out.desc.stored_len = src.size();
        out.desc.codec      = disk_cache_codec_id::raw;
        return out;
    }

    std::vector<char> encoded;
    const disk_cache_codec_id id = disk_cache_codec_encode(src.data(), src.size(), encoded);

    if (id == disk_cache_codec_id::lz && encoded.size() < src.size()) {
        out.bytes       = std::move(encoded);
        out.was_encoded = true;
    } else {
        out.bytes = src;
    }

    out.desc.raw_len    = src.size();
    out.desc.stored_len = out.bytes.size();
    out.desc.codec      = out.was_encoded ? disk_cache_codec_id::lz : disk_cache_codec_id::raw;
    return out;
}

// the round trip that matters: a segment that goes through encode must come back
// byte for byte
bool round_trips(const std::vector<char> & src, bool allow_codec = true) {
    const stored_segment s = store_segment(src, allow_codec);

    std::vector<char> back;
    if (!disk_cache_codec_decode(s.bytes.data(), s.bytes.size(), s.desc, back)) {
        return false;
    }

    return back == src;
}

std::vector<char> make_bytes(size_t n, unsigned seed) {
    std::vector<char> v(n);
    unsigned x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        v[i] = (char) (x >> 16);
    }
    return v;
}

std::vector<char> make_zeros(size_t n) {
    return std::vector<char>(n, 0);
}

std::vector<char> make_repeated_pattern(size_t n) {
    std::vector<char> v(n);
    const char pat[7] = { 'K', 'V', 0, 0, 1, 2, 3 };
    for (size_t i = 0; i < n; i++) {
        v[i] = pat[i % 7];
    }
    return v;
}

// ---------- the basic corpus ------------------------------------------------

void test_empty_segment() {
    const std::vector<char> empty;
    CHECK(round_trips(empty));

    // an empty segment is stored raw with both lengths zero: nothing to compress
    const stored_segment s = store_segment(empty, true);
    CHECK_EQ(s.bytes.size(), 0);
    CHECK_EQ(s.desc.codec, disk_cache_codec_id::raw);
    CHECK_EQ(s.desc.raw_len, 0);
    CHECK(!s.was_encoded);
}

void test_tiny_segments() {
    for (size_t n = 0; n <= 8; n++) {
        const std::vector<char> v = make_bytes(n, (unsigned) n + 1);
        CHECK(round_trips(v));
    }
}

void test_incompressible_segment() {
    // random bytes must round-trip even though they will not be encoded
    const std::vector<char> v = make_bytes(64 * 1024, 12345);

    CHECK(round_trips(v));

    const stored_segment s = store_segment(v, true);
    CHECK(!s.was_encoded);              // the encoder declined to make it bigger
    CHECK_EQ(s.desc.codec, disk_cache_codec_id::raw);
    CHECK_EQ(s.desc.stored_len, v.size());
}

void test_compressible_segment() {
    const std::vector<char> v = make_zeros(256 * 1024);

    CHECK(round_trips(v));

    const stored_segment s = store_segment(v, true);
    CHECK(s.was_encoded);
    CHECK_EQ(s.desc.codec, disk_cache_codec_id::lz);
    CHECK(s.desc.stored_len < s.desc.raw_len);
    CHECK_EQ(s.desc.raw_len, v.size());
}

void test_repetitive_pattern() {
    const std::vector<char> v = make_repeated_pattern(128 * 1024);
    CHECK(round_trips(v));

    const stored_segment s = store_segment(v, true);
    CHECK(s.was_encoded);
    CHECK(s.desc.stored_len < s.desc.raw_len / 4); // a 7-byte pattern compresses well
}

// ---------- segment independence --------------------------------------------

void test_mixed_segments_in_one_snapshot() {
    // one snapshot's carriers, each encoded on its own
    const std::vector<char> main_state  = make_zeros(4096);
    const std::vector<char> draft_state = make_bytes(4096, 999);       // incompressible
    const std::vector<char> ckpt_spec   = make_repeated_pattern(2048);
    const std::vector<char> ckpt_tgt    = make_zeros(2048);
    const std::vector<char> ckpt_dft    = make_bytes(2048, 7);

    const std::vector<std::vector<char>> segs = { main_state, draft_state, ckpt_spec, ckpt_tgt, ckpt_dft };

    std::vector<stored_segment> stored;
    for (const auto & s : segs) {
        stored.push_back(store_segment(s, true));
    }

    // encoded and verbatim coexist in the same file
    CHECK_EQ(stored[0].desc.codec, disk_cache_codec_id::lz);
    CHECK_EQ(stored[1].desc.codec, disk_cache_codec_id::raw);
    CHECK_EQ(stored[2].desc.codec, disk_cache_codec_id::lz);
    CHECK_EQ(stored[3].desc.codec, disk_cache_codec_id::lz);
    CHECK_EQ(stored[4].desc.codec, disk_cache_codec_id::raw);

    // each decodes back to exactly its own source
    for (size_t i = 0; i < segs.size(); i++) {
        std::vector<char> back;
        CHECK(disk_cache_codec_decode(stored[i].bytes.data(), stored[i].bytes.size(), stored[i].desc, back));
        CHECK(back == segs[i]);
    }
}

void test_segments_are_not_concatenated() {
    // Two different sources must not be able to influence each other through the
    // encoder. Encoding each independently is what guarantees that; this checks the
    // observable consequence - decoding a segment never needs another one.
    const std::vector<char> a = make_zeros(8192);
    const std::vector<char> b = make_repeated_pattern(8192);

    const stored_segment sa = store_segment(a, true);
    const stored_segment sb = store_segment(b, true);

    std::vector<char> back_a;
    CHECK(disk_cache_codec_decode(sa.bytes.data(), sa.bytes.size(), sa.desc, back_a));
    CHECK(back_a == a);

    // corrupting B's bytes does not touch A's ability to decode
    std::vector<char> corrupt_b = sb.bytes;
    if (!corrupt_b.empty()) {
        corrupt_b[corrupt_b.size() / 2] ^= 0x5A;
    }

    std::vector<char> back_a2;
    CHECK(disk_cache_codec_decode(sa.bytes.data(), sa.bytes.size(), sa.desc, back_a2));
    CHECK(back_a2 == a);

    std::vector<char> back_b;
    // a corrupted encoded stream must be rejected, not silently accepted
    const bool ok_b = disk_cache_codec_decode(corrupt_b.data(), corrupt_b.size(), sb.desc, back_b);
    if (sb.was_encoded) {
        CHECK(!ok_b || back_b != b);
    }
}

// ---------- rejection paths --------------------------------------------------

void test_length_mismatch_rejected() {
    const std::vector<char> v = make_zeros(4096);
    const stored_segment s = store_segment(v, true);

    // stored_len must match the bytes actually handed in
    disk_cache_segment_desc d = s.desc;
    d.stored_len = s.desc.stored_len + 1;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(s.bytes.data(), s.bytes.size(), d, out));
}

void test_raw_length_mismatch_rejected() {
    // a raw segment whose declared raw_len differs from its stored length is
    // internally inconsistent
    std::vector<char> bytes = make_bytes(1024, 5);

    disk_cache_segment_desc d;
    d.raw_len    = 1023;
    d.stored_len = 1024;
    d.codec      = disk_cache_codec_id::raw;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(bytes.data(), bytes.size(), d, out));
}

void test_declared_length_over_bounds() {
    std::vector<char> bytes = make_bytes(64, 3);

    disk_cache_segment_desc d;
    d.raw_len    = DISK_CACHE_SEGMENT_RAW_MAX + 1; // past the ceiling
    d.stored_len = bytes.size();
    d.codec      = disk_cache_codec_id::lz;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(bytes.data(), bytes.size(), d, out));

    // guards against a header claiming an enormous output for a tiny input
    CHECK(out.empty());
}

void test_unknown_codec_rejected() {
    std::vector<char> bytes = make_bytes(64, 3);

    disk_cache_segment_desc d;
    d.raw_len    = 64;
    d.stored_len = bytes.size();
    d.codec      = (disk_cache_codec_id) 0xDEADBEEF;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(bytes.data(), bytes.size(), d, out));
}

void test_truncated_stream_rejected() {
    const std::vector<char> v = make_zeros(8192);
    const stored_segment s = store_segment(v, true);
    CHECK(s.was_encoded);

    // cut the stream short and re-declare the stored length to match, so the only
    // thing wrong is that the tokens run out before the output does
    std::vector<char> truncated(s.bytes.begin(), s.bytes.end() - s.bytes.size() / 3);

    disk_cache_segment_desc d = s.desc;
    d.stored_len = truncated.size();

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(truncated.data(), truncated.size(), d, out));
}

void test_overshooting_match_rejected() {
    // hand-built stream: no literals, one match of length 5 at distance 1, but the
    // declared output is only 2 bytes
    std::vector<char> stream;
    stream.push_back((char) 0x01);  // flags: carries a match
    stream.push_back((char) 0x00);  // lit_len = 0
    stream.push_back((char) 0x02);  // match length 5 (2 + 3)
    stream.push_back((char) 0x01);  // distance 1

    disk_cache_segment_desc d;
    d.raw_len    = 2;
    d.stored_len = stream.size();
    d.codec      = disk_cache_codec_id::lz;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(stream.data(), stream.size(), d, out));
}

void test_backreference_into_nothing_rejected() {
    // a match at output position 0 has nothing to refer back to
    std::vector<char> stream;
    stream.push_back((char) 0x01); // flags: match
    stream.push_back((char) 0x00); // lit_len = 0
    stream.push_back((char) 0x00); // match length 3
    stream.push_back((char) 0x01); // distance 1, but no output yet

    disk_cache_segment_desc d;
    d.raw_len    = 16;
    d.stored_len = stream.size();
    d.codec      = disk_cache_codec_id::lz;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(stream.data(), stream.size(), d, out));
}

void test_literal_overrun_rejected() {
    // claims 100 literal bytes but only 4 are present
    std::vector<char> stream;
    stream.push_back((char) 0x00); // flags: literal-only token
    stream.push_back((char) 100);  // lit_len = 100
    stream.push_back('a');
    stream.push_back('b');
    stream.push_back('c');
    stream.push_back('d');

    disk_cache_segment_desc d;
    d.raw_len    = 100;
    d.stored_len = stream.size();
    d.codec      = disk_cache_codec_id::lz;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(stream.data(), stream.size(), d, out));
}

void test_empty_encoded_segment_rejected() {
    // the encoder never produces an encoded zero-length segment; a header claiming
    // one is inconsistent
    std::vector<char> empty;
    disk_cache_segment_desc d;
    d.raw_len    = 0;
    d.stored_len = 0;
    d.codec      = disk_cache_codec_id::lz;

    std::vector<char> out;
    CHECK(!disk_cache_codec_decode(empty.data(), empty.size(), d, out));
}

// ---------- determinism and bounds -------------------------------------------

void test_encode_is_deterministic() {
    const std::vector<char> v = make_repeated_pattern(64 * 1024);

    std::vector<char> a;
    std::vector<char> b;
    disk_cache_codec_encode(v.data(), v.size(), a);
    disk_cache_codec_encode(v.data(), v.size(), b);

    CHECK(a == b);
}

void test_bound_is_respected() {
    // a corpus spanning the interesting shapes
    const std::vector<std::vector<char>> corpus = {
        make_zeros(1),
        make_zeros(4096),
        make_bytes(4096, 1),
        make_repeated_pattern(4096),
        make_repeated_pattern(100),
    };

    for (const auto & v : corpus) {
        std::vector<char> enc;
        disk_cache_codec_encode(v.data(), v.size(), enc);

        // the encoder either shrinks or falls back to a verbatim copy, so it can
        // never exceed the source
        CHECK(enc.size() <= v.size());

        // and it certainly stays within the declared bound
        CHECK(enc.size() <= disk_cache_codec_bound(v.size()) || enc == v);
    }
}

void test_long_runs() {
    // long runs exercise the overlapping back-reference path
    std::vector<char> v(512 * 1024, 'A');
    CHECK(round_trips(v));

    std::vector<char> v2;
    for (int i = 0; i < 1000; i++) {
        v2.insert(v2.end(), 500, (char) (i & 0xFF));
    }
    CHECK(round_trips(v2));
}

void test_codec_mode_names() {
    CHECK(std::string(disk_cache_codec_mode_name(disk_cache_codec_mode::none)) == "none");
    CHECK(std::string(disk_cache_codec_mode_name(disk_cache_codec_mode::lz)) == "lz");

    disk_cache_codec_mode m = disk_cache_codec_mode::lz;

    CHECK(disk_cache_codec_mode_parse("none", m) && m == disk_cache_codec_mode::none);
    CHECK(disk_cache_codec_mode_parse("lz", m) && m == disk_cache_codec_mode::lz);

    // an unknown spelling is refused rather than quietly becoming the default
    CHECK(!disk_cache_codec_mode_parse("gzip", m));
    CHECK(!disk_cache_codec_mode_parse("", m));
    CHECK(!disk_cache_codec_mode_parse(nullptr, m));
}

} // namespace

int main() {
    test_empty_segment();
    test_tiny_segments();
    test_incompressible_segment();
    test_compressible_segment();
    test_repetitive_pattern();
    test_mixed_segments_in_one_snapshot();
    test_segments_are_not_concatenated();
    test_length_mismatch_rejected();
    test_raw_length_mismatch_rejected();
    test_declared_length_over_bounds();
    test_unknown_codec_rejected();
    test_truncated_stream_rejected();
    test_overshooting_match_rejected();
    test_backreference_into_nothing_rejected();
    test_literal_overrun_rejected();
    test_empty_encoded_segment_rejected();
    test_encode_is_deterministic();
    test_bound_is_respected();
    test_long_runs();
    test_codec_mode_names();

    if (g_failures == 0) {
        std::printf("all disk-cache codec checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
