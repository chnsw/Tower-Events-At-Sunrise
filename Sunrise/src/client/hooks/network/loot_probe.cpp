#include "loot_probe.h"

#include <Windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"

namespace sunrise::client::hooks::network::loot_probe {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/** 0x4CB170: clear bit edx of the object's flag byte; drains the deferred queue when edx == 1. */
constexpr std::string_view kClearBitText =
    "48 89 5C 24 08 57 48 83 EC 20 0F B6 01 8B FA 0F B3 D0 48 8B D9 88 01 41 83 F8 01";
constexpr auto kClearBit = signature<signature_length(kClearBitText)>(kClearBitText);
/** 0x4CB400: submit one deferred job into the object's queue. */
constexpr std::string_view kSubmitText =
    "48 89 74 24 10 57 48 83 EC 20 48 8B 79 08 45 8B D1 4D 8B D8 48 8B F1 48 63 87 00 80 00 00";
constexpr auto kSubmit = signature<signature_length(kSubmitText)>(kSubmitText);
/** 0x4CB620: drain the deferred queue. */
constexpr std::string_view kDrainText =
    "48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 48 8B F1 8B FD 39 A9 00 80 00 00";
constexpr auto kDrain = signature<signature_length(kDrainText)>(kDrainText);
/** 0x3F76C0: the accessor that returns the session object whose byte 0 carries the bits. */
constexpr std::string_view kAccessorText =
    "40 53 48 83 EC 20 48 8B 1D 4B 66 2F 02 48 85 DB 0F 84 ? ? ? ? 48 89 5C 24 30 E8 ? ? ? ? 33 C3";
constexpr auto kAccessor = signature<signature_length(kAccessorText)>(kAccessorText);

using ClearBit = void(__fastcall*)(std::uint8_t*, std::uint32_t, std::uint32_t) noexcept;
using Submit = bool(__fastcall*)(void*, const char*, void*, std::uint32_t, std::uint64_t, std::uint64_t) noexcept;
using Drain = void(__fastcall*)(void*) noexcept;
using Accessor = std::uint8_t*(*)() noexcept;

std::array<hooking::detour::Handle, 3> g_handles{};
std::atomic<ClearBit> g_clear{nullptr};
std::atomic<Submit> g_submit{nullptr};
std::atomic<Drain> g_drain{nullptr};
std::atomic<Accessor> g_accessor{nullptr};

void line(const char* text, std::size_t length) noexcept {
    core::log::write(core::log::Channel::client, core::log::Level::info, {text, length});
}

std::uint8_t read_byte(const std::uint8_t* at, bool& ok) noexcept {
    __try {
        ok = true;
        return *at;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        return 0;
    }
}

std::size_t copy_name(const char* from, char* to, std::size_t cap) noexcept {
    __try {
        std::size_t n = 0;
        while (n + 1 < cap && from[n] != '\0' && from[n] >= 32 && from[n] < 127) {
            to[n] = from[n];
            ++n;
        }
        to[n] = '\0';
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        to[0] = '\0';
        return 0;
    }
}

__declspec(noinline) void __fastcall clear_bit(std::uint8_t* object,
                                               std::uint32_t bit,
                                               std::uint32_t mode) noexcept {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    bool ok = false;
    const std::uint8_t before = read_byte(object, ok);
    std::array<char, 160> text{};
    const int n = std::snprintf(text.data(), text.size(),
                                "ev=lootprobe stage=clear bit=%u mode=%u flags_before=0x%02X caller=0x%llX",
                                bit, mode, ok ? before : 0xFF,
                                static_cast<unsigned long long>(caller >= base ? caller - base : caller));
    if (n > 0) {
        line(text.data(), static_cast<std::size_t>(n));
    }
    const ClearBit original = g_clear.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(object, bit, mode);
    }
}

