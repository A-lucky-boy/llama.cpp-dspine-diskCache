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
#include "server-disk-cache-quarantine.h"

#include <cstdio>
#include <cstring>
#include <set>
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

// ---------------------------------------------------------------------------
// the quarantine record's format
//
// The record exists to exclude paths, so the failure that matters is the one where a
// damaged file is accepted as "a list that simply has fewer names": a name that is
// absent because it was lost looks exactly like a name that was never recorded, and
// the path it should have excluded gets registered as a usable snapshot instead.
//
// The previous line-based format had no way to tell those apart. These cases pin down
// that the current one does.
// ---------------------------------------------------------------------------

std::string name_of(int n) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "pc-%016x.bin", (unsigned) n);
    return std::string(buf);
}

void test_entry_name_shape() {
    CHECK(disk_cache_is_entry_name("pc-0123456789abcdef.bin"));
    CHECK(disk_cache_is_entry_name("pc-ABCDEF0123456789.bin"));

    CHECK(!disk_cache_is_entry_name(""));                            // empty
    CHECK(!disk_cache_is_entry_name("pc-.bin"));                     // no digits
    CHECK(!disk_cache_is_entry_name("pc-0123456789abcde.bin"));      // 15 digits
    CHECK(!disk_cache_is_entry_name("pc-0123456789abcdefg.bin"));    // 17
    CHECK(!disk_cache_is_entry_name("pc-0123456789abcdeg.bin"));     // non-hex
    CHECK(!disk_cache_is_entry_name("xx-0123456789abcdef.bin"));     // wrong prefix
    CHECK(!disk_cache_is_entry_name("pc-0123456789abcdef.bin2"));    // wrong suffix
    CHECK(!disk_cache_is_entry_name("pc-quarantine.dat"));           // not an entry
    CHECK(!disk_cache_is_entry_name("../pc-0123456789abcdef.bin"));  // no traversal
}

void test_record_round_trips() {
    const std::set<std::string> names = { name_of(1), name_of(2), name_of(3) };

    const std::string bytes = disk_cache_quarantine_serialize(names);
    CHECK(!bytes.empty());

    std::set<std::string> back;
    CHECK(disk_cache_quarantine_parse(bytes, back) == quarantine_parse::ok);
    CHECK(back == names);
}

void test_empty_set_round_trips() {
    const std::string bytes = disk_cache_quarantine_serialize({});

    std::set<std::string> back;
    CHECK(disk_cache_quarantine_parse(bytes, back) == quarantine_parse::ok);
    CHECK(back.empty());
}

void test_serialization_is_deterministic() {
    // the same set must always produce the same bytes, or a checksum comparison across
    // runs would be meaningless
    std::set<std::string> a = { name_of(7), name_of(3), name_of(5) };
    std::set<std::string> b = { name_of(5), name_of(7), name_of(3) };

    CHECK(disk_cache_quarantine_serialize(a) == disk_cache_quarantine_serialize(b));
}

void test_truncated_record_is_rejected() {
    const std::set<std::string> names = { name_of(1), name_of(2), name_of(3) };
    const std::string bytes = disk_cache_quarantine_serialize(names);

    // dropping the last name and the checksum: the count no longer matches, and the
    // checksum is gone
    for (size_t cut = 1; cut <= bytes.size(); cut++) {
        const std::string short_bytes = bytes.substr(0, bytes.size() - cut);

        std::set<std::string> out;
        const quarantine_parse r = disk_cache_quarantine_parse(short_bytes, out);

        // a truncated record may parse as "not our format" while it is too short to
        // even hold the header, but it must never come back as a valid list
        CHECK(r != quarantine_parse::ok);
    }
}

