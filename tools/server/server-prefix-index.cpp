#include "server-prefix-index.h"

#include <algorithm>

//
// Implementation notes
//
// The trie is a path-compressed radix tree over `unit` = (token id, media
// identity). The media identity is an interned string index, not a hash, so two
// units are equal only when the token id and the media chunk identity are both
// equal - no digest is ever used as proof of equality. A request whose media
// chunk differs at some position therefore stops matching exactly at the first
// token of that chunk, which is the boundary the planning document requires.
//
// Every input is validated before use: a media array whose three parts disagree,
// or a chunk that is empty, out of range or overlapping its predecessor, makes
// the whole observation invalid. There is no "token path only" fallback - that
// would let two different images share a prefix.
//
// Read-only queries resolve media identities without inserting them; an identity
// that was never observed maps to k_media_unknown, which no stored unit can hold.
//
// `nodes` only grows while entries are alive: dead entries are skipped and
// compacted periodically. Every entry point takes `mtx`; no method touches a
// llama_context, a slot or a disk file, and none calls back into the disk tier -
// which is why the disk tier may deliver its notifications while holding its own
// lock without a lock inversion.
//

const char * prefix_state_name(prefix_state state) {
    switch (state) {
        case prefix_state::observed:    return "observed";
        case prefix_state::eligible:    return "eligible";
        case prefix_state::persisted:   return "persisted";
        case prefix_state::unavailable: return "unavailable";
    }

    return "unknown";
}

const char * prefix_input_status_name(prefix_input_status status) {
    switch (status) {
        case prefix_input_status::ok:            return "ok";
        case prefix_input_status::invalid_media: return "invalid_media";
        case prefix_input_status::capacity:      return "capacity";
    }

    return "unknown";
}

server_prefix_index::server_prefix_index(const std::string & config_sig, const prefix_index_options & opt)
    : config_sig(config_sig), opt(opt) {
    node root;
    root.parent = -1;
    nodes.push_back(std::move(root));
}

std::string server_prefix_index::media_key(const std::string & id, size_t token_count) {
    // '#' cannot appear in the placeholder ids produced by the mtmd path, and the
    // length is appended so that (id, count) pairs stay distinct
    return id + "#" + std::to_string(token_count);
}

uint32_t server_prefix_index::lookup_media(const std::string & id) const {
    const auto it = media_intern.find(id);
    return it == media_intern.end() ? 0 : it->second;
}

uint32_t server_prefix_index::intern_media(const std::string & id) const {
    const auto it = media_intern.find(id);
    if (it != media_intern.end()) {
        return it->second;
    }

    const uint32_t ref = (uint32_t) media_names.size() + 1; // 0 means "no media here"
    media_names.push_back(id);
    media_intern.emplace(id, ref);

    return ref;
}

bool server_prefix_index::to_units(const llama_tokens & tokens,
        const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts,
        const std::vector<std::string> & media_ids,
        bool allow_intern,
        std::vector<unit> & seq,
        std::vector<size_t> & media_starts) const {
    seq.assign(tokens.size(), unit(0, 0));
    media_starts.clear();

    for (size_t i = 0; i < tokens.size(); ++i) {
        seq[i].first = tokens[i];
    }

    // fail closed: the three media arrays must describe the same chunks
    if (media_indices.size() != media_token_counts.size() || media_indices.size() != media_ids.size()) {
        return false;
    }

    size_t previous_end = 0;

    for (size_t i = 0; i < media_indices.size(); ++i) {
        const size_t start = media_indices[i];
        const size_t count = media_token_counts[i];

        // empty id, zero length, out of range, or overlapping the previous chunk
        if (media_ids[i].empty() || count == 0 || start < previous_end ||
                start >= tokens.size() || count > tokens.size() - start) {
            return false;
        }

        // the identity is the chunk id *and* its token count: the same id with a
        // different length occupies a different KV range, so it must not share
        const std::string key = media_key(media_ids[i], count);

        uint32_t ref = 0;

        if (allow_intern) {
            ref = intern_media(key);
        } else {
            ref = lookup_media(key);
            if (ref == 0) {
                ref = k_media_unknown;
            }
        }

        seq[start].second = ref;
        media_starts.push_back(start);
        previous_end = start + count;
    }

    return true;
}

