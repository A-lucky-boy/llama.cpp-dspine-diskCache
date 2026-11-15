#pragma once

#include "common.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

//
// single-user common-prefix directory
//
// this index only records *where reuse could happen*: it holds immutable token
// and media identities, never a llama_context*, never a server_slot and never a
// mutable token container. it cannot restore anything - a restore still goes
// through server_prompt_cache / server_disk_cache, which re-validate every entry
// (v8 CRCs, media, configuration signature, state restore) before touching a
// context.
//
// the quantities the directory keeps apart:
//   observed    - a prefix was shared by successive requests, nothing more
//   eligible    - long and frequent enough to be worth capturing a state for
//   persisted   - a validated snapshot covers exactly this prefix
//   unavailable - the snapshot this prefix pointed at is gone
// only `persisted` is backed by bytes on disk. `observed` and `eligible` must
// never be reported as cache hits.
//
// every input is validated: malformed media metadata is refused instead of being
// downgraded to a token-only path, and a read-only query never grows the media
// intern table.
//

enum class prefix_state : uint8_t {
    observed = 0,
    eligible,
    persisted,
    unavailable,
};

const char * prefix_state_name(prefix_state state);

// why an observation was not accepted. `invalid_media` and `capacity` are
// refusals, not partial updates: nothing was inserted
enum class prefix_input_status : uint8_t {
    ok = 0,
    invalid_media, // media arrays inconsistent, or a chunk out of range, overlapping or empty
    capacity,      // a new media identity would exceed the index budget
};

const char * prefix_input_status_name(prefix_input_status status);

// one request's immutable prefix view, published at a request boundary. token
// ids and media identities only - no prompt text, no paths
struct prefix_observation {
    uint64_t              request_id = 0;
    llama_tokens          tokens;             // copy, media placeholders included
    std::vector<uint32_t> media_indices;      // start token of each media chunk
    std::vector<uint32_t> media_token_counts;
    std::vector<std::string> media_ids;
};

// a discovered prefix. never an instruction to restore anything
struct prefix_candidate {
    size_t       prefix_tokens = 0;
    uint32_t     observations  = 0;
    prefix_state state         = prefix_state::observed;
    std::string  snapshot_path; // set only for persisted candidates
};

struct prefix_index_stats {
    uint64_t n_nodes          = 0;
    uint64_t n_tokens         = 0; // token/media units held by the trie
    uint64_t n_snapshots      = 0; // live physical references
    uint64_t n_media_ids      = 0; // interned media identities
    uint64_t n_requests       = 0; // distinct requests observed
    uint64_t n_observed       = 0; // prefixes currently in `observed`
    uint64_t n_eligible       = 0; // prefixes currently in `eligible`
    uint64_t n_persisted      = 0; // prefixes currently in `persisted`
    uint64_t n_unavailable    = 0; // prefixes whose snapshot went away
    uint64_t n_invalidated    = 0; // references withdrawn, cumulative
    uint64_t n_evicted        = 0; // nodes dropped by the capacity limits, cumulative
    uint64_t n_rejected       = 0; // capacity refusals, cumulative (at most one per observation)
    uint64_t n_input_rejected = 0; // malformed observations refused, cumulative
    uint64_t n_candidates     = 0; // candidates reported to callers, cumulative
    uint64_t n_observed_events = 0; // nodes created, cumulative
    uint64_t n_eligible_events = 0; // transitions into `eligible`, cumulative
    size_t   bytes            = 0; // approximate retained bytes
};

struct prefix_index_options {
    size_t min_tokens       = 512;
    size_t min_observations = 2;
    size_t max_candidates   = 256; // nodes
    size_t max_index_bytes  = 64ull*1024ull*1024ull;
};

// result of one observation: the accepted candidates plus whether the input was
// usable at all
struct prefix_observation_result {
    prefix_input_status           status = prefix_input_status::ok;
    std::vector<prefix_candidate> candidates;
};

class server_prefix_index {
public:
    server_prefix_index(const std::string & config_sig, const prefix_index_options & opt);

    // publish one request boundary and return the prefixes it shares with
    // earlier requests, longest first. a repeated request_id is ignored, so the
    // input and output boundary of one request are counted once. malformed input
    // is refused as a whole
    prefix_observation_result observe(const prefix_observation & event);

