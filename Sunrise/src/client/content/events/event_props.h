#pragma once

namespace sunrise::client::content::events {

/**
 * Places the Tower's seasonal event decorations in every Tower area.
 *
 * The events themselves reach the client through the roster, but their authored placements exist
 * only in the Courtyard's slice set - measured 2026-08-27 on all seven, and confirmed in game when
 * advertising a key into the Hangar rendered nothing and crashed on the way out. The decorations
 * are ordinary class definitions carried by authored 144-byte spawn entries, so the other areas are
 * furnished by instantiating those entities at translated coordinates rather than by advertising.
 *
 * Safe to call every frame: each area is placed once.
 */
void place_event_props() noexcept;

} // namespace sunrise::client::content::events