int32_t server_prefix_index::walk(const std::vector<unit> & seq, std::vector<int32_t> & path,
        size_t * partial, uint32_t * partial_observations) const {
    int32_t cur = 0;
    size_t  pos = 0;

    if (partial) {
        *partial = 0;
    }
    if (partial_observations) {
        *partial_observations = 0;
    }

    while (pos < seq.size()) {
        const auto it = nodes[cur].children.find(seq[pos]);
        if (it == nodes[cur].children.end()) {
            break;
        }

        const int32_t child = it->second;
        const auto  & seg   = nodes[child].seg;

        size_t k = 0;
        while (k < seg.size() && pos + k < seq.size() && seg[k] == seq[pos + k]) {
            ++k;
        }

        if (k < seg.size()) {
            // divergence inside the edge: the shared prefix ends at pos + k and has
            // no node of its own. every request that reached `child` passed that
            // point, so the edge's observation count applies
            if (partial) {
                *partial = pos + k;
            }
            if (partial_observations) {
                *partial_observations = nodes[child].observations;
            }
            break;
        }

        pos += k;
        cur  = child;
        path.push_back(cur);
    }

    return cur;
}

void server_prefix_index::insert(const std::vector<unit> & seq, bool count_observation) {
    int32_t cur = 0;
    size_t  pos = 0;

    while (pos < seq.size()) {
        const auto it = nodes[cur].children.find(seq[pos]);

        if (it == nodes[cur].children.end()) {
            const int32_t leaf = (int32_t) nodes.size();
            nodes.push_back(node{});
            nodes[leaf].seg.assign(seq.begin() + (ptrdiff_t) pos, seq.end());
            nodes[leaf].tokens_end = seq.size();
            nodes[leaf].parent     = cur;
            nodes[cur].children[seq[pos]] = leaf;

            counters.n_observed_events++;

            cur = leaf;
            break;
        }

        const int32_t child = it->second;
        const size_t  k     = [&]() {
            const auto & seg = nodes[child].seg;
            size_t n = 0;
            while (n < seg.size() && pos + n < seq.size() && seg[n] == seq[pos + n]) {
                ++n;
            }
            return n;
        }();

        if (k == nodes[child].seg.size()) {
            pos += k;
            cur  = child;
            continue;
        }

        // split the edge at k: `mid` takes the shared part, `child` keeps the rest
        const size_t child_end = nodes[child].tokens_end;
        const size_t child_len = nodes[child].seg.size();

        const int32_t mid = (int32_t) nodes.size();
        nodes.push_back(node{});
        counters.n_observed_events++;
        nodes[mid].seg.assign(nodes[child].seg.begin(), nodes[child].seg.begin() + (ptrdiff_t) k);
        nodes[mid].tokens_end    = child_end - child_len + k;
        nodes[mid].parent        = cur;
        nodes[mid].observations  = nodes[child].observations; // whoever reached child passed mid
        nodes[mid].last_observed = nodes[child].last_observed;

        nodes[child].seg.erase(nodes[child].seg.begin(), nodes[child].seg.begin() + (ptrdiff_t) k);
        nodes[child].parent = mid;
        nodes[mid].children[nodes[child].seg[0]] = child;

        nodes[cur].children[seq[pos]] = mid;

        cur  = mid;
        pos += k;

        if (pos < seq.size()) {
            const int32_t leaf = (int32_t) nodes.size();
            nodes.push_back(node{});
            nodes[leaf].seg.assign(seq.begin() + (ptrdiff_t) pos, seq.end());
            nodes[leaf].tokens_end = seq.size();
            nodes[leaf].parent     = cur;
            nodes[cur].children[seq[pos]] = leaf;

            counters.n_observed_events++;

            cur = leaf;
        }
        break;
    }

    if (!count_observation) {
        return;
    }

    // bump every node on the path. walking again after the insert is cheap
    // (bounded depth) and avoids tracking the splits above
    const int64_t now = ++seq_counter;

    int32_t idx = 0;
    size_t  at  = 0;

    while (at < seq.size()) {
        const auto it = nodes[idx].children.find(seq[at]);
        if (it == nodes[idx].children.end()) {
            break;
        }

        idx = it->second;
        at  = nodes[idx].tokens_end;

        nodes[idx].observations += 1;
        nodes[idx].last_observed = now;
        nodes[idx].ref_invalidated = false;
        refresh_state(idx);
    }
}

prefix_state server_prefix_index::state_for(size_t prefix_tokens, uint32_t observations) const {
    return (prefix_tokens >= opt.min_tokens && observations >= opt.min_observations)
        ? prefix_state::eligible
        : prefix_state::observed;
}

