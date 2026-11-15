// standalone self-check for the disk-cache quota planner
//
// Pure planning: no model, no lock, no file. Build it with the server toolchain
// (Windows, repository root, vcvars64 loaded):
//
//   cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
//      /I tools/server /I common /I include /I ggml/include ^
//      /Fe:test-run\disk_cache_quota_test.exe ^
//      tools/server/tests/unit/test_disk_cache_quota.cpp tools/server/server-disk-cache-quota.cpp
//
// The scenario it pins down is the ordering bug: a long snapshot L supersedes a
// short snapshot S, the quota can hold only one of them, and a quarantined
// snapshot Q occupies bytes that cannot be reclaimed. Deleting S first and then
// evicting L leaves the cache with neither.

#include "server-disk-cache-quota.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    const long long va = (long long) (a); \
    const long long vb = (long long) (b); \
    if (va != vb) { \
        std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); \
        g_failures++; \
    } \
} while (0)

namespace {

constexpr size_t MiB = 1024ull*1024ull;

// owns the path strings the items point at
struct fixture {
    std::vector<std::string>           paths;
    std::vector<disk_cache_quota_item> items;

    void add(const char * name, size_t bytes, int64_t used, bool quarantined = false, bool protected_ = false) {
        paths.push_back(name);

        disk_cache_quota_item it;
        it.path        = &paths.back();
        it.size_file   = bytes;
        it.last_used   = used;
        it.quarantined = quarantined;
        it.protected_  = protected_;
        items.push_back(it);
    }

    // re-point the items after `paths` grew: a vector reallocation invalidates
    // the stored pointers
    void refresh() {
        for (size_t i = 0; i < items.size(); ++i) {
            items[i].path = &paths[i];
        }
    }
};

bool plan_evicts(const disk_cache_quota_plan & plan, size_t index) {
    for (const size_t i : plan.evict_order) {
        if (i == index) {
            return true;
        }
    }
    return false;
}

// the regression: Q (quarantined) + S (short) fit, adding L (long) overflows, and
// deleting S is still not enough. plan must refuse to admit L, which is what keeps
// the caller from deleting S
void test_short_snapshot_is_not_traded_away() {
    // Q holds 600 MiB and cannot be deleted, S is a usable 200 MiB short
    // snapshot, L is the 300 MiB long snapshot that supersedes S. the limit is
    // 800 MiB, so Q+S fits but Q+L does not: deleting S buys nothing, because L
    // would still be over the limit and would be evicted in turn - leaving only Q
    const size_t limit = 800 * MiB;

    fixture fx;
    fx.add("Q", 600 * MiB, 1, /*quarantined=*/true);
    fx.add("S", 200 * MiB, 2);
    fx.add("L", 300 * MiB, 3, /*quarantined=*/false, /*protected_=*/true);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, limit);

    CHECK_EQ(plan.total, 1100 * MiB);
    CHECK_EQ(plan.protected_bytes, 600 * MiB + 300 * MiB); // Q is quarantined, L is protected
    CHECK(!plan.fits);
    CHECK_EQ(plan.overshoot, 100 * MiB); // what remains after S is gone
    CHECK(!plan_evicts(plan, 2)); // L is never a candidate
    CHECK(!plan_evicts(plan, 0)); // Q cannot be deleted
    CHECK(plan_evicts(plan, 1));  // S is the only reclaimable item
}

// the same set with a limit that does admit L: the plan says so, and the caller
// may then reclaim S knowing L stays
void test_admission_when_reclaiming_is_enough() {
    const size_t limit = 900 * MiB;

    fixture fx;
    fx.add("Q", 600 * MiB, 1, /*quarantined=*/true);
    fx.add("S", 200 * MiB, 2);
    fx.add("L", 300 * MiB, 3, /*quarantined=*/false, /*protected_=*/true);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, limit);

    CHECK(plan.fits);
    CHECK_EQ(plan.overshoot, 0);
    CHECK(plan_evicts(plan, 1));  // S is reclaimed
    CHECK(!plan_evicts(plan, 0)); // Q is not
    CHECK(!plan_evicts(plan, 2)); // L survives
}

