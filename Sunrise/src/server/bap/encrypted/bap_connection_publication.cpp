#include "bap_connection_publication.h"

#include <Windows.h>

#include <array>
#include <cstdio>

#include "../../../core/logging/log.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Measured delay before the second Family-4 snapshot. */
constexpr std::uint64_t kFamily4RepushDelayMs = 400;
/** The banner pair lands the same unsolicited way and hits the same record-state race. */
constexpr std::uint64_t kBannerRepushDelayMs = 400;
/**
 * How long the roster keeps its faster cadence after a load starts.
 * The slice-set load step costs 9.2 to 14.1 s, so this covers it.
 */
constexpr std::uint64_t kTransitionWindowMs = 15'000;

/** Reports the one-shot normal keepalive re-arm caused by the first client patch epoch. */
void report_post_epoch_roster_arm(std::uint64_t sessionId) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=strike stage=patch_epoch result=roster_armed session=0x%llX",
        static_cast<unsigned long long>(sessionId));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Captures the connection fields one service outcome carries. */
ConnectionFields connection_fields(const ServiceOutcome& outcome) noexcept {
    ConnectionFields fields{};
    if (!outcome.hasActivityTransaction) {
        return fields;
    }
    const auto& plan = outcome.activityPlan;
    if (plan.delivery == activity_message::Delivery::joinNotifications) {
        fields.joinMemberKey = plan.entitySlotMutation.memberKey;
        fields.joinCharacterSoid = plan.joinCharacterSoid;
        fields.joinsActivity = true;
    }
    // The initial load is a transition too, and its token does not arrive for several seconds.
    fields.opensTransitionWindow =
        plan.delivery == activity_message::Delivery::joinNotifications || plan.transitionStarted;
    if (plan.mutationDomain == activity_message::MutationDomain::patchEpoch) {
        fields.patchEpoch = plan.patchEpoch;
        fields.retainsPatchEpoch = true;
    }
    return fields;
}

/** Publishes the captured connection fields after a successful commit. */
void publish_connection_fields(Session& session,
                               const transactions::Publication& publication,
                               const ConnectionFields& fields) noexcept {
    if (publication.hasActivitySessionBinding) {
        session.activitySessionId = publication.activitySessionId;
    }
    if (fields.joinMemberKey != 0) {
        session.activityMemberKey = fields.joinMemberKey;
    }
    if (fields.joinCharacterSoid != 0) {
        session.activityCharacterSoid = fields.joinCharacterSoid;
    }
    if (fields.retainsPatchEpoch) {
        const bool firstPatchEpoch = !session.activityPatchEpochSeen;
        session.activityPatchEpoch = fields.patchEpoch;
        session.activityPatchEpochSeen = true;

        // Roster publication is gated on activityPatchEpochSeen, but the periodic keepalive may
        // already have moved its next due tick several seconds into the future after a pre-epoch
        // no-op. Re-arm the normal keepalive only on the first accepted epoch. This happens after
        // the service transaction commits, so the keepalive sees the retained epoch and sends the
        // ordinary global-state -> membership -> roster sequence with burst=false. Repeated type-52
        // sentinel messages do not keep forcing extra roster traffic.
        if (firstPatchEpoch && session.activitySessionId != 0) {
            session.activityKeepaliveDueTick = 0;
            report_post_epoch_roster_arm(session.activitySessionId);
        }
    }
    if (fields.opensTransitionWindow) {
        session.activityTransitionUntilTick = GetTickCount64() + kTransitionWindowMs;
    }
    // A join resets the client's roster container, so the warm-up is re-armed. Its unconditional
    // state-byte moves make the client deactivate and rebuild every roster-owned object, and the
    // player object binds to the published membership only on that rebuild.
    if (fields.joinsActivity) {
        session.activityRosterSends = 0;
        session.activityRosterGroups = 0;
    }
}

/** Arms the owed Family-4 and banner re-pushes when the queuez publication asks for them. */
void arm_repushes(Session& session, const queuez::StagedPublication& queuezPublication) noexcept {
    if (!queuezPublication.armsFamily4Repush || queuezPublication.family4RepushRoot == 0) {
        return;
    }
    const std::uint64_t now = GetTickCount64();
    session.family4RepushDueTick = now + kFamily4RepushDelayMs;
    session.family4RepushRoot = queuezPublication.family4RepushRoot;
    session.family4RepushArmed = true;
    session.bannerRepushDueTick = now + kBannerRepushDelayMs;
    session.bannerRepushRoot = queuezPublication.family4RepushRoot;
    session.bannerRepushArmed = true;
}

} // namespace sunrise::server::bap::encrypted
