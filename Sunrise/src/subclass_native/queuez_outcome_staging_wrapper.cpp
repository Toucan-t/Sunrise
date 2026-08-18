#include "../server/bap/encrypted/queuez/queuez_outcome_staging.h"
#include "subclass_runtime_bridge.h"

#define stage_service_outcome sunrise_base_stage_service_outcome
#include "../server/bap/encrypted/queuez/queuez_outcome_staging.cpp"
#undef stage_service_outcome

namespace sunrise::server::bap::encrypted::queuez {

bool stage_service_outcome(Scratch& scratch,
                           const SessionState& before,
                           const ServiceOutcome& outcome,
                           std::span<const std::byte, state::kAesKeySize> key,
                           std::array<std::byte, state::kBapNonceSize>& nonce,
                           std::span<std::byte> response,
                           std::size_t& written,
                           StagedPublication& publication) noexcept {
    subclass_native::PendingSelection& mutation = subclass_native::pending_selection();
    if (!mutation.active) {
        return sunrise_base_stage_service_outcome(
            scratch, before, outcome, key, nonce, response, written, publication);
    }

    publication = {};
    const std::uint64_t characterSoid = mutation.characterSoid;
    if (!before.family4Active || before.family4RootSoid == 0
        || before.family3Phase != Family3Phase::normal
        || before.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || before.family4RootSoid != mutation.accountSoid
        || !resident_character(before, characterSoid)
        || !resident_object(before, mutation.subclassInstanceSoid)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=native_subclass4 result=fail reason=inactive_or_missing");
        subclass_native::clear_pending_selection();
        return false;
    }

    state::AccountState preview = state::account_snapshot();
    if (mutation.characterIndex >= preview.characterCount
        || preview.primarySoid != mutation.accountSoid
        || preview.characters[mutation.characterIndex].soid != mutation.characterSoid) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=native_subclass4 result=fail reason=stale_preview");
        subclass_native::clear_pending_selection();
        return false;
    }
    preview.characters[mutation.characterIndex] = mutation.afterCharacter;
    if (!state::account::valid(preview)) {
        subclass_native::clear_pending_selection();
        return false;
    }

    SessionState after = before;
    ++after.family4Version;
    if (!valid(after)) {
        subclass_native::clear_pending_selection();
        return false;
    }

    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_socket_refresh_from_account(scratch,
                                                              before.family4RootSoid,
                                                              after.family4Version,
                                                              preview,
                                                              characterSoid,
                                                              mutation.subclassInstanceSoid,
                                                              true,
                                                              prepared)
        || !stage_family4_mutation(before, prepared.family, after)
        || after.family4ResidentCount != before.family4ResidentCount) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=native_subclass4 result=fail reason=prepare_or_manifest");
        subclass_native::clear_pending_selection();
        return false;
    }
    if (written > response.size()) {
        subclass_native::clear_pending_selection();
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
                         "ev=queuez stage=native_subclass4 result=fail reason=frame");
        subclass_native::clear_pending_selection();
        return false;
    }
    written += framedSize;
    middleware::secure_channel::advance_nonce(nonce);
    publication.hasState = true;
    publication.after = after;
    publication.armsEquipmentMirrors = true;
    publication.equipmentMirrorCharacterSoid = characterSoid;
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=queuez stage=native_subclass4 result=ok reason=item_then_character");
    return true;
}

} // namespace sunrise::server::bap::encrypted::queuez