// a quarantined entry is never a candidate, whatever the limit is
void test_quarantined_bytes_still_counted() {
    const size_t limit = 800 * MiB;

    fixture fx;
    fx.add("Q", 500 * MiB, 1, /*quarantined=*/true);
    fx.add("S", 200 * MiB, 2);
    fx.add("L", 300 * MiB, 3, /*quarantined=*/false, /*protected_=*/true);
    fx.refresh();

    // only a real deletion frees those bytes, and the caller performs it through
    // remove_file() outside this module
    const auto plan = disk_cache_plan_quota(fx.items, limit);
    CHECK(!plan_evicts(plan, 0));
    CHECK_EQ(plan.total, 1000 * MiB); // the quarantined bytes still count
}

// the plain capacity case: one unprotected entry has to go, worst first
void test_lru_order() {
    const size_t limit = 300 * MiB;

    fixture fx;
    fx.add("A", 200 * MiB, 10);
    fx.add("B", 200 * MiB, 5);  // least recently used
    fx.add("C", 100 * MiB, 20);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, limit);

    CHECK_EQ(plan.total, 500 * MiB);
    CHECK(plan.fits);
    CHECK_EQ(plan.evict_order.size(), 1);
    CHECK(plan_evicts(plan, 1));
    CHECK_EQ(plan.overshoot, 0);
}

// the protected entry is never chosen, even when it is the oldest
void test_protected_survives_lru() {
    const size_t limit = 250 * MiB;

    fixture fx;
    fx.add("oldest-but-new", 200 * MiB, 1, false, /*protected_=*/true);
    fx.add("middle", 100 * MiB, 5);
    fx.add("newest", 100 * MiB, 9);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, limit);

    CHECK(plan.fits);
    CHECK(!plan_evicts(plan, 0));
    // both unprotected entries have to go: the protected 200 MiB stays, so the
    // remaining total is 200 MiB, within the 250 MiB limit
    CHECK_EQ(plan.evict_order.size(), 2);
    CHECK(plan_evicts(plan, 1));
    CHECK(plan_evicts(plan, 2));
}

// nothing reclaimable and still over: `fits` is false so the caller rolls back
void test_nothing_reclaimable() {
    const size_t limit = 100 * MiB;

    fixture fx;
    fx.add("Q1", 400 * MiB, 1, /*quarantined=*/true);
    fx.add("Q2", 300 * MiB, 2, /*quarantined=*/true);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, limit);

    CHECK(!plan.fits);
    CHECK_EQ(plan.evict_order.size(), 0);
    CHECK_EQ(plan.overshoot, 600 * MiB);
}

// exactly at the limit fits; one byte over does not
void test_boundary() {
    fixture fx;
    fx.add("A", 500 * MiB, 1);
    fx.add("B", 300 * MiB, 2);
    fx.refresh();

    CHECK(disk_cache_plan_quota(fx.items, 800 * MiB).fits);
    CHECK_EQ(disk_cache_plan_quota(fx.items, 800 * MiB).evict_order.size(), 0);

    // one byte over the limit: reclaimable by evicting the older entry, so the
    // plan admits the set - "was over at first" is not "cannot fit"
    {
        const auto plan = disk_cache_plan_quota(fx.items, 800 * MiB - 1);
        CHECK(plan.fits);
        CHECK_EQ(plan.evict_order.size(), 1);
        CHECK(plan_evicts(plan, 0)); // A is older
        CHECK_EQ(plan.overshoot, 0);
    }

    // a very small limit is still satisfiable here: every byte is reclaimable, so
    // both entries go and the remaining total is zero. an unsatisfiable case only
    // exists when protected or quarantined bytes alone exceed the limit - see
    // test_limit_below_entry_size and test_nothing_reclaimable
    {
        const auto plan = disk_cache_plan_quota(fx.items, 100 * MiB);
        CHECK(plan.fits);
        CHECK_EQ(plan.evict_order.size(), 2);
        CHECK_EQ(plan.overshoot, 0);
    }
}

// a limit smaller than the entry itself: the entry is rejected rather than the
// whole cache being emptied for nothing
void test_limit_below_entry_size() {
    fixture fx;
    fx.add("existing", 50 * MiB, 1);
    fx.add("L", 400 * MiB, 2, false, /*protected_=*/true);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, 100 * MiB);

    CHECK(!plan.fits);
    CHECK(plan_evicts(plan, 0)); // the existing one is reclaimable ...
    CHECK_EQ(plan.overshoot, 300 * MiB); // ... and the protected 400 MiB still exceeds the limit
}

