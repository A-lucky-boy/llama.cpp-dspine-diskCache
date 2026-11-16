#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

//
// choosing the prefix to materialize
//
// The directory says a prefix is worth capturing - it has been observed often
// enough. That is not the same as "there is a snapshot of it on disk", and it is
// not yet a plan: the request must also be able to stop at that exact position.
//
// This module only decides *which* prefix a request should try to capture, and
// *why not* when the answer is nothing. It takes no lock, touches no file, knows
// nothing about llama_context. The caller reads the live positions and performs
// the capture; everything here is decided from metadata that is already
// request-scoped.
//

// a candidate as the directory reports it. only the fields this decision needs
struct disk_cache_materialize_candidate {
    size_t   prefix_tokens = 0;
    uint32_t observations  = 0;
    // the directory considers this prefix worth capturing
    bool     eligible      = false;
    // true when this exact prefix already has a live physical reference. such a
    // candidate is skipped: the point is to create a snapshot that does not exist
    bool     persisted     = false;
};

// why a request will not materialize anything. the caller reports these apart
// from real cache hits
enum class disk_cache_materialize_skip : uint8_t {
    none = 0,
    disabled,        // the feature is off
    no_candidates,   // the directory knows nothing about this prompt
    too_short,       // no eligible candidate reaches the disk minimum
    already_persisted, // every eligible candidate already has a snapshot
    beyond_input,    // the chosen prefix is longer than this request
    media,           // the prefix end falls inside a media chunk
    past,            // the KV already moved past the position
    duplicate,       // this request already scheduled one
    // this request carries media blocks, and the sequence state of a multimodal
    // prefix has not been verified to export and restore correctly. the request
    // is still planned and observed; it is simply not captured
    media_state_unverified,
};

const char * disk_cache_materialize_skip_name(disk_cache_materialize_skip reason);

// the request-scoped capture plan
struct disk_cache_materialize_plan {
    bool   capture = false;
    size_t prefix_tokens = 0; // the position the prefill batch must stop at
};

// one media chunk, as a half-open token range [start, end)
struct disk_cache_media_chunk {
    size_t start = 0;
    size_t end   = 0;
};

// true when token index `k` falls strictly inside one of the chunks.
//
// The endpoints are deliberately *not* inside a chunk: a chunk occupies its whole
// range at once, so a prefix that ends exactly where a chunk starts, or exactly
// where one ends, still describes a complete state over the tokens before it.
// Only a prefix ending between a chunk's first and last token has no boundary to
// stop at.
//
// `chunks` must be non-overlapping and ordered; the caller validates that first.
bool disk_cache_prefix_in_media(size_t k, const std::vector<disk_cache_media_chunk> & chunks);

// structural validation of the chunk list taken from the request. A malformed
// list must refuse materialization rather than be treated as "no media": the
// alternative would be capturing a prefix whose media boundary is unknown
bool disk_cache_media_chunks_valid(const std::vector<disk_cache_media_chunk> & chunks, size_t n_tokens_total);

// inputs describing this request, all of them already known before prefill starts
struct disk_cache_materialize_request {
    bool   enabled             = false;
    size_t min_tokens          = 0; // the disk tier's own minimum, not the directory's
    size_t n_input_tokens      = 0; // tokens the task wants to process
    int64_t n_past             = -1; // how much of the prompt is already in the KV
    // the request's real media chunks. an ordered, non-overlapping list, not one
    // span from the first to the last: a prefix ending between two chunks is a
    // legal boundary and must not be refused
    std::vector<disk_cache_media_chunk> media_chunks;
};

// pick the longest eligible candidate that is both long enough for the disk tier
// and reachable in this request. `skipped` explains an empty plan
disk_cache_materialize_plan disk_cache_plan_materialize(const disk_cache_materialize_request & req,
        const std::vector<disk_cache_materialize_candidate> & candidates,
        disk_cache_materialize_skip & skipped);

//
// where a capture stands, in terms of the two contexts' real sequence endpoints
//
// Everything here is a **position**, not a token count. The two are the same
// number for text only: a media chunk occupies more positions than the number of
// tokens it stands for (`mtmd_input_chunk_get_n_pos()` vs
// `mtmd_input_chunk_get_n_tokens()`), so for a prefix that contains one, the
// position its KV ends at is not its token count.
//
// Each endpoint is "one past the highest position the sequence holds", which is
// what `llama_memory_seq_pos_max() + 1` gives. The expected endpoint is
// `server_tokens::pos_next(prefix_tokens)`, again a position.
//
enum class disk_cache_capture_bounds : uint8_t {
    exact = 0,     // both endpoints sit on the prefix: this is the only exportable state
    target_short,  // the target has not reached the prefix yet
    target_past,   // the target has moved beyond it
    draft_short,   // the draft lags the target
    draft_past,    // the draft has moved beyond it
    draft_missing, // a draft context is configured but holds no positions
};

const char * disk_cache_capture_bounds_name(disk_cache_capture_bounds bounds);

// A snapshot pairs the target and the draft state, so both must end exactly on
// the prefix. "the batch was cut at K tokens" makes this true for the target but
// says nothing about the draft: speculative decoding advances it on its own path,
// and a state copied across two contexts that disagree would describe a prompt
// that was never executed.
//
// All three numbers are positions. `expected_position_end` comes from
// `server_tokens::pos_next(prefix_tokens)`; passing a token count here would
// compare two different units and silently misjudge every multimodal prefix.
//
// `has_draft == false` keeps the target-only path: a deployment without a draft
// model has one context to check.
disk_cache_capture_bounds disk_cache_check_capture_position_bounds(int64_t expected_position_end,
        int64_t target_position_end, bool has_draft, int64_t draft_position_end);

// select the checkpoints that may travel with a prefix snapshot.
//
// A checkpoint covers positions [pos_min, pos_max] and `n_tokens` tokens. The
// snapshot has two bounds, and they are not the same number: it holds
// `prefix_tokens` tokens, and its KV ends at position `prefix_pos_max`.
//
// With text only, positions and token indices coincide up to the off-by-one:
// `prefix_tokens` tokens occupy positions [0, prefix_tokens), so the highest
// covered position is `prefix_tokens - 1`. Comparing a checkpoint's `pos_max`
// against `prefix_tokens` therefore admits one position too many - a checkpoint
// whose KV the entry cannot account for.
//
// With media chunks the two bounds diverge, since a chunk occupies more positions
// than its token count. Each field is checked against its own bound, and the
// position bound is a real verified endpoint - never derived from the token
// count, which is only valid for text.
struct disk_cache_checkpoint_view {
    int64_t pos_min  = 0;
    int64_t pos_max  = 0;
    int64_t n_tokens = 0;
};

bool disk_cache_checkpoint_fits(const disk_cache_checkpoint_view & ckpt,
        size_t prefix_tokens, int64_t prefix_pos_max);

// The position a text-only prefix of `prefix_tokens` tokens ends at, i.e.
// `prefix_tokens - 1` (and -1 for an empty prefix).
//
// This is the text-only identity `pos_next(N) == N`. It is kept for unit tests
// that pin that identity down and must NOT be used as the position bound for a
// request that may contain media: there the two numbers differ, and the caller
// has the verified endpoint from the live memory instead.
int64_t disk_cache_prefix_pos_max_text(size_t prefix_tokens);

