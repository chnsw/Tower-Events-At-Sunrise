#pragma once

#include "../../../../middleware/bap/activity_message/activity_message_request_parser.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::festival_pickups {

/** Called only after the activity route has verified exact connection ownership. BAP lock held. */
void receive(const ActivityClientBinding& binding,
             const middleware::bap::activity_message::Request& request) noexcept;

/** Publishes an owed reward on an authenticated account subscriber. BAP lock held. */
[[nodiscard]] bool consume(Session& session, Scratch& scratch, std::span<std::byte> response,
                           std::size_t& written, bool& touchesScratch) noexcept;

} // namespace sunrise::server::bap::encrypted::festival_pickups
