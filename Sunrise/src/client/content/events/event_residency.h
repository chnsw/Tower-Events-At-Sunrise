#pragma once

namespace sunrise::client::content::events {

/**
 * Reports whether the content tags listed in `residency_probe.txt` beside settings.json are
 * registered (their package has a datum table) and resolvable (the game's own resolver answers).
 *
 * Diagnostic only. A placed object whose entity tag never becomes resolvable cannot be created,
 * whatever the roster publishes - the Guardian Games podium entities live in packages the Tower's
 * authored package set may not pull in. Safe to call every frame: it samples every ten seconds for
 * the first ten minutes and logs `ev=residency` lines.
 */
void probe_residency() noexcept;

} // namespace sunrise::client::content::events
