#include "../server/bap/encrypted/transactions/service_outcome_commit.h"

#include "../state/activity/runtime.h"
#include "../state/matchmaking/matchmaking_state.h"
#include "../state/runtime/runtime.h"
#include "../state/runtime/storage/internal.h"
#include "../server/bap/encrypted/internal.h"
#include "subclass_runtime_bridge.h"

namespace sunrise::server::bap::encrypted::transactions {
namespace {

[[nodiscard]] bool same_selection(const state::CharacterState& left,
                                  const state::CharacterState& right) noexcept {
    return left.soid == right.soid && left.selected == right.selected
           && left.movementAbilityEntry == right.movementAbilityEntry
           && left.grenadeAbilityEntry == right.grenadeAbilityEntry
           && left.superAbilityEntry == right.superAbilityEntry
           && left.meleeAbilityEntry == right.meleeAbilityEntry
           && left.classAbilityEntry == right.classAbilityEntry;
}

[[nodiscard]] bool commit_subclass_selection() noexcept {
    const subclass_native::PendingSelection prepared = subclass_native::pending_selection();
    if (!prepared.active || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.subclassInstanceSoid == 0 || prepared.characterIndex >= state::kCharacterCapacity) {
        subclass_native::clear_pending_selection();
        return false;
    }

    constexpr std::size_t kSubclassSlot =
        static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);
    AcquireSRWLockExclusive(&state::runtime::storage::g_stateLock);
    state::AccountState candidate = state::runtime::storage::g_state.account;
    bool valid = candidate.primarySoid == prepared.accountSoid
                 && prepared.characterIndex < candidate.characterCount
                 && same_selection(candidate.characters[prepared.characterIndex],
                                   prepared.beforeCharacter);
    if (valid) {
        const auto& subclass = candidate.characters[prepared.characterIndex].equipment.slots[kSubclassSlot];
        valid = subclass.has_value() && subclass->instanceSoid == prepared.subclassInstanceSoid;
    }
    if (valid) {
        candidate.characters[prepared.characterIndex] = prepared.afterCharacter;
        valid = state::account::valid(candidate);
    }
    if (valid) {
        state::runtime::storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&state::runtime::storage::g_stateLock);

    if (!valid) {
        subclass_native::clear_pending_selection();
        return false;
    }

    // The exact after-selection ability row was synchronously resolved before Queuez staging.
    // Keep that published domain intact: clearing it here makes the immediately-following character
    // encoder lose the row it needs and also re-arms the global extraction overlay indefinitely.
    // The existing character store already owns these five fields, so make the successful native
    // pick durable immediately instead of depending on a later appearance mirror for persistence.
    (void)state::checkpoint_characters();
    subclass_native::clear_pending_selection();
    return true;
}

} // namespace

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
    if (subclass_native::pending_selection().active) {
        if (mutationCount != 0U) {
            subclass_native::clear_pending_selection();
            return false;
        }
        return commit_subclass_selection();
    }
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
        return outcome.activityPlan.mutationDomain == activity_message::MutationDomain::patchEpoch;
    }
    if (outcome.hasMatchmakingMutation) {
        return state::matchmaking::commit(outcome.matchmakingMutation);
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::transactions
