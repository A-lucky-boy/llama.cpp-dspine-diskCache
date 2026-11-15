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
};

const char * disk_cache_materialize_skip_name(disk_cache_materialize_skip reason);

// the request-scoped capture plan
struct disk_cache_materialize_plan {
    bool   capture = false;
    size_t prefix_tokens = 0; // the position the prefill batch must stop at
};

// inputs describing this request, all of them already known before prefill starts
struct disk_cache_materialize_request {
    bool   enabled             = false;
    size_t min_tokens          = 0; // the disk tier's own minimum, not the directory's
    size_t n_input_tokens      = 0; // tokens the task wants to process
    int64_t n_past             = -1; // how much of the prompt is already in the KV
    bool   has_media           = false;
    size_t media_chunk_start   = 0; // start token of the first media chunk, if any
    size_t media_chunk_end     = 0; // one past its end
};

// pick the longest eligible candidate that is both long enough for the disk tier
// and reachable in this request. `skipped` explains an empty plan
disk_cache_materialize_plan disk_cache_plan_materialize(const disk_cache_materialize_request & req,
        const std::vector<disk_cache_materialize_candidate> & candidates,
        disk_cache_materialize_skip & skipped);