    // query without publishing anything. read-only: an unknown media identity
    // neither matches a stored path nor grows the intern table
    std::vector<prefix_candidate> candidates(const llama_tokens & tokens,
            const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts,
            const std::vector<std::string> & media_ids) const;

    // attach a committed snapshot to the node whose prefix it matches exactly.
    // `epoch` is the disk tier's monotone event sequence: a commit older than a
    // withdrawal of the same path is ignored, so a late notification can never
    // resurrect a deleted snapshot
    void on_snapshot_committed(const std::string & path, uint64_t epoch, const llama_tokens & tokens,
            const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts,
            const std::vector<std::string> & media_ids);
    void on_snapshot_invalidated(const std::string & path, uint64_t epoch);

    prefix_index_stats stats() const;

    const std::string         config_sig;
    const prefix_index_options opt;

private:
    // one token plus the identity of the media chunk it starts (0 for a plain
    // token). identities are interned strings, so comparing two units is an
    // exact comparison - no digest is ever used as proof of equality.
    // k_media_unknown is used by read-only lookups of an identity that was never
    // observed: it matches nothing, and is never stored
    using unit = std::pair<llama_token, uint32_t>;

    static constexpr uint32_t k_media_unknown = UINT32_MAX;

    struct node {
        std::vector<unit>       seg;
        size_t                  tokens_end  = 0;
        int32_t                 parent      = -1;
        std::map<unit, int32_t> children;
        uint32_t                observations = 0;
        int64_t                 last_observed = 0;
        std::string             snapshot_path;
        prefix_state            state = prefix_state::observed;
        bool                    ref_invalidated = false; // a snapshot was withdrawn
        bool                    alive = true;
    };

    struct snapshot_ref {
        size_t   tokens = 0;
        int32_t  node   = -1;
        uint64_t epoch  = 0;
    };

    // the following helpers require mtx to be held
    bool to_units(const llama_tokens & tokens, const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids,
            bool allow_intern, std::vector<unit> & seq, std::vector<size_t> & media_starts) const;
    // media identity = chunk id + token count
    static std::string media_key(const std::string & id, size_t token_count);
    uint32_t lookup_media(const std::string & key) const;
    // const because a read-only query may still resolve an identity that is
    // already known; the tables are mutable and append-only
    uint32_t intern_media(const std::string & id) const;
    int32_t  walk(const std::vector<unit> & seq, std::vector<int32_t> & path,
            size_t * partial = nullptr, uint32_t * partial_observations = nullptr) const;
    void     insert(const std::vector<unit> & seq, bool count_observation);
    void     refresh_state(int32_t idx);
    prefix_state state_for(size_t prefix_tokens, uint32_t observations) const;
    void     note_capacity();
    bool     over_limits() const;
    size_t   approx_bytes() const;
    bool     evict_one(); // true only when a node was really removed
    void     clamp_candidates(std::vector<prefix_candidate> & res, const std::vector<size_t> & media_starts,
            const std::vector<uint32_t> & media_token_counts) const;
    void     compact();
    void     remember_tombstone(const std::string & path, uint64_t epoch);
    uint64_t tombstone_epoch(const std::string & path) const;
    size_t   alive_nodes() const;

    mutable std::mutex mtx;

    std::vector<node> nodes; // nodes[0] is the root and never leaves

    // append-only interning tables; mutable so a read-only query can resolve an
    // existing identity without inserting a new one
    mutable std::unordered_map<std::string, uint32_t> media_intern;
    mutable std::vector<std::string>                  media_names;

    std::unordered_map<std::string, snapshot_ref> snapshots; // by entry path

    // withdrawal epochs, so an out-of-order commit cannot resurrect a snapshot.
    // bounded: the oldest entry is dropped first
    std::unordered_map<std::string, uint64_t> tombstones;
    std::deque<std::string>                   tombstone_order;
    static constexpr size_t                   k_tombstones_max = 512;

    std::unordered_set<uint64_t> seen_requests; // input/output dedup
    std::vector<uint64_t>        seen_order;
    size_t                       seen_pos = 0;

    int64_t            seq_counter = 0;
    prefix_index_stats counters;
};
