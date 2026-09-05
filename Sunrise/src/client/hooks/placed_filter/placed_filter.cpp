#include "placed_filter.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::placed_filter {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * `Obj_AllocDatumAndInit`, from its entry.
 *
 * Sixteen bytes of this prologue are shared by twelve functions and twenty by eleven; twenty-four
 * are unique. Identified by the copy the reference tree describes — `movzx eax, byte [classDef+150]`
 * then `mov [object+8], al` — which appears in only two functions in the image, the other being a
 * clone helper.
 */
constexpr std::string_view kAllocatorSignatureText =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 55 41 56 41 57 48 83 EC 30 44 8B";
constexpr auto kAllocatorSignature =
    signature<signature_length(kAllocatorSignatureText)>(kAllocatorSignatureText);

/** A spawn entry names its class by tag here. */
constexpr std::ptrdiff_t kEntryClassTagOffset = 0;
/** And carries its position here, as four floats. */
constexpr std::ptrdiff_t kEntryPositionOffset = 32;
/** Tag handles start here; the package is the next 13 bits up. */
constexpr std::uint32_t kTagBase = 0x80800000U;
constexpr std::uint32_t kTagIndexBits = 13;
/** Below this a value is not a tag handle at all. */
constexpr std::uint32_t kTagCeiling = 0x82000000U;

/**
 * The hide set, read from disk at install.
 *
 * Package is the wrong key and three selections proved it. The Tower's later package
 * `city_tower_d2_0369_2` places 74% of its objects from the shared `globals_0238` package, which the
 * base map also draws on, so hiding a package either misses the dressing or takes the architecture
 * with it. Separation has to be per object list.
 *
 * The allocator is only handed the spawn entry, which names a class but not the list it came from.
 * The offline export supplies the missing half: for every one of the 161,592 entries it records the
 * class tag and the exact position, and that pair identifies one placed object. So the set is a list
 * of `(class, position)` keys generated offline from whichever lists are under test.
 *
 * It is read from a file rather than compiled in so that trying a different list costs a relaunch
 * instead of a rebuild. Ten builds went into learning that package was the wrong key; the eleventh
 * should not be needed to try the second list.
 */
/** Keys the set may hold. The largest single Tower list is 200 entries. */
constexpr std::size_t kHideSetCapacity = 8192;

/** One placed object, identified by what it is and exactly where it was authored. */
struct HideKey {
    std::uint32_t tag{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};

    [[nodiscard]] constexpr bool operator==(const HideKey& other) const noexcept = default;
};

std::array<HideKey, kHideSetCapacity> g_keys{};
std::atomic<std::size_t> g_keyCount{};

/** @return True when this exact authored object is in the hide set. */
[[nodiscard]] bool hidden_key(const HideKey& key) noexcept {
    const std::size_t count = g_keyCount.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count && index < g_keys.size(); ++index) {
        if (g_keys[index] == key) {
            return true;
        }
    }
    return false;
}

/**
 * Loads the hide set. Each line is four hex words: class, then the raw bits of x, y and z.
 * Raw bits rather than parsed floats, so a key never fails to match on a rounding difference.
 */
void load_hide_set() noexcept {
    g_keyCount.store(0, std::memory_order_release);
    core::path::Buffer hidePath;
    if (!core::path::artifact_file(L"exports\\hide_set.txt", hidePath)) {
        return;
    }
    const HANDLE file = CreateFileW(hidePath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    static std::array<char, 1 << 20> text{};
    DWORD read = 0;
    const bool ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size() - 1), &read, nullptr);
    (void)CloseHandle(file);
    if (!ok || read == 0) {
        return;
    }
    text[read] = '\0';
    std::size_t count = 0;
    const char* cursor = text.data();
    while (*cursor != '\0' && count < g_keys.size()) {
        HideKey key{};
        if (sscanf_s(cursor, "%x %x %x %x", &key.tag, &key.x, &key.y, &key.z) == 4) {
            g_keys[count++] = key;
        }
        const char* const nextLine = std::strchr(cursor, '\n');
        if (nextLine == nullptr) {
            break;
        }
        cursor = nextLine + 1;
    }
    g_keyCount.store(count, std::memory_order_release);
}

