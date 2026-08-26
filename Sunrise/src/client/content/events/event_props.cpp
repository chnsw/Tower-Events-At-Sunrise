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

#include "../../../core/logging/log.h"
#include "../../hooks/teleport/runtime.h"
#include "../../patterns/image_scan.h"

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
    void* const component = hooks::teleport::local_player_component();
    if (component == nullptr) {
        return kAreaAnchors.size();
    }
    hooks::teleport::Vector at{};
    if (!hooks::teleport::read_position(component, at)) {
        return kAreaAnchors.size();
    }
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
[[nodiscard]] std::uint32_t place_one(std::uint32_t tag, const Placement& at) noexcept {
    std::uint32_t handle = kInvalidDatum;
    __try {
        PlacementStorage storage{};
        reset_storage(storage);
        bool initialized = g_initialize(storage.bytes.data(), tag) != 0;
        if (!initialized && g_directInitialize != nullptr) {
            reset_storage(storage);
            initialized = g_directInitialize(storage.bytes.data(), tag) != 0;
        }
        if (!initialized) {
            g_lastStep = "initialize";
            return kInvalidDatum;
        }
        void* const descriptor = descriptor_of(storage);
        // The authored rotations on these entries are identity or a half turn, and identity is
        // correct for a decoration, so no quaternion table is carried for no visible difference.
        const std::array<float, 4> rotation{0.0F, 0.0F, 0.0F, 1.0F};
        const std::array<float, 4> position{at.x, at.y, at.z, 1.0F};
        std::memcpy(static_cast<std::byte*>(descriptor) + kDescriptorRotation,
                    rotation.data(),
                    sizeof rotation);
        std::memcpy(static_cast<std::byte*>(descriptor) + kDescriptorPosition,
                    position.data(),
                    sizeof position);
        *(static_cast<std::uint8_t*>(descriptor) + kDescriptorFlags) = kAuthoredFlagByte;
        (void)g_instantiate(&handle, descriptor);
        if (handle == kInvalidDatum && g_factory != nullptr) {
            (void)g_factory(&handle, descriptor);
        }
        if (handle == kInvalidDatum) {
            g_lastStep = "instantiate";
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_lastStep = "fault";
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

} // namespace

void place_event_props() noexcept {
    if (!bind()) {
        return;
    }
    // Only the area the player is actually in.
    const std::size_t index = occupied_area();
    // Report where the player is and which area that resolves to, a handful of times. Without this
    // a gate that never opens is completely silent, which cost a whole walk-through to discover.
    {
        static std::atomic_uint32_t told{0};
        if (told.fetch_add(1, std::memory_order_relaxed) < 12) {
            void* const component = hooks::teleport::local_player_component();
            hooks::teleport::Vector at{};
            const bool read = component != nullptr
                              && hooks::teleport::read_position(component, at);
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
            if (place_one(kDawningDecoration, at) != kInvalidDatum) {
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
