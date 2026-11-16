#include "server-prefix-materialize.h"

//
// Implementation notes
//
// The order of the checks is the point of this module. A candidate has to clear,
// in this order:
//
//   1. it is eligible (the directory saw it often enough)
//   2. it is not already persisted - capturing a prefix that already has a
//      snapshot would write a duplicate file and gain nothing
//   3. it is at least as long as the disk tier's own minimum, which is separate
//      from the directory's observation threshold. the disk tier must not be
//      talked into accepting a prefix it would normally refuse
//   4. it fits inside this request, and its end does not fall inside a media
//      chunk - a chunk's KV cannot be split, so a prefix ending inside one has no
//      well-defined complete state
//   5. the KV has not already moved past the position. stopping the batch at an
//      already-computed position would mean either re-running the prefix or
//      slicing live KV, and neither is allowed
//
// When several candidates pass, the longest one wins: it subsumes the shorter
// ones, so one capture covers as much future reuse as possible.
//

bool disk_cache_media_chunks_valid(const std::vector<disk_cache_media_chunk> & chunks, size_t n_tokens_total) {
    int64_t prev_end = 0;

    for (const auto & c : chunks) {
        // an empty chunk has no interior, so "strictly inside" could never be true
        // and the entry would silently weaken the check
        if (c.end <= c.start) {
            return false;
        }

        // overlaps and out-of-order ranges would make the "is k inside a chunk"
        // answer depend on the scan order
        if ((int64_t) c.start < prev_end) {
            return false;
        }

        if (c.end > n_tokens_total) {
            return false;
        }

        prev_end = (int64_t) c.end;
    }

    return true;
}

bool disk_cache_prefix_in_media(size_t k, const std::vector<disk_cache_media_chunk> & chunks) {
    // strict interior only. a chunk's start and its end are both legal places for
    // a prefix to finish: at the start the chunk is entirely ahead, at the end it
    // is entirely behind, and neither case splits a chunk
    for (const auto & c : chunks) {
        if (k > c.start && k < c.end) {
            return true;
        }

        // ordered and non-overlapping, so once a chunk starts past k there is
        // nothing left to match
        if (c.start >= k) {
            break;
        }
    }

    return false;
}

const char * disk_cache_capture_bounds_name(disk_cache_capture_bounds bounds) {
    switch (bounds) {
        case disk_cache_capture_bounds::exact:         return "exact";
        case disk_cache_capture_bounds::target_short:  return "target_short";
        case disk_cache_capture_bounds::target_past:   return "target_past";
        case disk_cache_capture_bounds::draft_short:   return "draft_short";
        case disk_cache_capture_bounds::draft_past:    return "draft_past";
        case disk_cache_capture_bounds::draft_missing: return "draft_missing";
    }
    return "unknown";
}

disk_cache_capture_bounds disk_cache_check_capture_bounds(size_t prefix_tokens,
        int64_t target_end, bool has_draft, int64_t draft_end) {
    // a negative endpoint means the sequence holds nothing
    if (target_end < 0 || (size_t) target_end < prefix_tokens) {
        return disk_cache_capture_bounds::target_short;
    }

    if ((size_t) target_end > prefix_tokens) {
        return disk_cache_capture_bounds::target_past;
    }

    // the target sits exactly on the prefix. without a draft that is the whole
    // question
    if (!has_draft) {
        return disk_cache_capture_bounds::exact;
    }

    if (draft_end < 0) {
        return disk_cache_capture_bounds::draft_missing;
    }

    if ((size_t) draft_end < prefix_tokens) {
        return disk_cache_capture_bounds::draft_short;
    }

    if ((size_t) draft_end > prefix_tokens) {
        // the draft ran ahead. the only ways to fix that here would be to rewind
        // it or to slice its opaque state, and neither is allowed for a state that
        // is part of a live request
        return disk_cache_capture_bounds::draft_past;
    }

    return disk_cache_capture_bounds::exact;
}

bool disk_cache_checkpoint_fits(const disk_cache_checkpoint_view & ckpt,
        size_t prefix_tokens, int64_t prefix_pos_max) {
    if (prefix_tokens == 0) {
        return false;
    }

    if (ckpt.n_tokens < 0 || ckpt.pos_min < 0 || ckpt.pos_max < 0) {
        return false;
    }

    if (ckpt.pos_min > ckpt.pos_max) {
        return false;
    }

    // the entry claims `prefix_tokens` tokens: a checkpoint covering more of them
    // describes state the entry cannot reproduce
    if ((size_t) ckpt.n_tokens > prefix_tokens) {
        return false;
    }

    // and its KV ends at prefix_pos_max. `>=` is not used here: the bound is a
    // position, and a checkpoint at exactly that position is still inside
    if (prefix_pos_max < 0 || ckpt.pos_max > prefix_pos_max) {
        return false;
    }

    return true;
}