__declspec(noinline) bool __fastcall submit(void* object,
                                            const char* name,
                                            void* arg,
                                            std::uint32_t size,
                                            std::uint64_t a5,
                                            std::uint64_t a6) noexcept {
    std::array<char, 96> nm{};
    copy_name(name, nm.data(), nm.size());
    bool ok = false;
    const std::uint8_t flags = read_byte(static_cast<const std::uint8_t*>(object), ok);
    std::array<char, 200> text{};
    const int n = std::snprintf(text.data(), text.size(),
                                "ev=lootprobe stage=submit name=%s size=%u flags=0x%02X",
                                nm.data(), size, ok ? flags : 0xFF);
    if (n > 0) {
        line(text.data(), static_cast<std::size_t>(n));
    }
    const Submit original = g_submit.load(std::memory_order_acquire);
    return original != nullptr ? original(object, name, arg, size, a5, a6) : false;
}

__declspec(noinline) void __fastcall drain(void* queue) noexcept {
    std::uint32_t count = 0;
    __try {
        count = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uint8_t*>(queue) + 0x8000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0xFFFFFFFFu;
    }
    std::array<char, 96> text{};
    const int n = std::snprintf(text.data(), text.size(), "ev=lootprobe stage=drain count=%u", count);
    if (n > 0) {
        line(text.data(), static_cast<std::size_t>(n));
    }
    const Drain original = g_drain.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(queue);
    }
}

} // namespace

bool install() noexcept {
    if (g_handles[0].attached) {
        return true;
    }
    std::byte* const clearTarget = scan_main_image_unique(kClearBit, "lootprobe_clear");
    std::byte* const submitTarget = scan_main_image_unique(kSubmit, "lootprobe_submit");
    std::byte* const drainTarget = scan_main_image_unique(kDrain, "lootprobe_drain");
    std::byte* const accessor = scan_main_image_unique(kAccessor, "lootprobe_accessor");
    std::array<char, 160> text{};
    const int n = std::snprintf(text.data(), text.size(),
                                "ev=lootprobe stage=install clear=%d submit=%d drain=%d accessor=%d",
                                clearTarget != nullptr, submitTarget != nullptr,
                                drainTarget != nullptr, accessor != nullptr);
    if (n > 0) {
        line(text.data(), static_cast<std::size_t>(n));
    }
    if (clearTarget == nullptr || submitTarget == nullptr || drainTarget == nullptr) {
        return false;
    }
    g_accessor.store(reinterpret_cast<Accessor>(accessor), std::memory_order_release);
    const std::array specs{
        hooking::detour::Spec{clearTarget, reinterpret_cast<void*>(&clear_bit)},
        hooking::detour::Spec{submitTarget, reinterpret_cast<void*>(&submit)},
        hooking::detour::Spec{drainTarget, reinterpret_cast<void*>(&drain)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        line("ev=lootprobe stage=install result=fail reason=attach", 52);
        return false;
    }
    g_clear.store(reinterpret_cast<ClearBit>(g_handles[0].original), std::memory_order_release);
    g_submit.store(reinterpret_cast<Submit>(g_handles[1].original), std::memory_order_release);
    g_drain.store(reinterpret_cast<Drain>(g_handles[2].original), std::memory_order_release);
    line("ev=lootprobe stage=install result=ok", 36);
    return true;
}

void poll() noexcept {
    static std::atomic<std::uint32_t> ticks{0};
    if (ticks.fetch_add(1, std::memory_order_relaxed) % 1200 != 0) {
        return;
    }
    const Accessor accessor = g_accessor.load(std::memory_order_acquire);
    if (accessor == nullptr) {
        return;
    }
    std::uint8_t* object = nullptr;
    __try {
        object = accessor();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        object = nullptr;
    }
    bool ok = false;
    const std::uint8_t flags = object != nullptr ? read_byte(object, ok) : 0;
    std::array<char, 96> text{};
    const int n = std::snprintf(text.data(), text.size(), "ev=lootprobe stage=flags object=%d flags=0x%02X",
                                object != nullptr, ok ? flags : 0xFF);
    if (n > 0) {
        line(text.data(), static_cast<std::size_t>(n));
    }
}

} // namespace sunrise::client::hooks::network::loot_probe
