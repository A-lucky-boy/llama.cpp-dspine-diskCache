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

disk_cache_capture_bounds disk_cache_check_capture_position_bounds(int64_t expected_position_end,
        int64_t target_position_end, bool has_draft, int64_t draft_position_end) {
    // every argument is a position. comparing them to each other is what makes
    // this correct for prefixes whose token count and position span differ
    if (expected_position_end <= 0) {
        // a prefix with no positions has no state to capture
        return disk_cache_capture_bounds::target_short;
    }

    // a negative endpoint means the sequence holds nothing
    if (target_position_end < 0 || target_position_end < expected_position_end) {
        return disk_cache_capture_bounds::target_short;
    }

    if (target_position_end > expected_position_end) {
        return disk_cache_capture_bounds::target_past;
    }

    // the target sits exactly on the prefix. without a draft that is the whole
    // question
    if (!has_draft) {
        return disk_cache_capture_bounds::exact;
    }

    if (draft_position_end < 0) {
        return disk_cache_capture_bounds::draft_missing;
    }

    if (draft_position_end < expected_position_end) {
        return disk_cache_capture_bounds::draft_short;
    }

    if (draft_position_end > expected_position_end) {
        // the draft ran ahead. the only ways to fix that here would be to rewind
        // it or to slice its opaque state, and neither is allowed for a state that
        // is part of a live request
        return disk_cache_capture_bounds::draft_past;
    }

    return disk_cache_capture_bounds::exact;
}

int64_t disk_cache_prefix_pos_max_text(size_t prefix_tokens) {
    if (prefix_tokens == 0) {
        return -1;
    }

    return (int64_t) prefix_tokens - 1;
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

const char * disk_cache_materialize_action_name(disk_cache_materialize_action action) {
    switch (action) {
        case disk_cache_materialize_action::wait_next_batch: return "wait_next_batch";
        case disk_cache_materialize_action::export_state:    return "export_state";
        case disk_cache_materialize_action::skip_export:     return "skip_export";
        case disk_cache_materialize_action::cancel_plan:     return "cancel_plan";
    }
    return "unknown";
}

disk_cache_materialize_action disk_cache_decide_materialize_action(
        const disk_cache_materialize_decision_input & in) {
    // a plan that is not live, or belongs to a request that has moved on, must
    // never be acted on: the slot may already hold another task's state
    if (!in.armed || in.done) {
        return disk_cache_materialize_action::cancel_plan;
    }

    if (!in.task_matches) {
        return disk_cache_materialize_action::cancel_plan;
    }

    // the batch that reaches the prefix has not been decoded yet. this is the case
    // that was once turned into a permanent failure: the view or the target is
    // simply not there yet, and the next batch will bring it
    if (in.tokens_filled < in.prefix_tokens) {
        return disk_cache_materialize_action::wait_next_batch;
    }

    // positions compared to positions, and the two contexts are judged separately -
    // the same verdict means something different depending on who is behind.
    //
    // the target is what decides whether this batch is the one. If it has not
    // reached the prefix, the batch that does has not been decoded yet: that is a
    // wait, and nothing has failed.
    if (in.expected_position_end <= 0 || in.target_position_end < in.expected_position_end) {
        return disk_cache_materialize_action::wait_next_batch;
    }

    // past the prefix, which cannot be undone: the state can only be exported as a
    // longer prefix, so the optional capture ends here rather than producing a
    // snapshot that does not describe what it claims
    if (in.target_position_end > in.expected_position_end) {
        return disk_cache_materialize_action::skip_export;
    }

    // The target sits exactly on the prefix, so this batch is the one that reaches
    // it. Whatever the draft is doing now, it will not get another chance within
    // this capture: the next batch moves past the prefix. A draft that disagrees -
    // behind, ahead, or holding nothing - therefore cannot be paired up, and a
    // snapshot from two disagreeing contexts would describe a prompt that was never
    // executed.
    if (in.has_draft && in.draft_position_end != in.expected_position_end) {
        return disk_cache_materialize_action::skip_export;
    }

    // the state is capturable. whether exporting it is worth it is the disk tier's
    // answer, and only its "nothing to gain" outcomes stop the export
    switch (in.preflight) {
        case disk_cache_preflight_verdict::export_needed:
            return disk_cache_materialize_action::export_state;

        case disk_cache_preflight_verdict::skip:
        case disk_cache_preflight_verdict::conflict:
        case disk_cache_preflight_verdict::invalid:
            return disk_cache_materialize_action::skip_export;
    }

    return disk_cache_materialize_action::skip_export;
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
        case disk_cache_materialize_skip::media_state_unverified: return "media_state_unverified";
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

    // a request that carries media gets the full read-only treatment above - the
    // candidate is selected, its length and reachability are checked, and its end
    // is verified not to split a chunk. what it does not get is the capture: the
    // sequence state of a multimodal prefix has never been verified to export and
    // restore correctly, and none of the checks above can establish that. putting
    // the gate here, at the point a capture would be armed, keeps the reasons
    // above meaningful for media requests
    if (plan.capture && !req.media_chunks.empty()) {
        plan.capture       = false;
        plan.prefix_tokens = 0;
        skipped = disk_cache_materialize_skip::media_state_unverified;
        return plan;
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
