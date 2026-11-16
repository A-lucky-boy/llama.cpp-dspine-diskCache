// The quarantine record: the durable list of entry files that must not be treated as
// usable until their state is resolved.
//
// This lives in its own translation unit because it is a format with its own
// correctness rules - a header, a record count and a checksum - and those rules need
// to be testable directly rather than only through a running server. The reader's
// behaviour on a damaged record is the whole point of the format, so it has to be
// exercisable on bytes.

#pragma once

#include <cstdint>
#include <set>
#include <string>

// How reading the record ended.
//
// "There is no record" and "there is a record I could not read" are different facts,
// and the difference decides whether a cache directory may be trusted. A missing
// record is the normal state of a cache that has never failed a write. A record that
// exists but cannot be validated means some paths may be unusable, and the only safe
// reading is that any of them might be - the file exists precisely because something
// went wrong, so failing open would contradict its purpose.
enum class disk_cache_quarantine_status {
    absent,       // no record; and its absence was confirmed
    valid,        // read, parsed and checksum-verified in full
    invalid,      // a record exists but its contents cannot be trusted
    io_failure,   // a record may exist but could not be read at all
};

// What a write to the record did.
enum class disk_cache_commit_status {
    committed,
    failed,
};

// True when `name` has the shape of an entry file this cache produces
// (`pc-<16 hex digits>.bin`). The record names entries, so a name of any other shape
// means the file was not written by this code.
bool disk_cache_is_entry_name(const std::string & name);

// Serialize `names` in the current format. Deterministic: `std::set` iterates in
// ascending order, so the same set always produces the same bytes.
std::string disk_cache_quarantine_serialize(const std::set<std::string> & names);

// How a parse attempt ended.
enum class quarantine_parse {
    ok,
    not_current_format, // no magic/version match - may be the older line format
    corrupt,            // the header matched, but the body does not hold up
};

// Parse bytes in the current format. On `ok`, `out` holds the names.
quarantine_parse disk_cache_quarantine_parse(const std::string & bytes, std::set<std::string> & out);

// How a legacy parse attempt ended.
enum class quarantine_migrate {
    ok,
    not_old_format,
    corrupt,
};

// Parse bytes in the older one-name-per-line format. This is only for the upgrade
// path: a legacy file is accepted when every line is a plausible entry name and the
// file ends on a line boundary, so a visibly truncated file is refused rather than
// read as "a list that simply has fewer names".
quarantine_migrate disk_cache_quarantine_parse_legacy(const std::string & bytes, std::set<std::string> & out);
