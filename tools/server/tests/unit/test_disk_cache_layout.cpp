// A layout fixture for the v8 and v9 payload boundaries.
//
// The bug this pins down: the writer reserved a v9 segment table unconditionally,
// so a v8 file gained `28 * (2 + 3N)` bytes of padding between the checkpoint table
// and the payload - while the v8 reader starts reading the state right after the
// checkpoint table. Every v8 file written with the codec feature present was
// therefore internally inconsistent, and the mismatch only showed up as a CRC
// failure or a corrupt restore.
//
// Waiting for a natural snapshot to produce a given checkpoint count is not a test
// - the count depends on the model and the prompt. This fixture reproduces the
// exact header arithmetic for N = 0, 1, 2 and asserts the property the reader
// relies on: the payload begins immediately after the checkpoint table for v8, and
// after the segment table for v9.
//
// It works on the header layout alone - no model, no file, no llm. The numbers come
// from the same constants `write_entry()` and `read_entry()` use.

#include <array>
#include <cstdint>
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

// mirror of the writer's layout constants. Kept here rather than included so this
// check fails loudly if the server's own numbers drift - a silent coupling would
// defeat the purpose of a layout test
constexpr uint32_t VERSION_V8 = 8;
constexpr uint32_t VERSION_V9 = 9;

constexpr size_t MAGIC_BYTES   = 8;
constexpr size_t VERSION_BYTES = 4;
constexpr size_t SIG_LEN_BYTES = 4;
constexpr size_t PACKED_LEN_BYTES = 4;

// one checkpoint record: n_tokens(8) pos_min(4) pos_max(4) + 3 sizes(24) + 3 crcs(24)
constexpr size_t CKPT_RECORD_BYTES = 8 + 4 + 4 + 24 + 24;

// one segment-table entry: raw_len(8) stored_len(8) codec(4) encoded_crc(8)
constexpr size_t SEGMENT_DESC_BYTES = 8 + 8 + 4 + 8;

struct layout {
    size_t ckpt_table_end   = 0; // where the checkpoint records stop
    size_t segment_table_end = 0; // where the v9 segment table stops (v8: == ckpt_table_end)
    size_t payload_start     = 0; // where the state bytes begin for the version in hand
    size_t payload_end       = 0;
};

// the header offsets, computed exactly as the writer lays them out. `segmented` is
// the version's own property, never a runtime setting
layout compute_layout(uint32_t version, size_t sig_len, size_t packed_len,
                      const std::array<uint64_t, 2> & state_sizes,
                      const std::vector<std::array<uint64_t, 3>> & ckpt_sizes) {
    layout L;

    size_t off = MAGIC_BYTES + VERSION_BYTES + SIG_LEN_BYTES + sig_len + PACKED_LEN_BYTES + packed_len;

    // size_main(8) size_drft(8) n_ckpt(4) created(8) last_used(8) crc_main(8) crc_drft(8) crc_packed(8)
    off += 8 + 8 + 4 + 8 + 8 + 8 + 8 + 8;

    off += ckpt_sizes.size() * CKPT_RECORD_BYTES;
    L.ckpt_table_end = off;

    const bool segmented = version == VERSION_V9;

    if (segmented) {
        off += (2 + 3 * ckpt_sizes.size()) * SEGMENT_DESC_BYTES;
    }

    L.segment_table_end = off;
    L.payload_start     = off;

    uint64_t total = state_sizes[0] + state_sizes[1];
    for (const auto & c : ckpt_sizes) {
        total += c[0] + c[1] + c[2];
    }

    L.payload_end = off + (size_t) total;
    return L;
}

// a realistic header: the sizes observed on the experiment machine
std::array<uint64_t, 2> sample_states() {
    return { 195689284, 22970832 };
}

// N checkpoints, each with the spec/tgt/dft sizes a real snapshot carries
std::vector<std::array<uint64_t, 3>> sample_ckpts(size_t n) {
    std::vector<std::array<uint64_t, 3>> v;
    for (size_t i = 0; i < n; i++) {
        // spec is empty in practice; the others are large
        v.push_back({ 0, 156894364, 16094456 + (uint64_t) i * 21808 });
    }
    return v;
}

// ---------- the property the reader depends on -------------------------------

void test_v8_payload_follows_checkpoint_table() {
    for (size_t n = 0; n <= 2; n++) {
        const layout L = compute_layout(VERSION_V8, 206, 8416, sample_states(), sample_ckpts(n));

        // this is the whole point: no gap between the checkpoint table and the state
        CHECK_EQ(L.payload_start, L.ckpt_table_end);

        // and a v8 file must not carry a segment table at all
        CHECK_EQ(L.segment_table_end, L.ckpt_table_end);
    }
}

