#include "../../../middleware/content/packages/tables/roster_intersection.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"
#include <array>
#include <cstdio>

#include "../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

/** How many hops the chain from a handle to a descriptor blob may take. */
constexpr std::size_t kChainDepthLimit = 8;

/**
 * Records one descriptor as a slot of the object being resolved.
 * @param context Roster storage.
 * @param descriptor Descriptor read from a placed-object blob.
 * @return Always true, because a descriptor this pass cannot use is ordinary.
 */
bool collect_slot(void* context, const tables::SlotDescriptor& descriptor) noexcept {
    record_slot(*static_cast<RosterStorage*>(context), descriptor);
    return true;
}

/**
 * Follows one placed handle to its descriptor blob and records what it declares.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param handle Tag from a placed object's per-bubble sub-block.
 * @param registryKey Registry key the descriptors must name.
 */
void follow_handle(const reader::Source& source,
                   reader::Scratch& scratch,
                   RosterStorage& storage,
                   std::uint32_t handle,
                   std::uint32_t registryKey) noexcept {
    std::uint32_t tag = handle;
    for (std::size_t depth = 0; depth < kChainDepthLimit; ++depth) {
        std::uint32_t classId = 0;
        ++storage.reads;
        if (!reader::read_tag(source, scratch, tag, storage.chain, classId)) {
            // Every Tower event group is short by exactly its last slot. Name the handle whose
            // chain cannot be read, so an absent package can be told from a malformed chain.
            if (tables::is_event_roster_key(registryKey)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int written = std::snprintf(
                    line.data(), line.size(),
                    "ev=event_chain key=0x%08X handle=0x%08X tag=0x%08X depth=%zu result=unreadable",
                    registryKey, handle, tag, depth);
                if (written > 0) {
                    core::log::write(core::log::Channel::state, core::log::Level::warn,
                                     {line.data(), static_cast<std::size_t>(written)});
                }
            }
            return;
        }
        if (classId == tables::kPlacedObjectClass) {
            (void)tables::visit_slot_descriptors(
                storage.chain, tag, registryKey, &collect_slot, &storage);
            return;
        }
        std::uint32_t next = 0;
        if (!tables::next_descriptor_tag(storage.chain, classId, next)) {
            if (tables::is_event_roster_key(registryKey)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int written = std::snprintf(
                    line.data(), line.size(),
                    "ev=event_chain key=0x%08X handle=0x%08X tag=0x%08X class=0x%08X depth=%zu "
                    "result=no_next",
                    registryKey, handle, tag, classId, depth);
                if (written > 0) {
                    core::log::write(core::log::Channel::state, core::log::Level::warn,
                                     {line.data(), static_cast<std::size_t>(written)});
                }
            }
            return;
        }
        tag = next;
    }
}

/**
 * Collects every descriptor one group object declares, over all of its per-bubble sub-blocks.
 * Every leaf is followed: one leaf is one slot, so stopping early would drop slots rather than
 * merely leave a slot type unresolved.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage receiving the descriptors.
 * @param objectBlob Whole placed-object bytes.
 * @param registryKey Registry key the descriptors must name.
 */
void collect_descriptors(const reader::Source& source,
                         reader::Scratch& scratch,
                         RosterStorage& storage,
                         std::span<const std::byte> objectBlob,
                         std::uint32_t registryKey) noexcept {
    tables::Array bubbles{};
    if (!tables::object_bubbles(objectBlob, bubbles)) {
        return;
    }
    for (std::uint64_t index = 0; index < bubbles.count; ++index) {
        tables::ObjectBubble bubble{};
        if (!tables::object_bubble_at(objectBlob, bubbles, index, bubble)) {
            return;
        }
        for (std::uint64_t slot = 0; slot < bubble.handleCount; ++slot) {
            std::uint32_t handle = 0;
            if (!tables::object_placed_handle_at(objectBlob, bubble, slot, handle)) {
                return;
            }
            follow_handle(source, scratch, storage, handle, registryKey);
        }
    }
}

/** @param storage Working storage. @param tag Object tag. @return Its memo slot, or capacity. */
[[nodiscard]] std::size_t memo_slot(const RosterStorage& storage, std::uint32_t tag) noexcept {
    std::size_t probe = tag % kObjectMemoCapacity;
    for (std::size_t step = 0; step < kObjectMemoCapacity; ++step) {
        if (storage.memo[probe].tag == 0 || storage.memo[probe].tag == tag) {
            return probe;
        }
        probe = (probe + 1) % kObjectMemoCapacity;
    }
    return kObjectMemoCapacity;
}

} // namespace

/**
 * Finds the roster group of one placed object, reading it only the first time it is seen.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage.
 * @param storage Working storage for this pass.
 * @param objectTag Tag from an object registry.
 * @param group Receives the roster group index, or the not-a-group sentinel.
 * @return True when the object was read or was already known.
 */