// an unlimited cache (limit 0) always fits and never evicts
void test_unlimited() {
    fixture fx;
    fx.add("A", 10 * 1024 * MiB, 1, false, /*protected_=*/true);
    fx.add("B", 10 * 1024 * MiB, 2);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, 0);
    CHECK(plan.fits);
    CHECK_EQ(plan.evict_order.size(), 0);
    CHECK_EQ(plan.overshoot, 0);
}

// a dependency is only safe when its replacement is protected and usable
void test_replacement_dependencies() {
    fixture fx;
    fx.add("S", 100 * MiB, 1);
    fx.add("L-admitted", 200 * MiB, 2, false, /*protected_=*/true);
    fx.add("L-pending", 300 * MiB, 3, false, /*protected_=*/false);
    fx.add("L-quarantined", 400 * MiB, 4, /*quarantined=*/true, /*protected_=*/true);
    fx.refresh();

    std::vector<disk_cache_replacement_dep> deps = {
        { 0, 1 }, // S <- L-admitted
        { 0, 2 }, // S <- L-pending: not protected yet
        { 0, 3 }, // S <- L-quarantined: protected but unusable
    };

    const auto safe = disk_cache_replacements_safe(deps, fx.items);

    CHECK_EQ(safe.size(), 3);
    CHECK(safe[0]);
    CHECK(!safe[1]);
    CHECK(!safe[2]);

    // an out-of-range dependency is never safe
    std::vector<disk_cache_replacement_dep> bad = { { 0, 99 } };
    const auto safe2 = disk_cache_replacements_safe(bad, fx.items);
    CHECK_EQ(safe2.size(), 1);
    CHECK(!safe2[0]);
}

// ---------------------------------------------------------------------------
// an explicit comparison of the two worker orders, replayed over a simulated
// file set.
//
// this does not run the server: it replays the *decisions* the worker takes, using
// the same planner, so the ordering bug is pinned down without a model or a
// multi-gigabyte write. `removable` models a Windows sharing violation.
// ---------------------------------------------------------------------------
struct sim_file {
    std::string name;
    size_t      bytes     = 0;
    int64_t     used      = 0;
    bool        removable = true;
    bool        present   = true;
};

struct sim_result {
    bool s_present  = false;
    bool l_present  = false;
    bool over_quota = false;
};

