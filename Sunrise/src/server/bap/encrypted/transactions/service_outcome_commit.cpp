#include "service_outcome_commit.h"

#include "../../../../state/activity/runtime.h"
#include "../../../../state/matchmaking/matchmaking_state.h"
#include "../../../../state/runtime/runtime.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::transactions {

/**
 * Commits at most one delayed State transaction.
 * @param outcome Checked service result whose pending transaction is used up.
 * @param publication Gets connection fields to publish after the output copy.
 * @return True when there is no transaction, or the one transaction commits.
 */
bool commit(ServiceOutcome& outcome, Publication& publication) noexcept {
    publication = {};
    const unsigned mutationCount = static_cast<unsigned>(outcome.hasCharacterDeletion)
                                   + static_cast<unsigned>(outcome.hasEquipmentMutation)
                                   + static_cast<unsigned>(outcome.hasItemAcquisition)
                                   + static_cast<unsigned>(outcome.hasProfileItemAcquisition)
                                   + static_cast<unsigned>(outcome.hasItemDismantle)
                                   + static_cast<unsigned>(outcome.hasProfileItemDismantle)
                                   + static_cast<unsigned>(outcome.hasSocketMutation)
                                   + static_cast<unsigned>(outcome.hasItemStateMutation)
                                   + static_cast<unsigned>(outcome.hasActivitySessionAllocation)
                                   + static_cast<unsigned>(outcome.hasActivityTransaction)
                                   + static_cast<unsigned>(outcome.hasMatchmakingMutation);
    // A service route may never combine independently versioned State transactions.
    if (mutationCount > 1U) {
        return false;
    }
    if (outcome.hasCharacterDeletion) {
        return state::commit_character_deletion(outcome.characterDeletion);
    }
    if (outcome.hasEquipmentMutation) {
        return state::commit_inventory_move(outcome.equipmentMutation);
    }
    if (outcome.hasItemAcquisition) {
        return state::commit_item_acquisition(outcome.itemAcquisition);
    }
    if (outcome.hasProfileItemAcquisition) {
        return state::commit_profile_item_acquisition(outcome.profileItemAcquisition);
    }
    if (outcome.hasItemDismantle) {
        return state::commit_item_dismantle(outcome.itemDismantle);
    }
    if (outcome.hasProfileItemDismantle) {
        return state::commit_profile_item_dismantle(outcome.profileItemDismantle);
    }
    if (outcome.hasSocketMutation) {
        return state::commit_socket_plug(outcome.socketMutation);
    }
    if (outcome.hasItemStateMutation) {
        return state::commit_item_state(outcome.itemStateMutation);
    }
    if (outcome.hasActivitySessionAllocation) {
        const std::uint64_t sessionId = outcome.activitySessionAllocation.sessionId;
        if (sessionId == state::activity::kAbsentSessionId
            || !state::activity::commit(outcome.activitySessionAllocation)) {
            return false;
        }
        publication.activitySessionId = sessionId;
        publication.hasActivitySessionBinding = true;
        return true;
    }
    if (outcome.hasActivityTransaction) {
        if (outcome.activityPlan.mutationDomain == activity_message::MutationDomain::entitySlots) {
            return state::activity::entity_slots::commit(outcome.activityPlan.entitySlotMutation);
        }
        if (outcome.activityPlan.mutationDomain == activity_message::MutationDomain::membership) {
            return state::activity::membership::commit(outcome.activityPlan.membershipMutation);
        }
        // The retained patch epoch is connection state, so it commits nothing here.
        return outcome.activityPlan.mutationDomain == activity_message::MutationDomain::patchEpoch;
    }
    if (outcome.hasMatchmakingMutation) {
        return state::matchmaking::commit(outcome.matchmakingMutation);
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::transactions
