#include "server-disk-cache-match.h"

#include <algorithm>

//
// Implementation notes
//
// The match is computed in three steps, mirroring the checks load() performs:
//
//   1. the raw longest common token prefix
//   2. a bidirectional media comparison over that prefix: a chunk of A must exist
//      in B at the same start with the same length and id, and vice versa. a
//      prefix whose media metadata is a strict subset of the other side is not
//      enough - the KV of a chunk is only interchangeable with the same chunk
//   3. a boundary clamp: a usable prefix may not end inside a media chunk
//
// Malformed media metadata (arrays of different length, overlapping or
// out-of-range chunks) makes the comparison stop at the first position it cannot
// verify, which is the conservative answer for every caller.
//

namespace {

bool media_structurally_ok(const disk_cache_prompt_identity & id) {
    if (id.media_indices == nullptr || id.media_token_counts == nullptr || id.media_ids == nullptr) {
        return true; // no media metadata at all
    }
    return id.media_indices->size() == id.media_token_counts->size() &&
           id.media_indices->size() == id.media_ids->size();
}

// every chunk of `a` that starts before `limit` must exist in `b` with the same
// start, the same token count and the same id. returns false on the first
// difference, which is where the usable prefix ends
bool media_side_ok(const disk_cache_prompt_identity & a, const disk_cache_prompt_identity & b, size_t limit, size_t * clamp_to) {
    if (a.media_indices == nullptr || a.media_indices->empty()) {
        return true;
    }

    for (size_t i = 0; i < a.media_indices->size(); ++i) {
        const uint32_t idx = (*a.media_indices)[i];
        if ((size_t) idx >= limit) {
            continue;
        }

        // a chunk present on one side and absent on the other is a different
        // prompt at that position, even when every token id matches (media
        // placeholders are all LLAMA_TOKEN_NULL)
        if (b.media_indices == nullptr) {
            *clamp_to = std::min(*clamp_to, (size_t) idx);
            return false;
        }

        const auto & b_idx = *b.media_indices;
        const auto it = std::lower_bound(b_idx.begin(), b_idx.end(), idx);
        if (it == b_idx.end() || *it != idx) {
            *clamp_to = std::min(*clamp_to, (size_t) idx);
            return false;
        }

        const size_t j = (size_t) (it - b_idx.begin());
        if ((*a.media_token_counts)[i] != (*b.media_token_counts)[j] ||
                (*a.media_ids)[i] != (*b.media_ids)[j]) {
            *clamp_to = std::min(*clamp_to, (size_t) idx);
            return false;
        }
    }

    return true;
}

// a prefix may not end strictly inside a media chunk of either side
void clamp_inside_media(const disk_cache_prompt_identity & id, size_t * lcp) {
    if (id.media_indices == nullptr || id.media_token_counts == nullptr) {
        return;
    }

    for (size_t i = 0; i < id.media_indices->size(); ++i) {
        const size_t start = (*id.media_indices)[i];
        const size_t end   = start + (*id.media_token_counts)[i];
        if (*lcp > start && *lcp < end) {
            *lcp = start;
            return;
        }
    }
}

size_t token_lcp(const llama_tokens & a, const llama_tokens & b) {
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) {
        ++i;
    }
    return i;
}

} // namespace

disk_cache_prefix_match disk_cache_match_prefix(const disk_cache_prompt_identity & a,
        const disk_cache_prompt_identity & b) {
    disk_cache_prefix_match res;

    if (a.tokens == nullptr || b.tokens == nullptr) {
        return res;
    }

    res.token_lcp  = token_lcp(*a.tokens, *b.tokens);
    res.usable_lcp = res.token_lcp;

    if (!media_structurally_ok(a) || !media_structurally_ok(b)) {
        // inconsistent metadata: nothing may be claimed beyond what the tokens
        // alone prove, and that is exactly what the conservative answer is
        res.usable_lcp = 0;
        return res;
    }

    size_t clamp_to = res.usable_lcp;

    if (!media_side_ok(a, b, res.usable_lcp, &clamp_to) ||
            !media_side_ok(b, a, res.usable_lcp, &clamp_to)) {
        res.usable_lcp = clamp_to;
    }

    clamp_inside_media(a, &res.usable_lcp);
    clamp_inside_media(b, &res.usable_lcp);

    res.usable_lcp = std::min(res.usable_lcp, res.token_lcp);
    res.media_compatible = res.usable_lcp == res.token_lcp;

    return res;
}

