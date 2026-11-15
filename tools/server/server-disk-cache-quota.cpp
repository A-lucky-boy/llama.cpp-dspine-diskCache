#include "server-disk-cache-quota.h"

#include <algorithm>

//
// Implementation notes
//
// The plan is deliberately dumb: sum the accounted bytes, then drop unprotected
// items in LRU order until the total reaches the limit. What makes it correct is
// what it refuses to do:
//
//   - a quarantined item is never a candidate. its file could not be deleted, so
//     its bytes keep occupying the disk and the plan has to work around them
//   - a protected item is never a candidate, including the provisional new entry
//     and every snapshot that would only become redundant *because* of it
//
// `fits` answers the only question the caller needs before it may delete
// anything: after reclaiming everything that is genuinely reclaimable, is the
// accounted total within the limit? If not, the caller must roll the new entry
// back instead of trading one snapshot for another.
//

disk_cache_quota_plan disk_cache_plan_quota(const std::vector<disk_cache_quota_item> & items, size_t limit) {
    disk_cache_quota_plan plan;

    for (const auto & it : items) {
        plan.total += it.size_file;
        if (it.protected_ || it.quarantined) {
            plan.protected_bytes += it.size_file;
        }
    }

    // limit 0 means "unlimited": the server maps --cache-disk-max -1 to 0, so
    // nothing is ever reclaimed and everything fits
    if (limit == 0) {
        plan.fits = true;
        return plan;
    }

    // candidates, worst (least recently used) first
    std::vector<size_t> candidates;
    candidates.reserve(items.size());

    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].protected_ || items[i].quarantined) {
            continue;
        }
        candidates.push_back(i);
    }

    std::stable_sort(candidates.begin(), candidates.end(), [&items](size_t a, size_t b) {
        return items[a].last_used < items[b].last_used;
    });

    size_t running = plan.total;

    for (const size_t idx : candidates) {
        if (running <= limit) {
            break;
        }
        running -= std::min(running, items[idx].size_file);
        plan.evict_order.push_back(idx);
    }

    plan.overshoot = running > limit ? running - limit : 0;
    plan.fits      = running <= limit;

    return plan;
}

std::vector<bool> disk_cache_replacements_safe(const std::vector<disk_cache_replacement_dep> & deps,
        const std::vector<disk_cache_quota_item> & items) {
    std::vector<bool> res;
    res.reserve(deps.size());

    for (const auto & dep : deps) {
        const bool longer_known =
            dep.longer_index < items.size() &&
            items[dep.longer_index].protected_ &&
            !items[dep.longer_index].quarantined;

        res.push_back(longer_known);
    }

    return res;
}