bool resolve_object(const reader::Source& source,
                    reader::Scratch& scratch,
                    RosterStorage& storage,
                    std::uint32_t objectTag,
                    std::uint16_t& group) noexcept {
    group = kNotARosterGroup;
    const std::size_t slot = memo_slot(storage, objectTag);
    if (slot == kObjectMemoCapacity) {
        return false;
    }
    if (storage.memo[slot].tag == objectTag) {
        group = storage.memo[slot].group;
        return true;
    }
    storage.memo[slot].tag = objectTag;
    storage.memo[slot].group = kNotARosterGroup;
    ++storage.reads;
    if (!reader::read_tag(source, scratch, objectTag, storage.object)) {
        return true;
    }

    layouts::RosterGroup candidate{};
    tables::Array declared{};
    // Does any placed object in this install actually carry a known Tower event registry key?
    // The six keys were attributed on the pre-merge build; nothing since has observed one. Report
    // every hit with what it declares, before any admission gate can hide it.
    {
        std::uint32_t probeKey = 0;
        if (tables::object_key(storage.object, probeKey)) {
            constexpr std::array<std::uint32_t, 7> kEventKeys{
                0x7C6DE64FU, 0x27060E6CU, 0x6CEFCC01U, 0xD5B68262U,
                0x00ACD208U, 0x4F4ED92FU, 0x50CC9C7DU};
            for (const std::uint32_t key : kEventKeys) {
                if (probeKey != key) {
                    continue;
                }
                tables::Array probeSlots{};
                const bool hasSlots = tables::object_slots(storage.object, probeSlots);
                std::array<char, core::log::kLineCapacity> line{};
                const int written = std::snprintf(
                    line.data(), line.size(),
                    "ev=event_key tag=0x%08X key=0x%08X slots=%u wire=%u",
                    objectTag, probeKey,
                    hasSlots ? static_cast<unsigned>(probeSlots.count) : 0U,
                    tables::carries_roster_slot(storage.object) ? 1U : 0U);
                if (written > 0) {
                    core::log::write(core::log::Channel::state, core::log::Level::warn,
                                     {line.data(), static_cast<std::size_t>(written)});
                }
                break;
            }
        }
    }
    // Admission. carries_roster_slot stays the general gate: removing it entirely was measured on
    // 2026-08-26 and saturates kRosterGroupCapacity (groups=512) before the walk finishes, which
    // aborts destinations wholesale, so the original comment's warning is correct.
    //
    // It is not sufficient on its own. Every Tower seasonal event is carried by one placed object
    // with its own registry key, and all seven declare real slots (2 to 29) while declaring NO
    // wire slot type - measured 2026-08-26, wire=0 on every one. So the wire-type gate rejects
    // every event, which is why the Tower published one group instead of thirty-six and no
    // per-bubble sub-block at all. Admitting them by key restores the events and costs seven
    // groups, and makes each event independently selectable through the existing exclude file.
    if (!tables::object_key(storage.object, candidate.registryKey) || candidate.registryKey == 0
        || !(tables::carries_roster_slot(storage.object)
             || tables::is_event_roster_key(candidate.registryKey))
        || !tables::object_slots(storage.object, declared) || declared.count == 0
        || declared.count > layouts::kRosterSlotCapacity) {
        return true;
    }
    storage.slotCount = 0;
    storage.slotsOverflowed = false;
    collect_descriptors(source, scratch, storage, storage.object, candidate.registryKey);
    // Why an event group is dropped: fill_slots demands slotCount == declaredSlotCount exactly,
    // and record_slot silently skips a descriptor whose type is 0 or above kMaximumSlotType, or
    // whose index repeats. Report the two counts for the event objects so a resolution failure can
    // be told from a classification one.
    if (tables::is_event_roster_key(candidate.registryKey)) {
        std::array<char, core::log::kLineCapacity> line{};
        int written = std::snprintf(
            line.data(), line.size(),
            "ev=event_fill tag=0x%08X key=0x%08X declared=%u collected=%zu overflow=%u",
            objectTag, candidate.registryKey, static_cast<unsigned>(declared.count),
            storage.slotCount, storage.slotsOverflowed ? 1U : 0U);
        // Which slot indices were actually recorded. Every event group is short by exactly one,
        // so the gap in this list names the descriptor record_slot refused.
        for (std::size_t entry = 0; entry < storage.slotCount && written > 0; ++entry) {
            const int more = std::snprintf(
                line.data() + written, line.size() - static_cast<std::size_t>(written),
                " %u/%u", static_cast<unsigned>(storage.slots[entry].index),
                static_cast<unsigned>(storage.slots[entry].type));
            if (more <= 0) {
                break;
            }
            written += more;
        }
        if (written > 0) {
            core::log::write(core::log::Channel::state, core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (!fill_slots(storage, declared.count, candidate)) {
        // The client registers a record per slot the object declares and refuses its whole apply
        // while any record in the current bubble is unseeded, so a group missing one descriptor is
        // dropped rather than published short.
        ++storage.unresolvedGroups;
        return true;
    }
    candidate.objectTag = objectTag;
    // One key may carry different layouts in different activities, so only exact layouts reuse.
    for (std::size_t index = 0; index < storage.groupCount; ++index) {
        if (same_group_layout(storage.groups[index], candidate)) {
            storage.memo[slot].group = static_cast<std::uint16_t>(index);
            group = storage.memo[slot].group;
            return true;
        }
    }
    if (storage.groupCount == layouts::kRosterGroupCapacity) {
        return false;
    }
    storage.groups[storage.groupCount] = candidate;
    storage.memo[slot].group = static_cast<std::uint16_t>(storage.groupCount);
    group = storage.memo[slot].group;
    ++storage.groupCount;
    return true;
}

} // namespace sunrise::client::content::scenarios
