// Segmented lossless codec for disk snapshot payloads.
//
// A disk entry stores several byte carriers that are independent of each other: the
// target state, the draft state, and each checkpoint's spec/tgt/dft. This module
// encodes them one at a time.
//
// Per segment, not per file, is the whole point. read_entry() restores exactly one
// checkpoint, so concatenating every checkpoint into a single stream would force
// decompressing the ones it does not need - exactly the on-demand property the file
// format already has. It would also make a corrupt stream fail the whole file
// instead of one segment.
//
// Every encoder here is deterministic and self-contained: no DLL, no version
// negotiation at runtime. A segment is only ever replaced by its encoded form when
// that is genuinely smaller, and the decision is recorded per segment, so a single
// snapshot may mix encoded and verbatim segments.
//
// The codec is an optimisation, never a substitute for integrity: the caller's
// CRC64 stays bound to the *decoded* bytes, and decoding verifies the output length
// against the length declared in the header before anything is restored.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// what a segment's bytes are
enum class disk_cache_codec_id : uint32_t {
    raw  = 0, // stored verbatim
    lz   = 1, // the built-in lossless encoder
};

// how a file's payload was written
enum class disk_cache_codec_mode : uint32_t {
    none = 0, // v8: every segment verbatim, no segment table
    lz   = 1, // v9: per-segment table, encoded where it helped
};

const char * disk_cache_codec_mode_name(disk_cache_codec_mode mode);

// `none` and `lz` are the accepted spellings for the CLI; returns false on anything
// else, so an unknown name is a startup error rather than a silent downgrade
bool disk_cache_codec_mode_parse(const char * name, disk_cache_codec_mode & out);

// One segment's header entry. `raw_len` is the original byte count and `stored_len`
// what occupies the file; they are equal for raw segments. `encoded_crc` covers the
// bytes as stored, which catches a flipped byte in the encoded form early - the
// caller's own CRC is what ultimately proves the decoded bytes.
struct disk_cache_segment_desc {
    uint64_t             raw_len     = 0;
    uint64_t             stored_len  = 0;
    disk_cache_codec_id  codec       = disk_cache_codec_id::raw;
    uint64_t             encoded_crc = 0;
};

// Bounds the decoder applies before it trusts anything from a header. A corrupt or
// hostile length must not turn into a huge allocation.
//
// The largest legitimate segment is a full target or draft state, so the ceiling is
// generous but finite; the project's own `DISK_CACHE_MAX_BYTES_PER_TOKEN` guard and
// the state API's own sizes are far below it.
constexpr uint64_t DISK_CACHE_SEGMENT_RAW_MAX = 1ull << 40; // 1 TiB
constexpr uint64_t DISK_CACHE_SEGMENT_COMPRESSED_MAX = DISK_CACHE_SEGMENT_RAW_MAX + (1ull << 20);

// The byte carriers come from two different containers: the KV states are
// `common_state_data` (a vector with a no-init allocator) and the encoded form is a
// plain byte buffer. Both are contiguous, so these take raw pointers and sizes
// rather than a container type - that keeps this module independent of the server's
// own aliases and lets the tests drive it with plain vectors.
//
// encode `src` into `dst`. Returns the codec actually used and fills `dst` with the
// bytes to store; the caller must compare the resulting size and may fall back to
// storing `src` verbatim (in which case the returned id is meaningless to it).
//
// Never fails on content - the worst case is output larger than input, which the
// caller detects by size.
disk_cache_codec_id disk_cache_codec_encode(const void * src, size_t src_len, std::vector<char> & dst);

// decode `src_len` bytes of `src` into `dst`, which receives exactly the number of
// bytes `desc.raw_len` declares. Returns false, without usable output, when:
//   * the declared lengths are impossible or exceed the bounds above
//   * `src_len` does not match `desc.stored_len`
//   * the codec id is not recognised
//   * the decoded byte count is not exactly `raw_len`
// A `raw` segment simply copies: the format allows a verbatim segment inside an
// encoded file, and it must round-trip like any other.
bool disk_cache_codec_decode(const void * src, size_t src_len, disk_cache_segment_desc desc, std::vector<char> & dst);

// the smallest buffer the encoder may need for a segment of `raw_len` bytes.
// Callers use it to keep the working set bounded instead of growing a vector to the
// worst case for a multi-hundred-MiB state.
size_t disk_cache_codec_bound(size_t raw_len);
