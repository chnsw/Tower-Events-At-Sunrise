#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>

#include "../../../core/logging/log.h"

#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/**
 * Orders the safe groups the way the destination publishes them.
 * A group that binds the player or reports the lifetime comes first, then one reached through the
 * destination's own registry array, then the lower key.
 * @return True when left publishes before right.
 */
[[nodiscard]] bool publishes_first(const Candidate& left, const Candidate& right) noexcept {
    const bool leftFilled = left.bindsPlayer || left.reportsLifetime;
    const bool rightFilled = right.bindsPlayer || right.reportsLifetime;
    if (leftFilled != rightFilled) {
        return leftFilled;
    }
    if (left.primaryRegistry != right.primaryRegistry) {
        return left.primaryRegistry;
    }
    return left.key < right.key;
}

/**
 * Writes the top-level half: the candidates whose key is in every slice set.
 * @param walk Accumulator for one destination.
 * @param row Destination row receiving its group indices.
 */
void publish_top_level(Walk& walk, layouts::Definition& row) noexcept {
    std::array<std::uint32_t, tables::kRosterKeyCapacity> safe{};
    std::size_t safeCount = 0;
    if (!tables::safe_roster_keys(walk.intersection, safe, safeCount) || safeCount == 0) {
        return;
    }
    std::array<Candidate, tables::kRosterKeyCapacity> kept{};
    std::size_t keptCount = 0;
    for (std::size_t index = 0; index < walk.candidateCount; ++index) {
        const Candidate& candidate = walk.candidates[index];
        const auto last = safe.begin() + static_cast<std::ptrdiff_t>(safeCount);
        const bool keep = std::find(safe.begin(), last, candidate.key) != last;
        if (keep && keptCount < kept.size()) {
            kept[keptCount++] = candidate;
        }
    }
    std::sort(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(keptCount), publishes_first);
    // A roster missing either filled type seeds nothing the spawn gate reads, so publish none.
    bool binds = false;
    bool reports = false;
    for (std::size_t index = 0; index < keptCount; ++index) {
        binds = binds || kept[index].bindsPlayer;
        reports = reports || kept[index].reportsLifetime;
    }
    if (!binds || !reports) {
        return;
    }
    const std::size_t published = (std::min)(keptCount, layouts::kDestinationGroupCapacity);
    for (std::size_t index = 0; index < published; ++index) {
        row.rosterGroups[index] = kept[index].group;
    }
    row.rosterGroupCount = static_cast<std::uint8_t>(published);
}

/**
 * Writes the per-bubble half: the candidates whose key is in some slice sets and not all.
 * @param walk Accumulator for one destination.
 * @param row Destination row receiving its per-bubble group indices and their bubbles.
 */
void publish_per_bubble(Walk& walk, layouts::Definition& row) noexcept {
    std::array<std::uint32_t, tables::kRosterKeyCapacity> keys{};
    std::array<std::uint64_t, tables::kRosterKeyCapacity> masks{};
    std::size_t partialCount = 0;
    if (!tables::partial_roster_keys(walk.intersection, keys, masks, partialCount)
        || partialCount == 0) {
        return;
    }
    std::size_t published = 0;
    for (std::size_t index = 0;
         index < walk.candidateCount && published < layouts::kDestinationBubbleGroupCapacity;
         ++index) {
        const Candidate& candidate = walk.candidates[index];
        for (std::size_t partial = 0; partial < partialCount; ++partial) {
            if (keys[partial] != candidate.key) {
                continue;
            }
            row.bubbleGroups[published] = candidate.group;
            row.bubbleGroupMasks[published] = masks[partial];
            ++published;
            break;
        }
    }
    row.bubbleGroupCount = static_cast<std::uint8_t>(published);
}

} // namespace

/**
 * Reports what a destination could publish against what it did.
 * publish_per_bubble stops at kDestinationBubbleGroupCapacity without saying so, unlike
 * safe_roster_keys and partial_roster_keys which both fail loudly on overflow. A destination whose
 * partial count exceeds the capacity silently loses every group past the fourth, and which four
 * survive depends on candidate order - so an event authored in a late bubble never publishes.
 */