/** Where a hidden object is put. Far enough below the map that nothing reaches it. */
constexpr float kHiddenHeight = -100000.0F;

/** Packages the filter may hold. The Tower draws on 41; a selection is always far smaller. */
constexpr std::size_t kHiddenCapacity = 16;
/** Hidden objects traced individually before the log falls back to counting them. */
constexpr std::uint32_t kTracedHides = 24;
/** A running total is written every this many hides, so the trace cap does not hide the scale. */
constexpr std::uint32_t kTotalInterval = 100;
/**
 * Wildcard package: hides every object with a tag-shaped class.
 *
 * This is a control, not a feature. Two package selections were hidden with the filter demonstrably
 * firing and nothing changed on screen, which leaves two very different explanations: the selection
 * is wrong, or this allocator is not what places visible geometry. Hiding everything separates them
 * in one run. A world that still renders says the mechanism is wrong and no selection would ever
 * have worked.
 */
constexpr std::uint16_t kAllPackages = 0xFFFFU;

using Allocator = std::uint64_t(__fastcall*)(std::uint32_t,
                                             void*,
                                             std::uint32_t,
                                             std::uint32_t) noexcept;

hooking::detour::Handle g_handle{};
std::atomic<Allocator> g_original{nullptr};
std::array<std::uint16_t, kHiddenCapacity> g_hidden{};
std::atomic<std::size_t> g_hiddenCount{};
std::atomic<std::uint32_t> g_hides{};
/**
 * Allocations seen per class package, whether hidden or not.
 *
 * The first two selections were drawn from the offline export — what the Tower's object lists
 * *declare* — and sank fewer than a hundred objects between them while changing nothing on screen.
 * The control then sank 700 in orbit alone and visibly broke it, so the mechanism was never the
 * problem: the selection was. What a destination declares and what the runtime actually allocates
 * are different sets, and only this census measures the second one.
 */
constexpr std::size_t kPackageSpan = 4096;
// A whole orbit load produced about 700 tagged allocations under the control, so an interval
// of 2000 never reported and read as though nothing was allocated at all.
constexpr std::uint32_t kCensusInterval = 100;
std::array<std::uint32_t, kPackageSpan> g_census{};
std::atomic<std::uint32_t> g_seen{};

/**
 * Every allocation this run, as class tag and position.
 *
 * The census answers "how many, and from where", which is not the question. Telling two roster
 * groups apart needs to know *which* objects one of them carries, and comparing two screenshots
 * cannot do it: the Tower currently wears several events at once, so snow, petals and autumn leaves
 * are all on screen together and no eye can say which group brought which.
 *
 * A manifest can. One line per placement, so a run with a group excluded diffs against a run
 * without it and the difference is the literal list of what that group placed. The 98 class tags
 * the community name dumps resolve then read out as names.
 *
 * Written straight through rather than buffered, because the interesting runs are the ones that
 * end in a hang or a black screen, and a buffer that is still in memory when the process dies
 * measures nothing.
 */
HANDLE g_manifest = INVALID_HANDLE_VALUE;
SRWLOCK g_manifestLock = SRWLOCK_INIT;

