#pragma once

namespace sunrise::server::bap {

/**
 * Arms an account resync for every authenticated session with an active Family-4 subscription,
 * under a fresh account generation. The deferred push then publishes the current account graph
 * to each of them, which is how a client learns of a profile mutation that no Web Service reply
 * carried - a world loot pickup credited in-process, for one.
 */
void arm_account_resync_for_all() noexcept;

} // namespace sunrise::server::bap