void server_prefix_index::refresh_state(int32_t idx) {
    node & n = nodes[idx];

    prefix_state next;

    if (!n.snapshot_path.empty()) {
        next = prefix_state::persisted;
    } else if (n.ref_invalidated) {
        // the prefix is still known, but the snapshot it pointed at is gone
        next = prefix_state::unavailable;
    } else {
        next = state_for(n.tokens_end, n.observations);
    }

    // cumulative transition counter: a node that stays eligible is not counted
    // again on every request that shares the prefix. entering `observed` is
    // counted at node creation instead - a new node starts in that state
    if (next != n.state && next == prefix_state::eligible) {
        counters.n_eligible_events++;
    }

    n.state = next;
}

size_t server_prefix_index::alive_nodes() const {
    size_t res = 0;
    for (const auto & n : nodes) {
        res += n.alive ? 1 : 0;
    }
    return res;
}

size_t server_prefix_index::approx_bytes() const {
    size_t res = 0;

    for (const auto & n : nodes) {
        if (!n.alive) {
            continue;
        }
        res += sizeof(node);
        res += n.seg.size() * sizeof(unit);
        res += n.children.size() * (sizeof(unit) + sizeof(int32_t) + 2 * sizeof(void *));
        res += n.snapshot_path.size();
    }

    for (const auto & m : media_names) {
        res += sizeof(std::string) + m.size();
    }
    for (const auto & kv : media_intern) {
        res += sizeof(kv) + kv.first.size() + kv.first.capacity();
    }

    for (const auto & kv : snapshots) {
        res += sizeof(kv) + kv.first.size();
    }

    for (const auto & kv : tombstones) {
        res += sizeof(kv) + kv.first.size();
    }

    res += seen_requests.size() * (sizeof(uint64_t) + 2 * sizeof(void *));
    res += seen_order.capacity() * sizeof(uint64_t);

    return res;
}

bool server_prefix_index::over_limits() const {
    return alive_nodes() > opt.max_candidates || approx_bytes() > opt.max_index_bytes;
}

bool server_prefix_index::evict_one() {
    int32_t best = -1;

    for (size_t i = 1; i < nodes.size(); ++i) {
        const auto & n = nodes[i];
        if (!n.alive || !n.children.empty() || !n.snapshot_path.empty()) {
            continue; // only leaves without a physical reference are droppable
        }

        if (best < 0 || n.observations < nodes[best].observations ||
                (n.observations == nodes[best].observations && n.last_observed < nodes[best].last_observed)) {
            best = (int32_t) i;
        }
    }

    if (best < 0) {
        return false; // nothing droppable; the caller counts the refusal once
    }

    if (nodes[best].parent >= 0) {
        nodes[nodes[best].parent].children.erase(nodes[best].seg[0]);
    }

    nodes[best].alive = false;
    nodes[best].seg.clear();
    nodes[best].seg.shrink_to_fit();
    nodes[best].children.clear();
    counters.n_evicted++;

    return true;
}

void server_prefix_index::note_capacity() {
    // terminates: evict_one() only reports success when a node was really
    // removed, so a directory whose remaining leaves are all protected by a
    // physical reference stops after a single refusal instead of counting
    // "rejected" as progress and looping forever
    while (over_limits()) {
        if (!evict_one()) {
            counters.n_rejected++;
            return;
        }
    }
}

void server_prefix_index::compact() {
    size_t dead = 0;
    for (const auto & n : nodes) {
        dead += n.alive ? 0 : 1;
    }

    if (dead < 32 || dead * 2 < nodes.size()) {
        return;
    }

    std::vector<int32_t> remap(nodes.size(), -1);
    std::vector<node>    kept;
    kept.reserve(nodes.size() - dead);

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].alive) {
            continue;
        }
        remap[i] = (int32_t) kept.size();
        kept.push_back(std::move(nodes[i]));
    }

    for (auto & n : kept) {
        n.parent = n.parent >= 0 ? remap[n.parent] : -1;
        for (auto & kv : n.children) {
            kv.second = remap[kv.second];
        }
    }

    for (auto & kv : snapshots) {
        kv.second.node = kv.second.node >= 0 ? remap[kv.second.node] : -1;
    }

    nodes = std::move(kept);
}

void server_prefix_index::remember_tombstone(const std::string & path, uint64_t epoch) {
    const auto it = tombstones.find(path);
    if (it != tombstones.end()) {
        it->second = std::max(it->second, epoch);
        return;
    }

    if (tombstone_order.size() >= k_tombstones_max) {
        tombstones.erase(tombstone_order.front());
        tombstone_order.pop_front();
    }

    tombstones.emplace(path, epoch);
    tombstone_order.push_back(path);
}

