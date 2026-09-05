/**
 * Residency probe for named content tags.
 *
 * Two answers per tag, because they differ (measured 2026-08-29 on Ada-1, master reference 11.x2):
 * the package table's entry base says the package is REGISTERED - its datum table exists - and the
 * game's resolver says the tag is RESOLVABLE, which is what creation needs. Both are sampled over
 * time because residency follows the player across bubbles.
 */

#include "event_residency.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/rule_text.h"

namespace sunrise::client::content::events {
namespace {

/** The tag list, beside settings.json. One hex tag per line, `0x` optional, `#` comments. */
constexpr std::wstring_view kFileName = L"residency_probe.txt";
/** Global holding the heap package-manager object (master reference 11.5). */
constexpr std::uintptr_t kPackageTableGlobal = 0x2439C70;
/** The canonical tag resolver (master reference 11.6). Null for an unregistered package. */
constexpr std::uintptr_t kDatumResolveRva = 0x1258970;
constexpr std::size_t kEntryStride = 64;
constexpr std::size_t kCapacity = 32;
/** Frames between samples: about ten seconds. */
constexpr std::uint32_t kSamplePeriod = 600;
/** Frames after which sampling stops: about ten minutes. */
constexpr std::uint32_t kSampleLimit = 36000;

using TagResolver = const std::byte*(__fastcall*)(std::uint32_t) noexcept;

std::array<std::uint32_t, kCapacity> g_tags{};
std::size_t g_count{};
std::atomic_bool g_loaded{false};

/** Reads the tag list once. An absent file probes nothing. */
void load() noexcept {
    static std::array<char, core::rule_text::kRuleTextCapacity> text{};
    if (!core::path::read_artifact_text(kFileName, text)) {
        return;
    }
    // The cursor reads bare hex, so a `0x` that starts a field is blanked first.
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
    while (rules.seek_field() && g_count < g_tags.size()) {
        const std::uint32_t value = rules.read_hex();
        if (value != 0) {
            g_tags[g_count] = value;
            ++g_count;
        }
    }
}

/** Reads one package-table entry. No C++ objects with destructors here, because of __try. */
bool read_entry(std::uintptr_t base,
                std::uint32_t ordinal,
                std::uint64_t& entryBase,
                std::uint32_t& stride,
                std::uint32_t& mask) noexcept {
    __try {
        const std::uint64_t mgr = *reinterpret_cast<const std::uint64_t*>(base + kPackageTableGlobal);
        const std::uint64_t array = *reinterpret_cast<const std::uint64_t*>(mgr);
        const std::uint8_t* entry = reinterpret_cast<const std::uint8_t*>(array)
                                    + static_cast<std::uint64_t>(ordinal) * kEntryStride;
        entryBase = *reinterpret_cast<const std::uint64_t*>(entry + 0x08);
        stride = *reinterpret_cast<const std::uint32_t*>(entry + 0x30);
        mask = *reinterpret_cast<const std::uint32_t*>(entry + 0x34);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/** Asks the game's resolver. */
bool resolvable(std::uintptr_t base, std::uint32_t tag) noexcept {
    __try {
        const auto resolve = reinterpret_cast<TagResolver>(base + kDatumResolveRva);
        return resolve(tag) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void probe_one(std::uintptr_t base, std::uint32_t tag, std::uint32_t sample) noexcept {
    const std::uint32_t ordinal = (tag >> 13) & 0xFFFFu;
    const std::uint32_t package = (tag - 0x80800000u) >> 13;
    std::uint64_t entryBase = 0;
    std::uint32_t stride = 0;
    std::uint32_t mask = 0;
    const bool read = read_entry(base, ordinal, entryBase, stride, mask);
    const bool live = resolvable(base, tag);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=residency sample=%u tag=0x%08X pkg=0x%03X ordinal=0x%04X base=0x%llX stride=0x%X "
        "mask=0x%X registered=%d resolvable=%d",
        sample,
        tag,
        package,
        ordinal,
        static_cast<unsigned long long>(entryBase),
        stride,
        mask,
        read && entryBase != 0 ? 1 : 0,
        live ? 1 : 0);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

void probe_residency() noexcept {
    if (!g_loaded.load(std::memory_order_acquire)) {
        load();
        g_loaded.store(true, std::memory_order_release);
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(), line.size(), "ev=residency stage=gate tags=%zu", g_count);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (g_count == 0) {
        return;
    }
    static std::atomic_uint32_t ticks{0};
    const std::uint32_t tick = ticks.fetch_add(1, std::memory_order_relaxed);
    if (tick % kSamplePeriod != 0 || tick > kSampleLimit) {
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for (std::size_t index = 0; index < g_count; ++index) {
        probe_one(base, g_tags[index], tick / kSamplePeriod);
    }
}

} // namespace sunrise::client::content::events
