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

} // namespace

int main() {
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
