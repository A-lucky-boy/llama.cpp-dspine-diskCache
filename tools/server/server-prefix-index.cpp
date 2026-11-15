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
// `nodes` only grows while entries are alive: dead entries are skipped and
// compacted periodically. Every entry point takes `mtx`; no method touches a
// llama_context, a slot or a disk file.
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

server_prefix_index::server_prefix_index(const std::string & config_sig, const prefix_index_options & opt)
    : config_sig(config_sig), opt(opt) {
    node root;
    root.parent = -1;
    nodes.push_back(std::move(root));
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

std::vector<server_prefix_index::unit> server_prefix_index::to_units(const llama_tokens & tokens,
        const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts,
        const std::vector<std::string> & media_ids,
        std::vector<size_t> & media_starts) const {
    std::vector<unit> seq(tokens.size(), unit(0, 0));

    for (size_t i = 0; i < tokens.size(); ++i) {
        seq[i].first = tokens[i];
    }

    media_starts.clear();

    if (media_indices.size() != media_token_counts.size() || media_indices.size() != media_ids.size()) {
        return seq; // inconsistent metadata: identity is unknown, token path only
    }

    for (size_t i = 0; i < media_indices.size(); ++i) {
        const size_t start = media_indices[i];
        const size_t count = media_token_counts[i];

        if (count == 0 || start >= tokens.size() || count > tokens.size() - start || media_ids[i].empty()) {
            continue;
        }

        seq[start].second = intern_media(media_ids[i]);
        media_starts.push_back(start);
    }

    return seq;
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

void server_prefix_index::refresh_state(int32_t idx) {
    node & n = nodes[idx];

    prefix_state next;

    if (!n.snapshot_path.empty()) {
        next = prefix_state::persisted;
    } else if (n.ref_invalidated) {
        // the prefix is still known, but the snapshot it pointed at is gone
        next = prefix_state::unavailable;
    } else {
        next = (n.tokens_end >= opt.min_tokens && n.observations >= opt.min_observations)
            ? prefix_state::eligible
            : prefix_state::observed;
    }

    // cumulative transition counter: a node that stays eligible is not counted
    // again on every request that shares the prefix. entering `observed` is
    // counted at node creation instead - a new node starts in that state
    if (next != n.state && next == prefix_state::eligible) {
        counters.n_eligible_events++;
    }

    n.state = next;
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

    return res;
}

void server_prefix_index::evict_one() {
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
        counters.n_rejected++;
        return;
    }

    if (nodes[best].parent >= 0) {
        nodes[nodes[best].parent].children.erase(nodes[best].seg[0]);
    }

    nodes[best].alive = false;
    nodes[best].seg.clear();
    nodes[best].seg.shrink_to_fit();
    nodes[best].children.clear();
    counters.n_evicted++;
}

void server_prefix_index::note_capacity() {
    for (;;) {
        size_t alive = 0;
        for (const auto & n : nodes) {
            alive += n.alive ? 1 : 0;
        }

        if (alive <= opt.max_candidates && approx_bytes() <= opt.max_index_bytes) {
            return;
        }

        const uint64_t before = counters.n_evicted + counters.n_rejected;
        evict_one();
        if (counters.n_evicted + counters.n_rejected == before) {
            return; // everything left is either the root or a persisted node
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

// clamp a candidate to the start of a media chunk it would end inside: a prefix
// that stops in the middle of an image or an audio chunk is not a usable
// boundary
static void clamp_candidates(std::vector<prefix_candidate> & res, const std::vector<size_t> & media_starts,
        const std::vector<uint32_t> & media_token_counts) {
    for (auto & c : res) {
        for (size_t i = 0; i < media_starts.size() && i < media_token_counts.size(); ++i) {
            const size_t start = media_starts[i];
            const size_t end   = start + media_token_counts[i];
            if (c.prefix_tokens > start && c.prefix_tokens < end) {
                c.prefix_tokens = start;
                break;
            }
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

std::vector<prefix_candidate> server_prefix_index::observe(const prefix_observation & event) {
    std::lock_guard<std::mutex> lock(mtx);

    if (event.tokens.empty()) {
        return {};
    }

    // the input and the output boundary of one request are one observation
    if (event.request_id != 0) {
        if (seen_requests.count(event.request_id) != 0) {
            return {};
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

    std::vector<size_t> media_starts;
    const std::vector<unit> seq = to_units(event.tokens, event.media_indices, event.media_token_counts,
            event.media_ids, media_starts);

    // candidates first: the request must not be compared with itself
    std::vector<prefix_candidate> res;

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

            res.push_back(std::move(c));
        }

        // a prefix that ends inside an edge has no node and no snapshot: report
        // it as metadata only, never as persisted
        if (partial > 0) {
            prefix_candidate c;
            c.prefix_tokens = partial;
            c.observations  = partial_obs;
            c.state         = (partial >= opt.min_tokens && partial_obs >= opt.min_observations)
                ? prefix_state::eligible
                : prefix_state::observed;
            res.push_back(std::move(c));
        }

        clamp_candidates(res, media_starts, event.media_token_counts);
    }

    insert(seq, true);
    note_capacity();
    compact();

    counters.n_candidates += res.size();

    return res;
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

    std::vector<size_t> media_starts;
    const std::vector<unit> seq = to_units(tokens, media_indices, media_token_counts, media_ids, media_starts);

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
        c.state         = (partial >= opt.min_tokens && partial_obs >= opt.min_observations)
            ? prefix_state::eligible
            : prefix_state::observed;
        res.push_back(std::move(c));
    }

    clamp_candidates(res, media_starts, media_token_counts);

    return res;
}

void server_prefix_index::on_snapshot_committed(const std::string & path, const llama_tokens & tokens,
        const std::vector<uint32_t> & media_indices,
        const std::vector<uint32_t> & media_token_counts,
        const std::vector<std::string> & media_ids) {
    if (path.empty() || tokens.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx);

    std::vector<size_t> media_starts;
    const std::vector<unit> seq = to_units(tokens, media_indices, media_token_counts, media_ids, media_starts);

    // a snapshot is attached to the node whose prefix it matches exactly: the
    // snapshot of A+B never marks A as persisted
    insert(seq, false);
    note_capacity();
    compact();

    std::vector<int32_t> path_nodes;
    walk(seq, path_nodes);

    if (path_nodes.empty()) {
        return;
    }

    const int32_t idx = path_nodes.back();
    if (nodes[idx].tokens_end != seq.size()) {
        return;
    }

    nodes[idx].snapshot_path    = path;
    nodes[idx].ref_invalidated  = false;
    refresh_state(idx);

    snapshot_ref ref;
    ref.tokens = seq.size();
    ref.node   = idx;
    snapshots[path] = ref;
}

void server_prefix_index::on_snapshot_invalidated(const std::string & path) {
    if (path.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx);

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
    out.bytes       = approx_bytes();

    return out;
}
