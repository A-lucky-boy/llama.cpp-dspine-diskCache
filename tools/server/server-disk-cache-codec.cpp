#include "server-disk-cache-codec.h"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// the built-in encoder
//
// A compact LZ77 with a byte-oriented token stream. It is deliberately simple: the
// payloads it sees are opaque KV state, whose redundancy is unknown and may well be
// none. The encoder's job is to never be the reason a write fails, and to be small
// enough to audit completely. Every branch below is exercised by the unit tests,
// including the incompressible case, which must round-trip like any other.
//
// Token stream (little-endian varints, no padding):
//
//   flags      byte   bit 0: this token carries a match
//   lit_len    varint number of literal bytes that follow
//   literals   raw    lit_len bytes, written as-is
//   [ match ]  present only when bit 0 is set:
//     tag      byte   match length - 3, so 3..128 maps to 0..125
//     distance varint back-reference, >= 1 and <= the output produced so far
//
// The flags byte makes the two token shapes unambiguous: a final literal-only token
// carries no match fields at all, which is exactly how the stream ends. The decoder
// stops when the output reaches the declared length, so a truncated or padded stream
// cannot run away or be mistaken for a complete one.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t LZ_MIN_MATCH  = 3;
constexpr size_t LZ_MAX_MATCH  = 128;   // 125 + 3, the most a tag byte can express
constexpr size_t LZ_MAX_OFFSET = 1 << 16;

// The hash reads four bytes, so a position is only a valid hash source when four
// bytes remain. This is deliberately a separate constant from LZ_MIN_MATCH: the
// smallest match is three bytes, but the hash window is four, and conflating them
// is exactly how the tail of a buffer gets read out of bounds. A position with only
// three bytes left is still a fine place for the encoder to *be* - it just cannot be
// hashed, so it stays a literal.
constexpr size_t LZ_HASH_BYTES = 4;
constexpr size_t LZ_HASH_BITS  = 15;
constexpr size_t LZ_HASH_SIZE  = (size_t) 1 << LZ_HASH_BITS;
constexpr uint8_t LZ_FLAG_MATCH = 0x01;

