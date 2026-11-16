#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

//
// pure quota planning for the on-disk prompt cache
//
// The problem this solves is an ordering one. A newly written long snapshot may
// be used to justify deleting the short snapshots it supersedes, but the same
// long snapshot can then be evicted by the quota pass - leaving neither. The
// rule that prevents it is: never delete a snapshot in favour of another one
// unless that other one is guaranteed to survive the same capacity transaction.
//
// This module only *plans*. It does not take a lock, does not touch a file and
// does not know what a llama_context is. The caller feeds it the current entry
// set plus the provisional new entry, gets back "does this fit, and in which
// order may bytes be reclaimed", performs the actual deletions, and re-plans if
// a deletion really failed. Every size in this interface is a committed file
// size, never a pre-write estimate.
//

// one entry of the current set, or the provisional new one
struct disk_cache_quota_item {
    const std::string * path        = nullptr;
    size_t              size_file   = 0;    // bytes actually occupied on disk
    int64_t             last_used   = 0;    // LRU ordering key
    bool                quarantined = false; // the file could not be deleted: bytes stay, entry is unusable
    // the path occupies space whose size was never measured, so `size_file` is not a
    // real figure. Such an item can never be counted as reclaimable: dropping it from
    // the plan's arithmetic would free capacity on paper while the bytes remain
    bool                size_unknown = false;
    // must survive this transaction. `true` for the provisional new entry, and
    // for every snapshot whose disappearance the transaction would justify - a
    // short prefix may only be marked free-to-delete after its replacement is
    // protected as well
    bool                protected_  = false;
};

// the outcome of a planning pass
struct disk_cache_quota_plan {
    bool                  fits       = false; // every protected item can be kept
    size_t                total      = 0;     // accounted bytes of all items
    size_t                protected_bytes = 0;
    size_t                overshoot  = 0;     // bytes still over the limit when nothing else can go
    std::vector<size_t>   evict_order;        // indices into the input, LRU first

    // set when an item's footprint is unknown. The plan cannot certify that any
    // amount of free space remains while such a path is live, because the bytes it
    // holds are real but unmeasured
    bool                  blocked_by_unknown = false;
};

// plan the eviction of unprotected items until the accounted total fits the
// limit. quarantined items are never candidates: their bytes stay accounted for
// and their entry is not usable, so removing them is not an option
disk_cache_quota_plan disk_cache_plan_quota(const std::vector<disk_cache_quota_item> & items, size_t limit);

// which protected items depend on a given one surviving
//
// the caller uses this to decide whether a short snapshot may be deleted *before*
// its replacement is admitted: as long as the replacement is not yet in the
// protected set, the short snapshot has to be protected too
struct disk_cache_replacement_dep {
    size_t shorter_index = 0; // item that would be deleted
    size_t longer_index  = 0; // item that is expected to replace it
};

// returns, for each dependency, whether the longer item is itself protected. a
// dependency whose longer item is not protected means deleting the shorter item
// would be unsafe in this transaction
std::vector<bool> disk_cache_replacements_safe(const std::vector<disk_cache_replacement_dep> & deps,
        const std::vector<disk_cache_quota_item> & items);

// what the entry set says about one path at the end of a write transaction
//
// A path being present in `entries` only means bytes are accounted for. It says
// nothing about whether the snapshot can be restored: a write that was rejected
// and could not be rolled back stays there as a quarantined entry, occupying
// space and serving nothing. The prefix directory must only be told about
// entries that are really usable, otherwise it would record a `persisted`
// reference to a file the disk tier itself refuses to load - a state that also
// disappears on restart, because startup registration skips quarantined entries.
// whether a path may be charged a byte count by the ledger.
//
// A failed write can leave bytes at the final path, and the ledger has to decide what
// to record for them. Two facts are needed, and neither alone is enough:
//
//   placed_by_this_attempt - did this write's own rename put the bytes there? If not,
//                            whatever sits at the path belongs to an earlier attempt,
//                            and this writer's byte count describes a different file.
//   size_measured          - was a real size obtained? If not there is no figure to
//                            charge; the writer's count describes the temporary file,
//                            not the object left at the final path.
//
// Only when both hold can the count be used. Otherwise the footprint is unknown, which
// is recorded as such and blocks capacity admission rather than being silently taken
// as zero.
bool disk_cache_orphan_chargeable(bool placed_by_this_attempt, bool size_measured);

struct disk_cache_commit_candidate {
    std::string path;
    bool        quarantined = false;
    bool        has_tokens  = true; // an entry with no tokens has nothing to compare against
};

// true when `path` is held by an entry that is admissible as a physical
// reference: present, not quarantined and usable. this is the only condition
// under which a commit notification may be sent
bool disk_cache_commit_eligible(const std::vector<disk_cache_commit_candidate> & entries,
        const std::string & path);