sim_file * sim_find(std::vector<sim_file> & files, const char * name) {
    for (auto & f : files) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

size_t sim_total(const std::vector<sim_file> & files) {
    size_t total = 0;
    for (const auto & f : files) {
        if (f.present) {
            total += f.bytes;
        }
    }
    return total;
}

// "old" order: delete the superseded short snapshot first, then run the quota pass
sim_result sim_old_order(std::vector<sim_file> files, size_t limit, const char * s_name, const char * l_name) {
    sim_file * S = sim_find(files, s_name);
    sim_file * L = sim_find(files, l_name);

    // drop_prefixes(): the long entry is a replacement purely statically, so the
    // short snapshot is removed right away
    if (S != nullptr && S->present && S->removable) {
        S->present = false;
    }

    // enforce_limit(): the short snapshot is already gone, so the long entry is
    // the LRU victim that brings the total back under the limit
    if (limit != 0 && sim_total(files) > limit && L != nullptr && L->present && L->removable) {
        L->present = false;
    }

    sim_result res;
    res.s_present  = S != nullptr && S->present;
    res.l_present  = L != nullptr && L->present;
    res.over_quota = limit != 0 && sim_total(files) > limit;
    return res;
}

// "new" order: plan the capacity transaction with L protected, then only delete S
// when L is guaranteed to survive it
sim_result sim_new_order(std::vector<sim_file> files, size_t limit, const char * s_name, const char * l_name) {
    sim_file * S = sim_find(files, s_name);
    sim_file * L = sim_find(files, l_name);

    std::vector<std::string>           paths;
    std::vector<disk_cache_quota_item> items;

    for (const auto & f : files) {
        if (!f.present) {
            continue;
        }
        paths.push_back(f.name);
    }

    for (size_t i = 0; i < paths.size(); ++i) {
        disk_cache_quota_item it;
        it.path        = &paths[i];
        it.size_file   = files[i].bytes;
        it.last_used   = files[i].used;
        it.quarantined = files[i].name == "Q" && !files[i].removable;
        it.protected_  = files[i].name == l_name;
        items.push_back(it);
    }

    const auto plan = disk_cache_plan_quota(items, limit);

    if (!plan.fits) {
        // roll L back instead of trading S for a file that cannot stay
        if (L != nullptr) {
            L->present = false;
        }
    } else {
        for (const size_t idx : plan.evict_order) {
            sim_file * victim = sim_find(files, files[idx].name.c_str());
            if (victim != nullptr && victim->removable) {
                victim->present = false;
            }
        }

        // L survived the capacity pass - only now may it justify deleting S
        if (L != nullptr && L->present && S != nullptr && S->present && S->removable) {
            S->present = false;
        }
    }

    sim_result res;
    res.s_present  = S != nullptr && S->present;
    res.l_present  = L != nullptr && L->present;
    res.over_quota = limit != 0 && sim_total(files) > limit;
    return res;
}

void test_order_regression() {
    const size_t limit = 800 * MiB;

    // Q cannot be deleted (sharing violation) and holds 600 MiB; S is a usable
    // 200 MiB short snapshot; L is the 300 MiB long one that supersedes S
    const std::vector<sim_file> base = {
        { "Q", 600 * MiB, 1, /*removable=*/false, /*present=*/true },
        { "S", 200 * MiB, 2, /*removable=*/true,  /*present=*/true },
        { "L", 300 * MiB, 3, /*removable=*/true,  /*present=*/true },
    };

    const sim_result old_res = sim_old_order(base, limit, "S", "L");

    // the old order: S deleted because L "replaces" it, then L evicted because the
    // quota is still exceeded - the cache ends up with neither
    CHECK(!old_res.s_present);
    CHECK(!old_res.l_present);

    const sim_result new_res = sim_new_order(base, limit, "S", "L");

    // the new order refuses to admit L, so S is untouched: the cache keeps what it
    // had instead of losing both
    CHECK(new_res.s_present);
    CHECK(!new_res.l_present);
    CHECK(!new_res.over_quota);
}

// when the limit does admit L, the new order admits it and only then reclaims S
void test_order_regression_admitted() {
    const size_t limit = 900 * MiB;

    const std::vector<sim_file> base = {
        { "Q", 600 * MiB, 1, false, true },
        { "S", 200 * MiB, 2, true,  true },
        { "L", 300 * MiB, 3, true,  true },
    };

    const sim_result new_res = sim_new_order(base, limit, "S", "L");

    CHECK(new_res.l_present);  // L survived the capacity pass
    CHECK(!new_res.s_present); // and only then did S go
    CHECK(!new_res.over_quota);
}

// with no quarantined entry the new order behaves like a plain LRU
void test_order_regression_plain_lru() {
    const size_t limit = 400 * MiB;

    const std::vector<sim_file> base = {
        { "old", 100 * MiB, 1, true, true },
        { "S",   200 * MiB, 2, true, true },
        { "L",   300 * MiB, 3, true, true },
    };

    const sim_result new_res = sim_new_order(base, limit, "S", "L");

    CHECK(new_res.l_present);
    CHECK(!new_res.over_quota);
}

// ---------------------------------------------------------------------------
// protection identity must survive the erases the eviction pass performs.
//
// `entries` is a std::vector, so removing an element moves everything after it.
// a protection set built from pointers into that vector silently stops matching
// the entry it was meant to keep. this replays exactly that sequence: the
// protected entry sits *behind* an evicted one, so its storage is reused.
// ---------------------------------------------------------------------------
struct sim_entry {
    std::string path;
    size_t      bytes = 0;
    int64_t     used  = 0;
    bool        alive = true;
};

// one eviction pass over a simulated vector, driven by the same planner
void sim_evict_once(std::vector<sim_entry> & vec, size_t limit,
        const std::unordered_set<std::string> & protected_paths) {
    if (limit == 0) {
        return;
    }

    // the planner sees the current set plus the protection set
    std::vector<std::string>           paths;
    std::vector<disk_cache_quota_item> items;

    for (const auto & e : vec) {
        if (!e.alive) {
            continue;
        }
        paths.push_back(e.path);
    }

    size_t n = 0;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (!vec[i].alive) {
            continue;
        }
        disk_cache_quota_item it;
        it.path       = &paths[n];
        it.size_file  = vec[i].bytes;
        it.last_used  = vec[i].used;
        it.protected_ = protected_paths.count(vec[i].path) > 0;
        items.push_back(it);
        n++;
    }

    const auto plan = disk_cache_plan_quota(items, limit);

    // apply the plan by *removing from the vector*, so the remaining elements
    // shift - which is what invalidates pointer-based protection
    for (const size_t idx : plan.evict_order) {
        const std::string victim = paths[idx];
        for (auto & e : vec) {
            if (e.alive && e.path == victim) {
                e.alive = false;
                break;
            }
        }
    }
}

