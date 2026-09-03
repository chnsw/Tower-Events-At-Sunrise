#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <string_view>

#include "../../../../../core/filesystem/path.h"
#include "../../../../../core/logging/log.h"
#include "../../../../../core/settings/rule_text.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/activity/defaults/activity_defaults_snapshot.h"
#include "../../../../../state/activity/destination/activity_destination_spawn_binding.h"
#include "../../../../../state/activity/membership/activity_membership_query.h"
#include "../../../../../state/activity/runtime.h"
#include "../../../../../state/build_data/runtime.h"
#include "../../../../../state/runtime/runtime.h"
#include "activity_arrival.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

namespace layouts = state::build_data::scenarios;

/**
 * The type-17 lifetime state the roster reports.
 * Only 3, 6 and 10 are safe: spawn gate G4 indexes a jump table with no bounds check, so any other
 * value is a wild jump, not a refusal. 3 is what a live activity measured.
 */
constexpr std::uint8_t kLifetimeState = 3;
/** Sends whose state byte moves regardless. A body absorbed while the world loads needs them. */
constexpr std::uint8_t kWarmupSends = 3;
/** The state byte is stored biased into one signed byte, so the sequence stays inside this. */
constexpr std::uint8_t kStateSequenceWrap = 128;
/** Standard 32-bit FNV-1a basis and prime fold the group set into one comparable value. */
constexpr std::uint32_t kFoldBasis = 2166136261U;
constexpr std::uint32_t kFoldPrime = 16777619U;
/** Only a type-13 slot binds the player, so only a group holding one may carry the key. */
constexpr std::uint8_t kSlotTypeParticipation = 13;
/** The join request names its character in the low half of the SOID, so compare on that half. */
constexpr std::uint64_t kIdentityLowMask = 0xFFFFFFFFULL;
/** The type-17 lifetime slot. A group carrying it or a type-13 slot holds the activity state. */
constexpr std::uint8_t kSlotTypeLifetime = 17;

/**
 * Registry keys the roster must not publish, from `roster_exclude_keys.txt` beside settings.json.
 *
 * The Tower's seasonal events are per-bubble roster groups, one key per event per area. Showing one
 * event is subtractive: every other event's keys are withheld. The list is a file rather than a
 * setting because it changes every run and settings.json has no array parsing; it is read once per
 * run, so a change costs a relaunch and not a rebuild. One hex key per line, `0x` optional, `#`
 * comments.
 *
 * A group carrying the activity state (a type-13 or type-17 slot) is never excluded whatever the
 * file says: withholding it stalls the boot at step 36, which presents as a hang, not a bare Tower.
 */
constexpr std::size_t kExcludedKeyCapacity = 64;
std::array<std::uint32_t, kExcludedKeyCapacity> g_excludedKeys{};
std::atomic<std::size_t> g_excludedCount{0};
std::atomic_bool g_excludedLoaded{false};

void load_excluded_keys() noexcept {
    if (g_excludedLoaded.load(std::memory_order_acquire)) {
        return;
    }
    static std::array<char, core::rule_text::kRuleTextCapacity> text{};
    std::size_t count = 0;
    if (core::path::read_artifact_text(L"roster_exclude_keys.txt", text)) {
        // The presets write `0x` prefixes. The cursor reads bare hex, so blank the prefix out
        // where it starts a field; a bare `0` would otherwise be read and dropped as key zero.
        for (std::size_t at = 0; at + 1 < text.size() && text[at] != '\0'; ++at) {
            const bool prefix = text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X');
            const bool starts = at == 0 || text[at - 1] == ' ' || text[at - 1] == '\t'
                                || text[at - 1] == '\n' || text[at - 1] == '\r';
            if (prefix && starts) {
                text[at] = ' ';
                text[at + 1] = ' ';
            }
        }
        core::rule_text::Cursor rules{text.data()};
        while (count < g_excludedKeys.size() && rules.seek_field()) {
            const std::uint32_t key = rules.read_hex();
            if (key != 0) {
                g_excludedKeys[count++] = key;
            }
        }
    }
    std::array<char, core::log::kLineCapacity> line{};
    int used = std::snprintf(line.data(), line.size(), "ev=roster stage=keys excluded=%zu", count);
    for (std::size_t index = 0; index < count && used > 0
                                && static_cast<std::size_t>(used) < line.size();
         ++index) {
        used += std::snprintf(line.data() + used, line.size() - static_cast<std::size_t>(used),
                              " 0x%08X", g_excludedKeys[index]);
    }
    if (used > 0) {
        core::log::write(core::log::Channel::server, core::log::Level::info,
                         {line.data(), (std::min)(static_cast<std::size_t>(used), line.size() - 1)});
    }
    g_excludedCount.store(count, std::memory_order_release);
    g_excludedLoaded.store(true, std::memory_order_release);
}