void report_publish(const Walk& walk, const layouts::Definition& row,
                    std::size_t safeCount, std::size_t partialCount) noexcept {
    const bool truncated = partialCount > layouts::kDestinationBubbleGroupCapacity
                           || safeCount > layouts::kDestinationGroupCapacity;
    std::array<char, core::log::kLineCapacity> line{};
    int at = std::snprintf(line.data(), line.size(),
                           "ev=roster_publish tag=0x%08X cands=%zu safe=%zu->%u partial=%zu->%u%s",
                           row.tag, walk.candidateCount,
                           safeCount, static_cast<unsigned>(row.rosterGroupCount),
                           partialCount, static_cast<unsigned>(row.bubbleGroupCount),
                           truncated ? " TRUNCATED" : "");
    for (std::size_t index = 0; index < row.bubbleGroupCount && at > 0; ++index) {
        at += std::snprintf(line.data() + at, line.size() - static_cast<std::size_t>(at),
                            " m%zu=0x%llX", index,
                            static_cast<unsigned long long>(row.bubbleGroupMasks[index]));
    }
    // The raw intersection. A key is dropped as neither safe nor partial when its mask is zero,
    // meaning it was never observed in ANY slice set of this destination - which is a different
    // failure from being observed in only some. Print every key so the two can be told apart.
    if (at > 0) {
        at += std::snprintf(line.data() + at, line.size() - static_cast<std::size_t>(at),
                            " obs=0x%llX keys=%zu",
                            static_cast<unsigned long long>(walk.intersection.observedSets),
                            walk.intersection.keyCount);
        for (std::size_t index = 0; index < walk.intersection.keyCount && at > 0; ++index) {
            at += std::snprintf(line.data() + at, line.size() - static_cast<std::size_t>(at),
                                " k%zu=0x%X/0x%llX", index,
                                walk.intersection.keys[index],
                                static_cast<unsigned long long>(walk.intersection.masks[index]));
        }
    }
    if (at > 0) {
        core::log::write(core::log::Channel::state,
                         truncated ? core::log::Level::warn : core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(at)});
    }
}

/**
 * Widens every seasonal event key to every slice set this destination reaches.
 *
 * An event's group object lives in exactly one bubble's registry - measured 2026-08-27, six of the
 * Tower's seven sit in bubble 6 (the Courtyard) and the bazaar urns in bubble 0 - so the observed
 * mask advertises each event for that one bubble only, and the event renders there and nowhere
 * else. Widening the mask to the whole observed set advertises the key in every bubble, which is
 * what lets an event appear throughout the Tower rather than in the room its group object happens
 * to be registered in.
 *
 * This deliberately overrides the intersection's own safety rule, which is that a key absent from a
 * slice set crashes the client on the switch out of it. That rule is why the mask is normally
 * narrow. It is overridden only for the seven known event keys, never for anything else.
 */
void widen_event_keys(Walk& walk) noexcept {
    for (std::size_t index = 0; index < walk.intersection.keyCount; ++index) {
        if (walk.intersection.masks[index] == 0
            || !tables::is_event_roster_key(walk.intersection.keys[index])) {
            continue;
        }
        walk.intersection.masks[index] = walk.intersection.observedSets;
    }
}

/** Splits the candidates between the destination row's two lists. */
void publish_groups(Walk& walk, layouts::Definition& row) noexcept {
    widen_event_keys(walk);
    row.rosterGroupCount = 0;
    row.rosterGroups = {};
    row.bubbleGroupCount = 0;
    row.bubbleGroups = {};
    row.bubbleGroupMasks = {};
    publish_top_level(walk, row);
    // The per-bubble half is independent of the top-level one: its keys register through the
    // delta's own field 1, and a destination may reach one half and not the other.
    publish_per_bubble(walk, row);
    if (walk.candidateCount != 0) {
        std::array<std::uint32_t, tables::kRosterKeyCapacity> safe{};
        std::array<std::uint32_t, tables::kRosterKeyCapacity> pkeys{};
        std::array<std::uint64_t, tables::kRosterKeyCapacity> pmasks{};
        std::size_t safeCount = 0;
        std::size_t partialCount = 0;
        (void)tables::safe_roster_keys(walk.intersection, safe, safeCount);
        (void)tables::partial_roster_keys(walk.intersection, pkeys, pmasks, partialCount);
        report_publish(walk, row, safeCount, partialCount);
    }
}

} // namespace sunrise::client::content::scenarios
