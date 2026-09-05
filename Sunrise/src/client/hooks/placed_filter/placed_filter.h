#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::placed_filter {

/**
 * Hides placed objects whose class comes from a chosen set of packages.
 *
 * The slice-set load pass walks every entry of every object list with no filter, which is why all of
 * the Tower's dressing instantiates at once and why two events have appeared together. Nothing in
 * the engine separates them, so the separation has to be imposed.
 *
 * The offline export of all 14,838 object lists and 161,592 spawn entries settled two things this
 * relies on. A spawn entry names its class by tag at `+0`, and a tag encodes its package as
 * `(tag - 0x80800000) >> 13`. The Tower's 3,219 entries draw classes from **41 packages** in three
 * generations, told apart by the patch suffix of the package file: `_0` for the base, `_2` for one
 * later batch, and `_3` for `0x6AB`, `0x6B7`, `0x6D5` and `0x6DB` — the newest environment content,
 * and the natural home of seasonal dressing. In the Tower those four account for 10 lists and 267
 * entries, eight of them tight rows of a repeated fixture layered into space the base lists already
 * occupy.
 *
 * ## Why this hooks the allocator, and why it sinks rather than refuses
 *
 * `Obj_AllocDatumAndInit` at RVA `0x56CB30` is the single choke point: it has one caller and every
 * placed object in the game passes through it. Its second argument is the spawn entry — the entry's
 * class tag at `+0` is resolved to a class definition by `0x4AE000`, and `lea rdx,[entry+0x10]`
 * feeds `call 0x559AA0` with the new object, which copies the transform in.
 *
 * Refusing the allocation would leave the caller holding a half-built object, and the reference tree
 * is explicit that a half-built object is not retained. So nothing is refused. The entry's position
 * is moved far below the map for the duration of the original call and **restored immediately
 * afterwards**, because the copy happens inside that call. The object is created exactly as it would
 * have been, and is simply somewhere nobody stands.
 *
 * That makes this a reversible visual test rather than a change to what exists, which is what the
 * question needs: drop a package generation, look at what disappears, and the packages are named by
 * what vanished.
 *
 * @return True when the target is found and the detour attaches.
 */
[[nodiscard]] bool install_placed_filter() noexcept;

/** Detaches the filter. */
void uninstall_placed_filter() noexcept;

/**
 * Sets which class packages are hidden.
 * @param packages Package ids to hide. An empty set restores every object.
 * @param count Entries in that set, capped by the internal capacity.
 */
void set_hidden_packages(const std::uint16_t* packages, std::size_t count) noexcept;

} // namespace sunrise::client::hooks::placed_filter
