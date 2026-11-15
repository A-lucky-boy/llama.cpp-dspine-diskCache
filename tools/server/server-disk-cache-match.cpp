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
// Both directions are always scanned: each produces its own earliest difference
// and the two positions are combined with min(). Evaluating them through a
// short-circuit `||` would leave the earlier difference of the second direction
// unexamined whenever the first one already diverged.
//
// Media metadata is validated structurally before it is trusted: the three arrays
// must agree in length, indices must be strictly increasing, chunks must be
// non-empty, in range and non-overlapping, and ids must be non-empty. Anything
// else makes the identity unverifiable, so the usable prefix becomes 0 instead of
// being trusted just because the array sizes happen to match.
//

namespace {

// a missing description is valid; anything present must be well formed
bool media_structurally_ok(const disk_cache_prompt_identity & id, size_t n_tokens) {
    if (id.media_indices == nullptr || id.media_token_counts == nullptr || id.media_ids == nullptr) {
        return true; // no media metadata at all
    }

    const size_t n = id.media_indices->size();

    if (id.media_token_counts->size() != n || id.media_ids->size() != n) {
        return false;
    }

    size_t previous_end = 0;

    for (size_t i = 0; i < n; ++i) {
        const size_t start = (*id.media_indices)[i];
        const size_t count = (*id.media_token_counts)[i];

        if ((*id.media_ids)[i].empty() || count == 0) {
            return false;
        }

        // strictly increasing: this also rejects duplicates and out-of-order entries
        if (i > 0 && start <= (*id.media_indices)[i - 1]) {
            return false;
        }

        if (start < previous_end || start >= n_tokens || count > n_tokens - start) {
            return false;
        }

        previous_end = start + count;
    }

    return true;
}

// every chunk of `a` that starts before `limit` must exist in `b` with the same
// start, the same token count and the same id. returns the earliest position
// where the two sides disagree, or `limit` when they agree everywhere below it.
// one call covers exactly one direction and is unaffected by the other
size_t media_side_earliest_difference(const disk_cache_prompt_identity & a,
        const disk_cache_prompt_identity & b, size_t limit) {
    if (a.media_indices == nullptr || a.media_indices->empty()) {
        return limit;
    }

    size_t earliest = limit;

    for (size_t i = 0; i < a.media_indices->size(); ++i) {
        const uint32_t idx = (*a.media_indices)[i];
        if ((size_t) idx >= limit) {
            continue;
        }

        // a chunk present on one side and absent on the other is a different
        // prompt at that position, even when every token id matches (media
        // placeholders are all LLAMA_TOKEN_NULL)
        bool same = false;

        if (b.media_indices != nullptr) {
            const auto & b_idx = *b.media_indices;
            const auto it = std::lower_bound(b_idx.begin(), b_idx.end(), idx);

            if (it != b_idx.end() && *it == idx) {
                const size_t j = (size_t) (it - b_idx.begin());
                same = (*a.media_token_counts)[i] == (*b.media_token_counts)[j] &&
                       (*a.media_ids)[i]          == (*b.media_ids)[j];
            }
        }

        if (!same) {
            earliest = std::min(earliest, (size_t) idx);
        }
    }

    return earliest;
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

    if (!media_structurally_ok(a, a.tokens->size()) || !media_structurally_ok(b, b.tokens->size())) {
        // unverifiable metadata: nothing may be claimed, not even the token prefix
        res.usable_lcp = 0;
        return res;
    }

    // both directions are evaluated unconditionally: the earlier difference of the
    // second direction has to be seen even when the first one already diverged
    const size_t diff_a_to_b = media_side_earliest_difference(a, b, res.usable_lcp);
    const size_t diff_b_to_a = media_side_earliest_difference(b, a, res.usable_lcp);

    res.usable_lcp = std::min(res.usable_lcp, std::min(diff_a_to_b, diff_b_to_a));

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

bool disk_cache_media_valid(const disk_cache_prompt_identity & id) {
    if (id.tokens == nullptr) {
        return false;
    }

    return media_structurally_ok(id, id.tokens->size());
}

bool disk_cache_identity_equal(const disk_cache_prompt_identity & a, const disk_cache_prompt_identity & b) {
    if (a.tokens == nullptr || b.tokens == nullptr) {
        return false;
    }

    if (*a.tokens != *b.tokens) {
        return false;
    }

    if (!disk_cache_media_valid(a) || !disk_cache_media_valid(b)) {
        return false; // an unverifiable description is never "equal"
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
        const disk_cache_prompt_identity & wanted, bool same_hash) {
    if (!same_hash) {
        return disk_cache_duplicate::none;
    }

    // the hash only selects a candidate; the identity decides. a differing token
    // length is a conflict as well, not a reason to skip the check - the file name
    // is derived from the hash, so no other prompt may use that path
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
    // otherwise deleting the shorter one would remove the only economical source.
    // file_bytes has to be the real committed size: a pre-write estimate may not
    // decide whether an existing snapshot survives
    return disk_cache_cost_ok(longer.file_bytes, shorter.tokens->size());
}