const char * disk_cache_materialize_skip_name(disk_cache_materialize_skip reason) {
    switch (reason) {
        case disk_cache_materialize_skip::none:             return "none";
        case disk_cache_materialize_skip::disabled:         return "disabled";
        case disk_cache_materialize_skip::no_candidates:    return "no_candidates";
        case disk_cache_materialize_skip::too_short:        return "too_short";
        case disk_cache_materialize_skip::already_persisted:return "already_persisted";
        case disk_cache_materialize_skip::beyond_input:     return "beyond_input";
        case disk_cache_materialize_skip::media:            return "media";
        case disk_cache_materialize_skip::past:             return "past";
        case disk_cache_materialize_skip::duplicate:        return "duplicate";
    }
    return "unknown";
}

disk_cache_materialize_plan disk_cache_plan_materialize(const disk_cache_materialize_request & req,
        const std::vector<disk_cache_materialize_candidate> & candidates,
        disk_cache_materialize_skip & skipped) {
    disk_cache_materialize_plan plan;
    skipped = disk_cache_materialize_skip::none;

    if (!req.enabled) {
        skipped = disk_cache_materialize_skip::disabled;
        return plan;
    }

    // a malformed media list means the boundary of this request is unknown. it is
    // refused outright instead of being read as "no media": the alternative would
    // be capturing a prefix without knowing whether its end splits a chunk
    if (!disk_cache_media_chunks_valid(req.media_chunks, req.n_input_tokens)) {
        skipped = disk_cache_materialize_skip::media;
        return plan;
    }

    if (candidates.empty()) {
        skipped = disk_cache_materialize_skip::no_candidates;
        return plan;
    }

    bool saw_any          = false; // the report contained at least one real candidate
    bool saw_eligible     = false; // ... and at least one the directory considers worth capturing
    // an eligible entry that is already persisted: a different situation from
    // "the directory has not qualified anything yet"
    bool saw_eligible_only_persisted = false;
    bool saw_short        = false;
    bool saw_beyond_input = false;
    bool saw_media        = false;
    bool saw_past         = false;

    for (const auto & c : candidates) {
        // a candidate with no length describes nothing worth capturing
        if (c.prefix_tokens == 0) {
            continue;
        }

        saw_any = true;

        if (c.eligible) {
            saw_eligible = true;

            if (c.persisted) {
                // qualified, but it already has a snapshot: nothing to create
                saw_eligible_only_persisted = true;
                continue;
            }
        } else if (c.persisted) {
            // has a snapshot but is not eligible any more (e.g. withdrawn):
            // not a capture target either
            continue;
        } else {
            // `observed` is not enough: the directory has not decided this prefix
            // is worth capturing
            continue;
        }

        // the disk tier's minimum applies here, not the directory's threshold
        if (req.min_tokens > 0 && c.prefix_tokens < req.min_tokens) {
            saw_short = true;
            continue;
        }

        if (c.prefix_tokens > req.n_input_tokens) {
            // this request never reaches that position
            saw_beyond_input = true;
            continue;
        }

        // a prefix whose end lands inside a media chunk has no complete-state
        // boundary to stop at. ending between two chunks, or exactly on a chunk's
        // start or end, is fine
        if (disk_cache_prefix_in_media(c.prefix_tokens, req.media_chunks)) {
            saw_media = true;
            continue;
        }

        if (req.n_past >= 0 && (size_t) req.n_past > c.prefix_tokens) {
            // the KV is already beyond the boundary. the state would have to be
            // rewound or the prefix recomputed, and neither is permitted
            saw_past = true;
            continue;
        }

        // the longest usable candidate wins: it is a superset of the shorter ones
        if (!plan.capture || c.prefix_tokens > plan.prefix_tokens) {
            plan.capture       = true;
            plan.prefix_tokens = c.prefix_tokens;
        }
    }

    if (!plan.capture) {
        if (saw_media) {
            skipped = disk_cache_materialize_skip::media;
        } else if (saw_past) {
            skipped = disk_cache_materialize_skip::past;
        } else if (saw_beyond_input) {
            skipped = disk_cache_materialize_skip::beyond_input;
        } else if (saw_short) {
            skipped = disk_cache_materialize_skip::too_short;
        } else if (saw_eligible_only_persisted) {
            // every eligible-looking entry already has a snapshot: there is
            // genuinely nothing to create
            skipped = disk_cache_materialize_skip::already_persisted;
        } else {
            // the report held entries, but none the directory has qualified for
            // capture - the same reason a fully empty report gets
            skipped = disk_cache_materialize_skip::no_candidates;
        }
    }

    return plan;
}