/** @param key Registry key about to be published. @return True when the file withholds it. */
[[nodiscard]] bool key_excluded(std::uint32_t key) noexcept {
    const std::size_t count = g_excludedCount.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count; ++index) {
        if (g_excludedKeys[index] == key) {
            return true;
        }
    }
    return false;
}

/** @param group Resolved roster group. @return True when it holds the activity state. */
[[nodiscard]] bool carries_activity_state(const layouts::RosterGroup& group) noexcept {
    for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
        if (group.slotTypes[slot] == kSlotTypeParticipation
            || group.slotTypes[slot] == kSlotTypeLifetime) {
            return true;
        }
    }
    return false;
}

/**
 * Finds the full authored SOID for the character the join request named.
 * The client sends a short identity form. Publishing that form binds no object, so the full SOID
 * goes out instead.
 * @param joinCharacter Character id the join request carried, or zero when it carried none.
 * @return Authored SOID of the named character, or of the selected character when nothing matches.
 */
[[nodiscard]] std::uint64_t roster_player_key(std::uint64_t joinCharacter) noexcept {
    const state::AccountState account = state::account_snapshot();
    const std::uint64_t selected = state::account::selected_character_soid(account);
    if (joinCharacter == 0) {
        return selected;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const std::uint64_t soid = account.characters[index].soid;
        if ((soid & kIdentityLowMask) == (joinCharacter & kIdentityLowMask)) {
            return soid;
        }
    }
    return selected;
}

/**
 * Copies one roster group into the encoder's fixed input.
 * @param tableIndex Roster table index the destination row names.
 * @param scratch Lock-owned roster group storage the spans point into.
 * @param slot Storage and input slot to fill, which are the same ordinal.
 * @param roster Receives the group.
 * @return True when the named group was found.
 */
[[nodiscard]] bool fill_group(std::uint16_t tableIndex,
                              Scratch& scratch,
                              std::size_t slot,
                              message::Roster& roster) noexcept {
    layouts::RosterGroup& group = scratch.rosterGroups[slot];
    if (!state::build_data::find_roster_group(tableIndex, group)) {
        return false;
    }
    roster.groups[slot].key = group.registryKey;
    roster.groups[slot].slotTypes =
        std::span<const std::uint8_t>(group.slotTypes.data(), group.slotCount);
    roster.groups[slot].slotFlags =
        std::span<const std::uint8_t>(group.slotFlags.data(), group.slotCount);
    roster.groups[slot].slotIndices =
        std::span<const std::uint16_t>(group.slotIndices.data(), group.slotCount);
    return true;
}

/**
 * Builds the per-bubble sub-blocks from the destination's per-bubble groups.
 * The row holds one bubble mask per group. The wire wants the transpose: one sub-block per
 * bubble, carrying every key that bubble registers.
 * @param layout Destination row carrying the groups and their bubble masks.
 * @param scratch Lock-owned sub-block storage the spans point into.
 * @param roster Groups already filled, whose per-bubble half starts at the top-level count.
 * @return The sub-blocks to publish, which is empty when the destination has no per-bubble group.
 */
[[nodiscard]] std::span<const message::BubbleSubBlock> fill_sub_blocks(
    std::span<const std::uint64_t> bubbleMasks, Scratch& scratch,
    const message::Roster& roster) noexcept {
    std::size_t published = 0;
    for (std::size_t bubble = 0; bubble < scratch.rosterSubBlocks.size(); ++bubble) {
        std::size_t keyCount = 0;
        for (std::size_t index = 0; index < bubbleMasks.size(); ++index) {
            if ((bubbleMasks[index] & (std::uint64_t{1} << bubble)) == 0) {
                continue;
            }
            scratch.rosterSubBlockKeys[published][keyCount] =
                roster.groups[roster.topLevelGroupCount + index].key;
            ++keyCount;
        }
        if (keyCount == 0) {
            continue;
        }
        scratch.rosterSubBlocks[published].bubble = static_cast<std::uint32_t>(bubble);
        scratch.rosterSubBlocks[published].keys =
            std::span<const std::uint32_t>(scratch.rosterSubBlockKeys[published].data(), keyCount);
        ++published;
    }
    return std::span(scratch.rosterSubBlocks).first(published);
}

/**
 * Copies the destination's published groups into the encoder's fixed input.
 * @param layout Destination row naming its groups by roster table index.
 * @param scratch Lock-owned roster group storage the spans point into.
 * @param roster Receives the groups and the group that binds the player.
 * @return True when every named group was found and one of them binds the player.
 */
