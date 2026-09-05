#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string_view>
#include "middleware/bap/activity_message/loot_pickup.h"

using sunrise::middleware::bap::activity_message::loot_pickup::Pickup;
using sunrise::middleware::bap::activity_message::loot_pickup::parse;

std::array<std::byte, 80> decode(std::string_view hex) {
    std::array<std::byte, 80> bytes{};
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    assert(hex.size() == bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>(nibble(hex[2 * i]) * 16 + nibble(hex[2 * i + 1]));
    }
    return bytes;
}

int main() {
    // Three blue pickups followed by one purple, captured in the build-86657 Festival Tower.
    constexpr std::array<std::string_view, 4> captures{{
        "2AE8B32453D546002002002029020472771604727716047277167AA8C0040040040439EAA3001001001002CF55180080080080C0000000F436E38820ACE6C4A107AB43A0E4101A40000003408E4EE280",
        "2AE8B32553D546002002002029020472771604727716047277167AA8C0040040040439EAA3001001001002CF55180080080080C0000000F436E388A0CBD99721184B75A0F2510840000003408E4EE280",
        "2AE8B32653D546002002002029020472771604727716047277167AA8C0040040040439EAA3001001001002CF55180080080080C0000000F436E38B20B189CA2109574020FF270840000003408E4EE280",
        "2AE8B32753D546002002002029020472771604727716047277167AA8C0040040040439EAA3001001001002CF55180080080080C0000000DB6B6E2CA0DD9F6021120EF02104A37D40000003408E4EE280",
    }};
    constexpr std::array<std::uint32_t, 4> hashes{0xE86DC710, 0xE86DC711, 0xE86DC716, 0xB6D6DC59};
    for (std::size_t index = 0; index < captures.size(); ++index) {
        const auto bytes = decode(captures[index]);
        Pickup output{};
        assert(parse(bytes, output));
        assert(output.sourceHash == hashes[index]);
        assert(output.nonce == 0x2AE8B324 + index);
        assert(output.bubble == 6);
        assert(output.accountSoid == 0x9EAA300100100100ULL);
        assert(output.characterSoid == 0x9EAA300100100101ULL);
        if (index == 0) { assert(std::abs(output.position[0] - 13.61267948F) < 0.00001F); }
        for (std::size_t length = 0; length < bytes.size(); ++length) {
            assert(!parse(std::span(bytes).first(length), output));
            assert(output.sourceHash == 0 && output.accountSoid == 0);
        }
        // Actor kind, null subject, flags, identity copies, count, and padding must be canonical.
        for (std::size_t bit : {32, 99, 105, 106, 107, 110, 206, 270, 340, 343, 345, 409, 601, 633}) {
            auto changed = bytes;
            changed[bit / 8] ^= static_cast<std::byte>(1U << (7 - bit % 8));
            assert(!parse(changed, output));
        }
        // Inject +infinity into the first coordinate.
        auto changed = bytes;
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const auto mask = static_cast<std::byte>(1U << (7 - (473 + bit) % 8));
            auto& byte = changed[(473 + bit) / 8];
            byte &= ~mask;
            if ((0x7F800000U >> (31 - bit)) & 1U) { byte |= mask; }
        }
        assert(!parse(changed, output));
    }
    std::puts("loot_pickup: 4 captured packets, 320 truncations, malformed fields and nonfinite positions passed");
}
