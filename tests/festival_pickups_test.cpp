#include <algorithm>
#include <cassert>
#include <cstdio>
#include "core/logging/log.h"
#include "middleware/bap/activity_message/incident.h"
#include "server/bap/encrypted/activity_message/festival_pickups.h"
#include "server/bap/encrypted/queuez/queuez_outcome_staging.h"
#include "server/bap/encrypted/queuez/queuez_state_validation.h"
#include "state/activity/events/activity_event_selection.h"
#include "state/activity/runtime.h"
#include "state/build_data/collectibles/collectible_catalog.h"
#include <Windows.h>

namespace stubs {
sunrise::state::AccountState account{};
bool hidden = false;
bool encode = true;
bool commit = true;
int grants = 0;
std::uint32_t item = 0;
std::int32_t amount = 0;
}
namespace sunrise::core::log { void write(Channel, Level, std::string_view) noexcept {} }
namespace sunrise::state {
AccountState account_snapshot() noexcept { return stubs::account; }
namespace account {
std::uint64_t selected_character_soid(const AccountState&) noexcept { return 0x9EAA300100100101ULL; }
}
namespace activity {
bool binding_matches(const SessionBinding&) noexcept { return true; }
namespace events { bool withheld(std::uint32_t) noexcept { return stubs::hidden; } }
}
const BapState& bap() noexcept { static BapState value{}; return value; }
bool prepare_profile_item_acquisition(std::uint16_t collectible, std::uint32_t item,
    PendingProfileItemAcquisition& mutation, std::int32_t amount) noexcept {
    assert(collectible == build_data::collectibles::kNoCollectibleIndex);
    mutation.accountSoid = stubs::account.primarySoid;
    mutation.acquiredDefinitionHash = item;
    mutation.acquiredAmount = amount;
    return true;
}
bool commit_profile_item_acquisition(PendingProfileItemAcquisition& mutation) noexcept {
    if (!stubs::commit) { return false; }
    ++stubs::grants;
    stubs::item = mutation.acquiredDefinitionHash;
    stubs::amount = mutation.acquiredAmount;
    return true;
}
}
namespace sunrise::server::bap::encrypted::queuez {
bool stage_profile_item_acquisition(const SessionState&, std::uint64_t, std::uint64_t,
    bool, bool, ProfileItemAcquisition&) noexcept { return true; }
bool stage_service_outcome(Scratch&, const SessionState& before, const ServiceOutcome&,
    std::span<const std::byte, state::kAesKeySize>,
    std::array<std::byte, state::kBapNonceSize>& nonce, std::span<std::byte> response,
    std::size_t& written, StagedPublication& publication) noexcept {
    if (!stubs::encode) { return false; }
    written = 1;
    response[0] = std::byte{42};
    nonce[0] = static_cast<std::byte>(std::to_integer<unsigned>(nonce[0]) + 1);
    publication.hasState = true;
    publication.after = before;
    ++publication.after.family4Version;
    return true;
}
}