[[nodiscard]] bool
fill_roster(const layouts::Definition& layout, Scratch& scratch, message::Roster& roster) noexcept {
    roster = {};
    const std::size_t groupCount =
        std::size_t{layout.rosterGroupCount} + std::size_t{layout.bubbleGroupCount};
    if (layout.rosterGroupCount == 0 || groupCount > scratch.rosterGroups.size()
        || groupCount > roster.groups.size()) {
        return false;
    }
    for (std::size_t index = 0; index < layout.rosterGroupCount; ++index) {
        if (!fill_group(layout.rosterGroups[index], scratch, index, roster)) {
            return false;
        }
    }
    // The per-bubble groups follow the top-level ones in the same array, because phase 2 seeds
    // every group the body registers and the client holds its apply back until they are all in.
    //
    // A withheld group is left out of BOTH halves - the group list and its bubble's sub-block -
    // and the survivors are compacted so each kept group still lines up with its own mask. A group
    // registered but named by no sub-block would be one the client waits on and never seeds.
    load_excluded_keys();
    std::array<std::uint64_t, layouts::kDestinationBubbleGroupCapacity> keptMasks{};
    std::size_t kept = 0;
    for (std::size_t index = 0; index < layout.bubbleGroupCount; ++index) {
        layouts::RosterGroup probe{};
        if (!state::build_data::find_roster_group(layout.bubbleGroups[index], probe)) {
            return false;
        }
        if (key_excluded(probe.registryKey) && !carries_activity_state(probe)) {
            continue;
        }
        if (!fill_group(
                layout.bubbleGroups[index], scratch, layout.rosterGroupCount + kept, roster)) {
            return false;
        }
        keptMasks[kept] = layout.bubbleGroupMasks[index];
        ++kept;
    }
    roster.topLevelGroupCount = layout.rosterGroupCount;
    roster.groupCount = std::size_t{layout.rosterGroupCount} + kept;
    roster.bubbleSubBlocks = fill_sub_blocks(std::span(keptMasks).first(kept), scratch, roster);
    // Only a top-level group can bind the player: its object is in every slice set, so the gate
    // reads it wherever the player is.
    for (std::size_t index = 0; index < roster.topLevelGroupCount && roster.playerKeyGroup == 0;
         ++index) {
        const layouts::RosterGroup& group = scratch.rosterGroups[index];
        for (std::size_t slot = 0; slot < group.slotCount; ++slot) {
            if (group.slotTypes[slot] == kSlotTypeParticipation) {
                roster.playerKeyGroup = group.registryKey;
                break;
            }
        }
    }
    return roster.playerKeyGroup != 0;
}

/** @param roster Published groups. @return One value that changes when the group set changes. */
[[nodiscard]] std::uint32_t fold_groups(const message::Roster& roster) noexcept {
    std::uint32_t folded = kFoldBasis;
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        folded = (folded ^ roster.groups[index].key) * kFoldPrime;
    }
    return folded;
}

/**
 * Picks the per-entry state byte and advances the connection's counters.
 * A burst send leaves the byte and the latched group set alone past the warm-up, so a group change
 * during a load is published by the next keepalive send instead.
 * @param session Connection-owned roster counters.
 * @param folded Current group set.
 * @param burst True for a send on the loading cadence.
 * @return The state byte to send.
 */
[[nodiscard]] std::uint8_t
next_state_sequence(Session& session, std::uint32_t folded, bool burst) noexcept {
    if (session.activityRosterSends < kWarmupSends
        || (!burst && session.activityRosterGroups != folded)) {
        session.activityRosterState =
            static_cast<std::uint8_t>((session.activityRosterState + 1) % kStateSequenceWrap);
        session.activityRosterGroups = folded;
    }
    if (session.activityRosterSends < kWarmupSends) {
        ++session.activityRosterSends;
    }
    return session.activityRosterState;
}

} // namespace

/** Resolves the one region a session publishes. */
EffectiveRegion effective_region(const state::activity::SessionBinding& binding) noexcept {
    EffectiveRegion region{};
    region.index = state::activity::membership::kAbsentRegionIndex;
    if (!state::activity::binding_matches(binding)) {
        return region;
    }
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    const state::activity::destination::DestinationSelection& selection = binding.destination;
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    // A missing layout leaves a cleared definition, and the arrival rule then returns the
    // authored fallback index.
    layouts::Definition layout{};
    static_cast<void>(state::build_data::find_scenario_layout(name, layout));
    region.arrival = arrival_slice_set(defaults.defaultDestination, selection, name, layout);
    const std::int32_t reported = state::activity::membership::reported_region(binding.sessionId);
    region.reported = reported >= 0;
    region.index = region.reported ? reported : static_cast<std::int32_t>(region.arrival);
    return region;
}