/** @param bits Raw float bits, as the hide key stores a position. @return That float. */
[[nodiscard]] float as_float(std::uint32_t bits) noexcept {
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

/** Records one placement. Cheap enough at the few thousand allocations a load produces. */
void record_placement(std::uint32_t tag,
                      std::uint32_t xBits,
                      std::uint32_t yBits,
                      std::uint32_t zBits) noexcept {
    const float x = as_float(xBits);
    const float y = as_float(yBits);
    const float z = as_float(zBits);
    AcquireSRWLockExclusive(&g_manifestLock);
    if (g_manifest != INVALID_HANDLE_VALUE) {
        std::array<char, 128> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "0x%08X %.2f %.2f %.2f\n",
                                          tag,
                                          static_cast<double>(x),
                                          static_cast<double>(y),
                                          static_cast<double>(z));
        if (written > 0) {
            DWORD put = 0;
            (void)WriteFile(
                g_manifest, line.data(), static_cast<DWORD>(written), &put, nullptr);
        }
    }
    ReleaseSRWLockExclusive(&g_manifestLock);
}

/** Writes the busiest packages seen so far. */
void report_census() noexcept {
    std::array<std::uint16_t, 16> top{};
    std::size_t held = 0;
    for (std::uint16_t package = 0; package < kPackageSpan; ++package) {
        if (g_census[package] == 0) {
            continue;
        }
        if (held < top.size()) {
            top[held++] = package;
        } else {
            std::size_t worst = 0;
            for (std::size_t index = 1; index < held; ++index) {
                if (g_census[top[index]] < g_census[top[worst]]) {
                    worst = index;
                }
            }
            if (g_census[package] > g_census[top[worst]]) {
                top[worst] = package;
            }
        }
    }
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=placed_filter stage=census seen=%u packages=",
                                g_seen.load(std::memory_order_relaxed));
    if (written <= 0) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(written);
    for (std::size_t index = 0; index < held; ++index) {
        const int part = std::snprintf(line.data() + length,
                                       line.size() - length,
                                       "%s0x%03X:%u",
                                       index == 0 ? "" : ",",
                                       static_cast<unsigned>(top[index]),
                                       g_census[top[index]]);
        if (part <= 0) {
            break;
        }
        length += static_cast<std::size_t>(part);
    }
    core::log::write(core::log::Channel::client, core::log::Level::info, {line.data(), length});
}

/** @param tag Class tag from a spawn entry. @return The package that authored it. */
[[nodiscard]] constexpr std::uint16_t package_of(std::uint32_t tag) noexcept {
    return static_cast<std::uint16_t>((tag - kTagBase) >> kTagIndexBits);
}

/** @param package Package id. @return True when it is currently hidden. */
[[nodiscard]] bool hidden(std::uint16_t package) noexcept {
    const std::size_t count = g_hiddenCount.load(std::memory_order_acquire);
    for (std::size_t index = 0; index < count && index < g_hidden.size(); ++index) {
        if (g_hidden[index] == kAllPackages || g_hidden[index] == package) {
            return true;
        }
    }
    return false;
}

/**
 * Replacement allocator.
 *
 * The entry is sunk and restored around the original rather than the allocation being refused,
 * because the transform is copied inside that call and a refused allocation would leave the caller
 * holding an object the engine does not retain.
 */