bool disk_cache_cost_ok(size_t file_bytes, size_t usable_lcp) {
    if (usable_lcp == 0) {
        return false; // nothing to restore, nothing to save
    }

    return file_bytes <= usable_lcp * DISK_CACHE_MAX_BYTES_PER_TOKEN;
}

bool disk_cache_identity_equal(const disk_cache_prompt_identity & a, const disk_cache_prompt_identity & b) {
    if (a.tokens == nullptr || b.tokens == nullptr) {
        return false;
    }

    if (*a.tokens != *b.tokens) {
        return false;
    }

    const auto n_media = [](const disk_cache_prompt_identity & id) -> size_t {
        return id.media_indices == nullptr ? 0 : id.media_indices->size();
    };

    if (n_media(a) != n_media(b)) {
        return false;
    }

    if (n_media(a) == 0) {
        return true;
    }

    return *a.media_indices == *b.media_indices &&
           *a.media_token_counts == *b.media_token_counts &&
           *a.media_ids == *b.media_ids;
}

disk_cache_duplicate disk_cache_classify_duplicate(const disk_cache_prompt_identity & existing,
        const disk_cache_prompt_identity & wanted, bool same_hash_and_length) {
    if (!same_hash_and_length) {
        return disk_cache_duplicate::none;
    }

    // the hash and the length only select a candidate; the identity decides. a
    // conflicting identity is reported as `none`, so the caller refuses the write
    // instead of overwriting the file or counting it as a duplicate
    if (!disk_cache_identity_equal(existing, wanted)) {
        return disk_cache_duplicate::none;
    }

    return disk_cache_duplicate::identical;
}

bool disk_cache_suppresses_write(const disk_cache_prompt_identity & existing,
        const disk_cache_prompt_identity & wanted, size_t min_tokens, size_t max_prefix_lag_tokens) {
    if (existing.tokens == nullptr || wanted.tokens == nullptr || wanted.tokens->empty()) {
        return false;
    }

    const disk_cache_prefix_match match = disk_cache_match_prefix(existing, wanted);

    // the shared prefix must be usable: media metadata has to agree over it
    if (!match.media_compatible) {
        return false;
    }

    const size_t usable = std::min(match.usable_lcp, wanted.tokens->size());

    if (usable < min_tokens) {
        return false;
    }

    // keep the original "all but a short tail" tolerance
    if (usable + max_prefix_lag_tokens < wanted.tokens->size()) {
        return false;
    }

    // ... but only when the existing entry could actually be restored for that
    // prefix: a 1.2 GiB entry cannot serve a 1.4k-token request
    return disk_cache_cost_ok(existing.file_bytes, usable);
}

bool disk_cache_can_replace(const disk_cache_prompt_identity & longer,
        const disk_cache_prompt_identity & shorter, size_t min_tokens) {
    if (longer.tokens == nullptr || shorter.tokens == nullptr) {
        return false;
    }

    // the shorter one must be a strict prefix, and the match has to be usable over
    // its whole length: a media chunk that differs (or that would be cut) inside
    // the range makes the longer entry a different prompt
    if (shorter.tokens->size() >= longer.tokens->size() || shorter.tokens->size() < min_tokens) {
        return false;
    }

    const disk_cache_prefix_match match = disk_cache_match_prefix(longer, shorter);

    if (match.usable_lcp != shorter.tokens->size()) {
        return false;
    }

    // restoring the longer entry to serve this prefix must pass the cost guard,
    // otherwise deleting the shorter one would remove the only economical source
    return disk_cache_cost_ok(longer.file_bytes, shorter.tokens->size());
}