int main() {
    namespace bap = sunrise::server::bap;
    namespace pickups = bap::encrypted::festival_pickups;
    namespace wire = sunrise::middleware::bap::activity_message;
    stubs::account.primarySoid = 0x9EAA300100100100ULL;
    constexpr std::string_view hex =
        "2AE8B32453D546002002002029020472771604727716047277167AA8C0040040040439EAA3001001001002CF55180080080080C0000000F436E38820ACE6C4A107AB43A0E4101A40000003408E4EE280";
    wire::incident::Incident incident{};
    incident.primaryTarget = 3539;
    incident.payloadLength = 80;
    incident.hasPayload = true;
    auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    for (std::size_t i = 0; i < 80; ++i) {
        incident.payload[i] = static_cast<std::byte>(nibble(hex[2*i])*16+nibble(hex[2*i+1]));
    }
    std::array<std::byte, 100> bytes{};
    std::size_t size{};
    const auto frame = [&] {
        sunrise::middleware::encoding::bits::Writer writer(bytes);
        assert(wire::incident::write(writer, incident));
        assert(writer.finish(size));
    };
    frame();
    wire::Request request{};
    request.messageType = 19;
    request.payload = std::span(bytes).first(size);
    bap::ActivityClientBinding binding{};
    binding.session.sessionId = 1;
    binding.session.createdRevision = 1;
    constexpr std::string_view tower = "city_tower_social_d2";
    binding.session.destination.packageNameLength = static_cast<std::uint8_t>(tower.size());
    std::copy(tower.begin(), tower.end(), binding.session.destination.packageName.begin());
    static bap::Session session{};
    static bap::Scratch scratch{};
    session.authenticated = true;
    session.queuez.family4Active = true;
    session.queuez.family4RootSoid = stubs::account.primarySoid;
    std::array<std::byte, 10> response{};
    bool touches = false;
    std::size_t written{};
    const auto consume = [&] { return pickups::consume(session, scratch, response, written, touches); };

    stubs::hidden = true;
    pickups::receive(binding, request);
    assert(!consume() && stubs::grants == 0);
    stubs::hidden = false;
    ++stubs::account.primarySoid;
    pickups::receive(binding, request);
    --stubs::account.primarySoid;
    assert(!consume() && stubs::grants == 0);
    auto wrongDestination = binding;
    wrongDestination.session.destination.packageNameLength = 0;
    pickups::receive(wrongDestination, request);
    assert(!consume());

    pickups::receive(binding, request);
    pickups::receive(binding, request);
    stubs::encode = false;
    assert(!consume() && stubs::grants == 0 && session.sendNonce[0] == std::byte{0});
    stubs::encode = true;
    Sleep(1010);
    assert(!pickups::consume(session, scratch, {}, written, touches));
    assert(stubs::grants == 0 && session.queuez.family4Version == 0);
    Sleep(1010);
    stubs::commit = false;
    assert(!consume() && stubs::grants == 0 && session.sendNonce[0] == std::byte{0});
    stubs::commit = true;
    Sleep(1010);
    assert(consume() && stubs::grants == 1 && stubs::amount == 50 && stubs::item == 4084398230U);
    assert(written == 1 && response[0] == std::byte{42});
    assert(session.accountMutationPublished && session.queuez.family4Version == 1);
    pickups::receive(binding, request);
    assert(!consume() && stubs::grants == 1);
    // Same authored pickup is valid again in a new activity generation.
    ++binding.session.createdRevision;
    pickups::receive(binding, request);
    assert(consume() && stubs::grants == 2);
    // Exercise every authored pickup, including the two distinct blue reward policies.
    struct ExpectedReward { std::uint32_t source; std::uint32_t bubble; int amount; };
    constexpr std::array<ExpectedReward, 9> expected{{
        {0xE86DC713U, 6, 50}, {0xE86DC710U, 6, 50},
        {0xE86DC711U, 6, 50}, {0xE86DC716U, 6, 50}, {0xB6D6DC59U, 6, 250},
        {0xE86DC717U, 1, 60}, {0xE86DC714U, 1, 60},
        {0xE86DC715U, 1, 60}, {0xB6D6DC5AU, 1, 250},
    }};
    const auto set_word = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t bit = 0; bit < 32; ++bit) {
            const auto mask = static_cast<std::byte>(1U << (7 - (offset + bit) % 8));
            auto& byte = incident.payload[(offset + bit) / 8];
            byte &= ~mask;
            if ((value >> (31 - bit)) & 1U) { byte |= mask; }
        }
    };
    ++binding.session.createdRevision;
    for (const auto& row : expected) {
        set_word(441, row.source);
        set_word(569, 0x80000000U + row.bubble);
        frame();
        const int before = stubs::grants;
        pickups::receive(binding, request);
        assert(consume() && stubs::grants == before + 1);
        assert(stubs::amount == row.amount && stubs::item == 4084398230U);
        pickups::receive(binding, request);
        assert(!consume() && stubs::grants == before + 1);
    }
    std::puts("festival_pickups: gating, retry atomicity, activity generation, and all 9 pickup payouts/duplicates passed (Courtyard 50, Bazaar 60, purple 250 Candy)");
}
