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
        // boundary to stop at
        if (req.has_media && req.media_chunk_end > req.media_chunk_start &&
                c.prefix_tokens > req.media_chunk_start && c.prefix_tokens < req.media_chunk_end) {
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
