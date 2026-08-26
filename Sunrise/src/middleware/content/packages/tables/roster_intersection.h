#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "scenario_reader.h"

namespace sunrise::middleware::content::packages::tables {

/**
 * A destination reaches at most this many slice sets.
 * Slice-set indices are spaced by the slice-set factor across a 512-wide region space, so this
 * is the hard bound. The widest installed destination reaches 47.
 */
inline constexpr std::size_t kSliceSetCapacity = 64;
/**
 * Roster keys tracked for one destination.
 * No installed destination reaches more than 2 objects carrying a wire slot type. This leaves
 * room to spare without a heap allocation.
 */
inline constexpr std::size_t kRosterKeyCapacity = 16;

static_assert(kSliceSetCapacity * kSliceSetIndexFactor == 512);
// One bit per slice set, so the mask must cover the whole capacity.
static_assert(kSliceSetCapacity == 64);

/**
 * Which slice sets each candidate key appears in.
 * A key is safe to send only when it appears in every slice set of the destination. The client
 * dereferences a miss unchecked, so a key missing from one crashes on the switch out of it.
 */
struct RosterIntersection {
    std::array<std::uint32_t, kRosterKeyCapacity> keys{};
    std::array<std::uint64_t, kRosterKeyCapacity> masks{};
    std::size_t keyCount{};
    /** Every slice set observed, whichever key carried it. */
    std::uint64_t observedSets{};
    /** Set when a key or a slice set did not fit, which makes the result unusable. */
    bool overflowed{};
    /** Set when a state's slice set could not be read, which makes every key unsafe. */
    bool unresolvedSet{};
};

/**
 * Records a slice set whose entry could not be read.
 * The destination still moves into that slice set and no key can be proved present in it, so no
 * key of this destination is safe to send afterwards.
 * @param state Accumulator for one destination.
 */
void observe_unresolved_slice_set(RosterIntersection& state) noexcept;

/**
 * The slot types that make an object worth publishing as a roster group.
 * Only 56 installed objects declare any of them, and the key limit above holds only for that
 * filtered set. Feeding every placed object instead overflows most destinations.
 */
inline constexpr std::array<std::uint16_t, 9> kRosterSlotTypes = {
    8, 13, 16, 17, 21, 35, 37, 41, 67};

/**
 * Tests whether one placed object is a roster candidate.
 * @param object Whole placed-object bytes.
 * @return True when it declares a slot of one of the wire types.
 */
[[nodiscard]] bool carries_roster_slot(std::span<const std::byte> object) noexcept;

/**
 * Every registry key the Tower's own bubble registries name.
 * Seasonal content is carried by placed objects that declare no wire slot type, so
 * `carries_roster_slot` rejects them and they never reach the roster. Admitting them by key is what
 * puts them back, and the key set has to cover every bubble: the six keys the knowledge graph
 * records as `event-keys` were attributed by group-testing in the Courtyard, and the Courtyard is
 * the only place they appear. Walking the Tower's scenario to its per-bubble registries - Annex
 * 0x80B4AF2D, Bazaar 0x80B4AF3E, Courtyard 0x80B4AF5F, Hangar 0x80B4AF66 - finds 124 keyed objects
 * carrying these keys, with the Bazaar's 33 and the Hangar's 41 sharing none of the six.
 *
 * Listing the Tower's keys explicitly keeps the cost bounded. Dropping the wire-type gate outright
 * admits every placed object in the install, which saturates the group table and hangs the roster
 * walk before it finishes - measured twice.
 */
inline constexpr std::array<std::uint32_t, 112> kEventRosterKeys = {
    0x009AEC2EU, 0x00ACD208U, 0x02C996DDU, 0x05053C90U, 0x05053C93U, 0x08E64D48U,
    0x099B0342U, 0x0AFC31B6U, 0x0D116593U, 0x0E4924B4U, 0x0E4924B7U, 0x15EFED42U,
    0x169C7EE9U, 0x17BEAED0U, 0x17BEAED3U, 0x18258BFAU, 0x1F708F98U, 0x1F708F9BU,
    0x2100D817U, 0x22560BFEU, 0x2488B37AU, 0x24F3CF79U, 0x263DF0C9U, 0x27060E6CU,
    0x29A3487DU, 0x2B33841AU, 0x2E2F5795U, 0x2E4B927CU, 0x2E4B927FU, 0x2EFB59ADU,
    0x2F2B8D00U, 0x3A23A717U, 0x3BC79A8DU, 0x40AEBF90U, 0x412D8185U, 0x4367863AU,
    0x43CF2C96U, 0x4C367B89U, 0x4F4ED92FU, 0x50CC9C7DU, 0x526D9E15U, 0x586C0FDFU,
    0x58B3D759U, 0x5C916B32U, 0x66508CC0U, 0x66508CC3U, 0x6733FE14U, 0x6747EE42U,
    0x6B449368U, 0x6B44936BU, 0x6C440D9CU, 0x6C8D1D54U, 0x6CEFCC01U, 0x6D3740C6U,
    0x6E087824U, 0x728E75D1U, 0x73BF1A62U, 0x78438031U, 0x7C6DE64FU, 0x8066C829U,
    0x82BA5299U, 0x8353EBD5U, 0x83C0D67EU, 0x87FE9FA7U, 0x8921C738U, 0x8A4E2843U,
    0x8A5F970CU, 0x8D8E87D8U, 0x9052672CU, 0x935429A1U, 0x941348D0U, 0x94DE15DEU,
    0x96E0A5E5U, 0x9855FF7CU, 0x9874FDE2U, 0x9D5CEE73U, 0x9E098FCCU, 0xAACA9424U,
    0xAACA9427U, 0xAC21AC48U, 0xAC21AC4BU, 0xAD4E8C09U, 0xAD4E8C0AU, 0xAE25D8EDU,
    0xAEAD9309U, 0xB5B4DF10U, 0xBE373EFDU, 0xC7B9E8F6U, 0xCD6CEC62U, 0xD227B29FU,
    0xD30F0080U, 0xD30F0083U, 0xD55899E6U, 0xD5B68262U, 0xD67F8F82U, 0xD88EF9E0U,
    0xDA01F80AU, 0xDA989AA3U, 0xDD490DD8U, 0xE7F0A95BU, 0xE8E9DF63U, 0xEE34BBABU,
    0xF25400A1U, 0xF25400A2U, 0xF36C5584U, 0xF8790DA5U, 0xFC6B8707U, 0xFEAAF75DU,
    0xFEAAF75EU, 0xFEAD0123U, 0xFFAAF8B0U, 0xFFAAF8B3U};  // renders nothing visible

/**
 * Tests whether one registry key names a Tower seasonal event object.
 * @param registryKey Registry key read from a placed object.
 * @return True when the key is one of the event keys.
 */
[[nodiscard]] bool is_event_roster_key(std::uint32_t registryKey) noexcept;

/**
 * Records a slice set the destination reaches, whether or not it holds a roster object.
 * The intersection is over every slice set of the destination, so a slice set that holds no
 * candidate still has to count. Leaving it out makes an unsafe key look safe.
 * @param state Accumulator for one destination.
 * @param sliceSetIndex Slice-set index as the entry reports it, already scaled by the factor.
 * @return True when the index is inside the region space.
 */
[[nodiscard]] bool observe_slice_set(RosterIntersection& state,
                                     std::uint32_t sliceSetIndex) noexcept;

/**
 * Records that one key appears in one slice set.
 * @param state Accumulator for one destination.
 * @param sliceSetIndex Slice-set index as the entry reports it, already scaled by the factor.
 * @param objectKey Registry key of the placed object.
 * @return True when the observation was recorded.
 */
[[nodiscard]] bool observe_roster_key(RosterIntersection& state,
                                      std::uint32_t sliceSetIndex,
                                      std::uint32_t objectKey) noexcept;

/**
 * Reports the keys present in every observed slice set.
 * @param state Accumulator for one destination.
 * @param output Receives the safe keys.
 * @param count Receives how many were written.
 * @return True when nothing overflowed and every safe key fits the output.
 */
[[nodiscard]] bool safe_roster_keys(const RosterIntersection& state,
                                    std::span<std::uint32_t> output,
                                    std::size_t& count) noexcept;

/**
 * Reports the keys present in some observed slice sets and not all, with the bubbles holding them.
 * These belong in a per-bubble sub-block: the top-level list would make the teardown sweep deref a
 * key the current slice set cannot find. One recorded bit is one bubble.
 * @param state Accumulator for one destination.
 * @param keys Receives the partially present keys.
 * @param masks Receives each key's bubbles, one bit per bubble index, in the same order.
 * @param count Receives how many were written.
 * @return True when nothing overflowed and every partial key fits the output.
 */
[[nodiscard]] bool partial_roster_keys(const RosterIntersection& state,
                                       std::span<std::uint32_t> keys,
                                       std::span<std::uint64_t> masks,
                                       std::size_t& count) noexcept;

} // namespace sunrise::middleware::content::packages::tables