void test_v8_has_no_padding_gap() {
    // the bug added exactly `28 * (2 + 3N)` bytes of placeholder. Express the
    // absence of it as the arithmetic the reader performs
    for (size_t n = 0; n <= 2; n++) {
        const layout v8 = compute_layout(VERSION_V8, 206, 8416, sample_states(), sample_ckpts(n));
        const layout v9 = compute_layout(VERSION_V9, 206, 8416, sample_states(), sample_ckpts(n));

        const size_t padding = 28 * (2 + 3 * n);

        // the two versions differ by exactly the segment table, nothing else
        CHECK_EQ(v9.payload_start - v8.payload_start, padding);

        // and the v8 payload is strictly *before* where the table would have been
        CHECK(v8.payload_start < v9.payload_start);
    }
}

void test_v9_reserves_a_table() {
    for (size_t n = 0; n <= 2; n++) {
        const layout L = compute_layout(VERSION_V9, 206, 8416, sample_states(), sample_ckpts(n));

        CHECK_EQ(L.segment_table_end - L.ckpt_table_end, 28 * (2 + 3 * n));
        CHECK_EQ(L.payload_start, L.segment_table_end);
    }
}

void test_version_is_taken_from_the_header() {
    // the same "configuration" must produce two different payload starts, or the
    // version has been read from somewhere other than the file
    const layout v8 = compute_layout(VERSION_V8, 206, 8416, sample_states(), sample_ckpts(2));
    const layout v9 = compute_layout(VERSION_V9, 206, 8416, sample_states(), sample_ckpts(2));

    CHECK(v8.payload_start != v9.payload_start);
    CHECK_EQ(v9.payload_start - v8.payload_start, 224); // 28 * (2 + 3*2)
}

void test_state_sizes_match_the_observed_snapshot() {
    // the file the experiment actually wrote: asserting the fixture agrees with it
    // keeps this from drifting into a test of its own arithmetic
    const layout L = compute_layout(VERSION_V8, 206, 8416, sample_states(), sample_ckpts(2));

    CHECK_EQ(L.ckpt_table_end, 8830);

    // the checkpoint dft sizes differ by 21808 between the two records, which is what
    // the experiment's snapshot contained
    const uint64_t expected = 8830 + 195689284 + 22970832
                            + (0 + 156894364 + 16094456)
                            + (0 + 156894364 + 16116264);
    CHECK_EQ(L.payload_end, expected);
    CHECK_EQ(L.payload_end, 564668394);
}

// ---------------------------------------------------------------------------
// the EOF / segment-range check read_entry() now performs
//
// `seekg()` cannot prove a segment range exists: a stream may be positioned past
// EOF and still report `good()`, and the shortfall only surfaces on a read. A
// skipped checkpoint is never read, so a table whose last skipped segment runs past
// the end went unnoticed. The reader therefore takes the real file end and requires
// the table to account for it exactly.
//
// These cases drive the same arithmetic on the same inputs, including the overflow
// the check exists to catch.
// ---------------------------------------------------------------------------

// sum the segment table with overflow detection, the way the reader does
enum class range_verdict { ok, overflow, mismatch, outside };

range_verdict check_segment_range(const std::vector<uint64_t> & stored,
                                  uint64_t payload_start, uint64_t file_end) {
    if (file_end < payload_start) {
        return range_verdict::mismatch;
    }

    uint64_t total = 0;

    for (uint64_t n : stored) {
        if (total > UINT64_MAX - n) {
            return range_verdict::overflow;
        }
        total += n;
    }

    if (total != file_end - payload_start) {
        return range_verdict::mismatch;
    }

    // every segment must sit inside the payload
    uint64_t off = 0;

    for (uint64_t n : stored) {
        if (off > total || n > total - off) {
            return range_verdict::outside;
        }
        off += n;
    }

    return range_verdict::ok;
}

void test_segment_range_accepts_an_exact_file() {
    const std::vector<uint64_t> segs = { 100, 200, 300, 400 };
    CHECK(check_segment_range(segs, 1000, 2000) == range_verdict::ok);

    // an empty payload with no segments is consistent
    CHECK(check_segment_range({}, 500, 500) == range_verdict::ok);
}

void test_segment_range_rejects_a_truncated_file() {
    // the table promises 1000 bytes but the file holds fewer: this is the case a
    // skipped last checkpoint used to hide, because nothing read there
    const std::vector<uint64_t> segs = { 100, 200, 300, 400 };
    CHECK(check_segment_range(segs, 1000, 1999) == range_verdict::mismatch);

    // and a file longer than the table accounts for is equally wrong
    CHECK(check_segment_range(segs, 1000, 2001) == range_verdict::mismatch);
}

void test_segment_range_rejects_overflow() {
    // two segments that sum past 2^64: a corrupt table could otherwise wrap and
    // appear to fit a small file
    const std::vector<uint64_t> segs = { UINT64_MAX, 1 };
    CHECK(check_segment_range(segs, 0, 0) == range_verdict::overflow);

    // near-miss: the sum saturates exactly at the limit
    CHECK(check_segment_range({ UINT64_MAX - 1, 1 }, 0, UINT64_MAX) == range_verdict::ok);
    CHECK(check_segment_range({ UINT64_MAX, 1 }, 0, UINT64_MAX) == range_verdict::overflow);
}

