#include "festival_pickups.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/activity_message/incident.h"
#include "../../../../middleware/bap/activity_message/loot_pickup.h"
#include "../../../../state/activity/events/activity_event_selection.h"
#include "../../../../state/activity/runtime.h"
#include "../../../../state/build_data/collectibles/collectible_catalog.h"
#include "../../../../state/runtime/runtime.h"
#include "../queuez/queuez_outcome_staging.h"
#include "../queuez/queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::festival_pickups {
namespace {
namespace loot = middleware::bap::activity_message::loot_pickup;
namespace incident = middleware::bap::activity_message::incident;

struct Reward {
    std::uint32_t source;
    std::uint32_t item;
    std::int32_t amount;
};

// Authored item_loot identifiers at +0x638 of AF3F/46/48/4A/4C (Courtyard)
// and AF2E/30/32/34 (Bazaar), build 86657. Translated props retain these identities.
// Sunrise's server policy: Courtyard blue = 50 Candy, Bazaar blue = 60 Candy;
// both purple pickups = 250 Candy. Resolve by authored source, not visual colour.
// The incident identifies the source; it never supplies an item or quantity to trust.
constexpr std::array<Reward, 9> kRewards{{
    {0xE86DC713U, 4084398230U, 50}, {0xE86DC710U, 4084398230U, 50},
    {0xE86DC711U, 4084398230U, 50}, {0xE86DC716U, 4084398230U, 50},
    {0xB6D6DC59U, 4084398230U, 250},
    {0xE86DC717U, 4084398230U, 60}, {0xE86DC714U, 4084398230U, 60},
    {0xE86DC715U, 4084398230U, 60}, {0xB6D6DC5AU, 4084398230U, 250},
}};

struct Claim {
    state::activity::SessionBinding activity{};
    loot::Pickup pickup{};
    Reward reward{};
    std::uint64_t retryAt{};
    bool occupied{};
    bool delivered{};
};
// BAP's route lock serializes both the activity ingress and account subscriber polls.
std::array<Claim, 256> g_claims{};

void report(const char* stage, const Claim& claim) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(), line.size(),
        "ev=loot stage=%s source=0x%08X item=%u quantity=%d bubble=%d nonce=0x%08X",
        stage, claim.pickup.sourceHash, claim.reward.item, claim.reward.amount,
        claim.pickup.bubble, claim.pickup.nonce);
    if (count > 0) {
        core::log::write(core::log::Channel::server, core::log::Level::info,
            {line.data(), static_cast<std::size_t>(count)});
    }
}

bool festival_visible(std::int32_t bubble) noexcept {
    std::uint32_t key{};
    switch (bubble) {
    case 6: key = 0x7C6DE64FU; break;
    case 1: key = 0xFC6B8707U; break;
    case 7: key = 0xEE34BBABU; break;
    case 0: key = 0x6D3740C6U; break;
    default: return false;
    }
    return !state::activity::events::withheld(key);
}

} // namespace

void receive(const ActivityClientBinding& binding,
             const middleware::bap::activity_message::Request& request) noexcept {
    constexpr std::string_view tower = "city_tower_social_d2";
    const auto& destination = binding.session.destination;
    if (destination.packageNameLength != tower.size()
        || !std::equal(tower.begin(), tower.end(), destination.packageName.begin())) {
        return;
    }
    incident::Incident framed{};
    loot::Pickup pickup{};
    if (incident::validate(request.payload, framed) != incident::Verdict::accepted
        || framed.primaryTarget != loot::kIncidentTarget || framed.extraTargetCount != 0
        || framed.hasCompressedSelector || framed.hasOptionalBlock
        || !loot::parse(std::span(framed.payload).first(framed.payloadLength), pickup)
        || !festival_visible(pickup.bubble)) {
        return;
    }
    const auto account = state::account_snapshot();
    if (pickup.accountSoid != account.primarySoid
        || pickup.characterSoid != state::account::selected_character_soid(account)) {
        return;
    }
    const auto reward = std::find_if(kRewards.begin(), kRewards.end(),
        [&pickup](const Reward& row) { return row.source == pickup.sourceHash; });
    if (reward == kRewards.end()) { return; }

    Claim* available = nullptr;
    for (auto& claim : g_claims) {
        if (claim.occupied && claim.activity.sessionId == binding.session.sessionId
            && claim.activity.createdRevision == binding.session.createdRevision
            && claim.pickup.characterSoid == pickup.characterSoid
            && claim.pickup.sourceHash == pickup.sourceHash && claim.pickup.bubble == pickup.bubble) {
            report("duplicate", claim);
            return;
        }
        // Completed claims live until their activity generation ends. Pending rewards survive
        // departure so a temporary subscriber/encoding failure cannot silently discard candy.
        if (!claim.occupied
            || (claim.delivered && !state::activity::binding_matches(claim.activity))) {
            if (available == nullptr) { available = &claim; }
        }
    }
    if (available == nullptr) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
            "ev=loot stage=queue result=full");
        return;
    }
    *available = {binding.session, pickup, *reward, 0, true, false};
    report("queued", *available);
}

bool consume(Session& session, Scratch& scratch, std::span<std::byte> response,
             std::size_t& written, bool& touchesScratch) noexcept {
    if (!session.authenticated || !session.queuez.family4Active) { return false; }
    for (auto& claim : g_claims) {
        if (!claim.occupied || claim.delivered || GetTickCount64() < claim.retryAt
            || claim.pickup.accountSoid != session.queuez.family4RootSoid) {
            continue;
        }
        claim.retryAt = GetTickCount64() + 1000;
        ServiceOutcome outcome{};
        auto& transaction = outcome.transaction.emplace<ProfileItemAcquisitionTransaction>();
        if (!state::prepare_profile_item_acquisition(
                state::build_data::collectibles::kNoCollectibleIndex, claim.reward.item,
                transaction.pending, claim.reward.amount)
            || !queuez::stage_profile_item_acquisition(session.queuez,
                transaction.pending.accountSoid, transaction.pending.acquiredInstanceSoid,
                transaction.pending.actionSource, transaction.pending.appended, transaction.update)) {
            report("retry_prepare", claim);
            continue;
        }
        touchesScratch = true;
        auto nonce = session.sendNonce;
        std::size_t size = 0;
        queuez::StagedPublication publication{};
        if (!queuez::stage_service_outcome(scratch, session.queuez, outcome,
                state::bap().sessionKey, nonce, scratch.framed, size, publication)
            || !publication.hasState || size == 0 || size > response.size()
            || !state::commit_profile_item_acquisition(transaction.pending)) {
            report("retry_publish", claim);
            continue;
        }
        std::copy_n(scratch.framed.begin(), size, response.begin());
        written = size;
        session.sendNonce = nonce;
        session.queuez = publication.after;
        session.accountMutationPublished = true;
        claim.delivered = true;
        report("granted", claim);
        return true;
    }
    return false;
}

} // namespace sunrise::server::bap::encrypted::festival_pickups