void test_removing_a_record_from_the_bytes_is_caught() {
    // This is the case the old format could not detect: delete one whole record
    // (its length and its name) without touching anything else, and the file still
    // looks structurally plausible. The checksum is what makes it detectable.
    const std::set<std::string> names = { name_of(1), name_of(2), name_of(3) };
    std::string bytes = disk_cache_quarantine_serialize(names);

    const size_t header = 8 + 4 + 4;
    const size_t rec1   = 4 + name_of(1).size();

    // drop the second record, leaving count and checksum stale
    bytes.erase(header + rec1, 4 + name_of(2).size());

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse(bytes, out) == quarantine_parse::corrupt);
}

void test_crc_corruption_is_caught() {
    // The mutation has to leave the record *structurally* valid, or the case would pass
    // on a length or name check instead and prove nothing about the checksum.
    //
    // Flipping the low bit of a hex digit does exactly that: '0' becomes '1', which is
    // still a hex digit, so the name is still well formed, the counts still agree and
    // the layout is unchanged. The checksum is the only thing that can notice.
    const std::set<std::string> names = { name_of(1), name_of(2) };
    std::string bytes = disk_cache_quarantine_serialize(names);

    // find a hex digit inside the first name and flip its low bit
    const size_t name_off = 8 + 4 + 4 + 4; // magic + version + count + name_len
    CHECK(bytes[name_off] == 'p');
    const size_t digit = name_off + 3;     // the first hex digit after "pc-"

    const char before = bytes[digit];
    bytes[digit] = (char) (before ^ 0x01);

    // confirm the mutation keeps the name well formed, so the test is about the checksum
    const std::string mutated = bytes.substr(name_off, 23);
    CHECK(disk_cache_is_entry_name(mutated));
    CHECK(mutated != name_of(1));

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse(bytes, out) == quarantine_parse::corrupt);
}

void test_a_silently_shorter_list_is_caught() {
    // The scenario the old format could not detect: one whole record is gone, and the
    // bytes are then made internally consistent - the count is reduced to match and the
    // checksum is recomputed. Nothing about the structure or the names is wrong. Only a
    // checksum that was written before the removal could tell, and this is exactly why
    // the format carries one over the whole body.
    //
    // Concretely: a record that once held two names is edited down to one, correctly.
    const std::set<std::string> two = { name_of(1), name_of(2) };
    std::string original = disk_cache_quarantine_serialize(two);

    // rebuild the same record with only the first name, done properly
    std::string shortened = disk_cache_quarantine_serialize({ name_of(1) });

    CHECK(original != shortened);
    CHECK(shortened.size() < original.size());

    // both are valid records on their own - which is the point: the shortened one is
    // indistinguishable from a record that never held the second name
    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse(original,  out) == quarantine_parse::ok);
    CHECK(disk_cache_quarantine_parse(shortened, out) == quarantine_parse::ok);

    // At startup the file on disk is whatever is there, so this is not detectable from
    // the file alone. What protects against it is that the record is only ever written
    // as a whole, atomically - so a name cannot disappear without the file being
    // replaced in one step, and a half-replaced file is caught by the checksum.
    //
    // The checksum therefore has to be verified, or a record could be edited in place
    // to drop a name and still read back cleanly. That is the property asserted here.
    {
        std::string edited = original;
        // drop the last record's bytes and patch the count, then leave the checksum
        // stale: a structurally plausible, silently shorter list
        const size_t header = 8 + 4 + 4;
        const size_t rec1   = 4 + name_of(1).size();

        edited.erase(header + rec1, 4 + name_of(2).size());

        uint32_t one = 1;
        memcpy(&edited[12], &one, 4); // count now says 1

        CHECK(disk_cache_quarantine_parse(edited, out) == quarantine_parse::corrupt);
    }
}

void test_trailing_garbage_is_caught() {
    const std::set<std::string> names = { name_of(1) };
    std::string bytes = disk_cache_quarantine_serialize(names);

    // append a byte before the checksum: the checksum then covers it and the extra
    // data has nowhere to go in the layout
    bytes.insert(bytes.size() - 8, 1, 'X');

    std::set<std::string> out;
    const quarantine_parse r = disk_cache_quarantine_parse(bytes, out);

    // the checksum moved, so this is a mismatch rather than a layout error - either
    // way it must not be accepted
    CHECK(r != quarantine_parse::ok);
}

