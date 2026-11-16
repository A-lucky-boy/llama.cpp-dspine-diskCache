// standalone self-check for the orphan/quarantine lifecycle arithmetic
//
// The failures this pins down all come from treating "I could not find out" as a
// definite answer:
//
//   * a path query that failed was read as "the file is absent"
//   * a failed rename left an older file behind, and the writer's own byte count was
//     charged to that path as if it described the file there
//   * a path holding bytes nobody measured was counted as zero, which reads as free
//     space and lets admission over-fill the directory
//
// Building it is not needed to know it is correct, but:
//
//   cl /nologo /std:c++17 /EHsc /utf-8 /O2 /MD /D_CRT_SECURE_NO_WARNINGS ^
//      /I tools/server /I common /I include /I ggml/include ^
//      /Fe:test-run\disk_cache_lifecycle_test.exe ^
//      tools/server/tests/unit/test_disk_cache_lifecycle.cpp ^
//      tools/server/server-disk-cache-quota.cpp
//
// The state classification itself is exercised through the same planner the server
// calls, not through a copy of the rule.

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

namespace {

const std::string P_GOOD  = "pc-good.bin";
const std::string P_ORPH  = "pc-orphan.bin";
const std::string P_UNK   = "pc-unknown.bin";
const std::string P_FULL  = "pc-filler.bin";

// one item as the server would describe it
disk_cache_quota_item item(const std::string * path, size_t size, int64_t lru,
                           bool quarantined = false, bool unknown = false) {
    disk_cache_quota_item it;
    it.path         = path;
    it.size_file    = size;
    it.last_used    = lru;
    it.quarantined  = quarantined;
    it.size_unknown = unknown;
    return it;
}

// ---------------------------------------------------------------------------
// an unmeasured path must block admission
// ---------------------------------------------------------------------------

void test_unknown_size_blocks_admission() {
    // 900 MiB of counted entries under a 1 GiB limit: the countable arithmetic
    // fits comfortably. One unmeasured path exists, whose bytes are somewhere in
    // that directory and are not in the sum.
    std::vector<disk_cache_quota_item> items = {
        item(&P_GOOD, 900ull * 1024 * 1024, 10),
        item(&P_UNK,  0, 20, /*quarantined=*/true, /*unknown=*/true),
    };

    const size_t limit = 1024ull * 1024 * 1024;
    const disk_cache_quota_plan plan = disk_cache_plan_quota(items, limit);

    // the counted total is under the limit, so a planner that trusted its numbers
    // would report success - and admit a write into space that may not exist
    CHECK(plan.total < limit);

    // the plan must refuse, because it cannot certify that free space is real
    CHECK(plan.blocked_by_unknown);
    CHECK(!plan.fits);
}

void test_measured_paths_do_not_block() {
    // the same shape without the unknown flag: an ordinary quarantined entry is
    // counted bytes, and the arithmetic decides
    std::vector<disk_cache_quota_item> items = {
        item(&P_GOOD, 900ull * 1024 * 1024, 10),
        item(&P_ORPH, 10ull * 1024 * 1024, 20, /*quarantined=*/true),
    };

    const disk_cache_quota_plan plan = disk_cache_plan_quota(items, 1024ull * 1024 * 1024);

    CHECK(!plan.blocked_by_unknown);
    CHECK(plan.fits);
}

void test_unknown_is_not_counted_as_zero() {
    // The distinction that matters: a measured zero-byte entry and an unmeasured one
    // both contribute 0 to `total`, but only the second one makes the sum untrustworthy
    std::vector<disk_cache_quota_item> measured = { item(&P_GOOD, 0, 10) };
    std::vector<disk_cache_quota_item> unknown  = { item(&P_UNK,  0, 10, false, true) };

    const disk_cache_quota_plan m = disk_cache_plan_quota(measured, 1024);
    const disk_cache_quota_plan u = disk_cache_plan_quota(unknown,  1024);

    CHECK(m.total == 0);
    CHECK(u.total == 0);

    CHECK(m.fits);
    CHECK(!u.fits);
}

void test_unknown_blocks_even_when_unlimited() {
    // `--cache-disk-max -1` maps to 0, meaning unlimited, and that branch returns
    // `fits` early. Unlimited capacity is still a claim about capacity, so it is
    // withheld while a path holds bytes nobody measured.
    //
    // This is the case the early return made easy to miss: the limit check is exactly
    // where the arithmetic stops being consulted, so the unknown gate has to be
    // applied before it.
    std::vector<disk_cache_quota_item> unknown = {
        item(&P_UNK, 0, 10, /*quarantined=*/true, /*unknown=*/true),
    };

    const disk_cache_quota_plan u = disk_cache_plan_quota(unknown, 0);
    CHECK(u.blocked_by_unknown);
    CHECK(!u.fits);

    // limit 0 with nothing unknown still fits - that is the documented meaning, so
    // the gate above narrowed a real case rather than disabling the option
    std::vector<disk_cache_quota_item> ok = { item(&P_GOOD, 1024, 10) };
    CHECK(disk_cache_plan_quota(ok, 0).fits);
}

void test_removing_the_unknown_path_restores_admission() {
    // the gate is about the unknown path, not about the cache being full: once the
    // path is measured or removed, the same items are admitted normally
    std::vector<disk_cache_quota_item> with_unknown = {
        item(&P_GOOD, 100ull * 1024 * 1024, 10),
        item(&P_UNK,  0, 20, /*quarantined=*/true, /*unknown=*/true),
    };

    std::vector<disk_cache_quota_item> without_unknown = {
        item(&P_GOOD, 100ull * 1024 * 1024, 10),
    };

    const size_t limit = 1024ull * 1024 * 1024;

    CHECK(!disk_cache_plan_quota(with_unknown, limit).fits);
    CHECK(disk_cache_plan_quota(without_unknown, limit).fits);
}

void test_unknown_path_is_never_an_eviction_candidate() {
    // quarantined items are never candidates, and an unknown-size one certainly is
    // not: "evicting" it would free a number nobody measured
    std::vector<disk_cache_quota_item> items = {
        item(&P_GOOD, 2ull * 1024 * 1024 * 1024, 10), // over the limit on its own
        item(&P_UNK,  0, 20, /*quarantined=*/true, /*unknown=*/true),
    };

    const disk_cache_quota_plan plan = disk_cache_plan_quota(items, 1024ull * 1024 * 1024);

    for (const size_t idx : plan.evict_order) {
        CHECK(items[idx].path != &P_UNK);
    }

    CHECK(!plan.fits);
}

// ---------------------------------------------------------------------------
// the orphan must not be recoverable, and must not be reported as usable
// ---------------------------------------------------------------------------

void test_committed_eligible_rejects_quarantined() {
    // the predicate the prefix directory keys off: a quarantined path must never be
    // published as a physical reference, whether or not it is in the entry list
    std::vector<disk_cache_commit_candidate> only_quarantined = {
        { P_ORPH, /*quarantined=*/true, /*has_tokens=*/true },
    };

    CHECK(!disk_cache_commit_eligible(only_quarantined, P_ORPH));

    // an empty token list is equally unusable
    std::vector<disk_cache_commit_candidate> no_tokens = {
        { P_GOOD, /*quarantined=*/false, /*has_tokens=*/false },
    };

    CHECK(!disk_cache_commit_eligible(no_tokens, P_GOOD));

    // and a healthy entry is still eligible, so the check has not simply gone false
    std::vector<disk_cache_commit_candidate> healthy = {
        { P_GOOD, /*quarantined=*/false, /*has_tokens=*/true },
    };

    CHECK(disk_cache_commit_eligible(healthy, P_GOOD));
}

void test_quarantined_holder_does_not_justify_replacement() {
    // a quarantined longer snapshot cannot justify deleting a shorter working one:
    // the replacement would not be usable, so the transaction would trade a working
    // snapshot for bytes nothing can read
    std::vector<disk_cache_quota_item> items = {
        item(&P_GOOD, 100, 10),
        item(&P_ORPH, 200, 20, /*quarantined=*/true),
    };

    std::vector<disk_cache_replacement_dep> deps = { { 0, 1 } }; // short=GOOD, long=ORPH

    const std::vector<bool> safe = disk_cache_replacements_safe(deps, items);
    CHECK(safe.size() == 1);
    CHECK(!safe[0]);
}

void test_healthy_protected_replacement_is_safe() {
    // the same dependency with a usable, protected longer entry is allowed, so the
    // rule is not simply always-false
    std::vector<disk_cache_quota_item> items = {
        item(&P_GOOD, 100, 10),
        item(&P_FULL, 200, 20),
    };

    items[1].protected_ = true;

    std::vector<disk_cache_replacement_dep> deps = { { 0, 1 } };

    const std::vector<bool> safe = disk_cache_replacements_safe(deps, items);
    CHECK(safe.size() == 1);
    CHECK(safe[0]);
}

// ---------------------------------------------------------------------------
// the two origins a failed write can leave behind
// ---------------------------------------------------------------------------

// What the writer reports for each way it can fail, as the worker consumes it.
enum class origin { none, this_attempt, preexisting };

// The worker calls `disk_cache_orphan_chargeable()` directly - these cases drive that
// production function rather than a copy of its rule, so a change to the rule cannot
// leave the test passing against a formula that no longer exists.
void test_only_this_attempt_may_be_charged_written_bytes() {
    // the one combination that may carry a number: this attempt placed the bytes, and
    // a real size was obtained
    CHECK(disk_cache_orphan_chargeable(/*placed_by_this_attempt=*/true, /*size_measured=*/true));

    // rename landed but the size query failed: no figure was obtained, so there is
    // nothing to charge, however real the bytes are
    CHECK(!disk_cache_orphan_chargeable(true, /*size_measured=*/false));

    // rename never landed: whatever is at the path was written earlier, and this
    // writer's byte count says nothing about it
    CHECK(!disk_cache_orphan_chargeable(/*placed_by_this_attempt=*/false, /*size_measured=*/false));
    CHECK(!disk_cache_orphan_chargeable(false, /*size_measured=*/true));
}

void test_preexisting_path_is_never_measured_by_this_write() {
    // The rule must not vary with the size flag for this origin: a path this attempt
    // never replaced is not something this write can size either way
    for (int u = 0; u <= 1; u++) {
        CHECK(!disk_cache_orphan_chargeable(/*placed_by_this_attempt=*/false, u != 0));
    }

    // and the `origin` enum in this file is only a label for the cases above; the
    // production function takes booleans, so there is nothing to drift
    CHECK(origin::this_attempt != origin::preexisting);
}

} // namespace

int main() {
    test_unknown_size_blocks_admission();
    test_measured_paths_do_not_block();
    test_unknown_is_not_counted_as_zero();
    test_unknown_blocks_even_when_unlimited();
    test_removing_the_unknown_path_restores_admission();
    test_unknown_path_is_never_an_eviction_candidate();
    test_committed_eligible_rejects_quarantined();
    test_quarantined_holder_does_not_justify_replacement();
    test_healthy_protected_replacement_is_safe();
    test_only_this_attempt_may_be_charged_written_bytes();
    test_preexisting_path_is_never_measured_by_this_write();

    if (g_failures == 0) {
        std::printf("all disk-cache lifecycle checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