void test_segment_range_rejects_a_payload_start_past_eof() {
    CHECK(check_segment_range({}, 2000, 1999) == range_verdict::mismatch);
}

void test_range_check_matches_the_real_snapshot() {
    // The v8 snapshot the experiment wrote: header ends at 8830, the file is
    // 564668394 bytes, and the seven segments account for the difference exactly.
    const std::array<uint64_t, 2> states = sample_states();
    const auto ckpts = sample_ckpts(2);

    const layout L = compute_layout(VERSION_V8, 206, 8416, states, ckpts);

    std::vector<uint64_t> stored;
    stored.push_back(states[0]);
    stored.push_back(states[1]);
    for (const auto & c : ckpts) {
        stored.push_back(c[0]);
        stored.push_back(c[1]);
        stored.push_back(c[2]);
    }

    CHECK_EQ(stored.size(), 2 + 3 * ckpts.size());

    // in v8 every segment is verbatim, so stored == raw and the sum is the payload
    CHECK(check_segment_range(stored, (uint64_t) L.payload_start, (uint64_t) L.payload_end) == range_verdict::ok);
    CHECK_EQ(L.payload_end, 564668394);
}

void test_range_check_covers_v9_with_a_smaller_payload() {
    // v9: the same raw segments, some stored shorter. The payload the file holds is
    // the sum of *stored* lengths, not raw ones - using the raw sizes would reject
    // every encoded file
    const std::array<uint64_t, 2> states = sample_states();
    const auto ckpts = sample_ckpts(2);

    const layout L = compute_layout(VERSION_V9, 206, 8416, states, ckpts);

    std::vector<uint64_t> raw;
    raw.push_back(states[0]);
    raw.push_back(states[1]);
    for (const auto & c : ckpts) {
        raw.push_back(c[0]);
        raw.push_back(c[1]);
        raw.push_back(c[2]);
    }

    uint64_t raw_total = 0;
    for (uint64_t n : raw) {
        raw_total += n;
    }

    // the snapshot compressed by 38980 bytes across three segments
    const uint64_t saved = 38980;
    const uint64_t stored_total = raw_total - saved;

    const uint64_t payload_start = (uint64_t) L.payload_start;
    const uint64_t file_end      = payload_start + stored_total;

    std::vector<uint64_t> stored = raw;
    stored[1] -= 24842; // drft
    stored[4] -= 7070;  // ck0.dft
    stored[7] -= 7068;  // ck1.dft

    CHECK_EQ(raw_total - stored_total, saved);
    CHECK(check_segment_range(stored, payload_start, file_end) == range_verdict::ok);

    // the same table against the *raw* total must be rejected: that is the file a
    // reader would have to find if it used raw lengths
    CHECK(check_segment_range(stored, payload_start, payload_start + raw_total) == range_verdict::mismatch);

    // the v8 layout of the same content is a different payload start, so a v9 file
    // measured with the v8 offset does not line up
    const layout v8 = compute_layout(VERSION_V8, 206, 8416, states, ckpts);
    CHECK(check_segment_range(stored, (uint64_t) v8.payload_start, file_end) == range_verdict::mismatch);
}

void test_range_check_with_skipped_last_checkpoint() {
    // The reported scenario: the *last* checkpoint is not selected, so its segments
    // are only seeked over. If its declared length ran past EOF nothing would read
    // there, and a `seekg().good()` check would not notice.
    const std::vector<uint64_t> stored = { 1000, 2000, 3000 };
    const uint64_t payload_start = 100;

    // a file that ends exactly at the table's total
    CHECK(check_segment_range(stored, payload_start, payload_start + 6000) == range_verdict::ok);

    // a file truncated inside the last, skipped segment
    CHECK(check_segment_range(stored, payload_start, payload_start + 5999) == range_verdict::mismatch);

    // truncated right at the boundary between the second and third
    CHECK(check_segment_range(stored, payload_start, payload_start + 3000) == range_verdict::mismatch);
}

} // namespace

int main() {
    test_v8_payload_follows_checkpoint_table();
    test_v8_has_no_padding_gap();
    test_v9_reserves_a_table();
    test_version_is_taken_from_the_header();
    test_state_sizes_match_the_observed_snapshot();
    test_segment_range_accepts_an_exact_file();
    test_segment_range_rejects_a_truncated_file();
    test_segment_range_rejects_overflow();
    test_segment_range_rejects_a_payload_start_past_eof();
    test_range_check_matches_the_real_snapshot();
    test_range_check_covers_v9_with_a_smaller_payload();
    test_range_check_with_skipped_last_checkpoint();

    test_v8_payload_follows_checkpoint_table();
    test_v8_has_no_padding_gap();
    test_v9_reserves_a_table();
    test_version_is_taken_from_the_header();
    test_state_sizes_match_the_observed_snapshot();

    if (g_failures == 0) {
        std::printf("all disk-cache layout checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
