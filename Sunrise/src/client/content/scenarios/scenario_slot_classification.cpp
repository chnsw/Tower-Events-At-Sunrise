/**
 * Turns the descriptors one placed object declares into the slots activity message 5 publishes.
 * Nothing here reads a package: the walk that follows a descriptor chain owns that, and this owns
 * what the descriptors already say. Keeping the two apart is what lets the classification be
 * checked without an installed content tree.
 */

#include <algorithm>

#include "internal.h"

namespace sunrise::client::content::scenarios {
namespace {

namespace tables = middleware::content::packages::tables;

} // namespace

/** Records one descriptor as a slot of the object being resolved. */
void record_slot(RosterStorage& storage, const tables::SlotDescriptor& descriptor) noexcept {
    if (descriptor.slotType == 0 || descriptor.slotType > layouts::kMaximumSlotType
        || descriptor.slotIndex >= layouts::kRosterSlotCapacity) {
        return;
    }
    for (std::size_t slot = 0; slot < storage.slotCount; ++slot) {
        if (storage.slots[slot].index == descriptor.slotIndex) {
            return;
        }
    }
    if (storage.slotCount == storage.slots.size()) {
        storage.slotsOverflowed = true;
        return;
    }
    const std::uint8_t flags = slot_flags(descriptor.authSchema, descriptor.senseSchema);
    storage.slots[storage.slotCount] = {descriptor.slotIndex,
                                        static_cast<std::uint8_t>(descriptor.slotType), flags};
    ++storage.slotCount;
    // Remember what this type looks like, so a slot elsewhere whose descriptor cannot be reached
    // can still be published with the right flags rather than with none.
    storage.typeFlags[descriptor.slotType] |= flags;
    storage.typeFlagsKnown[descriptor.slotType] = true;
}

/**
 * Fills one candidate group, in slot-index order.
 *
 * A slot's type comes from the object's own declared slot array; a descriptor only enriches it with
 * flags. That ordering matters: deriving the slot list FROM the descriptors instead refuses any
 * group whose chain cannot be walked in full, and every Tower seasonal event is exactly that case -
 * measured 2026-08-26, all seven collect declared-1 descriptors because their last declared slot
 * (the Dawning's is type 61) has no handle to follow. Refusing them is what removed every event
 * from the roster. A slot with no descriptor is published with its declared type and no flags,
 * which is what the working build did before the roster rewrite.
 */
bool fill_slots(RosterStorage& storage,
                std::span<const std::byte> objectBlob,
                const tables::Array& declaredSlots,
                std::size_t declaredSlotCount,
                layouts::RosterGroup& group) noexcept {
    // The declared array is no longer read here, but stays in the signature: it is what a future
    // fix would use if the unreachable descriptor is ever recovered.
    (void)objectBlob;
    (void)declaredSlots;
    if (storage.slotsOverflowed || declaredSlotCount == 0
        || declaredSlotCount > layouts::kRosterSlotCapacity) {
        return false;
    }
    // Publish ONLY the slots whose descriptor was actually reached.
    //
    // Padding out to the declared count was measured on 2026-08-27 and hangs the client at
    // `activity:initial_slice_set_loading` step 36 forever, on two separate runs, whether the
    // padded slot's flags are zero or inherited from its type. The host has no descriptor for
    // that slot and so can never seed a record for it, and the client will not apply the bubble
    // until every record it is told about is seeded. Naming fewer slots is the only remaining
    // way to publish a group whose chain cannot be walked in full.
    if (storage.slotCount == 0) {
        return false;
    }
    const auto last = storage.slots.begin() + static_cast<std::ptrdiff_t>(storage.slotCount);
    std::sort(storage.slots.begin(), last, [](const SlotRecord& first, const SlotRecord& second) {
        return first.index < second.index;
    });
    for (std::size_t slot = 0; slot < storage.slotCount; ++slot) {
        group.slotTypes[slot] = storage.slots[slot].type;
        group.slotFlags[slot] = storage.slots[slot].flags;
        group.slotIndices[slot] = storage.slots[slot].index;
    }
    group.slotCount = static_cast<std::uint16_t>(storage.slotCount);
    return true;
}

/** Unused legacy path kept out of the build. */
bool fill_slots_descriptor_only(RosterStorage& storage,
                                std::size_t declaredSlotCount,
                                layouts::RosterGroup& group) noexcept {
    if (storage.slotsOverflowed || storage.slotCount == 0
        || storage.slotCount != declaredSlotCount) {
        return false;
    }
    const auto last = storage.slots.begin() + static_cast<std::ptrdiff_t>(storage.slotCount);
    // The client reads each block independently, but ascending order is what the captured bodies
    // carry and it keeps a body diffable against them.
    std::sort(storage.slots.begin(), last, [](const SlotRecord& first, const SlotRecord& second) {
        return first.index < second.index;
    });
    for (std::size_t slot = 0; slot < storage.slotCount; ++slot) {
        group.slotTypes[slot] = storage.slots[slot].type;
        group.slotFlags[slot] = storage.slots[slot].flags;
        group.slotIndices[slot] = storage.slots[slot].index;
    }
    group.slotCount = static_cast<std::uint16_t>(storage.slotCount);
    return true;
}

} // namespace sunrise::client::content::scenarios