bool sim_alive(const std::vector<sim_entry> & vec, const std::string & path) {
    for (const auto & e : vec) {
        if (e.path == path) {
            return e.alive;
        }
    }
    return false;
}

size_t sim_sum(const std::vector<sim_entry> & vec) {
    size_t total = 0;
    for (const auto & e : vec) {
        if (e.alive) {
            total += e.bytes;
        }
    }
    return total;
}

void test_protection_survives_eviction() {
    const size_t limit = 300 * MiB;

    // "oldest" is evicted first; "new" is the entry this transaction is admitting
    // and sits behind it in the vector, so its storage shifts on removal
    std::vector<sim_entry> vec = {
        { "oldest", 200 * MiB, 1, true },
        { "middle", 100 * MiB, 5, true },
        { "new",    150 * MiB, 9, true },
    };

    const std::unordered_set<std::string> protected_paths = { "new" };

    sim_evict_once(vec, limit, protected_paths);

    CHECK(sim_alive(vec, "new"));    // the protected entry is never a candidate
    CHECK(!sim_alive(vec, "oldest")); // the LRU victim goes instead
    CHECK(sim_sum(vec) <= limit);
}

// ---------------------------------------------------------------------------
// protection identity must survive an erase that happens *before* it is used.
//
// the real sequence in enforce_limit() is:
//
//   1. worker() builds the protection set from pointers into `entries`
//   2. enforce_limit() calls sweep_expired(), which erases expired entries
//   3. the LRU loop consults the set built in step 1
//
// step 2 moves every element behind the erased one, so the pointers from step 1
// no longer address the entries they were taken from. this replays that order
// with both representations and asserts the path-keyed one holds.
// ---------------------------------------------------------------------------
// move the tail down exactly as std::vector::erase does, then shrink
void sim_erase_by_path(std::vector<sim_entry> & vec, const std::string & path) {
    for (auto it = vec.begin(); it != vec.end(); ++it) {
        if (it->path == path) {
            for (auto cur = it; cur + 1 != vec.end(); ++cur) {
                *cur = *(cur + 1);
            }
            vec.pop_back();
            return;
        }
    }
}

// the old representation: `keep` is captured as pointers, then an earlier entry is
// erased, then the LRU loop consults those pointers
bool sim_pointer_sequence_keeps(std::vector<sim_entry> vec, size_t limit,
        const std::string & protected_path, const std::string & expire_path) {
    std::vector<const sim_entry *> keep;
    for (const auto & e : vec) {
        if (e.path == protected_path) {
            keep.push_back(&e);
        }
    }

    sim_erase_by_path(vec, expire_path); // step 2: the vector shifts

    std::vector<std::string>           paths;
    std::vector<disk_cache_quota_item> items;

    for (const auto & e : vec) {
        paths.push_back(e.path);
    }

    for (size_t k = 0; k < vec.size(); ++k) {
        disk_cache_quota_item it;
        it.path       = &paths[k];
        it.size_file  = vec[k].bytes;
        it.last_used  = vec[k].used;
        it.protected_ = std::find(keep.begin(), keep.end(), &vec[k]) != keep.end();
        items.push_back(it);
    }

    const auto plan = disk_cache_plan_quota(items, limit);

    for (const size_t idx : plan.evict_order) {
        for (auto & e : vec) {
            if (e.path == paths[idx]) {
                e.alive = false;
                break;
            }
        }
    }

    return sim_alive(vec, protected_path);
}