uint64_t server_prefix_index::tombstone_epoch(const std::string & path) const {
    const auto it = tombstones.find(path);
    return it == tombstones.end() ? 0 : it->second;
}

// clamp a candidate to the start of a media chunk it would end inside: a prefix
// that stops in the middle of an image or an audio chunk is not a usable
// boundary. a clamped candidate no longer matches the end point of the snapshot
// it may have pointed at, so its physical reference is dropped and its state is
// recomputed from the shortened length
void server_prefix_index::clamp_candidates(std::vector<prefix_candidate> & res,
        const std::vector<size_t> & media_starts,
        const std::vector<uint32_t> & media_token_counts) const {
    for (auto & c : res) {
        bool clamped = false;

        for (size_t i = 0; i < media_starts.size() && i < media_token_counts.size(); ++i) {
            const size_t start = media_starts[i];
            const size_t end   = start + media_token_counts[i];
            if (c.prefix_tokens > start && c.prefix_tokens < end) {
                c.prefix_tokens = start;
                clamped = true;
                break;
            }
        }

        if (clamped) {
            c.snapshot_path.clear();
            c.state = state_for(c.prefix_tokens, c.observations);
        }
    }

    std::sort(res.begin(), res.end(), [](const prefix_candidate & a, const prefix_candidate & b) {
        return a.prefix_tokens > b.prefix_tokens;
    });

    res.erase(std::unique(res.begin(), res.end(), [](const prefix_candidate & a, const prefix_candidate & b) {
        return a.prefix_tokens == b.prefix_tokens;
    }), res.end());

    res.erase(std::remove_if(res.begin(), res.end(), [](const prefix_candidate & c) {
        return c.prefix_tokens == 0;
    }), res.end());
}

prefix_observation_result server_prefix_index::observe(const prefix_observation & event) {
    std::lock_guard<std::mutex> lock(mtx);

    prefix_observation_result result;

    if (event.tokens.empty()) {
        return result;
    }

    // validate before touching anything: a malformed observation must not create
    // nodes, intern identities or consume a request id
    std::vector<unit> seq;
    std::vector<size_t> media_starts;

    // budget the identities this observation would add *before* interning them,
    // so a stream of new images cannot grow the table past the index budget
    size_t projected = approx_bytes();
    for (const auto & id : event.media_ids) {
        if (!id.empty() && media_intern.find(id) == media_intern.end()) {
            projected += id.size() + sizeof(std::string) + 64;
        }
    }
    if (projected > opt.max_index_bytes) {
        counters.n_rejected++;
        result.status = prefix_input_status::capacity;
        return result;
    }

    if (!to_units(event.tokens, event.media_indices, event.media_token_counts, event.media_ids,
                /*allow_intern=*/true, seq, media_starts)) {
        counters.n_input_rejected++;
        result.status = prefix_input_status::invalid_media;
        return result;
    }

    // the input and the output boundary of one request are one observation
    if (event.request_id != 0) {
        if (seen_requests.count(event.request_id) != 0) {
            return result;
        }

        if (seen_order.size() < 4096) {
            seen_order.push_back(event.request_id);
            seen_requests.insert(event.request_id);
        } else {
            seen_requests.erase(seen_order[seen_pos]);
            seen_order[seen_pos] = event.request_id;
            seen_requests.insert(event.request_id);
            seen_pos = (seen_pos + 1) % seen_order.size();
        }
    }

    counters.n_requests++;

    // candidates first: the request must not be compared with itself
    {
        std::vector<int32_t> path;
        size_t   partial     = 0;
        uint32_t partial_obs = 0;
        walk(seq, path, &partial, &partial_obs);

        for (const int32_t idx : path) {
            const auto & n = nodes[idx];

            prefix_candidate c;
            c.prefix_tokens = n.tokens_end;
            c.observations  = n.observations;
            c.state         = n.state;
            c.snapshot_path = n.snapshot_path;

            result.candidates.push_back(std::move(c));
        }

        // a prefix that ends inside an edge has no node and no snapshot: report
        // it as metadata only, never as persisted
        if (partial > 0) {
            prefix_candidate c;
            c.prefix_tokens = partial;
            c.observations  = partial_obs;
            c.state         = state_for(partial, partial_obs);
            result.candidates.push_back(std::move(c));
        }

        clamp_candidates(result.candidates, media_starts, event.media_token_counts);
    }

    insert(seq, true);
    note_capacity();
    compact();

    counters.n_candidates += result.candidates.size();

    return result;
}

