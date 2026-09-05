/**
 * Places the Tower's seasonal event decorations in every Tower area.
 *
 * Why this exists. The seven Tower events reach the client through the roster, and after the roster
 * fixes all seven publish together - but their authored placements live only in the Courtyard's
 * slice set. That was established two ways: a package sweep over 1,706,553 entries found the
 * Dawning's key on exactly ten placed objects, all of them the ten handles its group object names;
 * and advertising the key into the Hangar anyway rendered nothing there and crashed the client on
 * the way out, which is the failure the roster intersection's own header predicts for a key absent
 * from a slice set. So the other areas cannot be furnished by advertising. They have to be placed.
 *
 * What makes that cheap: an event's props are authored 144-byte spawn entries laid out exactly like
 * the descriptor this file builds - entity class-definition tag at +0x00, rotation at +0x10 and
 * position at +0x20. The decorations are ordinary class definitions, so the same instantiation path
 * that places the vendors places these.
 */

#include "event_props.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <Windows.h>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../hooks/teleport/runtime.h"
#include "../../player/player_position.h"
#include "../../patterns/image_scan.h"
#include "../../../state/activity/events/activity_event_selection.h"

namespace sunrise::client::content::events {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** Builds a placement descriptor from a tag. */
constexpr std::string_view kPlacementInitializeText =
    "89 54 24 10 53 48 83 EC 20 48 8B D9 83 FA FF 0F 84 ? ? ? ? 48 8D 54 24 30 "
    "48 8D 4C 24 38 E8 ? ? ? ? 8B 44 24 30 83 F8 FF 0F 84 ? ? ? ? 48 8B 15 ? ? ? ?";
constexpr auto kPlacementInitialize =
    signature<signature_length(kPlacementInitializeText)>(kPlacementInitializeText);

/** The bare factory, for when the full instantiation path refuses. */
constexpr std::string_view kObjectFactoryText =
    "40 53 48 83 EC 20 41 83 C9 FF 41 83 C8 FF 48 8B D9 E8 ? ? ? ? 48 8B C3 "
    "48 83 C4 20 5B C3";
constexpr auto kObjectFactory =
    signature<signature_length(kObjectFactoryText)>(kObjectFactoryText);

/** The full instantiation path: registers the object and applies its transform. */
constexpr std::string_view kObjectInstantiateText =
    "48 89 5C 24 20 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC A0 00 00 00";
constexpr auto kObjectInstantiate =
    signature<signature_length(kObjectInstantiateText)>(kObjectInstantiateText);

/** The fallback initialiser, for a tag the primary one refuses. */
constexpr std::string_view kDirectInitializeText =
    "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 83 FA FF 74 33 8B CA E8 ? ? ? ? "
    "48 C7 47 30 00 00 00 00 48 8B CF 48 C7 47 10 00 00 00 00";
constexpr auto kDirectInitialize =
    signature<signature_length(kDirectInitializeText)>(kDirectInitializeText);

using PlacementInitialize = std::uint8_t(__fastcall*)(void*, std::uint32_t) noexcept;
using ObjectInstantiate = std::uint32_t*(__fastcall*)(std::uint32_t*, void*) noexcept;

PlacementInitialize g_initialize = nullptr;
PlacementInitialize g_directInitialize = nullptr;
ObjectInstantiate g_instantiate = nullptr;
ObjectInstantiate g_factory = nullptr;
std::atomic_bool g_bound{false};
/**
 * Whether translated placement runs at all this session.
 * Two routes can furnish the other areas - advertising each bubble's own event carriers through
 * the roster, or placing Courtyard decorations at translated coordinates - and running both at once
 * makes what appears on screen unattributable. Placement is on only when `event_props_enable.txt`
 * exists beside settings.json, read once when the entry points are bound.
 */
std::atomic_bool g_enabled{false};
/**
 * Which areas are done.
 * The client instantiates into the slice set it currently holds, so a Bazaar coordinate refuses
 * while the player is in the Courtyard - measured 2026-08-27 as step=instantiate with every entry
 * point resolved. Each area is therefore retried until the player is standing in it, which is the
 * same rule the vendor spawner applies through its own bubble check.
 */
std::array<std::atomic_bool, 3> g_areaPlaced{};
/** One refusal line per area, so repeated polls do not flood the log. */
std::array<std::atomic_bool, 3> g_areaReported{};
/** Where the first failed placement stopped, so a silent zero can be told apart from a fault. */
const char* g_lastStep = "-";
/** The engine call in flight, so a fault names the call that raised it. */
const char* g_stage = "-";
/** The last fault: exception code, faulting instruction RVA, and the access address. */
std::atomic<std::uint32_t> g_faultCode{0};
std::atomic<std::uint64_t> g_faultRva{0};
std::atomic<std::uint64_t> g_faultAccess{0};

/** Exception filter: records where the engine call faulted, then unwinds into the handler. */
int record_fault(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto at = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
        g_faultCode.store(record.ExceptionCode, std::memory_order_relaxed);
        g_faultRva.store(at >= base ? at - base : at, std::memory_order_relaxed);
        g_faultAccess.store(record.NumberParameters >= 2 ? record.ExceptionInformation[1] : 0,
                            std::memory_order_relaxed);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
/** @return The fault label for the call in flight. Pointer identity is enough: the labels are literals. */
const char* fault_label() noexcept {
    if (g_stage == static_cast<const char*>("initialize")) { return "fault_initialize"; }
    if (g_stage == static_cast<const char*>("direct")) { return "fault_direct"; }
    if (g_stage == static_cast<const char*>("instantiate")) { return "fault_instantiate"; }
    if (g_stage == static_cast<const char*>("factory")) { return "fault_factory"; }
    return "fault";
}

constexpr std::uint32_t kInvalidDatum = 0xFFFFFFFFU;
/** Placement storage: a header the initializer reads, then the arena it builds the descriptor in. */
constexpr std::size_t kPlacementHeaderBytes = 0x40;
constexpr std::size_t kPlacementPayloadBytes = 0x800;
constexpr std::size_t kDescriptorRotation = 0x10;
constexpr std::size_t kDescriptorPosition = 0x20;
/**
 * The authored-entry flag byte.
 * 161,106 of the install's 161,592 authored spawn entries carry 0x04 here, and a zero leaves the
 * object failing the slice-set teardown's unconditional-keep exemption.
 */
constexpr std::size_t kDescriptorFlags = 0x68;
constexpr std::uint8_t kAuthoredFlagByte = 0x04;

struct PlacementStorage {
    std::array<std::byte, kPlacementHeaderBytes + kPlacementPayloadBytes> bytes{};
};

/** One decoration placement, as its 144-byte spawn entry declares it. */
struct Placement {
    float x;
    float y;
    float z;
};

/**
 * The Dawning's decoration entity and its authored Courtyard placements.
 * Read out of prop object 0x80B4A415: fourteen entries at stride 0x90 from +0x580, every one
 * naming entity 0x80C4B2F1, which is a class definition (class 0x80809C0F).
 */
constexpr std::uint32_t kDawningDecoration = 0x80C4B2F1U;
constexpr std::array<Placement, 14> kDawningCourtyard{{
    {40.18F, 49.87F, 17.00F}, {15.86F, 34.22F, 18.12F}, {48.93F, 27.65F, 20.60F},
    {7.90F, 18.80F, 24.59F}, {10.53F, -2.28F, 16.77F}, {-15.62F, -3.75F, 16.01F},
    {-8.23F, 42.60F, 25.00F}, {-13.33F, 68.44F, 30.40F}, {43.07F, 59.43F, 29.65F},
    {8.75F, 46.20F, 18.12F}, {41.14F, 10.29F, 19.53F}, {21.49F, 1.28F, 16.03F},
    {-12.60F, 10.75F, 27.11F}, {37.18F, 26.85F, 14.95F}}};

/**
 * The Guardian Games podium beside Zavala, read on 2026-09-05 from the five slot records under
 * Courtyard registry key 0x0AFC31B6 (trophy_base 0x80B4AE82, trophy_titan 0x80B4AE85,
 * trophy_warlock 0x80B4AE88, trophy_hunter 0x80B4AE8B, banner_spotlight 0x80B4AE8E), each holding
 * one authored 144-byte spawn entry. The roster publishes that group and its entities load, but
 * the client never creates them: a group's dressing is spawned from its group-level placement
 * list, and this group's list is the shared empty object 0x80BFDD7B (24 bytes) where every other
 * Courtyard event carries 11-93 KB of entries. Bungie emptied it when the 2020 Games ended and left
 * the slot records behind, so the podium can only be placed directly. The three class trophies
 * share one position; the Titan one is placed because Titans won the 2020 Games.
 */
struct AuthoredPlacement {
    std::uint32_t tag;
    Placement at;
    std::array<float, 4> rotation;
};
constexpr std::array<AuthoredPlacement, 6> kGuardianGamesPodium{{
    // Control: the Dawning decoration that the same call placed on 2026-08-27, set down beside the
    // podium. If it places while the podium pieces fault, the entities differ; if it faults too,
    // the path itself is refused here.
    {0x80C4B2F1U, {40.0F, -10.0F, 17.0F}, {0.0F, 0.0F, 0.0F, 1.0F}},
    {0x8156E846U, {38.20F, -14.99F, 16.98F}, {0.0F, 0.0F, 0.70710677F, 0.70710677F}},
    {0x8157FF55U, {38.20F, -14.99F, 16.98F}, {0.0F, 0.0F, -0.70710677F, -0.70710677F}},
    {0x81584569U, {38.20F, -14.99F, 16.98F}, {0.0F, 0.0F, 0.70710677F, 0.70710677F}},
    {0x8156E8D5U, {38.20F, -14.99F, 16.98F}, {0.0F, 0.0F, -0.70710677F, -0.70710677F}},
    {0x8156E2CAU, {37.82F, -2.64F, 11.86F}, {0.978F, 0.012F, -0.010F, 0.209F}},
}};
constexpr std::uint32_t kGuardianGamesKey = 0x0AFC31B6U;
/** The podium is placed only while the player stands within this many metres of its base. */
constexpr float kPodiumReach = 80.0F;
std::atomic_bool g_podiumPlaced{false};
std::atomic_bool g_podiumReported{false};

/**
 * Where each Tower area sits relative to the Courtyard.
 * Derived from the authored vendor coordinates already recovered for all four areas: the offset is
 * the difference between that area's vendor centroid and the Courtyard's, so a Courtyard layout
 * mirrored by this much lands in the corresponding part of the target area. This is a starting
 * placement, not authored data - the events were never authored outside the Courtyard.
 */
struct AreaOffset {
    float dx;
    float dy;
    float dz;
    const char* name;
};
constexpr std::array<AreaOffset, 3> kAreaOffsets{{
    {-139.6F, 36.0F, -16.7F, "Bazaar"},
    {129.7F, 59.3F, -10.1F, "Hangar"},
    {-153.4F, 20.8F, -42.5F, "Annex"}}};

/**
 * Where each area is, so the player's own position says which one they are standing in.
 * The Courtyard is included because it is the area a player is usually in, and an area is only
 * furnished while the player occupies it: an object created in one slice set and positioned in
 * another is outside that slice set's loaded geometry, so it is invisible, and the switch into the
 * real area tears it down before it can ever be seen. Measured 2026-08-27: placing all three areas
 * at once from the Courtyard reported placed=14 of 14 for every one of them and rendered nothing.
 */
struct AreaAnchor {
    float x;
    float y;
    float z;
};
constexpr std::array<AreaAnchor, 3> kAreaAnchors{{
    {-121.2F, 55.2F, 0.8F},    // Bazaar
    {148.1F, 78.5F, 7.4F},     // Hangar
    {-135.0F, 40.0F, -25.0F}}};  // Annex
constexpr AreaAnchor kCourtyardAnchor{18.4F, 19.2F, 17.5F};

/**
 * Which target area the player is standing in.
 * @return Its index, or the area count when the player is elsewhere - the Courtyard included.
 */
[[nodiscard]] std::size_t occupied_area() noexcept {
    // The published snapshot, not a live component read: local_player_component() answers null
    // outside the physics sync - measured 2026-08-27 as component=0 read=0 while the player was
    // standing in the Bazaar. The frame poll keeps this current for exactly that reason.
    const player::position::Snapshot player = player::position::snapshot();
    if (!player.present) {
        return kAreaAnchors.size();
    }
    const hooks::teleport::Vector at = player.position;
    const auto squared = [&at](const AreaAnchor& anchor) noexcept {
        const float dx = at[0] - anchor.x;
        const float dy = at[1] - anchor.y;
        const float dz = at[2] - anchor.z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::size_t best = kAreaAnchors.size();
    float bestDistance = squared(kCourtyardAnchor);
    for (std::size_t index = 0; index < kAreaAnchors.size(); ++index) {
        const float distance = squared(kAreaAnchors[index]);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }
    return best;
}

/**
 * Seeds the header the initialiser reads before it will build anything.
 * A zeroed header declares a zero-capacity arena and the initialiser refuses every tag - measured
 * 2026-08-27 as placed=0 of=14 in all three areas, with no other symptom.
 */
void reset_storage(PlacementStorage& storage) noexcept {
    storage = {};
    constexpr std::uint64_t zero = 0;
    constexpr std::uint64_t capacity = kPlacementPayloadBytes;
    constexpr std::uint64_t alignment = 0x10;
    std::memcpy(storage.bytes.data() + 0x00, &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x10, &zero, sizeof zero);
    std::memcpy(storage.bytes.data() + 0x18, &kInvalidDatum, sizeof kInvalidDatum);
    std::memcpy(storage.bytes.data() + 0x20, &capacity, sizeof capacity);
    std::memcpy(storage.bytes.data() + 0x28, &alignment, sizeof alignment);
    std::memcpy(storage.bytes.data() + 0x30, &zero, sizeof zero);
}

/** @return The descriptor the initializer built inside the storage arena. */
[[nodiscard]] void* descriptor_of(PlacementStorage& storage) noexcept {
    return storage.bytes.data() + kPlacementHeaderBytes;
}

/** Instantiates one decoration. @return Its handle, or the invalid datum. */
[[nodiscard]] std::uint32_t place_one(std::uint32_t tag,
                                      const Placement& at,
                                      const std::array<float, 4>& rotation) noexcept {
    std::uint32_t handle = kInvalidDatum;
    __try {
        PlacementStorage storage{};
        reset_storage(storage);
        g_stage = "initialize";
        bool initialized = g_initialize(storage.bytes.data(), tag) != 0;
        if (!initialized && g_directInitialize != nullptr) {
            reset_storage(storage);
            g_stage = "direct";
            initialized = g_directInitialize(storage.bytes.data(), tag) != 0;
        }
        if (!initialized) {
            g_lastStep = "initialize";
            return kInvalidDatum;
        }
        void* const descriptor = descriptor_of(storage);
        const std::array<float, 4> position{at.x, at.y, at.z, 1.0F};
        std::memcpy(static_cast<std::byte*>(descriptor) + kDescriptorRotation,
                    rotation.data(),
                    sizeof rotation);
        std::memcpy(static_cast<std::byte*>(descriptor) + kDescriptorPosition,
                    position.data(),
                    sizeof position);
        *(static_cast<std::uint8_t*>(descriptor) + kDescriptorFlags) = kAuthoredFlagByte;
        g_stage = "instantiate";
        (void)g_instantiate(&handle, descriptor);
        if (handle == kInvalidDatum && g_factory != nullptr) {
            g_stage = "factory";
            (void)g_factory(&handle, descriptor);
        }
        if (handle == kInvalidDatum) {
            g_lastStep = "instantiate";
        }
    } __except (record_fault(GetExceptionInformation())) {
        g_lastStep = fault_label();
        return kInvalidDatum;
    }
    return handle;
}

/** Resolves the two engine entry points once. */
[[nodiscard]] bool bind() noexcept {
    if (g_bound.load(std::memory_order_acquire)) {
        return g_initialize != nullptr && g_instantiate != nullptr;
    }
    std::byte* const initialize = scan_main_image_unique(kPlacementInitialize, "event_placement");
    std::byte* const instantiate = scan_main_image_unique(kObjectInstantiate, "event_instantiate");
    std::byte* const direct = scan_main_image_unique(kDirectInitialize, "event_direct");
    std::byte* const factory = scan_main_image_unique(kObjectFactory, "event_factory");
    g_factory = reinterpret_cast<ObjectInstantiate>(factory);
    g_directInitialize = reinterpret_cast<PlacementInitialize>(direct);
    g_initialize = reinterpret_cast<PlacementInitialize>(initialize);
    g_instantiate = reinterpret_cast<ObjectInstantiate>(instantiate);
    {
        std::array<char, 64> flag{};
        const bool enabled = core::path::read_artifact_text(L"event_props_enable.txt", flag);
        g_enabled.store(enabled, std::memory_order_release);
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         enabled ? "ev=event_props stage=gate result=enabled"
                                 : "ev=event_props stage=gate result=disabled");
    }
    g_bound.store(true, std::memory_order_release);
    return initialize != nullptr && instantiate != nullptr;
}

/** Reports one area's outcome. */
void report(const char* area, std::size_t placed, std::size_t attempted) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=event_props area=%s placed=%zu of=%zu step=%s "
                                      "init=%d direct=%d inst=%d",
                                      area,
                                      placed,
                                      attempted,
                                      g_lastStep,
                                      g_initialize != nullptr ? 1 : 0,
                                      g_directInitialize != nullptr ? 1 : 0,
                                      g_instantiate != nullptr ? 1 : 0);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         placed != 0 ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Places the Guardian Games podium once, while the event is shown and the player is near it.
 * The entities become resolvable only once the Courtyard's own content has loaded, so a refusal
 * leaves the attempt for a later poll rather than spending it.
 */
void place_podium() noexcept {
    if (g_podiumPlaced.load(std::memory_order_acquire)
        || state::activity::events::withheld(kGuardianGamesKey)) {
        return;
    }
    const player::position::Snapshot player = player::position::snapshot();
    if (!player.present) {
        return;
    }
    const hooks::teleport::Vector at = player.position;
    const Placement& base = kGuardianGamesPodium[0].at;
    const float dx = at[0] - base.x;
    const float dy = at[1] - base.y;
    const float dz = at[2] - base.z;
    if (dx * dx + dy * dy + dz * dz > kPodiumReach * kPodiumReach) {
        return;
    }
    std::size_t placed = 0;
    std::array<const char*, kGuardianGamesPodium.size()> steps{};
    std::array<std::uint32_t, kGuardianGamesPodium.size()> handles{};
    for (std::size_t index = 0; index < kGuardianGamesPodium.size(); ++index) {
        const AuthoredPlacement& entry = kGuardianGamesPodium[index];
        g_lastStep = "-";
        handles[index] = place_one(entry.tag, entry.at, entry.rotation);
        steps[index] = g_lastStep;
        if (handles[index] != kInvalidDatum) {
            ++placed;
        }
    }
    if (placed == 0) {
        // One attempt per session: a refused placement has already allocated on the way to the
        // fault (the census shows the decoration five times), so retrying only leaks objects.
        g_podiumPlaced.store(true, std::memory_order_release);
        if (!g_podiumReported.exchange(true, std::memory_order_acq_rel)) {
            for (std::size_t index = 0; index < kGuardianGamesPodium.size(); ++index) {
                std::array<char, core::log::kLineCapacity> entryLine{};
                const int put = std::snprintf(entryLine.data(),
                                              entryLine.size(),
                                              "ev=event_props area=podium tag=0x%08X handle=0x%08X step=%s "
                                              "code=0x%08X rva=0x%llX access=0x%llX",
                                              kGuardianGamesPodium[index].tag,
                                              handles[index],
                                              steps[index],
                                              g_faultCode.load(std::memory_order_relaxed),
                                              static_cast<unsigned long long>(
                                                  g_faultRva.load(std::memory_order_relaxed)),
                                              static_cast<unsigned long long>(
                                                  g_faultAccess.load(std::memory_order_relaxed)));
                if (put > 0) {
                    core::log::write(core::log::Channel::client,
                                     core::log::Level::warn,
                                     {entryLine.data(), static_cast<std::size_t>(put)});
                }
            }
            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(line.data(),
                                              line.size(),
                                              "ev=event_props area=podium placed=0 of=%zu step=%s deferred=1",
                                              kGuardianGamesPodium.size(),
                                              g_lastStep);
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        return;
    }
    g_podiumPlaced.store(true, std::memory_order_release);
    report("podium", placed, kGuardianGamesPodium.size());
}

} // namespace

void place_event_props() noexcept {
    if (!bind() || !g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    // Diagnostic only, like the translated route: the bare instantiate path faults at RVA
    // 0x59A03F for the podium pieces and for the Dawning decoration alike (2026-09-05). The podium
    // is roster content whose placement list the shipped data emptied; restoring it is a data
    // patch, not a placement.
    place_podium();
    // Only the area the player is actually in.
    const std::size_t index = occupied_area();
    // Report where the player is and which area that resolves to, a handful of times. Without this
    // a gate that never opens is completely silent, which cost a whole walk-through to discover.
    {
        // Sample periodically, not the first N calls. Logging the first twelve spent the whole
        // budget on the twelve frames after the hook installed, before the player existed, so every
        // line read component=0 and looked like a failure that was really a mistimed probe.
        static std::atomic_uint32_t ticks{0};
        const std::uint32_t tick = ticks.fetch_add(1, std::memory_order_relaxed);
        if (tick % 600 == 0 && tick < 12000) {
            const player::position::Snapshot snap = player::position::snapshot();
            const hooks::teleport::Vector at = snap.position;
            const bool read = snap.present;
            void* const component = read ? reinterpret_cast<void*>(1) : nullptr;
            std::array<char, core::log::kLineCapacity> line{};
            const int written = std::snprintf(
                line.data(), line.size(),
                "ev=event_area component=%d read=%d at=%.1f,%.1f,%.1f area=%zu",
                component != nullptr ? 1 : 0, read ? 1 : 0,
                static_cast<double>(at[0]), static_cast<double>(at[1]),
                static_cast<double>(at[2]), index);
            if (written > 0) {
                core::log::write(core::log::Channel::client, core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
    }
    if (index < kAreaOffsets.size() && !g_areaPlaced[index].load(std::memory_order_acquire)) {
        const AreaOffset& area = kAreaOffsets[index];
        std::size_t placed = 0;
        for (const Placement& source : kDawningCourtyard) {
            const Placement at{source.x + area.dx, source.y + area.dy, source.z + area.dz};
            // The authored rotations on these entries are identity or a half turn, and identity
            // is correct for a decoration.
            if (place_one(kDawningDecoration, at, {0.0F, 0.0F, 0.0F, 1.0F}) != kInvalidDatum) {
                ++placed;
            }
        }
        // Nothing placed means the engine refused here, so leave the area for a later poll rather
        // than burning its one attempt. Report the first refusal per area, once, so a walk-through
        // that shows nothing still says WHY.
        if (placed == 0) {
            if (!g_areaReported[index].exchange(true, std::memory_order_acq_rel)) {
                std::array<char, core::log::kLineCapacity> line{};
                const int written = std::snprintf(
                    line.data(), line.size(),
                    "ev=event_props area=%s placed=0 of=%zu step=%s deferred=1",
                    area.name, kDawningCourtyard.size(), g_lastStep);
                if (written > 0) {
                    core::log::write(core::log::Channel::client, core::log::Level::warn,
                                     {line.data(), static_cast<std::size_t>(written)});
                }
            }
            return;
        }
        g_areaPlaced[index].store(true, std::memory_order_release);
        report(area.name, placed, kDawningCourtyard.size());
    }
}

} // namespace sunrise::client::content::events
