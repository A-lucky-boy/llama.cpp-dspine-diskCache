#pragma once

#include "common.h"

#include <cstdint>
#include <string>
#include <vector>

//
// pure metadata helpers for the on-disk prompt cache
//
// Everything here answers one question without reading a KV file, touching a
// llama_context or taking a lock: "may entry X be treated as a substitute for
// prompt Y?". The answers feed the write-suppression, redundancy and prefix-dedup
// paths, so they must describe exactly the deterministic pre-filters that
// server_disk_cache::load() applies before it restores anything:
//
//   - token prefix, media identities (both directions) and a legal boundary
//   - the minimum match length (--cache-disk-min-tokens)
//   - the read-cost guard (bytes per restored token)
//
// They never replace the real validation in load(): a file is still opened,
// checksummed, media-checked and restored there. A positive answer here only
// means "a request would not be rejected on metadata grounds".
//

// the maximum bytes a restore may read per token it avoids prefilling. The value
// is unchanged from the original implementation; only its use site moved here so
// that every suppression decision and load() share one definition
inline constexpr size_t DISK_CACHE_MAX_BYTES_PER_TOKEN = 768ull*1024ull;

// one prompt's immutable identity: token ids plus the media chunks they carry.
// pointers are non-owning so callers can pass either an entry or a pending write
// without copying gigabytes of state
struct disk_cache_prompt_identity {
    const llama_tokens *             tokens             = nullptr;
    const std::vector<uint32_t> *    media_indices      = nullptr;
    const std::vector<uint32_t> *    media_token_counts = nullptr;
    const std::vector<std::string> * media_ids          = nullptr;

    // size of the entry file this identity is stored in, 0 when unknown (a
    // pending write has no file yet). only used by the cost guard
    size_t file_bytes = 0;
};

// result of comparing two identities
struct disk_cache_prefix_match {
    size_t token_lcp  = 0; // raw longest common token prefix
    size_t usable_lcp = 0; // after the bidirectional media checks and boundary clamp
    bool   media_compatible = false; // usable_lcp == token_lcp, i.e. media added no cut
};

// compare two identities. pure metadata, symmetric in its arguments
disk_cache_prefix_match disk_cache_match_prefix(const disk_cache_prompt_identity & a,
        const disk_cache_prompt_identity & b);

// true when the cost guard allows restoring an entry of `file_bytes` for a
// prefix of `usable_lcp` tokens
bool disk_cache_cost_ok(size_t file_bytes, size_t usable_lcp);

// true when the two identities describe the same prompt: identical token ids and
// identical media triples. a hash match alone never implies this
bool disk_cache_identity_equal(const disk_cache_prompt_identity & a, const disk_cache_prompt_identity & b);

// what an already stored entry means for a write of `wanted`
enum class disk_cache_duplicate {
    none,       // not the same prompt
    identical,  // same tokens and media - the write is redundant
};

// classify an exact duplicate. `same_hash_and_length` is the cheap index
// condition the caller already computed; a hash/length hit whose identity differs
// is reported as `none` so the caller refuses the write instead of treating a
// collision as a duplicate
disk_cache_duplicate disk_cache_classify_duplicate(const disk_cache_prompt_identity & existing,
        const disk_cache_prompt_identity & wanted, bool same_hash_and_length);

// true when `existing` already covers all but a short tail of `wanted`, and could
// actually be restored for it: same media over the shared prefix, the tail within
// `max_prefix_lag_tokens`, and the entry's bytes-per-token within the cost guard
bool disk_cache_suppresses_write(const disk_cache_prompt_identity & existing,
        const disk_cache_prompt_identity & wanted, size_t min_tokens, size_t max_prefix_lag_tokens);

// true when every request that `shorter` could serve would be served at least as
// well by `longer`: `shorter` is a media-compatible token prefix of `longer`, and
// restoring `longer` for such a request passes the cost guard. This is the only
// condition under which a longer entry may supersede or suppress a shorter one
bool disk_cache_can_replace(const disk_cache_prompt_identity & longer,
        const disk_cache_prompt_identity & shorter, size_t min_tokens);
