#pragma once

#include <cstdint>

#include "definition.h"

namespace sunrise::state::activity::events {

/**
 * The withheld-key set is kept twice: the set the roster publishes from, and the set the file
 * `roster_exclude_keys.txt` beside settings.json holds. The Events page writes the file. A fresh
 * activity join copies the file into the published set, and nothing else does. That join is the
 * one moment the client rebuilds its roster from nothing, so a change never lands under a client
 * standing in the Tower; going to orbit and back is how one is applied.
 *
 * The file stays the source of truth so the presets in `event_presets` and hand edits work the
 * same way the page does. One hex key per line, `0x` optional, `#` comments. No file, or an empty
 * one, withholds nothing: every event shows.
 */

/** Re-reads the file into both sets. Called on a fresh activity join, and once on first use. */
void reload() noexcept;

/**
 * @param key Registry key the roster is about to publish.
 * @return True when the published set withholds it.
 */
[[nodiscard]] bool withheld(std::uint32_t key) noexcept;

/**
 * Copies both sets.
 * @param published Keys the roster withholds now.
 * @param pending Keys the file holds, which the next join will withhold.
 */
void snapshot(KeySet& published, KeySet& pending) noexcept;

/**
 * Rewrites the file from one set and makes it the pending set.
 * @param pending Keys the next join is to withhold.
 * @return True when every byte reached the file.
 */
[[nodiscard]] bool save(const KeySet& pending) noexcept;

} // namespace sunrise::state::activity::events