/** Resolves the region one prepared membership body publishes. */
EffectiveRegion planned_region(const state::activity::membership::PendingMutation& mutation,
                               const state::activity::SessionBinding& binding) noexcept {
    EffectiveRegion region = effective_region(binding);
    // The same rule the State merge uses, so the body and the record it will commit agree. A
    // negative index is the unset value the client sends on its way out, not a position.
    if (mutation.authoritativeInput.hasRegion
        && mutation.authoritativeInput.region.index
               > state::activity::membership::kAbsentRegionIndex) {
        region.index = mutation.authoritativeInput.region.index;
        region.reported = true;
    }
    return region;
}

/** Builds the roster body input for one session's current destination. */
RosterOutcome build_roster_snapshot(Session& session,
                                    Scratch& scratch,
                                    message::Snapshot& snapshot,
                                    std::span<char> destination,
                                    std::size_t& destinationLength,
                                    bool burst) noexcept {
    snapshot = {};
    destinationLength = 0;
    state::activity::defaults::ActivityDefaults defaults{};
    state::activity::defaults::snapshot(defaults);
    if (session.activity.role == ActivityClientRole::none
        || !state::activity::binding_matches(session.activity.session)
        || !state::activity::binding_matches(session.activity.source)) {
        return RosterOutcome::noLayout;
    }
    // Public targets keep the destination copied from their exact advertised source generation.
    const state::activity::destination::DestinationSelection& selection =
        session.activity.session.destination;
    layouts::Definition layout{};
    const std::string_view name(reinterpret_cast<const char*>(selection.packageName.data()),
                                selection.packageNameLength);
    destinationLength = (std::min)(name.size(), destination.size());
    std::copy_n(name.begin(), destinationLength, destination.begin());
    if (!state::build_data::find_scenario_layout(name, layout)) {
        return RosterOutcome::noLayout;
    }
    if (!fill_roster(layout, scratch, snapshot.roster)) {
        return RosterOutcome::noGroups;
    }

    const state::activity::defaults::FallbackPolicy& fallback =
        defaults.defaultDestination.fallback;
    // One resolution serves this body and the citizen advertisement in message 12. Two would let
    // the join descriptor land in a region record the client is not pending on.
    EffectiveRegion region{};
    region.arrival = arrival_slice_set(defaults.defaultDestination, selection, name, layout);
    if (session.activity.role == ActivityClientRole::publicTarget) {
        region.index = session.activity.advertisedRegion;
        region.reported = region.index >= 0;
    } else {
        const std::int32_t reported =
            state::activity::membership::reported_region(session.activity.source.sessionId);
        region.reported = reported >= 0;
        region.index = region.reported ? reported : static_cast<std::int32_t>(region.arrival);
    }
    if (region.index < 0) {
        return RosterOutcome::noLayout;
    }
    snapshot.patchEpoch = session.activityPatchEpoch.value;
    // The character the join named wins, resolved to its authored SOID. The client binds its
    // player by matching this value against the object registry, and the short form the join
    // carries matches nothing.
    snapshot.playerKey = roster_player_key(session.activityCharacterSoid);
    // The old encoder documents this key as message 12's member record `+16` while its own code
    // sends the character SOID. That field is the membership identity, so this sends it instead.
    if (defaults.rosterKeyFromIdentity) {
        const std::uint64_t identity =
            state::activity::membership::join_identity(session.activity.session.sessionId);
        if (identity != 0) {
            snapshot.playerKey = identity;
        }
    }
    snapshot.lifetime = kLifetimeState;
    snapshot.keyOnEveryParticipationSlot = defaults.rosterKeyOnAllSlots;
    // The participation record's `+0` latches only when the region index is known.
    snapshot.region = static_cast<std::uint32_t>(region.index);
    snapshot.hasRegion = true;
    // The spawn override always names the destination's own arrival, never the player's position.
    snapshot.spawnSliceSet = region.arrival;
    snapshot.spawnSetHash =
        state::activity::destination::attachable_spawn_set_hash(selection, fallback.spawnSetHash);
    snapshot.hasSpawnOverride =
        snapshot.spawnSetHash != 0 && snapshot.spawnSetHash != message::kAbsentSpawnSetHash;
    snapshot.stateSequence = next_state_sequence(session, fold_groups(snapshot.roster), burst);
    return RosterOutcome::published;
}

} // namespace sunrise::server::bap::encrypted::push::activity