std::vector<prefix_candidate> server_prefix_index::candidates(const llama_tokens & tokens,
        const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts,
        const std::vector<std::string> & media_ids) const {
    std::lock_guard<std::mutex> lock(mtx);

    std::vector<prefix_candidate> res;

    if (tokens.empty()) {
        return res;
    }

    std::vector<unit> seq;
    std::vector<size_t> media_starts;

    // read-only: an identity that was never observed maps to k_media_unknown and
    // matches nothing, and the intern table does not grow
    if (!to_units(tokens, media_indices, media_token_counts, media_ids,
                /*allow_intern=*/false, seq, media_starts)) {
        return res;
    }

    std::vector<int32_t> path;
    size_t   partial     = 0;
    uint32_t partial_obs = 0;
    walk(seq, path, &partial, &partial_obs);

    for (const int32_t idx : path) {
        const auto & n = nodes[idx];

        prefix_candidate c;
        c.prefix_tokens = n.tokens_end;
        c.observations  = n.observations;
        c.state         = n.state;
        c.snapshot_path = n.snapshot_path;
        res.push_back(std::move(c));
    }

    if (partial > 0) {
        prefix_candidate c;
        c.prefix_tokens = partial;
        c.observations  = partial_obs;
        c.state         = state_for(partial, partial_obs);
        res.push_back(std::move(c));
    }

    clamp_candidates(res, media_starts, media_token_counts);

    return res;
}

void server_prefix_index::on_snapshot_committed(const std::string & path, uint64_t epoch, const llama_tokens & tokens,
        const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts,
        const std::vector<std::string> & media_ids) {
    if (path.empty() || tokens.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx);

    // a commit that is older than a withdrawal of the same path describes a file
    // that no longer exists: ignore it instead of reviving a persisted ref
    if (epoch != 0 && epoch <= tombstone_epoch(path)) {
        return;
    }

    std::vector<unit> seq;
    std::vector<size_t> media_starts;

    if (!to_units(tokens, media_indices, media_token_counts, media_ids,
                /*allow_intern=*/true, seq, media_starts)) {
        counters.n_input_rejected++;
        return;
    }

    // a snapshot is attached to the node whose prefix it matches exactly: the
    // snapshot of A+B never marks A as persisted
    insert(seq, false);

    std::vector<int32_t> path_nodes;
    walk(seq, path_nodes);

    if (path_nodes.empty()) {
        return;
    }

    const int32_t idx = path_nodes.back();
    if (nodes[idx].tokens_end != seq.size()) {
        return;
    }

    // attach the reference *before* the capacity pass: a referenced node is never
    // evicted, and running note_capacity() first could drop the node this commit
    // is about to protect
    nodes[idx].snapshot_path   = path;
    nodes[idx].ref_invalidated = false;
    refresh_state(idx);

    snapshot_ref ref;
    ref.tokens = seq.size();
    ref.node   = idx;
    ref.epoch  = epoch;
    snapshots[path] = ref;

    note_capacity();
    compact();
}

void server_prefix_index::on_snapshot_invalidated(const std::string & path, uint64_t epoch) {
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx);

    // remember the withdrawal even when this process never saw the commit: a
    // late commit event must not be able to attach it afterwards
    remember_tombstone(path, epoch);

    const auto it = snapshots.find(path);
    if (it == snapshots.end()) {
        return;
    }

    if (it->second.node >= 0 && it->second.node < (int32_t) nodes.size() &&
            nodes[it->second.node].alive && nodes[it->second.node].snapshot_path == path) {
        nodes[it->second.node].snapshot_path.clear();
        nodes[it->second.node].ref_invalidated = true;
        refresh_state(it->second.node);
    }

    snapshots.erase(it);
    counters.n_invalidated++;
}

prefix_index_stats server_prefix_index::stats() const {
    std::lock_guard<std::mutex> lock(mtx);

    prefix_index_stats out = counters;

    for (const auto & n : nodes) {
        if (!n.alive || n.parent < 0) {
            continue;
        }

        out.n_nodes++;
        out.n_tokens += n.seg.size();

        switch (n.state) {
            case prefix_state::observed:    out.n_observed++;    break;
            case prefix_state::eligible:    out.n_eligible++;    break;
            case prefix_state::persisted:   out.n_persisted++;   break;
            case prefix_state::unavailable: out.n_unavailable++; break;
        }
    }

    out.n_snapshots = snapshots.size();
    out.n_media_ids = media_names.size();
    out.bytes       = approx_bytes();

    return out;
}
