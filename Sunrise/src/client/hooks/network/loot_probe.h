#pragma once

namespace sunrise::client::hooks::network::loot_probe {

/**
 * Temporary instrumentation for the world loot pickup path (2026-09-05). Logs the session flag
 * byte the loot component tests, every clear of a bit on it (with the caller), every deferred-job
 * submission with its name, and every drain of the deferred queue, as `ev=lootprobe` lines.
 * @return True when every detour attached.
 */
[[nodiscard]] bool install() noexcept;

/** Samples the flag byte; call from the frame poll. */
void poll() noexcept;

} // namespace sunrise::client::hooks::network::loot_probe