// the fixed representation: the path is copied, so an erase cannot invalidate it
bool sim_path_sequence_keeps(std::vector<sim_entry> vec, size_t limit,
        const std::string & protected_path, const std::string & expire_path) {
    const std::unordered_set<std::string> protected_paths = { protected_path };

    sim_erase_by_path(vec, expire_path);

    sim_evict_once(vec, limit, protected_paths);

    return sim_alive(vec, protected_path);
}

void test_protection_identity_survives_prior_erase() {
    // "expiring" is removed by the TTL pass and sits *before* the protected entry,
    // so the protected entry's storage shifts down one slot
    const auto make = []() {
        return std::vector<sim_entry> {
            { "expiring", 50 * MiB,  1, true },
            { "new",      150 * MiB, 9, true },
            { "other",    100 * MiB, 5, true },
        };
    };

    const size_t limit = 120 * MiB;

    // the fixed code keeps the entry it promised to keep
    CHECK(sim_path_sequence_keeps(make(), limit, "new", "expiring"));

    // the pointer-based sequence is *not* asserted: reading through a stale
    // pointer is undefined behaviour, so its outcome is not a contract. it is kept
    // here so the regression is documented rather than assumed harmless
    const bool ptr_res = sim_pointer_sequence_keeps(make(), limit, "new", "expiring");
    (void) ptr_res;
}

// the same pass repeated: after the first erase the protected entry has moved, so
// a stale pointer would now match a different entry. two evictions are needed to
// bring the total down
void test_protection_survives_repeated_eviction() {
    const size_t limit = 150 * MiB;

    std::vector<sim_entry> vec = {
        { "a-oldest", 100 * MiB, 1, true },
        { "b",        100 * MiB, 2, true },
        { "new",      150 * MiB, 9, true },
    };

    const std::unordered_set<std::string> protected_paths = { "new" };

    sim_evict_once(vec, limit, protected_paths);

    CHECK(sim_alive(vec, "new"));
    CHECK(!sim_alive(vec, "a-oldest"));
    CHECK(!sim_alive(vec, "b")); // both unprotected entries are gone
    CHECK(sim_sum(vec) <= limit);
}

// a protected entry that is also the least recently used must still not be taken
void test_protected_never_lru_victim() {
    const size_t limit = 100 * MiB;

    std::vector<sim_entry> vec = {
        { "new",     150 * MiB, 1, true }, // oldest by last_used, but protected
        { "other",   200 * MiB, 9, true },
    };

    const std::unordered_set<std::string> protected_paths = { "new" };

    sim_evict_once(vec, limit, protected_paths);

    CHECK(sim_alive(vec, "new"));
    CHECK(!sim_alive(vec, "other"));
}

// a quarantined entry is counted and never selected, which can make the set
// unsatisfiable even though an unprotected entry is also present
void test_quarantined_and_protected_together() {
    fixture fx;
    fx.add("Q", 600 * MiB, 1, /*quarantined=*/true);
    fx.add("keep", 200 * MiB, 2, false, /*protected_=*/true);
    fx.add("victim", 300 * MiB, 3);
    fx.refresh();

    const auto plan = disk_cache_plan_quota(fx.items, 700 * MiB);

    CHECK(!plan.fits);            // 600 + 200 = 800 MiB survives, over the limit
    CHECK_EQ(plan.overshoot, 100 * MiB);
    CHECK(plan_evicts(plan, 2));  // the victim goes first
    CHECK(!plan_evicts(plan, 0)); // the quarantined bytes stay
    CHECK(!plan_evicts(plan, 1)); // the protected entry stays
}

// ---------------------------------------------------------------------------
// a rejected write whose rollback fails.
//
// the file is still on disk and still occupies space, so it has to be registered
// as a quarantined entry: accounted for in the total, excluded from reuse, and
// never counted as a successful write. this pins down that arithmetic.
// ---------------------------------------------------------------------------
struct sim_rollback {
    size_t total_size  = 0;
    size_t write_bytes = 0; // bytes the caller believes it stored
    bool   quarantined = false;
    bool   rejected    = false;
};

