#pragma once

#include "common.h"

#include <cstdint>
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

enum class prefix_state : uint8_t {
    observed = 0,
    eligible,
    persisted,
    unavailable,
};

const char * prefix_state_name(prefix_state state);

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
    uint64_t n_nodes       = 0;
    uint64_t n_tokens      = 0; // token/media units held by the trie
    uint64_t n_snapshots   = 0; // live physical references
    uint64_t n_requests    = 0; // distinct requests observed
    uint64_t n_observed    = 0; // prefixes currently in `observed`
    uint64_t n_eligible    = 0; // prefixes currently in `eligible`
    uint64_t n_persisted   = 0; // prefixes currently in `persisted`
    uint64_t n_unavailable = 0; // prefixes whose snapshot went away
    uint64_t n_invalidated = 0; // references withdrawn, cumulative
    uint64_t n_evicted     = 0; // nodes dropped by the capacity limits, cumulative
    uint64_t n_rejected    = 0; // observations refused by the capacity limits, cumulative
    uint64_t n_candidates  = 0; // candidates reported to callers, cumulative
    uint64_t n_observed_events = 0; // transitions into `observed`, cumulative
    uint64_t n_eligible_events = 0; // transitions into `eligible`, cumulative
    size_t   bytes         = 0; // approximate retained bytes
};

struct prefix_index_options {
    size_t min_tokens       = 512;
    size_t min_observations = 2;
    size_t max_candidates   = 256; // nodes
    size_t max_index_bytes  = 64ull*1024ull*1024ull;
};

class server_prefix_index {
public:
    server_prefix_index(const std::string & config_sig, const prefix_index_options & opt);

    // publish one request boundary and return the prefixes it shares with
    // earlier requests, longest first. a repeated request_id is ignored, so the
    // input and the output boundary of one request count once
    std::vector<prefix_candidate> observe(const prefix_observation & event);

    // query without publishing anything (diagnostics / tests)
    std::vector<prefix_candidate> candidates(const llama_tokens & tokens,
            const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts,
            const std::vector<std::string> & media_ids) const;

    // attach a committed snapshot to the node whose prefix it matches exactly.
    // the caller must have validated and committed the file already
    void on_snapshot_committed(const std::string & path, const llama_tokens & tokens,
            const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts,
            const std::vector<std::string> & media_ids);
    void on_snapshot_invalidated(const std::string & path);

    prefix_index_stats stats() const;

    const std::string         config_sig;
    const prefix_index_options opt;

private:
    // one token plus the identity of the media chunk it starts (0 for a plain
    // token). identities are interned strings, so comparing two units is an
    // exact comparison - no digest is ever used as proof of equality
    using unit = std::pair<llama_token, uint32_t>;

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
        size_t  tokens = 0;
        int32_t node   = -1;
    };

    // the following helpers require mtx to be held
    std::vector<unit> to_units(const llama_tokens & tokens, const std::vector<uint32_t> & media_indices,
            const std::vector<uint32_t> & media_token_counts, const std::vector<std::string> & media_ids,
            std::vector<size_t> & media_starts) const;
    uint32_t intern_media(const std::string & id) const;
    // walk the request along the trie. `path` receives every node whose edge was
    // matched completely; `partial` receives the length of the prefix shared
    // inside the edge that diverged (0 when the walk reached a node boundary or
    // had no edge to follow), and `partial_observations` the observation count of
    // that edge, which is the number of requests that ever reached it
    int32_t  walk(const std::vector<unit> & seq, std::vector<int32_t> & path,
            size_t * partial = nullptr, uint32_t * partial_observations = nullptr) const;
    void     insert(const std::vector<unit> & seq, bool count_observation);
    void     refresh_state(int32_t idx);
    void     note_capacity();
    size_t   approx_bytes() const;
    void     evict_one();
    void     compact();

    mutable std::mutex mtx;

    std::vector<node> nodes; // nodes[0] is the root and never leaves

    // append-only interning tables; mutable so a read-only query can still
    // resolve a new identity (it simply matches nothing)
    mutable std::unordered_map<std::string, uint32_t> media_intern;
    mutable std::vector<std::string>                  media_names;

    std::unordered_map<std::string, snapshot_ref> snapshots; // by entry path

    std::unordered_set<uint64_t> seen_requests; // input/output dedup
    std::vector<uint64_t>        seen_order;
    size_t                       seen_pos = 0;

    int64_t            seq_counter = 0;
    prefix_index_stats counters;
};
