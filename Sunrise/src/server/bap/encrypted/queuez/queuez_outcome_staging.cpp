#include <limits>

#include "queuez_outcome_staging.h"

#include "../../../../core/logging/log.h"
#include "../../../../middleware/datagen/definitions.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "../push/queuez/queuez_update_frame.h"
#include "../push/snapshot/snapshot.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {
namespace {

[[nodiscard]] bool resident_character(const SessionState& state,
                                      std::uint64_t characterSoid) noexcept {
    if (characterSoid == 0) {
        return false;
    }
    for (std::size_t index = 0; index < state.family4ResidentCount; ++index) {
        const ResidentObject& resident = state.family4Residents[index];
        if (resident.definitionId == middleware::datagen::kCharacterObjectId
            && resident.objectSoid == characterSoid) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool resident_object(const SessionState& state, std::uint64_t objectSoid) noexcept {
    if (objectSoid == 0) {
        return false;
    }
    for (std::size_t index = 0; index < state.family4ResidentCount; ++index) {
        if (state.family4Residents[index].objectSoid == objectSoid) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool resident_item(const SessionState& state, std::uint64_t instanceSoid) noexcept {
    if (instanceSoid == 0) {
        return false;
    }
    for (std::size_t index = 0; index < state.family4ResidentCount; ++index) {
        const ResidentObject& resident = state.family4Residents[index];
        if (resident.objectSoid == instanceSoid) {
            return resident.definitionId == middleware::datagen::kItemInstanceObjectId;
        }
    }
    return false;
}

/**
 * Character deletion is allowed after opcode 505 has moved the peer into preselection. Reuse the
 * proven Family-4 mutation validator with only its normal-phase gate temporarily normalized, then
 * restore the actual character-select phase in the resulting peer after-image.
 */
[[nodiscard]] bool stage_character_delete_family4(const SessionState& before,
                                                  const middleware::queuez::Family& family,
                                                  SessionState& after) noexcept {
    if (before.family3Phase == Family3Phase::normal) {
        return stage_family4_mutation(before, family, after);
    }
    SessionState normalized = before;
    normalized.family3Phase = Family3Phase::normal;
    SessionState staged{};
    if (!valid(normalized) || !stage_family4_mutation(normalized, family, staged)) {
        return false;
    }
    staged.family3Phase = before.family3Phase;
    if (!valid(staged)) {
        return false;
    }
    after = staged;
    return true;
}

} // namespace

/** Stages queuez subscription, unsubscription, or character-move output for one peer. */
bool stage_service_outcome(Scratch& scratch,
                           const SessionState& before,
                           const ServiceOutcome& outcome,
                           std::span<const std::byte, state::kAesKeySize> key,
                           std::array<std::byte, state::kBapNonceSize>& nonce,
                           std::span<std::byte> response,
                           std::size_t& written,
                           StagedPublication& publication) noexcept {
    publication = {};
    SessionState after{};
    bool armsRepush = false;
    std::uint64_t repushRoot = 0;
    if (outcome.hasSubscription) {
        const state::AccountState account = state::account_snapshot();
        const bool coldBannerSubscription =
            outcome.subscription.familyType == kBannerFamilyType && !before.family4Active
            && account.primarySoid != 0 && state::account::selected_character_soid(account) == 0;
        if (coldBannerSubscription) {
            // The Client asks for its banner family before a Guardian exists. Retail already has
            // the roster/account presentation environment by then; Sunrise historically waited
            // for a later Family-3 subscription, leaving the first creator preview black. Seed the
            // existing roster and its Family-4 companion now. Family zero itself remains empty
            // until the first character is selected.
            middleware::queuez::Subscription bootstrap{};
            bootstrap.familyType = kRosterFamilyType;
            bootstrap.familyRootSoid = account.primarySoid;
            const std::size_t beforeBytes = written;
            push::append_queuez_notification(scratch,
                                             before,
                                             bootstrap,
                                             key,
                                             nonce,
                                             response,
                                             written,
                                             after,
                                             armsRepush);
            repushRoot = account.primarySoid;
            core::log::write(
                core::log::Channel::server,
                written > beforeBytes ? core::log::Level::info : core::log::Level::warn,
                written > beforeBytes
                    ? "ev=queuez stage=preselection_bootstrap result=ok"
                    : "ev=queuez stage=preselection_bootstrap result=fail reason=no_frames");
        } else {
            push::append_queuez_notification(scratch,
                                             before,
                                             outcome.subscription,
                                             key,
                                             nonce,
                                             response,
                                             written,
                                             after,
                                             armsRepush);
            repushRoot = outcome.subscription.familyRootSoid;
        }
    } else if (outcome.hasUnsubscription) {
        stage_unsubscription(before, outcome.unsubscription.familyRootSoid, after);
    } else if (outcome.hasChangeCharacter) {
        // The reply already carries the version this patch promises. A patch that cannot be built
        // leaves the ladder where it is, instead of holding back that reply.
        if (!push::append_change_character_notification(
                scratch, outcome.changeCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=change result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.changeCharacter.after;
    } else if (outcome.hasSelectCharacter) {
        // The reply is the Client's task completion and the move is a separate frame. A move that
        // cannot be built leaves the selection where it is, instead of holding back that reply.
        if (!push::append_select_character_notification(
                scratch, outcome.selectCharacter, key, nonce, response, written)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=select result=fail");
            return true;
        }
        middleware::secure_channel::advance_nonce(nonce);
        after = outcome.selectCharacter.after;
        // The banner pair follows the family-four move, so it is sent against the state that move
        // made. A pair that cannot be built leaves the emblem where it is.
        const SessionState& bannerBefore = after;
        SessionState bannerAfter{};
        if (push::append_banner_move_notification(scratch,
                                                  bannerBefore,
                                                  outcome.selectCharacter.selectedCharacterSoid,
                                                  key,
                                                  nonce,
                                                  response,
                                                  written,
                                                  bannerAfter)) {
            after = bannerAfter;
        }
    } else if (outcome.hasCharacterDeletion && outcome.characterDeletion.prepared) {
        const state::PendingCharacterDeletion& mutation = outcome.characterDeletion;
        const state::AccountState current = state::account_snapshot();
        const std::uint64_t runtimeSelected = state::account::selected_character_soid(current);
        const bool safePreselection =
            runtimeSelected == 0 || before.family3Phase != Family3Phase::normal;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_object(before, before.family4RootSoid) || !safePreselection) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete4 result=fail reason=active_character");
            return false;
        }

        state::AccountState preview{};
        if (!state::preview_character_deletion(mutation, preview)
            || preview.primarySoid != before.family4RootSoid
            || state::account::selected_character_soid(preview) != 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete4 result=fail reason=stale_preview");
            return false;
        }

        const bool releaseCharacter = resident_character(before, mutation.deletedCharacterSoid);
        std::array<std::uint64_t, state::kCharacterOwnedItemCapacity> residentItems{};
        std::size_t residentItemCount = 0;
        for (std::size_t index = 0; index < mutation.releasedItemCount; ++index) {
            const std::uint64_t itemSoid = mutation.releasedItemSoids[index];
            if (!resident_object(before, itemSoid)) {
                continue;
            }
            if (!resident_item(before, itemSoid) || residentItemCount >= residentItems.size()) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=character_delete4 result=fail reason=item_resident_type");
                return false;
            }
            residentItems[residentItemCount++] = itemSoid;
        }

        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_character_deletion_from_account(
                scratch,
                before.family4RootSoid,
                before.family4Version + 1,
                preview,
                mutation.deletedCharacterSoid,
                releaseCharacter,
                std::span(residentItems).first(residentItemCount),
                prepared)
            || !stage_character_delete_family4(before, prepared.family, after)
            || before.family4ResidentCount
                   != after.family4ResidentCount + residentItemCount
                          + static_cast<std::size_t>(releaseCharacter)
            || resident_character(after, mutation.deletedCharacterSoid)) {
            core::log::write(
                core::log::Channel::server,
                core::log::Level::warn,
                "ev=queuez stage=character_delete4 result=fail reason=prepare_or_manifest");
            return false;
        }
        for (std::size_t index = 0; index < residentItemCount; ++index) {
            if (resident_object(after, residentItems[index])) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=queuez stage=character_delete4 result=fail reason=item_still_resident");
                return false;
            }
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=character_delete4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=character_delete4 result=ok reason=account_then_releases");
    } else if (outcome.hasItemAcquisition && outcome.itemAcquisition.prepared) {
        const state::PendingItemAcquisition& mutation = outcome.itemAcquisition;
        const std::uint64_t characterSoid = mutation.characterSoid;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_character(before, characterSoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_acquire4 result=fail reason=inactive");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_item_acquisition(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_acquire4 result=fail reason=stale_preview");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_item_acquisition_from_account(scratch,
                                                                   before.family4RootSoid,
                                                                   before.family4Version + 1,
                                                                   preview,
                                                                   characterSoid,
                                                                   mutation.acquiredInstanceSoid,
                                                                   prepared)
            || !stage_family4_mutation(before, prepared.family, after)
            || after.family4ResidentCount != before.family4ResidentCount + 1U) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_acquire4 result=fail reason=prepare_or_manifest");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_acquire4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        publication.armsEquipmentMirrors = true;
        publication.equipmentMirrorCharacterSoid = characterSoid;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=native_acquire4 result=ok reason=item_then_character");
    } else if (outcome.hasProfileItemAcquisition
               && outcome.profileItemAcquisition.prepared) {
        const state::PendingProfileItemAcquisition& mutation = outcome.profileItemAcquisition;
        const bool addResident = mutation.actionSource && mutation.appended;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || (addResident && (mutation.acquiredInstanceSoid == 0
                                || resident_object(before, mutation.acquiredInstanceSoid)))
            || (!addResident && mutation.actionSource
                && !resident_object(before, mutation.acquiredInstanceSoid))) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_acquire4 result=fail reason=inactive_or_resident");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_profile_item_acquisition(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_acquire4 result=fail reason=stale_preview");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_profile_item_refresh_from_account(
                scratch,
                before.family4RootSoid,
                before.family4Version + 1,
                preview,
                mutation.acquiredInstanceSoid,
                addResident,
                mutation.acquiredMutationSerial,
                mutation.acquiredQuantity,
                prepared)
            || !stage_family4_mutation(before, prepared.family, after)
            || after.family4ResidentCount
                   != before.family4ResidentCount + static_cast<std::size_t>(addResident)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_acquire4 result=fail reason=prepare_or_manifest");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_acquire4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         addResident
                             ? "ev=queuez stage=native_profile_acquire4 result=ok reason=item_then_account"
                             : "ev=queuez stage=native_profile_acquire4 result=ok reason=account");
    } else if (outcome.hasProfileItemDismantle
               && outcome.profileItemDismantle.prepared) {
        const state::PendingProfileItemDismantle& mutation = outcome.profileItemDismantle;
        const bool releaseResident = mutation.releasedInstanceSoid != 0;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_object(before, before.family4RootSoid)
            || (releaseResident && !resident_object(before, mutation.releasedInstanceSoid))) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_dismantle4 result=fail reason=inactive_or_resident");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_profile_item_dismantle(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_dismantle4 result=fail reason=stale_preview");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_profile_item_dismantle_from_account(
                scratch,
                before.family4RootSoid,
                before.family4Version + 1,
                preview,
                mutation.releasedInstanceSoid,
                prepared)
            || !stage_family4_mutation(before, prepared.family, after)
            || before.family4ResidentCount
                   != after.family4ResidentCount + static_cast<std::size_t>(releaseResident)) {
            core::log::write(
                core::log::Channel::server,
                core::log::Level::warn,
                "ev=queuez stage=native_profile_dismantle4 result=fail reason=prepare_or_manifest");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_profile_dismantle4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        core::log::write(
            core::log::Channel::server,
            core::log::Level::info,
            releaseResident
                ? "ev=queuez stage=native_profile_dismantle4 result=ok reason=account_then_release"
                : "ev=queuez stage=native_profile_dismantle4 result=ok reason=account");
    } else if (outcome.hasItemDismantle && outcome.itemDismantle.prepared) {
        const state::PendingItemDismantle& mutation = outcome.itemDismantle;
        const std::uint64_t characterSoid = mutation.characterSoid;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_character(before, characterSoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_dismantle4 result=fail reason=inactive");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_item_dismantle(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_dismantle4 result=fail reason=stale_preview");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_item_dismantle_from_account(scratch,
                                                                 before.family4RootSoid,
                                                                 before.family4Version + 1,
                                                                 preview,
                                                                 characterSoid,
                                                                 mutation.dismantledInstanceSoid,
                                                                 prepared)
            || !stage_family4_mutation(before, prepared.family, after)
            || before.family4ResidentCount != after.family4ResidentCount + 1U) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_dismantle4 result=fail reason=prepare_or_manifest");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_dismantle4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        publication.armsEquipmentMirrors = true;
        publication.equipmentMirrorCharacterSoid = characterSoid;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=native_dismantle4 result=ok reason=character_then_release");
    } else if (outcome.hasItemStateMutation && outcome.itemStateMutation.prepared) {
        const state::PendingItemState& mutation = outcome.itemStateMutation;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_character(before, mutation.characterSoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_item_state4 result=fail reason=inactive");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_item_state(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_item_state4 result=fail reason=stale_preview");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_character_refresh_from_account(scratch,
                                                                    before.family4RootSoid,
                                                                    before.family4Version + 1,
                                                                    preview,
                                                                    mutation.characterSoid,
                                                                    prepared)
            || !stage_family4_mutation(before, prepared.family, after)
            || after.family4ResidentCount != before.family4ResidentCount) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_item_state4 result=fail reason=prepare_or_manifest");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=native_item_state4 result=ok reason=character_after_image");
    } else if (outcome.hasSocketMutation && outcome.socketMutation.prepared) {
        const state::PendingSocketPlug& mutation = outcome.socketMutation;
        const std::uint64_t characterSoid = mutation.characterSoid;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_character(before, characterSoid)
            || !resident_object(before, mutation.targetInstanceSoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_socket4 result=fail reason=inactive_or_missing");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_socket_plug(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_socket4 result=fail reason=stale_preview");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_socket_refresh_from_account(scratch,
                                                                  before.family4RootSoid,
                                                                  before.family4Version + 1,
                                                                  preview,
                                                                  characterSoid,
                                                                  mutation.targetInstanceSoid,
                                                                  mutation.targetEquipped,
                                                                  prepared)
            || !stage_family4_mutation(before, prepared.family, after)
            || after.family4ResidentCount != before.family4ResidentCount) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_socket4 result=fail reason=prepare_or_manifest");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_socket4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        publication.armsEquipmentMirrors = mutation.targetEquipped;
        publication.equipmentMirrorCharacterSoid =
            mutation.targetEquipped ? characterSoid : 0;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         mutation.targetEquipped
                             ? "ev=queuez stage=native_socket4 result=ok reason=item_then_character"
                             : "ev=queuez stage=native_socket4 result=ok reason=item_only");
    } else if (outcome.hasEquipmentMutation && outcome.equipmentMutation.prepared) {
        const state::PendingInventoryMove& mutation = outcome.equipmentMutation;
        const std::uint64_t characterSoid = mutation.characterSoid;
        if (!before.family4Active || before.family4RootSoid == 0
            || before.family3Phase != Family3Phase::normal
            || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
            || !resident_character(before, characterSoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_equipment4 result=fail reason=inactive");
            return false;
        }
        state::AccountState preview{};
        if (!state::preview_inventory_move(mutation, preview)
            || preview.primarySoid != before.family4RootSoid) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_equipment4 result=fail reason=stale_preview");
            return false;
        }
        after = before;
        ++after.family4Version;
        if (!valid(after)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_equipment4 result=fail reason=state");
            return false;
        }
        push::snapshot::Prepared prepared{};
        if (!push::snapshot::prepare_character_refresh_from_account(scratch,
                                                                    before.family4RootSoid,
                                                                    after.family4Version,
                                                                    preview,
                                                                    characterSoid,
                                                                    prepared)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_equipment4 result=fail reason=prepare");
            return false;
        }
        if (written > response.size()) {
            return false;
        }
        std::size_t framedSize = 0;
        if (!push::queuez_frame::append(scratch,
                                        prepared.family,
                                        prepared.rawClearSize,
                                        prepared.compressedClearSize,
                                        key,
                                        nonce,
                                        response.subspan(written),
                                        framedSize)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=native_equipment4 result=fail reason=frame");
            return false;
        }
        written += framedSize;
        middleware::secure_channel::advance_nonce(nonce);
        publication.armsEquipmentMirrors = true;
        publication.equipmentMirrorCharacterSoid = characterSoid;
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=native_equipment4 result=ok reason=character_after_image");
    } else if (outcome.hasCreatedCharacter && outcome.createdCharacterSoid != 0
               && !before.family4Active) {
        // Character creation is legal while no Guardian is selected. In that cold path the Client
        // has not necessarily established Family 3/4 yet, so waiting for a normal subscription
        // leaves character-select on its cached roster until the next transition. Bootstrap the
        // same measured roster -> account companion burst from the account root immediately after
        // opcode 501. Family zero intentionally skips while no character is selected.
        const state::AccountState account = state::account_snapshot();
        if (account.primarySoid == 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=queuez stage=create_bootstrap result=skip reason=no_root");
            return true;
        }
        middleware::queuez::Subscription subscription{};
        subscription.familyType = kRosterFamilyType;
        subscription.familyRootSoid = account.primarySoid;
        const std::size_t beforeBytes = written;
        push::append_queuez_notification(
            scratch, before, subscription, key, nonce, response, written, after, armsRepush);
        repushRoot = account.primarySoid;
        const bool bootstrapRecorded =
            written > beforeBytes && valid(after) && after.family4Active;
        publication.createdCharacterBootstrap = bootstrapRecorded;
        core::log::write(
            core::log::Channel::server,
            bootstrapRecorded ? core::log::Level::info : core::log::Level::warn,
            bootstrapRecorded
                ? "ev=queuez stage=create_bootstrap result=ok"
                : (written > beforeBytes
                       ? "ev=queuez stage=create_bootstrap result=partial reason=no_resident_manifest"
                       : "ev=queuez stage=create_bootstrap result=fail reason=no_frames"));
    } else {
        return true;
    }
    // The frame is already written by here. A mirror that fails validation is logged and dropped,
    // never turned into a refusal to send what the Client waits for.
    publication.hasState = valid(after);
    if (publication.hasState) {
        publication.after = after;
    } else {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=publish result=unrecorded");
    }
    publication.armsFamily4Repush = armsRepush;
    publication.family4RepushRoot = armsRepush ? repushRoot : 0;
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
