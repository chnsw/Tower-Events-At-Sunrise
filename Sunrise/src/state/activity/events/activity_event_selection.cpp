#include "activity_event_selection.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/rule_text.h"

namespace sunrise::state::activity::events {
namespace {

/** The file, beside settings.json. */
constexpr std::wstring_view kFileName = L"roster_exclude_keys.txt";
/** Room for the header, every mapped key with its comment, and any unmapped keys after them. */
constexpr std::size_t kDocumentCapacity = 8192;
/** What the page writes above the keys, so a hand editor knows what the file does. */
constexpr char kHeader[] =
    "# Tower events. Every key listed here is WITHHELD from the roster, so the event it carries is "
    "not shown.\n"
    "# Written by the Events page of the Sunrise menu. Hand edits and the presets in event_presets "
    "work the same way.\n"
    "# A change applies the next time the client loads into the Tower (orbit and back), never "
    "while standing in it.\n";

SRWLOCK g_lock = SRWLOCK_INIT;
/** Keys the roster withholds now. */
KeySet g_published{};
/** Keys the file holds. */
KeySet g_pending{};
bool g_loaded{};

/** Logs one set. @param stage Which set moved and why. */
void report(const char* stage, const KeySet& keys) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int used = std::snprintf(
        line.data(), line.size(), "ev=events stage=%s withheld=%zu", stage, keys.count);
    for (std::size_t index = 0;
         index < keys.count && used > 0 && static_cast<std::size_t>(used) < line.size();
         ++index) {
        used += std::snprintf(line.data() + used,
                              line.size() - static_cast<std::size_t>(used),
                              " 0x%08X",
                              keys.keys[index]);
    }
    if (used > 0) {
        const std::size_t length = (std::min)(static_cast<std::size_t>(used), line.size() - 1);
        core::log::write(
            core::log::Channel::state, core::log::Level::info, {line.data(), length});
    }
}

/** Reads the file into one set. An absent or empty file is an empty set. */
void read_file(KeySet& keys) noexcept {
    keys = {};
    static std::array<char, core::rule_text::kRuleTextCapacity> text{};
    if (!core::path::read_artifact_text(kFileName, text)) {
        return;
    }
    // The cursor reads bare hex, so a `0x` prefix that starts a field is blanked out first; a bare
    // `0` would otherwise be read and dropped as key zero.
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
    while (rules.seek_field()) {
        // Zero and anything past the capacity are dropped. The field is stepped past either way.
        (void)keys.insert(rules.read_hex());
    }
}

/** Loads the file into both sets. The caller holds the lock exclusively. */
void load_locked(const char* stage) noexcept {
    read_file(g_pending);
    g_published = g_pending;
    g_loaded = true;
    report(stage, g_published);
}

/** Loads the file the first time anything asks, so a roster built before any join is right. */
void ensure_loaded() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool loaded = g_loaded;
    ReleaseSRWLockShared(&g_lock);
    if (loaded) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (!g_loaded) {
        load_locked("first");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/**
 * Appends one key line.
 * @param document Text being built.
 * @param used Bytes written so far, advanced.
 * @param key Key to write.
 * @param entry Its map entry, or null for a key the map does not name.
 * @return True when the line fitted.
 */
[[nodiscard]] bool append_key(std::array<char, kDocumentCapacity>& document,
                              int& used,
                              std::uint32_t key,
                              const EventKey* entry) noexcept {
    if (used < 0 || static_cast<std::size_t>(used) >= document.size()) {
        return false;
    }
    const std::size_t left = document.size() - static_cast<std::size_t>(used);
    const int written =
        entry != nullptr
            ? std::snprintf(document.data() + used,
                            left,
                            "0x%08X   # %s (%s)\n",
                            key,
                            kEventNames[static_cast<std::size_t>(entry->event)],
                            entry->area)
            : std::snprintf(document.data() + used, left, "0x%08X   # not in the key map\n", key);
    if (written <= 0 || static_cast<std::size_t>(written) >= left) {
        return false;
    }
    used += written;
    return true;
}

} // namespace

/** Re-reads the file into both sets. */
void reload() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    load_locked("join");
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return True when the published set withholds the key. */
bool withheld(std::uint32_t key) noexcept {
    ensure_loaded();
    AcquireSRWLockShared(&g_lock);
    const bool result = g_published.contains(key);
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/** Copies both sets. */
void snapshot(KeySet& published, KeySet& pending) noexcept {
    ensure_loaded();
    AcquireSRWLockShared(&g_lock);
    published = g_published;
    pending = g_pending;
    ReleaseSRWLockShared(&g_lock);
}

/** Rewrites the file from one set and makes it the pending set. */
bool save(const KeySet& pending) noexcept {
    ensure_loaded();
    std::array<char, kDocumentCapacity> document{};
    int used = std::snprintf(document.data(), document.size(), "%s", kHeader);
    if (used <= 0) {
        return false;
    }
    // Mapped keys go out in map order, which groups each event's areas together. Whatever else the
    // set holds follows, so a hand-added key survives a visit to the page.
    bool complete = true;
    for (const EventKey& entry : kEventKeys) {
        if (pending.contains(entry.key)) {
            complete = append_key(document, used, entry.key, &entry) && complete;
        }
    }
    for (std::size_t index = 0; index < pending.count; ++index) {
        const std::uint32_t key = pending.keys[index];
        if (find_key(key) == nullptr) {
            complete = append_key(document, used, key, nullptr) && complete;
        }
    }
    if (!complete
        || !core::path::write_artifact_text(
            kFileName, std::string_view{document.data(), static_cast<std::size_t>(used)})) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=events stage=save result=failed");
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_pending = pending;
    ReleaseSRWLockExclusive(&g_lock);
    report("save", pending);
    return true;
}

} // namespace sunrise::state::activity::events