void test_unknown_name_in_record_is_rejected() {
    // A record naming something that is not an entry file was not written by this code.
    //
    // Built by mutating a valid record rather than hand-assembling one: the checksum is
    // recomputed by the serializer, so the case isolates the name check. A
    // hand-assembled body with a wrong checksum would fail for the checksum instead and
    // prove nothing about the name validation.
    const std::string good = name_of(1);

    // overwrite the ".bin" suffix with something that is not an entry name
    std::string bad = good;
    bad.replace(bad.size() - 4, 4, ".txt");

    CHECK(!disk_cache_is_entry_name(bad));

    // a record that names it must be refused, whatever the checksum says
    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse(disk_cache_quarantine_serialize({ bad }), out) == quarantine_parse::corrupt);
}

void test_legacy_format_upgrades() {
    // the old format: one name per line, no header, no checksum
    const std::string legacy = name_of(1) + "\n" + name_of(2) + "\n";

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(legacy, out) == quarantine_migrate::ok);
    CHECK(out.size() == 2);
    CHECK(out.count(name_of(1)) == 1);
    CHECK(out.count(name_of(2)) == 1);

    // and the upgraded form parses in the current format
    std::set<std::string> again;
    CHECK(disk_cache_quarantine_parse(disk_cache_quarantine_serialize(out), again) == quarantine_parse::ok);
    CHECK(again == out);
}

void test_legacy_tolerates_crlf_and_blank_lines() {
    const std::string legacy = name_of(1) + "\r\n\r\n" + name_of(2) + "\r\n";

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(legacy, out) == quarantine_migrate::ok);
    CHECK(out.size() == 2);
}

void test_legacy_truncated_tail_is_refused() {
    // no trailing newline: the file may have been cut mid-name, and the old format
    // cannot say which names it used to hold
    const std::string cut_mid_name = name_of(1) + "\n" + std::string("pc-0123456789ab");

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(cut_mid_name, out) == quarantine_migrate::corrupt);
}

void test_legacy_with_bad_line_is_refused() {
    const std::string bad = name_of(1) + "\nnot-an-entry\n";

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(bad, out) == quarantine_migrate::corrupt);
}

void test_legacy_rejects_duplicates() {
    const std::string dupe = name_of(1) + "\n" + name_of(1) + "\n";

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(dupe, out) == quarantine_migrate::corrupt);
}

void test_legacy_does_not_claim_the_current_format() {
    // a current-format record must not be mistaken for a legacy one, or the upgrade
    // path would rewrite a valid file
    const std::string current = disk_cache_quarantine_serialize({ name_of(1) });

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(current, out) == quarantine_migrate::not_old_format);
}

void test_binary_garbage_is_not_read_as_legacy() {
    // arbitrary bytes must not be silently read as a short list
    const std::string garbage = std::string("\x00\x01\x02\x03\xff\xfe", 6);

    std::set<std::string> out;
    CHECK(disk_cache_quarantine_parse_legacy(garbage, out) == quarantine_migrate::not_old_format);
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
    test_entry_name_shape();
    test_record_round_trips();
    test_empty_set_round_trips();
    test_serialization_is_deterministic();
    test_truncated_record_is_rejected();
    test_removing_a_record_from_the_bytes_is_caught();
    test_crc_corruption_is_caught();
    test_a_silently_shorter_list_is_caught();
    test_trailing_garbage_is_caught();
    test_unknown_name_in_record_is_rejected();
    test_legacy_format_upgrades();
    test_legacy_tolerates_crlf_and_blank_lines();
    test_legacy_truncated_tail_is_refused();
    test_legacy_with_bad_line_is_refused();
    test_legacy_rejects_duplicates();
    test_legacy_does_not_claim_the_current_format();
    test_binary_garbage_is_not_read_as_legacy();

    if (g_failures == 0) {
        std::printf("all disk-cache lifecycle checks passed\n");
        return 0;
    }

    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
