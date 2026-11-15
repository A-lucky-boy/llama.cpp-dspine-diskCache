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

#include <cstdio>
#include <string>
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

    if (g_failures == 0) {
        std::printf("all disk-cache quota checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