sim_rollback sim_reject(bool rollback_deletes, size_t size_file) {
    sim_rollback r;
    r.rejected = true;

    if (rollback_deletes) {
        // the file is gone: nothing is accounted for, nothing was written
        return r;
    }

    // deletion failed: the bytes are still there and must be accounted for
    r.quarantined = true;
    r.total_size  = size_file;
    return r;
}

void test_rejected_write_rollback() {
    const size_t size_file = 300 * MiB;

    const sim_rollback ok = sim_reject(/*rollback_deletes=*/true, size_file);
    CHECK(ok.rejected);
    CHECK(!ok.quarantined);
    CHECK_EQ(ok.total_size, 0);      // no phantom bytes
    CHECK_EQ(ok.write_bytes, 0);     // not counted as a successful write

    const sim_rollback failed = sim_reject(/*rollback_deletes=*/false, size_file);
    CHECK(failed.rejected);
    CHECK(failed.quarantined);       // registered, not silently forgotten
    CHECK_EQ(failed.total_size, size_file); // the ledger keeps the real bytes
    CHECK_EQ(failed.write_bytes, 0); // still not a successful write
}

// ---------------------------------------------------------------------------
// commit eligibility: which transaction outcomes may publish a persisted
// reference to the prefix directory.
//
// "present in entries" only means the bytes are accounted for. a rejected write
// whose rollback failed stays there as a quarantined entry, and telling the
// directory about it would create a physical reference that load() refuses to
// use - and that also disappears on restart, because startup registration skips
// quarantined entries.
// ---------------------------------------------------------------------------
void test_commit_eligibility() {
    // 1. a snapshot that really made it in: eligible
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-aaa.bin", /*quarantined=*/false, /*has_tokens=*/true },
        };
        CHECK(disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }

    // 2. rejected, rolled back: the path is gone entirely
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-other.bin", false, true },
        };
        CHECK(!disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }

    // 3. rejected, rollback failed: quarantined, must not be published
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-aaa.bin", /*quarantined=*/true, /*has_tokens=*/true },
        };
        CHECK(!disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }

    // 4. quota unmet after the physical pass, rollback failed: same answer
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-other.bin", true,  true  },
            { "pc-aaa.bin",   true,  true  },
        };
        CHECK(!disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }

    // 5. an entry with no tokens has nothing to compare against
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-aaa.bin", false, /*has_tokens=*/false },
        };
        CHECK(!disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }

    // 6. a *different* path being quarantined does not block this one
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-quarantined.bin", true,  true },
            { "pc-aaa.bin",         false, true },
        };
        CHECK(disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }

    // 7. the entry is valid even when the cache is over quota because of an
    // unrelated quarantined file: over-quota is not the same as unusable
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-stuck.bin", true,  true }, // unremovable, still counted
            { "pc-new.bin",   false, true }, // admitted and usable
        };
        CHECK(disk_cache_commit_eligible(entries, "pc-new.bin"));
    }

    // 8. only the usable record at a path makes it eligible, whatever order the
    // duplicates appear in
    {
        const std::vector<disk_cache_commit_candidate> entries = {
            { "pc-aaa.bin", true,  true },
            { "pc-aaa.bin", false, true },
        };
        CHECK(disk_cache_commit_eligible(entries, "pc-aaa.bin"));
    }
}

} // namespace

int main() {
    test_short_snapshot_is_not_traded_away();
    test_admission_when_reclaiming_is_enough();
    test_quarantined_bytes_still_counted();
    test_lru_order();
    test_protected_survives_lru();
    test_nothing_reclaimable();
    test_boundary();
    test_limit_below_entry_size();
    test_unlimited();
    test_replacement_dependencies();
    test_order_regression();
    test_order_regression_admitted();
    test_order_regression_plain_lru();
    test_protection_survives_eviction();
    test_protection_identity_survives_prior_erase();
    test_protection_survives_repeated_eviction();
    test_protected_never_lru_victim();
    test_quarantined_and_protected_together();
    test_rejected_write_rollback();
    test_commit_eligibility();

    if (g_failures == 0) {
        std::printf("all disk-cache quota checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
