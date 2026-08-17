#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/datagen/definitions.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"
#include "../internal.h"
#include "../push/activity/activity_keepalive_push.h"
#include "../push/queuez/queuez_update_frame.h"
#include "../push/snapshot/snapshot.h"
#include "queuez_state_validation.h"

namespace sunrise::server::bap::encrypted {
namespace {

/** Widest re-push report, sized for the fields below. */
constexpr std::size_t kRepushReportLimit = 96;
/** Editor refresh lines include stage, result, reason and frame size. */
constexpr std::size_t kEditorReportLimit = 144;

/**
 * Logs one delayed re-push with its framed size, so it can be compared to the first copy.
 * @param bytes Framed size of the published notification.
 */
void report_repush(const char* stage, std::size_t bytes) noexcept {
    std::array<char, kRepushReportLimit> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=queuez stage=%s result=ok bytes=%zu", stage, bytes);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Emits one bounded result for a user-initiated character/item refresh. */
void report_editor(const char* stage,
                   const char* result,
                   const char* reason,
                   std::size_t bytes = 0) noexcept {
    std::array<char, kEditorReportLimit> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=queuez stage=%s result=%s reason=%s bytes=%zu",
                                    stage,
                                    result,
                                    reason,
                                    bytes);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                           : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Checkpoints runtime editor State only after every required companion Queuez object has landed. */
void finish_editor_checkpoint_if_complete(Session& session) noexcept {
    if (!session.editorPersistencePending || session.inventoryAddRefreshPending
        || session.equipmentRefreshPending || session.socketRefreshPending
        || session.characterRefreshPending || session.profileRefreshPending
        || session.accountRefreshPending
        || session.rosterRefreshPending || session.bannerRefreshPending) {
        return;
    }
    const bool failed = session.editorRefreshFailed;
    session.editorPersistencePending = false;
    session.editorRefreshFailed = false;
    if (failed) {
        report_editor("editor_checkpoint", "skip", "refresh_failed");
        return;
    }
    const bool saved = state::checkpoint_characters();
    report_editor("editor_checkpoint", saved ? "ok" : "fail", saved ? "saved" : "disk_or_settings");
}

/** @return True when the active Family-4 manifest contains this selected-character object. */
[[nodiscard]] bool resident_character(const queuez::SessionState& state,
                                      std::uint64_t characterSoid) noexcept {
    for (std::size_t index = 0; index < state.family4ResidentCount; ++index) {
        const queuez::ResidentObject& resident = state.family4Residents[index];
        if (resident.definitionId == middleware::datagen::kCharacterObjectId
            && resident.objectSoid == characterSoid) {
            return true;
        }
    }
    return false;
}

/** @return True when the active Family-4 manifest contains this exact resident SOID. */
[[nodiscard]] bool resident_object(const queuez::SessionState& state,
                                   std::uint64_t objectSoid) noexcept {
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

/** Adds one new debug inventory item resident before publishing the character that references it. */
[[nodiscard]] bool consume_inventory_add_refresh(Session& session,
                                                 Scratch& scratch,
                                                 std::span<std::byte> response,
                                                 std::size_t& written,
                                                 bool& touchesScratch) noexcept {
    session.inventoryAddRefreshPending = false;
    const std::uint64_t characterSoid = session.inventoryAddCharacterSoid;
    const std::uint64_t instanceSoid = session.inventoryAddInstanceSoid;
    session.inventoryAddCharacterSoid = 0;
    session.inventoryAddInstanceSoid = 0;

    if (characterSoid == 0 || instanceSoid == 0 || !session.queuez.family4Active
        || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || resident_object(session.queuez, instanceSoid)) {
        session.editorRefreshFailed = true;
        report_editor("editor_inventory_add4", "skip", "inactive_or_already_resident");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    push::snapshot::Prepared prepared{};
    const std::int32_t nextVersion = session.queuez.family4Version + 1;
    if (!push::snapshot::prepare_inventory_item_addition(scratch,
                                                         session.queuez.family4RootSoid,
                                                         nextVersion,
                                                         characterSoid,
                                                         instanceSoid,
                                                         prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_inventory_add4", "fail", "prepare");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    queuez::SessionState after{};
    if (!queuez::stage_family4_additions(session.queuez, prepared.family, after)) {
        session.editorRefreshFailed = true;
        report_editor("editor_inventory_add4", "fail", "manifest");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_inventory_add4", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;

    // The resident must exist before the selected-character inventory row can reference it.
    session.characterRefreshSoid = characterSoid;
    session.characterRefreshPending = true;
    report_editor("editor_inventory_add4", "ok", "item_then_character", framedSize);
    return true;
}

/** Publishes one changed equipped instance before any selected-character reference to it. */
[[nodiscard]] bool consume_equipment_refresh(Session& session,
                                             Scratch& scratch,
                                             std::span<std::byte> response,
                                             std::size_t& written,
                                             bool& touchesScratch) noexcept {
    session.equipmentRefreshPending = false;
    const std::uint64_t characterSoid = session.equipmentRefreshCharacterSoid;
    const std::uint64_t instanceSoid = session.equipmentRefreshInstanceSoid;
    session.equipmentRefreshCharacterSoid = 0;
    session.equipmentRefreshInstanceSoid = 0;

    if (characterSoid == 0 || instanceSoid == 0 || !session.queuez.family4Active
        || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || !resident_object(session.queuez, instanceSoid)) {
        session.editorRefreshFailed = true;
        report_editor("editor_equipment4", "skip", "inactive_or_missing");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    queuez::SessionState after = session.queuez;
    ++after.family4Version;
    if (!queuez::valid(after)) {
        session.editorRefreshFailed = true;
        report_editor("editor_equipment4", "fail", "state");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_equipment_refresh(scratch,
                                                   session.queuez.family4RootSoid,
                                                   after.family4Version,
                                                   characterSoid,
                                                   instanceSoid,
                                                   prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_equipment4", "fail", "prepare");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_equipment4", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;

    // Roster and banner appearance summaries consume equipped definitions independently of the
    // Family-4 instance body, so they follow the same successful mutation as targeted mirrors.
    session.rosterRefreshCharacterSoid = characterSoid;
    session.rosterRefreshPending = true;
    session.rosterRefreshFromCreate = false;
    const std::uint64_t selected =
        state::account::selected_character_soid(state::account_snapshot());
    session.bannerRefreshCharacterSoid =
        session.queuez.family0Active && selected != 0 && selected == characterSoid
                && selected == session.queuez.family0Character
            ? characterSoid
            : 0;
    session.bannerRefreshPending = session.bannerRefreshCharacterSoid != 0;
    report_editor("editor_equipment4", "ok", "item_then_character", framedSize);
    return true;
}

/** Publishes one debug socket mutation through the same item/character ordering as native use. */
[[nodiscard]] bool consume_socket_refresh(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    session.socketRefreshPending = false;
    const std::uint64_t characterSoid = session.socketRefreshCharacterSoid;
    const std::uint64_t instanceSoid = session.socketRefreshInstanceSoid;
    const bool includeCharacter = session.socketRefreshIncludeCharacter;
    session.socketRefreshCharacterSoid = 0;
    session.socketRefreshInstanceSoid = 0;
    session.socketRefreshIncludeCharacter = false;

    if (characterSoid == 0 || instanceSoid == 0 || !session.queuez.family4Active
        || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || !resident_object(session.queuez, instanceSoid)
        || (includeCharacter && !resident_character(session.queuez, characterSoid))) {
        session.editorRefreshFailed = true;
        report_editor("editor_socket4", "skip", "inactive_or_missing");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    queuez::SessionState after = session.queuez;
    ++after.family4Version;
    if (!queuez::valid(after)) {
        session.editorRefreshFailed = true;
        report_editor("editor_socket4", "fail", "state");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_socket_refresh(scratch,
                                                session.queuez.family4RootSoid,
                                                after.family4Version,
                                                characterSoid,
                                                instanceSoid,
                                                includeCharacter,
                                                prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_socket4", "fail", "prepare");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_socket4", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;

    if (includeCharacter) {
        session.rosterRefreshCharacterSoid = characterSoid;
        session.rosterRefreshPending = true;
        session.rosterRefreshFromCreate = false;
        const std::uint64_t selected =
            state::account::selected_character_soid(state::account_snapshot());
        session.bannerRefreshCharacterSoid =
            session.queuez.family0Active && selected != 0 && selected == characterSoid
                    && selected == session.queuez.family0Character
                ? characterSoid
                : 0;
        session.bannerRefreshPending = session.bannerRefreshCharacterSoid != 0;
        report_editor("editor_socket4", "ok", "item_then_character", framedSize);
    } else {
        report_editor("editor_socket4", "ok", "item_only", framedSize);
        finish_editor_checkpoint_if_complete(session);
    }
    return true;
}

/** Publishes the Family-4 item-instance objects introduced by one native-created character. */
[[nodiscard]] bool consume_created_character_refresh(Session& session,
                                                     Scratch& scratch,
                                                     std::span<std::byte> response,
                                                     std::size_t& written,
                                                     bool& touchesScratch) noexcept {
    session.createdCharacterRefreshPending = false;
    const std::uint64_t characterSoid = session.createdCharacterRefreshSoid;
    session.createdCharacterRefreshSoid = 0;
    if (characterSoid == 0 || !session.queuez.family4Active
        || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()) {
        report_editor("create_family4", "skip", "inactive");
        return false;
    }
    touchesScratch = true;

    const std::int32_t nextVersion = session.queuez.family4Version + 1;
    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_created_character_items(scratch,
                                                         session.queuez.family4RootSoid,
                                                         nextVersion,
                                                         characterSoid,
                                                         prepared)) {
        report_editor("create_family4", "fail", "prepare");
        return false;
    }
    queuez::SessionState after{};
    if (!queuez::stage_family4_additions(session.queuez, prepared.family, after)) {
        push::queuez_frame::clear_object_storage(
            scratch, prepared.rawClearSize, prepared.compressedClearSize);
        report_editor("create_family4", "fail", "manifest");
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        report_editor("create_family4", "fail", "frame");
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;

    // Family 3 owns the visible character-select roster. Publish the complete new roster at the
    // next incremental version so the new row appears without forcing a resubscription/reload.
    session.rosterRefreshCharacterSoid = 0;
    session.rosterRefreshPending = true;
    session.rosterRefreshFromCreate = true;
    report_editor("create_family4", "ok", "items_added", framedSize);
    return true;
}

/** Publishes the normal Family-4 selection move and Family-0 replacement after native create. */
[[nodiscard]] bool consume_created_character_select(Session& session,
                                                    Scratch& scratch,
                                                    std::span<std::byte> response,
                                                    std::size_t& written,
                                                    bool& touchesScratch) noexcept {
    session.createdCharacterSelectPending = false;
    const std::uint64_t target = session.createdCharacterSelectSoid;
    session.createdCharacterSelectSoid = 0;
    const std::uint64_t selected =
        state::account::selected_character_soid(state::account_snapshot());
    if (target == 0 || selected != target || !session.queuez.family4Active
        || session.queuez.family4RootSoid == 0) {
        report_editor("create_select", "skip", "inactive");
        return false;
    }

    queuez::SelectCharacter select{};
    if (!queuez::stage_select_character(session.queuez, target, select)) {
        report_editor("create_select", "fail", "stage");
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::append_select_character_notification(scratch,
                                                    select,
                                                    state::bap().sessionKey,
                                                    nextSendNonce,
                                                    response,
                                                    framedSize)) {
        report_editor("create_select", "fail", "family4_frame");
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    queuez::SessionState after = select.after;

    // Cold creator bootstrap owns a synthetic Family-0 record. Reuse the ordinary banner move so
    // the Client first releases that fake record and then installs the real authored head/body.
    queuez::SessionState bannerAfter{};
    if (push::append_banner_move_notification(scratch,
                                              after,
                                              target,
                                              state::bap().sessionKey,
                                              nextSendNonce,
                                              response,
                                              framedSize,
                                              bannerAfter)) {
        after = bannerAfter;
    } else {
        report_editor("create_select", "fail", "family0_move");
        return false;
    }

    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    report_editor("create_select", "ok", "selected", framedSize);
    return true;
}

/**
 * Pushes character metadata through each object family that mirrors it. Family 4 receives only
 * the resident character object: the giant account body is intentionally left untouched because
 * selection handling has already shown that replacing it live can erase client-owned settings.
 */
[[nodiscard]] bool consume_character_refresh(Session& session,
                                             Scratch& scratch,
                                             std::span<std::byte> response,
                                             std::size_t& written,
                                             bool& touchesScratch) noexcept {
    session.characterRefreshPending = false;
    const std::uint64_t target = session.characterRefreshSoid;
    session.characterRefreshSoid = 0;

    // Family 3 mirrors identity/level for every character, independent of which one is resident
    // in Family 4. Family 0 mirrors only the currently selected character. Arm those first so an
    // inactive character edit still reaches the character-select roster.
    session.rosterRefreshCharacterSoid = target;
    session.rosterRefreshPending = true;
    const std::uint64_t selected =
        state::account::selected_character_soid(state::account_snapshot());
    session.bannerRefreshCharacterSoid =
        session.queuez.family0Active && target != 0 && target == selected
                && selected == session.queuez.family0Character
            ? target
            : 0;
    session.bannerRefreshPending = session.bannerRefreshCharacterSoid != 0;

    if (target == 0 || selected == 0 || target != selected) {
        report_editor("editor_character4", "skip", "not_resident");
        return false;
    }
    if (!session.queuez.family4Active || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || !resident_character(session.queuez, target)) {
        session.editorRefreshFailed = true;
        report_editor("editor_character4", "skip", "inactive");
        return false;
    }
    touchesScratch = true;

    queuez::SessionState after = session.queuez;
    ++after.family4Version;
    if (!queuez::valid(after)) {
        session.editorRefreshFailed = true;
        report_editor("editor_character4", "fail", "state");
        return false;
    }

    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_character_refresh(scratch,
                                                   session.queuez.family4RootSoid,
                                                   after.family4Version,
                                                   target,
                                                   prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_character4", "fail", "prepare");
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_character4", "fail", "frame");
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    report_editor("editor_character4", "ok", "published", framedSize);
    return true;
}

/** Publishes a profile inventory edit, adding a new source resident before the account when owed. */
[[nodiscard]] bool consume_profile_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    session.profileRefreshPending = false;
    const std::uint64_t instanceSoid = session.profileRefreshInstanceSoid;
    const bool addResident = session.profileRefreshAddResident;
    session.profileRefreshInstanceSoid = 0;
    session.profileRefreshAddResident = false;
    if (!session.queuez.family4Active || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || !resident_object(session.queuez, session.queuez.family4RootSoid)
        || (addResident && (instanceSoid == 0 || resident_object(session.queuez, instanceSoid)))) {
        session.editorRefreshFailed = true;
        report_editor("editor_profile4", "skip", "inactive_or_resident");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    const state::AccountState account = state::account_snapshot();
    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_profile_item_refresh_from_account(
            scratch,
            session.queuez.family4RootSoid,
            session.queuez.family4Version + 1,
            account,
            instanceSoid,
            addResident,
            0,
            0,
            prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_profile4", "fail", "prepare");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    queuez::SessionState after{};
    if (!queuez::stage_family4_mutation(session.queuez, prepared.family, after)
        || after.family4ResidentCount
               != session.queuez.family4ResidentCount + static_cast<std::size_t>(addResident)) {
        session.editorRefreshFailed = true;
        report_editor("editor_profile4", "fail", "manifest");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_profile4", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    report_editor("editor_profile4", "ok", addResident ? "item_then_account" : "account", framedSize);
    finish_editor_checkpoint_if_complete(session);
    return true;
}

/** Pushes only the resident Family-4 account body after a profile-inventory debug edit. */
[[nodiscard]] bool consume_account_refresh(Session& session,
                                           Scratch& scratch,
                                           std::span<std::byte> response,
                                           std::size_t& written,
                                           bool& touchesScratch) noexcept {
    session.accountRefreshPending = false;
    if (!session.queuez.family4Active || session.queuez.family4RootSoid == 0
        || session.queuez.family3Phase != queuez::Family3Phase::normal
        || session.queuez.family4Version == (std::numeric_limits<std::int32_t>::max)()
        || !resident_object(session.queuez, session.queuez.family4RootSoid)) {
        session.editorRefreshFailed = true;
        report_editor("editor_account4", "skip", "inactive_or_missing_account");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    queuez::SessionState after = session.queuez;
    ++after.family4Version;
    if (!queuez::valid(after)) {
        session.editorRefreshFailed = true;
        report_editor("editor_account4", "fail", "state");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    push::snapshot::Prepared prepared{};
    if (!push::snapshot::prepare_account_refresh(
            scratch, session.queuez.family4RootSoid, after.family4Version, prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_account4", "fail", "prepare");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_account4", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    report_editor("editor_account4", "ok", "published", framedSize);
    finish_editor_checkpoint_if_complete(session);
    return true;
}

/** Pushes either one edited Family-3 character record or the broader item-refresh snapshot. */
[[nodiscard]] bool consume_roster_refresh(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    session.rosterRefreshPending = false;
    const bool fromCreate = session.rosterRefreshFromCreate;
    session.rosterRefreshFromCreate = false;
    const std::uint64_t characterSoid = session.rosterRefreshCharacterSoid;
    session.rosterRefreshCharacterSoid = 0;
    if (!session.queuez.family4Active || session.queuez.family4RootSoid == 0
        || session.editorFamily3Version == (std::numeric_limits<std::int32_t>::max)()) {
        session.editorRefreshFailed = true;
        report_editor(fromCreate ? "create_roster" : "editor_roster", "skip", "inactive");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    const std::int32_t nextVersion = session.editorFamily3Version + 1;
    push::snapshot::Prepared prepared{};
    if (characterSoid != 0) {
        if (!push::snapshot::prepare_roster_character_refresh(scratch,
                                                              session.queuez.family4RootSoid,
                                                              nextVersion,
                                                              characterSoid,
                                                              prepared)) {
            session.editorRefreshFailed = true;
            report_editor(fromCreate ? "create_roster" : "editor_roster",
                          "fail",
                          "character_prepare");
            finish_editor_checkpoint_if_complete(session);
            return false;
        }
    } else {
        middleware::queuez::Subscription subscription{};
        subscription.familyType = queuez::kRosterFamilyType;
        subscription.familyRootSoid = session.queuez.family4RootSoid;
        if (!push::snapshot::prepare_initial(scratch, subscription, prepared)
            || prepared.family.type != queuez::kRosterFamilyType
            || prepared.family.objects.empty()) {
            push::queuez_frame::clear_object_storage(
                scratch, prepared.rawClearSize, prepared.compressedClearSize);
            session.editorRefreshFailed = true;
            report_editor(fromCreate ? "create_roster" : "editor_roster", "fail", "prepare");
            finish_editor_checkpoint_if_complete(session);
            return false;
        }
        prepared.family.version = nextVersion;
        prepared.family.flags = 0;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor(fromCreate ? "create_roster" : "editor_roster", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.editorFamily3Version = nextVersion;
    if (fromCreate && session.createdCharacterSelectSoid != 0) {
        session.createdCharacterSelectPending = true;
    }
    report_editor(fromCreate ? "create_roster" : "editor_roster",
                  "ok",
                  characterSoid != 0 ? "character_published" : "published",
                  framedSize);
    finish_editor_checkpoint_if_complete(session);
    return true;
}

/** Pushes either one edited Family-0 character record or the broader item-refresh pair. */
[[nodiscard]] bool consume_banner_refresh(Session& session,
                                          Scratch& scratch,
                                          std::span<std::byte> response,
                                          std::size_t& written,
                                          bool& touchesScratch) noexcept {
    session.bannerRefreshPending = false;
    const std::uint64_t characterSoid = session.bannerRefreshCharacterSoid;
    session.bannerRefreshCharacterSoid = 0;
    const std::uint64_t selected =
        state::account::selected_character_soid(state::account_snapshot());
    if (!session.queuez.family0Active || session.queuez.family4RootSoid == 0 || selected == 0
        || selected != session.queuez.family0Character
        || session.queuez.family0Version == (std::numeric_limits<std::int32_t>::max)()
        || (characterSoid != 0 && characterSoid != selected)) {
        session.editorRefreshFailed = true;
        report_editor("editor_banner", "skip", "inactive");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    touchesScratch = true;

    queuez::SessionState after = session.queuez;
    ++after.family0Version;
    if (!queuez::valid(after)) {
        session.editorRefreshFailed = true;
        report_editor("editor_banner", "fail", "state");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    push::snapshot::Prepared prepared{};
    if (characterSoid != 0) {
        if (!push::snapshot::prepare_banner_character_refresh(scratch,
                                                              session.queuez.family4RootSoid,
                                                              after.family0Version,
                                                              characterSoid,
                                                              prepared)) {
            session.editorRefreshFailed = true;
            report_editor("editor_banner", "fail", "character_prepare");
            finish_editor_checkpoint_if_complete(session);
            return false;
        }
    } else if (!push::snapshot::prepare_banner(scratch,
                                               session.queuez.family4RootSoid,
                                               after.family0Version,
                                               session.queuez.family0Character,
                                               prepared)) {
        session.editorRefreshFailed = true;
        report_editor("editor_banner", "fail", "prepare");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    if (!push::queuez_frame::append(scratch,
                                    prepared.family,
                                    prepared.rawClearSize,
                                    prepared.compressedClearSize,
                                    state::bap().sessionKey,
                                    nextSendNonce,
                                    response,
                                    framedSize)) {
        session.editorRefreshFailed = true;
        report_editor("editor_banner", "fail", "frame");
        finish_editor_checkpoint_if_complete(session);
        return false;
    }
    middleware::secure_channel::advance_nonce(nextSendNonce);
    written = framedSize;
    session.sendNonce = nextSendNonce;
    session.queuez = after;
    report_editor("editor_banner",
                  "ok",
                  characterSoid != 0 ? "character_published" : "published",
                  framedSize);
    finish_editor_checkpoint_if_complete(session);
    return true;
}

/**
 * Sends the owed banner re-push once its delay has passed.
 * The banner has no subscribe of its own, so the timer is its only second chance.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole banner notification is published.
 */
[[nodiscard]] bool consume_banner_repush(Session& session,
                                         Scratch& scratch,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         bool& touchesScratch) noexcept {
    if (!session.bannerRepushArmed || session.bannerRepushRoot == 0
        || GetTickCount64() < session.bannerRepushDueTick) {
        return false;
    }
    session.bannerRepushArmed = false;
    // Nothing is owed while no character is selected. The pair has no character to name, and the
    // first pick publishes it. Logging that as a failed re-push would be wrong.
    if (state::account::selected_character_soid(state::account_snapshot()) == 0) {
        return false;
    }
    touchesScratch = true;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState bannerAfter{};
    if (!push::append_banner_notification(scratch,
                                          session.queuez,
                                          session.bannerRepushRoot,
                                          state::bap().sessionKey,
                                          nextSendNonce,
                                          scratch.framed,
                                          framedSize,
                                          bannerAfter)
        || framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    // The frame is committed here, so the recorded delivery is committed with it.
    if (valid(bannerAfter)) {
        session.queuez = bannerAfter;
    }
    report_repush("banner_repush", framedSize);
    return true;
}

} // namespace

/**
 * Sends one due deferred notification. Character editor refreshes mirror the changed state through
 * Family 4 -> Family 3 -> Family 0 across polls; inactive characters start at Family 3.
 */
bool consume_deferred(Session& session,
                      Scratch& scratch,
                      std::span<std::byte> response,
                      std::size_t& written,
                      bool& touchesScratch) noexcept {
    written = 0;
    if (!session.authenticated) {
        return false;
    }
    if (session.createdCharacterRefreshPending) {
        return consume_created_character_refresh(
            session, scratch, response, written, touchesScratch);
    }
    if (session.inventoryAddRefreshPending) {
        return consume_inventory_add_refresh(session, scratch, response, written, touchesScratch);
    }
    if (session.equipmentRefreshPending) {
        return consume_equipment_refresh(session, scratch, response, written, touchesScratch);
    }
    if (session.socketRefreshPending) {
        return consume_socket_refresh(session, scratch, response, written, touchesScratch);
    }
    if (session.createdCharacterSelectPending) {
        return consume_created_character_select(
            session, scratch, response, written, touchesScratch);
    }
    if (session.characterRefreshPending) {
        if (consume_character_refresh(session, scratch, response, written, touchesScratch)) {
            return true;
        }
        // A nonresident character legitimately skips Family 4 and can continue straight to the
        // Family-3 mirror. A failed preparation used scratch; let the caller wipe it first.
        if (touchesScratch) {
            return false;
        }
    }
    if (session.profileRefreshPending) {
        return consume_profile_refresh(session, scratch, response, written, touchesScratch);
    }
    if (session.accountRefreshPending) {
        return consume_account_refresh(session, scratch, response, written, touchesScratch);
    }
    if (session.rosterRefreshPending) {
        return consume_roster_refresh(session, scratch, response, written, touchesScratch);
    }
    if (session.bannerRefreshPending) {
        return consume_banner_refresh(session, scratch, response, written, touchesScratch);
    }
    if (!session.family4RepushArmed || session.family4RepushRoot == 0
        || GetTickCount64() < session.family4RepushDueTick) {
        return consume_banner_repush(session, scratch, response, written, touchesScratch)
               || push::activity::consume_activity_keepalive(
                   session, scratch, response, written, touchesScratch);
    }
    // One attempt is owed. Disarm before trying.
    session.family4RepushArmed = false;
    touchesScratch = true;

    middleware::queuez::Subscription subscription{};
    subscription.familyType = queuez::kAccountFamilyType;
    subscription.familyRootSoid = session.family4RepushRoot;

    auto nextSendNonce = session.sendNonce;
    std::size_t framedSize = 0;
    queuez::SessionState after{};
    bool armsRepush = false;
    push::append_queuez_notification(scratch,
                                     session.queuez,
                                     subscription,
                                     state::bap().sessionKey,
                                     nextSendNonce,
                                     scratch.framed,
                                     framedSize,
                                     after,
                                     armsRepush);
    if (framedSize == 0 || framedSize > response.size()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=repush result=fail");
        return false;
    }
    std::copy_n(scratch.framed.begin(), framedSize, response.begin());
    written = framedSize;
    session.sendNonce = nextSendNonce;
    if (queuez::valid(after)) {
        session.queuez = after;
    }
    report_repush("repush", framedSize);
    return true;
}

} // namespace sunrise::server::bap::encrypted