void put_varint(std::vector<char> & out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back((char) ((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back((char) v);
}

bool get_varint(const char * in, size_t in_len, size_t & pos, uint64_t & v) {
    v = 0;
    int shift = 0;

    while (true) {
        if (pos >= in_len || shift > 63) {
            return false;
        }

        const uint8_t b = (uint8_t) in[pos++];
        v |= (uint64_t) (b & 0x7F) << shift;

        if ((b & 0x80) == 0) {
            return true;
        }

        shift += 7;
    }
}

// a 4-byte rolling hash over the upcoming bytes
inline uint32_t lz_hash(const char * p) {
    uint32_t h = (uint8_t) p[0];
    h = (h << 5) ^ (uint8_t) p[1];
    h = (h << 5) ^ (uint8_t) p[2];
    h = (h << 5) ^ (uint8_t) p[3];
    return h & (LZ_HASH_SIZE - 1);
}

// append one token: `lits` bytes of literal data, optionally followed by a match
void put_token(std::vector<char> & out, const char * lits, size_t lit_len,
               size_t match_len, size_t match_dist) {
    out.push_back((char) (match_len > 0 ? LZ_FLAG_MATCH : 0x00));
    put_varint(out, (uint64_t) lit_len);

    if (lit_len > 0) {
        out.insert(out.end(), lits, lits + lit_len);
    }

    if (match_len > 0) {
        out.push_back((char) (uint8_t) (match_len - LZ_MIN_MATCH));
        put_varint(out, (uint64_t) match_dist);
    }
}

} // namespace

const char * disk_cache_codec_mode_name(disk_cache_codec_mode mode) {
    switch (mode) {
        case disk_cache_codec_mode::none: return "none";
        case disk_cache_codec_mode::lz:   return "lz";
    }
    return "unknown";
}

bool disk_cache_codec_mode_parse(const char * name, disk_cache_codec_mode & out) {
    if (name == nullptr) {
        return false;
    }

    const std::string s = name;

    if (s == "none") { out = disk_cache_codec_mode::none; return true; }
    if (s == "lz")   { out = disk_cache_codec_mode::lz;   return true; }

    return false;
}

size_t disk_cache_codec_bound(size_t raw_len) {
    // worst case is one literal-only token: a flags byte, a varint length and the
    // bytes themselves. matches only ever replace literals, so this bounds the
    // output of the encoder below
    return raw_len + raw_len / 128 + 64;
}

disk_cache_codec_id disk_cache_codec_encode(const void * src_ptr, size_t src_len, std::vector<char> & dst) {
    dst.clear();

    const char * const srcp = (const char *) src_ptr;
    const size_t n = src_len;

    if (n == 0) {
        // an empty segment has nothing to encode and must stay empty, so the caller
        // records a zero-length raw segment
        return disk_cache_codec_id::raw;
    }

    if (n <= LZ_MIN_MATCH) {
        // too short to describe a match. store verbatim so the caller sees a
        // non-shrinking result
        dst.assign(srcp, srcp + n);
        return disk_cache_codec_id::raw;
    }

    dst.reserve(disk_cache_codec_bound(n));

    // head[h] is the most recent position with that hash; prev[i] the one before it.
    // positions are absolute offsets into src
    std::vector<int32_t> head(LZ_HASH_SIZE, -1);
    std::vector<int32_t> prev(n, -1);

    auto insert_pos = [&](size_t p) {
        // the same window the lookup uses: hashing reads four bytes, so a position
        // near the end that cannot supply them is simply not inserted
        if (n - p >= LZ_HASH_BYTES) {
            const uint32_t h = lz_hash(&srcp[p]);
            prev[p] = head[h];
            head[h] = (int32_t) p;
        }
    };

    size_t lit_start = 0;
    size_t pos       = 0;

    while (pos < n) {
        size_t best_len  = 0;
        size_t best_dist = 0;

        // A candidate needs four bytes to hash, not three to match. Using
        // LZ_MIN_MATCH here left the last three bytes of a buffer reading one byte
        // past its end: at `n - pos == 3` the hash loaded `srcp[pos+3]`.
        if (n - pos >= LZ_HASH_BYTES) {
            int32_t cand  = head[lz_hash(&srcp[pos])];
            size_t  tried = 0;

            while (cand >= 0 && tried < 32) {
                const size_t dist = pos - (size_t) cand;

                if (dist == 0 || dist > LZ_MAX_OFFSET) {
                    break;
                }

                // the hash covers 4 bytes but collisions are common on structured
                // data, so the first 3 bytes are verified before extending
                if (memcmp(&srcp[cand], &srcp[pos], LZ_MIN_MATCH) == 0) {
                    size_t len = LZ_MIN_MATCH;

                    while (len < LZ_MAX_MATCH && pos + len < n &&
                            srcp[(size_t) cand + len] == srcp[pos + len]) {
                        len++;
                    }

                    if (len > best_len) {
                        best_len  = len;
                        best_dist = dist;
                    }
                }

                cand = prev[cand];
                tried++;
            }
        }

        if (best_len >= LZ_MIN_MATCH) {
            put_token(dst, &srcp[lit_start], pos - lit_start, best_len, best_dist);

            // every position the match covers becomes a match source for later ones
            for (size_t k = 0; k < best_len; k++) {
                insert_pos(pos + k);
            }

            pos += best_len;
            lit_start = pos;
        } else {
            insert_pos(pos);
            pos++;
        }
    }

    // The trailing literal run ends the stream, when there is one. If the loop
    // finished exactly on a match then `lit_start == n` and the stream already ends
    // on that match token: writing an extra empty-literal token would leave a byte
    // the decoder never consumes, because it stops as soon as the output is
    // complete. The decoder requires the stream to be consumed exactly, so the
    // encoder must not produce a tail it will not read.
    if (lit_start < n) {
        put_token(dst, &srcp[lit_start], n - lit_start, 0, 0);
    }

    if (dst.size() >= n) {
        // not worth it: hand back the original so the caller stores it verbatim
        dst.assign(srcp, srcp + n);
        return disk_cache_codec_id::raw;
    }

    return disk_cache_codec_id::lz;
}

bool disk_cache_codec_decode(const void * src_ptr, size_t src_len, disk_cache_segment_desc desc, std::vector<char> & dst) {
    dst.clear();

    const char * const srcp = (const char *) src_ptr;
    const size_t in_len = src_len;

    if (desc.raw_len > DISK_CACHE_SEGMENT_RAW_MAX) {
        return false;
    }

    if (desc.stored_len != (uint64_t) in_len) {
        return false;
    }

    if (desc.codec == disk_cache_codec_id::raw) {
        // a verbatim segment inside an encoded file: the lengths must agree, or the
        // file contradicts itself
        if (desc.raw_len != desc.stored_len) {
            return false;
        }

        dst.assign(srcp, srcp + in_len);
        return true;
    }

    if (desc.codec != disk_cache_codec_id::lz) {
        return false;
    }

    const size_t want = (size_t) desc.raw_len;

    if (want == 0) {
        // an encoded empty segment is not something the encoder produces
        return false;
    }

    try {
        dst.resize(want);
    } catch (...) {
        return false;
    }

    size_t pos = 0;
    size_t out = 0;

    while (out < want) {
        if (pos >= in_len) {
            return false;
        }

        const uint8_t flags = (uint8_t) srcp[pos++];

        // Only bit 0 is defined. Accepting unknown bits would make a corrupted flags
        // byte decode as an ordinary token - flipping the high bits of `0x01` leaves
        // bit 0 set, so the stream would still parse and produce identical output.
        // That is not a hypothetical: it is what the corrupt-stream test caught.
        if ((flags & ~LZ_FLAG_MATCH) != 0) {
            return false;
        }

        uint64_t lit = 0;

        if (!get_varint(srcp, in_len, pos, lit)) {
            return false;
        }

        if (lit > want - out) {
            return false;
        }

        if (lit > in_len - pos) {
            return false;
        }

        if (lit > 0) {
            memcpy(dst.data() + out, srcp + pos, (size_t) lit);
            pos += (size_t) lit;
            out += (size_t) lit;
        }

        if ((flags & LZ_FLAG_MATCH) == 0) {
            // a literal-only token ends the stream. if output is still outstanding,
            // the stream is short
            return out == want;
        }

        if (pos >= in_len) {
            return false;
        }

        const size_t len = (size_t) (uint8_t) srcp[pos++] + LZ_MIN_MATCH;

        if (len > want - out) {
            // a match may not overshoot the declared output length
            return false;
        }

        uint64_t dist = 0;

        if (!get_varint(srcp, in_len, pos, dist)) {
            return false;
        }

        if (dist == 0 || dist > out) {
            // a back-reference into nothing, or past the start of the output
            return false;
        }

        const size_t from = out - (size_t) dist;

        // byte-wise, because the ranges overlap - that is how runs are expressed
        for (size_t k = 0; k < len; k++) {
            dst[out + k] = dst[from + k];
        }

        out += len;
    }

    // The output reached its declared length. The stream must also have been
    // consumed exactly: a well-formed stream ends on the final token, so anything
    // left over means the encoded bytes do not describe what the header claims.
    //
    // This is a structural check, not a substitute for the caller's CRC over the
    // decoded bytes - it catches a stream that is internally inconsistent rather
    // than one that is merely damaged.
    return out == want && pos == src_len;
}
