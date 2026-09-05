#include "loot_pickup.h"

#include <bit>
#include <cmath>

#include "../../encoding/bit_reader.h"

namespace sunrise::middleware::bap::activity_message::loot_pickup {

bool parse(std::span<const std::byte> payload, Pickup& output) noexcept {
    output = {};
    if (payload.size() != 80) {
        return false;
    }
    encoding::bits::Reader reader(payload);
    const auto expect = [&reader](std::uint8_t width, std::uint64_t expected) noexcept {
        std::uint64_t value{};
        return reader.read(width, value) && value == expected;
    };
    Pickup parsed{};
    std::uint64_t value{};
    if (!reader.read(32, value)) { return false; }
    parsed.nonce = static_cast<std::uint32_t>(value);
    if (!expect(3, 2) || !reader.read(64, parsed.characterSoid)
        // Nullable actor selector 18, absent subject -1; three false flags.
        || !expect(6, 18) || !expect(5, 0)
        || !expect(32, 0x811C9DC5U) || !expect(32, 0x811C9DC5U)
        || !expect(32, 0x811C9DC5U) || !expect(64, parsed.characterSoid)
        || !expect(6, 3) || !reader.read(64, parsed.accountSoid)
        || !expect(3, 1) || !expect(2, 1) || !expect(64, parsed.characterSoid)
        || !expect(32, 0x80000001U) || !reader.read(32, value)) {
        return false;
    }
    parsed.sourceHash = static_cast<std::uint32_t>(value);
    for (float& coordinate : parsed.position) {
        if (!reader.read(32, value)) { return false; }
        coordinate = std::bit_cast<float>(static_cast<std::uint32_t>(value));
        if (!std::isfinite(coordinate)) { return false; }
    }
    if (!reader.read(32, value)) { return false; }
    parsed.bubble = static_cast<std::int32_t>(static_cast<std::int64_t>(value) - 0x80000000LL);
    if (!expect(32, 0x811C9DC5U) || !expect(7, 0) || reader.remaining_bits() != 0
        || parsed.accountSoid == 0 || parsed.characterSoid == 0) {
        return false;
    }
    output = parsed;
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::loot_pickup