std::uint64_t __fastcall allocator(std::uint32_t handle,
                                   void* entry,
                                   std::uint32_t third,
                                   std::uint32_t fourth) noexcept {
    const Allocator original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return 0;
    }
    if (entry == nullptr) {
        return original(handle, entry, third, fourth);
    }
    auto* const bytes = static_cast<std::byte*>(entry);
    std::uint32_t tag = 0;
    std::memcpy(&tag, bytes + kEntryClassTagOffset, sizeof tag);
    if (tag < kTagBase || tag >= kTagCeiling) {
        return original(handle, entry, third, fourth);
    }
    // The census counts before any hiding decision. It sat below the empty-set early-out at first,
    // so with hiding disabled the function returned before ever counting and the census stayed
    // silent — measuring nothing, which reads exactly like measuring zero.
    const std::uint16_t package = package_of(tag);
    if (package < kPackageSpan) {
        ++g_census[package];
    }
    const std::uint32_t seen = g_seen.fetch_add(1, std::memory_order_relaxed);
    if (seen != 0 && seen % kCensusInterval == 0) {
        report_census();
    }
    HideKey key{tag, 0, 0, 0};
    std::memcpy(&key.x, bytes + kEntryPositionOffset, sizeof key.x);
    std::memcpy(&key.y, bytes + kEntryPositionOffset + 4, sizeof key.y);
    std::memcpy(&key.z, bytes + kEntryPositionOffset + 8, sizeof key.z);
    record_placement(tag, key.x, key.y, key.z);
    const bool byPackage = g_hiddenCount.load(std::memory_order_relaxed) != 0 && hidden(package);
    if (!byPackage && !hidden_key(key)) {
        return original(handle, entry, third, fourth);
    }

    std::array<float, 4> saved{};
    std::memcpy(saved.data(), bytes + kEntryPositionOffset, sizeof saved);
    std::array<float, 4> sunk = saved;
    sunk[1] = kHiddenHeight;
    std::memcpy(bytes + kEntryPositionOffset, sunk.data(), sizeof sunk);
    const std::uint64_t result = original(handle, entry, third, fourth);
    // The position is deliberately left sunk. Restoring it here had no visible effect at all, with
    // the hides firing, which says the object does not take its transform from the entry during
    // this call — it reads the entry later, and a restore put the object straight back. The entry
    // is a loaded tag blob, so this lasts the session and is re-read from the package next boot.

    const std::uint32_t hide = g_hides.fetch_add(1, std::memory_order_relaxed);
    if (hide != 0 && hide % kTotalInterval == 0) {
        std::array<char, core::log::kLineCapacity> total{};
        const int count = std::snprintf(
            total.data(), total.size(), "ev=placed_filter stage=total hidden=%u", hide);
        if (count > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {total.data(), static_cast<std::size_t>(count)});
        }
    }
    if (hide < kTracedHides) {
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=placed_filter stage=hide n=%u class=0x%08X pkg=0x%03X "
                                          "was=(%.1f,%.1f,%.1f)",
                                          hide,
                                          tag,
                                          static_cast<unsigned>(package),
                                          static_cast<double>(saved[0]),
                                          static_cast<double>(saved[1]),
                                          static_cast<double>(saved[2]));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return result;
}

} // namespace

/** Sets which class packages are hidden. */
void set_hidden_packages(const std::uint16_t* packages, std::size_t count) noexcept {
    g_hiddenCount.store(0, std::memory_order_release);
    if (packages == nullptr) {
        return;
    }
    const std::size_t held = count < g_hidden.size() ? count : g_hidden.size();
    for (std::size_t index = 0; index < held; ++index) {
        g_hidden[index] = packages[index];
    }
    g_hiddenCount.store(held, std::memory_order_release);
}

/** Attaches the filter. */
bool install_placed_filter() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kAllocatorSignature, "placed_allocator");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=placed_filter stage=install result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&allocator)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=placed_filter stage=install result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<Allocator>(g_handle.original), std::memory_order_release);
    load_hide_set();
    // Truncated per run, so the file is always exactly one run and two runs diff without bookkeeping.
    core::path::Buffer manifestPath;
    if (core::path::artifact_file(L"exports\\placed_manifest.txt", manifestPath)) {
        g_manifest = CreateFileW(manifestPath.chars.data(),
                                 GENERIC_WRITE,
                                 FILE_SHARE_READ,
                                 nullptr,
                                 CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    }
    std::array<char, core::log::kLineCapacity> line{};
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=placed_filter stage=install result=ok rva=0x%llX packages=%zu keys=%zu",
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(target) - base),
                      g_hiddenCount.load(std::memory_order_relaxed),
                      g_keyCount.load(std::memory_order_relaxed));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return true;
}

/** Detaches the filter. */
void uninstall_placed_filter() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
}

} // namespace sunrise::client::hooks::placed_filter
