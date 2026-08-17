#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/account/account_encoder.h"
#include "../../../../../middleware/datagen/family4/account/layout.h"
#include "../../../../../middleware/datagen/family4/character/character_encoder.h"
#include "../../../../../middleware/datagen/family4/character/layout.h"
#include "../../../../../middleware/datagen/family4/instance/instance_encoder.h"
#include "../../../../../middleware/datagen/family4/instance/layout.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace family4_datagen = middleware::datagen::family4;

/**
 * Publishes the staged family metadata once every needed object is done.
 * @param subscription Family id the Client picked.
 * @param compressedExtent Size of the used prefix of the sealed buffer.
 * @param reservation Cleanup extent carried over from any prior live snapshot.
 * @param output Gets the family snapshot only on success.
 * @return True when the staged descriptors pass the ownership check.
 */
[[nodiscard]] bool publish(const middleware::queuez::Subscription& subscription,
                           std::size_t objectCount,
                           std::size_t compressedExtent,
                           const Reservation& reservation,
                           Prepared& staged,
                           Prepared& output) noexcept {
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        subscription.familyRootSoid,
        kInitialFamilyVersion,
        middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, output);
}

} // namespace

/** Builds the Family-4 account, selected-character, and item-instance snapshot. */
bool prepare(Scratch& scratch,
             const middleware::queuez::Subscription& subscription,
             std::uint32_t accountObjectId,
             const Reservation& reservation,
             Prepared& prepared) noexcept {
    if (reservation.rawWriteOffset > scratch.plaintext.size()
        || reservation.compressedWriteOffset > scratch.sealed.size()) {
        return report_failure("reservation");
    }
    const auto rawStorage = std::span(scratch.plaintext).subspan(reservation.rawWriteOffset);
    if (family4_datagen::account::layout::kObjectSize > rawStorage.size()) {
        return report_failure("account_storage");
    }
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account)) {
        return report_failure("account_state");
    }

    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    Resolved selected{};
    if (selectedIndex.has_value() && !resolve(account, *selectedIndex, selected)) {
        return report_failure("selection");
    }

    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize,
                   reservation.rawWriteOffset + family4_datagen::account::layout::kObjectSize);
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)) {
        return report_failure("account_object");
    }
    if (!append_object(scratch,
                       accountBytes,
                       accountObjectId,
                       account.primarySoid,
                       staged.objects[kAccountObjectIndex],
                       compressedExtent)) {
        return report_failure("account_object");
    }
    // The character object is the only descriptor a selection owns. With no selection it is absent
    // and the items move up behind the account object, keeping the published prefix contiguous.
    const std::size_t itemBaseIndex =
        selectedIndex.has_value() ? kFirstItemObjectIndex : kFirstItemObjectIndexUnselected;
    if (selectedIndex.has_value()) {
        staged.rawClearSize = (std::max)(staged.rawClearSize,
                                         reservation.rawWriteOffset
                                             + family4_datagen::character::layout::kObjectSize);
        if (family4_datagen::character::layout::kObjectSize > rawStorage.size()) {
            return report_failure("character_storage");
        }
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        const state::CharacterState& selectedCharacter =
            account.characters[selected.characterIndex];
        if (!family4_datagen::character::encode(
                selectedCharacter, selected.loadout, selected.lightEvaluation, characterBytes)) {
            return report_failure("character_encode");
        }
        if (!append_object(scratch,
                           characterBytes,
                           selected.characterObjectId,
                           selectedCharacter.soid,
                           staged.objects[kCharacterObjectIndex],
                           compressedExtent)) {
            return report_failure("character_object");
        }
    }
    // Every character in the roster needs its item records. The equip-summary reader looks up an
    // instance with no null check, so a missing record is a null read.
    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("item_object_id");
    }
    std::size_t itemCursor = 0;
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
         ++characterIndex) {
        family4_datagen::loadout::ResolvedInstances instances{};
        if (!family4_datagen::loadout::resolve_owned_instances(account, characterIndex, instances)) {
            return report_failure("loadout");
        }
        if (instances.itemCount != 0
            && !append_items(scratch,
                             rawStorage,
                             itemInstanceObjectId,
                             instances,
                             itemBaseIndex,
                             staged,
                             itemCursor,
                             compressedExtent)) {
            return report_failure("items");
        }
    }

    if (!append_profile_items(scratch,
                              rawStorage,
                              itemInstanceObjectId,
                              account,
                              itemBaseIndex,
                              staged,
                              itemCursor,
                              compressedExtent)) {
        return report_failure("profile_items");
    }

    const std::size_t objectCount = itemBaseIndex + itemCursor;
    if (itemCursor != 0) {
        staged.rawClearSize =
            (std::max)(staged.rawClearSize,
                       reservation.rawWriteOffset + family4_datagen::instance::layout::kObjectSize);
    }
    return publish(subscription, objectCount, compressedExtent, reservation, staged, prepared);
}

/** Builds a metadata-only Family-4 incremental from a checked account after-image. */
bool prepare_character_refresh_from_account(Scratch& scratch,
                                            std::uint64_t familyRootSoid,
                                            std::int32_t version,
                                            const state::AccountState& account,
                                            std::uint64_t characterSoid,
                                            Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || family4_datagen::character::layout::kObjectSize > scratch.plaintext.size()) {
        return report_failure("character_refresh_input");
    }
    if (!state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("character_refresh_account");
    }
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!selectedIndex.has_value() || account.characters[*selectedIndex].soid != characterSoid) {
        return report_failure("character_refresh_selection");
    }

    Resolved selected{};
    if (!resolve(account, *selectedIndex, selected)) {
        return report_failure("character_refresh_resolve");
    }

    Prepared staged{};
    const auto characterBytes =
        std::span(scratch.plaintext).first(family4_datagen::character::layout::kObjectSize);
    const state::CharacterState& selectedCharacter = account.characters[*selectedIndex];
    if (!family4_datagen::character::encode(
            selectedCharacter, selected.loadout, selected.lightEvaluation, characterBytes)) {
        return report_failure("character_refresh_encode");
    }

    std::size_t compressedExtent = 0;
    if (!append_object(scratch,
                       characterBytes,
                       selected.characterObjectId,
                       selectedCharacter.soid,
                       staged.objects.front(),
                       compressedExtent)) {
        return report_failure("character_refresh_object");
    }
    staged.rawClearSize = family4_datagen::character::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(kSingleObjectCount),
    };
    return commit(staged, prepared);
}

/** Builds a metadata-only Family-4 incremental for the resident selected character. */
bool prepare_character_refresh(Scratch& scratch,
                               std::uint64_t familyRootSoid,
                               std::int32_t version,
                               std::uint64_t characterSoid,
                               Prepared& prepared) noexcept {
    return prepare_character_refresh_from_account(
        scratch, familyRootSoid, version, state::account_snapshot(), characterSoid, prepared);
}

/** Builds a one-object Family-4 account after-image without rebuilding resident manifests. */
bool prepare_account_refresh_from_account(Scratch& scratch,
                                          std::uint64_t familyRootSoid,
                                          std::int32_t version,
                                          const state::AccountState& account,
                                          Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion
        || family4_datagen::account::layout::kObjectSize > scratch.plaintext.size()
        || !state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("account_refresh_input");
    }

    std::uint32_t accountObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kAccountDefinitionSlotIndex, accountObjectId)) {
        return report_failure("account_refresh_object_id");
    }

    Prepared staged{};
    const auto accountBytes =
        std::span(scratch.plaintext).first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)) {
        return report_failure("account_refresh_encode");
    }

    std::size_t compressedExtent = 0;
    if (!append_object(scratch,
                       accountBytes,
                       accountObjectId,
                       account.primarySoid,
                       staged.objects.front(),
                       compressedExtent)) {
        return report_failure("account_refresh_object");
    }
    staged.rawClearSize = family4_datagen::account::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(kSingleObjectCount),
    };
    return commit(staged, prepared);
}

/** Builds account-only or resident-item -> account profile inventory refresh. */
bool prepare_profile_item_refresh_from_account(Scratch& scratch,
                                               std::uint64_t familyRootSoid,
                                               std::int32_t version,
                                               const state::AccountState& account,
                                               std::uint64_t instanceSoid,
                                               bool addResident,
                                               std::int32_t acquisitionMutationSerial,
                                               std::int32_t acquisitionQuantity,
                                               Prepared& prepared) noexcept {
    const bool acquisitionFeedback =
        acquisitionMutationSerial > 0 || acquisitionQuantity > 0;
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion
        || !state::account::valid(account) || account.primarySoid != familyRootSoid
        || family4_datagen::account::layout::kObjectSize > scratch.plaintext.size()
        || (acquisitionFeedback
            && (acquisitionMutationSerial <= 0 || acquisitionQuantity <= 0))
        || (addResident && (instanceSoid == 0
                            || family4_datagen::instance::layout::kObjectSize
                                   > scratch.plaintext.size()))) {
        return report_failure("profile_refresh_input");
    }
    std::uint32_t accountObjectId = 0;
    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kAccountDefinitionSlotIndex, accountObjectId)
        || (addResident
            && !middleware::datagen::object_id(
                kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId))) {
        return report_failure("profile_refresh_object_id");
    }

    Prepared staged{};
    std::size_t compressedExtent = 0;
    auto rawStorage = std::span(scratch.plaintext);
    std::size_t accountIndex = 0;
    if (addResident) {
        const state::account::inventory::ProfileItem* source = nullptr;
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const auto& item = account.profileItems[index];
            if (item.instanceSoid != instanceSoid) {
                continue;
            }
            if (source != nullptr) {
                return report_failure("profile_refresh_duplicate");
            }
            source = &item;
        }
        family4_datagen::instance::ResolvedInstance instance{};
        const auto itemBytes = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
        if (source == nullptr || !resolve_profile_item_instance(*source, instance)
            || !family4_datagen::instance::encode(instance, itemBytes)
            || !append_object(scratch,
                              itemBytes,
                              itemInstanceObjectId,
                              instanceSoid,
                              staged.objects[0],
                              compressedExtent)) {
            return report_failure("profile_refresh_item");
        }
        accountIndex = 1;
    }

    const auto accountBytes = rawStorage.first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)) {
        return report_failure("profile_refresh_account");
    }

    if (acquisitionFeedback) {
        // Profile shaders/mods live in the Family-4 account inventory rather than a character's
        // 350-row bank. The native account observer accepts the quantity change without this
        // transient descriptor, but it does not emit the ordinary Collections pickup/NEW feedback.
        // Keep the record in this one incremental only; canonical account snapshots leave it zero.
        constexpr std::uint16_t kAcquisitionChangeWriteSlot = 1;
        constexpr std::uint16_t kAcquisitionChangeNextSequence = 1;
        constexpr std::uint8_t kAcquisitionChangeKind = 1;
        auto& accountObject =
            *reinterpret_cast<family4_datagen::account::layout::Object*>(accountBytes.data());
        std::size_t acquiredRow = accountObject.profileItems.size();
        for (std::size_t row = 0; row < accountObject.profileItems.size(); ++row) {
            if (accountObject.profileItems[row].mutationSerial != acquisitionMutationSerial) {
                continue;
            }
            if (acquiredRow != accountObject.profileItems.size()) {
                return report_failure("profile_refresh_acquisition_duplicate");
            }
            acquiredRow = row;
        }
        const auto recordIsZero =
            [](const family4_datagen::account::layout::ProfileInventoryChangeRecord& record) noexcept {
                return record.sequence == 0 && record.reserved == 0 && record.mutationSerial == 0
                       && record.kind == 0 && record.reservedKind == 0 && record.flags == 0;
            };
        const bool recordsAreZero =
            std::all_of(accountObject.profileInventoryChanges.records.cbegin(),
                        accountObject.profileInventoryChanges.records.cend(),
                        recordIsZero);
        if (acquiredRow >= accountObject.profileItems.size()
            || accountObject.profileItems[acquiredRow].quantity != acquisitionQuantity
            || accountObject.profileInventoryChanges.writeSlot != 0
            || accountObject.profileInventoryChanges.nextSequence != 0 || !recordsAreZero) {
            return report_failure("profile_refresh_acquisition_state");
        }
        accountObject.profileInventoryChanges.writeSlot = kAcquisitionChangeWriteSlot;
        accountObject.profileInventoryChanges.nextSequence = kAcquisitionChangeNextSequence;
        auto& acquisitionChange = accountObject.profileInventoryChanges.records.front();
        acquisitionChange.sequence = 0;
        acquisitionChange.mutationSerial = acquisitionMutationSerial;
        acquisitionChange.kind = kAcquisitionChangeKind;
        acquisitionChange.flags = 0;
    }

    if (!append_object(scratch,
                       accountBytes,
                       accountObjectId,
                       account.primarySoid,
                       staged.objects[accountIndex],
                       compressedExtent)) {
        return report_failure("profile_refresh_account");
    }
    staged.rawClearSize = (std::max)(family4_datagen::account::layout::kObjectSize,
                                     family4_datagen::instance::layout::kObjectSize);
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(addResident ? 2U : 1U),
    };
    return commit(staged, prepared);
}

/** Builds account after-image before releasing one deleted character and its item residents. */
bool prepare_character_deletion_from_account(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    const state::AccountState& account,
    std::uint64_t deletedCharacterSoid,
    bool releaseCharacterResident,
    std::span<const std::uint64_t> releasedItemSoids,
    Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || deletedCharacterSoid == 0
        || !state::account::valid(account) || account.primarySoid != familyRootSoid
        || family4_datagen::account::layout::kObjectSize > scratch.plaintext.size()
        || 1U + static_cast<std::size_t>(releaseCharacterResident) + releasedItemSoids.size()
               > kObjectCapacity) {
        return report_failure("character_delete_input");
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == deletedCharacterSoid) {
            return report_failure("character_delete_still_owned");
        }
    }
    for (std::size_t releaseIndex = 0; releaseIndex < releasedItemSoids.size(); ++releaseIndex) {
        const std::uint64_t released = releasedItemSoids[releaseIndex];
        if (released == 0) {
            return report_failure("character_delete_zero_item");
        }
        for (std::size_t prior = 0; prior < releaseIndex; ++prior) {
            if (releasedItemSoids[prior] == released) {
                return report_failure("character_delete_duplicate_item");
            }
        }
        for (std::size_t characterIndex = 0; characterIndex < account.characterCount;
             ++characterIndex) {
            family4_datagen::loadout::ResolvedInstances instances{};
            if (!family4_datagen::loadout::resolve_owned_instances(
                    account, characterIndex, instances)) {
                return report_failure("character_delete_loadout");
            }
            for (std::size_t itemIndex = 0; itemIndex < instances.itemCount; ++itemIndex) {
                if (instances.items[itemIndex].instance.instanceSoid == released) {
                    return report_failure("character_delete_item_still_owned");
                }
            }
        }
    }

    std::uint32_t accountObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kAccountDefinitionSlotIndex, accountObjectId)) {
        return report_failure("character_delete_account_object_id");
    }

    Prepared staged{};
    std::size_t compressedExtent = 0;
    const auto accountBytes =
        std::span(scratch.plaintext).first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)
        || !append_object(scratch,
                          accountBytes,
                          accountObjectId,
                          account.primarySoid,
                          staged.objects[0],
                          compressedExtent)) {
        return report_failure("character_delete_account");
    }

    std::size_t cursor = 1;
    if (releaseCharacterResident) {
        staged.objects[cursor++] = middleware::queuez::Object{
            middleware::datagen::kCharacterObjectId,
            deletedCharacterSoid,
            middleware::queuez::Encoding::oodle,
            {},
        };
    }
    for (const std::uint64_t released : releasedItemSoids) {
        staged.objects[cursor++] = middleware::queuez::Object{
            middleware::datagen::kItemInstanceObjectId,
            released,
            middleware::queuez::Encoding::oodle,
            {},
        };
    }

    staged.rawClearSize = family4_datagen::account::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(cursor),
    };
    return commit(staged, prepared);
}

/** Builds account after-image then optional empty-payload release for a profile dismantle. */
bool prepare_profile_item_dismantle_from_account(Scratch& scratch,
                                                  std::uint64_t familyRootSoid,
                                                  std::int32_t version,
                                                  const state::AccountState& account,
                                                  std::uint64_t releasedInstanceSoid,
                                                  Prepared& prepared) noexcept {
    const bool releaseResident = releasedInstanceSoid != 0;
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion
        || !state::account::valid(account) || account.primarySoid != familyRootSoid
        || family4_datagen::account::layout::kObjectSize > scratch.plaintext.size()) {
        return report_failure("profile_dismantle_input");
    }
    if (releaseResident) {
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            if (account.profileItems[index].instanceSoid == releasedInstanceSoid) {
                return report_failure("profile_dismantle_still_owned");
            }
        }
    }

    std::uint32_t accountObjectId = 0;
    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kAccountDefinitionSlotIndex, accountObjectId)
        || (releaseResident
            && !middleware::datagen::object_id(
                kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId))) {
        return report_failure("profile_dismantle_object_id");
    }

    Prepared staged{};
    std::size_t compressedExtent = 0;
    const auto accountBytes =
        std::span(scratch.plaintext).first(family4_datagen::account::layout::kObjectSize);
    if (!family4_datagen::account::encode(account, accountBytes)
        || !append_object(scratch,
                          accountBytes,
                          accountObjectId,
                          account.primarySoid,
                          staged.objects[0],
                          compressedExtent)) {
        return report_failure("profile_dismantle_account");
    }
    if (releaseResident) {
        staged.objects[1] = middleware::queuez::Object{
            itemInstanceObjectId,
            releasedInstanceSoid,
            middleware::queuez::Encoding::oodle,
            {},
        };
    }

    staged.rawClearSize = family4_datagen::account::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(releaseResident ? 2U : 1U),
    };
    return commit(staged, prepared);
}

/** Builds the same one-account-object incremental from current runtime State. */
bool prepare_account_refresh(Scratch& scratch,
                             std::uint64_t familyRootSoid,
                             std::int32_t version,
                             Prepared& prepared) noexcept {
    return prepare_account_refresh_from_account(
        scratch, familyRootSoid, version, state::account_snapshot(), prepared);
}

/** Builds one dependency-ordered equipped-definition refresh without changing residents. */
bool prepare_equipment_refresh(Scratch& scratch,
                               std::uint64_t familyRootSoid,
                               std::int32_t version,
                               std::uint64_t characterSoid,
                               std::uint64_t instanceSoid,
                               Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || instanceSoid == 0 || family4_datagen::instance::layout::kObjectSize > scratch.plaintext.size()
        || family4_datagen::character::layout::kObjectSize > scratch.plaintext.size()) {
        return report_failure("equipment_refresh_input");
    }

    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("equipment_refresh_account");
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == characterSoid) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= account.characterCount) {
        return report_failure("equipment_refresh_character");
    }

    const state::CharacterState& character = account.characters[characterIndex];
    bool equipped = false;
    for (const auto& item : character.equipment.slots) {
        if (item.has_value() && item->instanceSoid == instanceSoid) {
            equipped = true;
            break;
        }
    }
    if (!equipped) {
        return report_failure("equipment_refresh_not_equipped");
    }

    family4_datagen::loadout::ResolvedInstances owned{};
    if (!family4_datagen::loadout::resolve_owned_instances(account, characterIndex, owned)) {
        return report_failure("equipment_refresh_instances");
    }
    const family4_datagen::instance::ResolvedInstance* targetInstance = nullptr;
    for (std::size_t index = 0; index < owned.itemCount; ++index) {
        const auto& candidate = owned.items[index].instance;
        if (candidate.instanceSoid != instanceSoid) {
            continue;
        }
        if (targetInstance != nullptr) {
            return report_failure("equipment_refresh_duplicate");
        }
        targetInstance = &candidate;
    }
    if (targetInstance == nullptr) {
        return report_failure("equipment_refresh_missing");
    }

    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("equipment_refresh_item_object_id");
    }

    Prepared staged{};
    std::size_t compressedExtent = 0;
    auto rawStorage = std::span(scratch.plaintext);
    const auto instanceBytes = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
    if (!family4_datagen::instance::encode(*targetInstance, instanceBytes)
        || !append_object(scratch,
                          instanceBytes,
                          itemInstanceObjectId,
                          instanceSoid,
                          staged.objects[0],
                          compressedExtent)) {
        return report_failure("equipment_refresh_item");
    }

    std::size_t objectCount = 1;
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (selectedIndex.has_value() && *selectedIndex == characterIndex) {
        Resolved selected{};
        if (!resolve(account, characterIndex, selected)) {
            return report_failure("equipment_refresh_selection");
        }
        const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
        if (!family4_datagen::character::encode(
                character, selected.loadout, selected.lightEvaluation, characterBytes)
            || !append_object(scratch,
                              characterBytes,
                              selected.characterObjectId,
                              characterSoid,
                              staged.objects[1],
                              compressedExtent)) {
            return report_failure("equipment_refresh_character_object");
        }
        objectCount = 2;
    }

    staged.rawClearSize =
        (std::max)(family4_datagen::instance::layout::kObjectSize,
                   family4_datagen::character::layout::kObjectSize);
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, prepared);
}

/** Builds one manifest-preserving socket/item-body refresh from a checked account after-image. */
bool prepare_socket_refresh_from_account(Scratch& scratch,
                                         std::uint64_t familyRootSoid,
                                         std::int32_t version,
                                         const state::AccountState& account,
                                         std::uint64_t characterSoid,
                                         std::uint64_t instanceSoid,
                                         bool includeCharacter,
                                         Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || instanceSoid == 0
        || family4_datagen::instance::layout::kObjectSize > scratch.plaintext.size()
        || (includeCharacter
            && family4_datagen::character::layout::kObjectSize > scratch.plaintext.size())) {
        return report_failure("socket_refresh_input");
    }
    if (!state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("socket_refresh_account");
    }

    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == characterSoid) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= account.characterCount) {
        return report_failure("socket_refresh_character");
    }

    family4_datagen::loadout::ResolvedInstances owned{};
    if (!family4_datagen::loadout::resolve_owned_instances(account, characterIndex, owned)) {
        return report_failure("socket_refresh_instances");
    }
    const family4_datagen::instance::ResolvedInstance* targetInstance = nullptr;
    for (std::size_t index = 0; index < owned.itemCount; ++index) {
        const auto& candidate = owned.items[index].instance;
        if (candidate.instanceSoid != instanceSoid) {
            continue;
        }
        if (targetInstance != nullptr) {
            return report_failure("socket_refresh_duplicate");
        }
        targetInstance = &candidate;
    }
    if (targetInstance == nullptr) {
        return report_failure("socket_refresh_missing");
    }

    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("socket_refresh_item_object_id");
    }

    Prepared staged{};
    std::size_t compressedExtent = 0;
    auto rawStorage = std::span(scratch.plaintext);
    const auto instanceBytes = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
    if (!family4_datagen::instance::encode(*targetInstance, instanceBytes)
        || !append_object(scratch,
                          instanceBytes,
                          itemInstanceObjectId,
                          instanceSoid,
                          staged.objects[0],
                          compressedExtent)) {
        return report_failure("socket_refresh_item");
    }

    std::size_t objectCount = 1;
    if (includeCharacter) {
        const std::optional<std::size_t> selectedIndex = find_character_index(account);
        if (!selectedIndex.has_value() || *selectedIndex != characterIndex) {
            return report_failure("socket_refresh_selection");
        }
        Resolved selected{};
        if (!resolve(account, characterIndex, selected)) {
            return report_failure("socket_refresh_resolve");
        }
        const auto characterBytes =
            rawStorage.first(family4_datagen::character::layout::kObjectSize);
        if (!family4_datagen::character::encode(account.characters[characterIndex],
                                                selected.loadout,
                                                selected.lightEvaluation,
                                                characterBytes)
            || !append_object(scratch,
                              characterBytes,
                              selected.characterObjectId,
                              characterSoid,
                              staged.objects[1],
                              compressedExtent)) {
            return report_failure("socket_refresh_character_object");
        }
        objectCount = 2;
    }

    staged.rawClearSize = includeCharacter
                              ? (std::max)(family4_datagen::instance::layout::kObjectSize,
                                           family4_datagen::character::layout::kObjectSize)
                              : family4_datagen::instance::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(objectCount),
    };
    return commit(staged, prepared);
}

/** Builds the same socket refresh from committed runtime State. */
bool prepare_socket_refresh(Scratch& scratch,
                            std::uint64_t familyRootSoid,
                            std::int32_t version,
                            std::uint64_t characterSoid,
                            std::uint64_t instanceSoid,
                            bool includeCharacter,
                            Prepared& prepared) noexcept {
    return prepare_socket_refresh_from_account(scratch,
                                               familyRootSoid,
                                               version,
                                               state::account_snapshot(),
                                               characterSoid,
                                               instanceSoid,
                                               includeCharacter,
                                               prepared);
}

/** Builds one new unequipped item resident before the selected-character after-image. */
bool prepare_inventory_item_addition(Scratch& scratch,
                                     std::uint64_t familyRootSoid,
                                     std::int32_t version,
                                     std::uint64_t characterSoid,
                                     std::uint64_t instanceSoid,
                                     Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || instanceSoid == 0
        || family4_datagen::instance::layout::kObjectSize > scratch.plaintext.size()) {
        return report_failure("inventory_add_input");
    }
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("inventory_add_account");
    }
    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == characterSoid) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= account.characterCount || !account.characters[characterIndex].selected) {
        return report_failure("inventory_add_character");
    }

    family4_datagen::loadout::ResolvedInstances owned{};
    if (!family4_datagen::loadout::resolve_owned_instances(account, characterIndex, owned)) {
        return report_failure("inventory_add_instances");
    }
    const family4_datagen::instance::ResolvedInstance* targetInstance = nullptr;
    for (std::size_t index = 0; index < owned.itemCount; ++index) {
        const auto& candidate = owned.items[index].instance;
        if (candidate.instanceSoid != instanceSoid) {
            continue;
        }
        if (targetInstance != nullptr) {
            return report_failure("inventory_add_duplicate");
        }
        targetInstance = &candidate;
    }
    if (targetInstance == nullptr) {
        return report_failure("inventory_add_missing");
    }

    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("inventory_add_object_id");
    }
    Prepared staged{};
    std::size_t compressedExtent = 0;
    const auto instanceBytes =
        std::span(scratch.plaintext).first(family4_datagen::instance::layout::kObjectSize);
    if (!family4_datagen::instance::encode(*targetInstance, instanceBytes)
        || !append_object(scratch,
                          instanceBytes,
                          itemInstanceObjectId,
                          instanceSoid,
                          staged.objects.front(),
                          compressedExtent)) {
        return report_failure("inventory_add_item");
    }
    staged.rawClearSize = family4_datagen::instance::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(kSingleObjectCount),
    };
    return commit(staged, prepared);
}

/** Builds item-create then character-update in one native acquisition Family-4 revision. */
bool prepare_item_acquisition_from_account(Scratch& scratch,
                                           std::uint64_t familyRootSoid,
                                           std::int32_t version,
                                           const state::AccountState& account,
                                           std::uint64_t characterSoid,
                                           std::uint64_t instanceSoid,
                                           Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || instanceSoid == 0
        || family4_datagen::instance::layout::kObjectSize > scratch.plaintext.size()
        || family4_datagen::character::layout::kObjectSize > scratch.plaintext.size()
        || !state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("acquire_refresh_input");
    }
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!selectedIndex.has_value() || account.characters[*selectedIndex].soid != characterSoid) {
        return report_failure("acquire_refresh_selection");
    }

    family4_datagen::loadout::ResolvedInstances owned{};
    if (!family4_datagen::loadout::resolve_owned_instances(account, *selectedIndex, owned)) {
        return report_failure("acquire_refresh_instances");
    }
    const family4_datagen::instance::ResolvedInstance* acquired = nullptr;
    for (std::size_t index = 0; index < owned.itemCount; ++index) {
        if (owned.items[index].instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (acquired != nullptr) {
            return report_failure("acquire_refresh_duplicate");
        }
        acquired = &owned.items[index].instance;
    }
    if (acquired == nullptr) {
        return report_failure("acquire_refresh_missing");
    }

    Resolved selected{};
    if (!resolve(account, *selectedIndex, selected)) {
        return report_failure("acquire_refresh_resolve");
    }
    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("acquire_refresh_item_object_id");
    }

    Prepared staged{};
    std::size_t compressedExtent = 0;
    auto rawStorage = std::span(scratch.plaintext);
    const auto instanceBytes = rawStorage.first(family4_datagen::instance::layout::kObjectSize);
    if (!family4_datagen::instance::encode(*acquired, instanceBytes)
        || !append_object(scratch,
                          instanceBytes,
                          itemInstanceObjectId,
                          instanceSoid,
                          staged.objects[0],
                          compressedExtent)) {
        return report_failure("acquire_refresh_item");
    }

    const state::CharacterState& character = account.characters[*selectedIndex];
    const auto characterBytes = rawStorage.first(family4_datagen::character::layout::kObjectSize);
    if (!family4_datagen::character::encode(
            character, selected.loadout, selected.lightEvaluation, characterBytes)) {
        return report_failure("acquire_refresh_character");
    }

    // New-item flags are transient acquisition feedback, not persistent inventory state. Mark only
    // the row created by this 1820 action; ordinary snapshots intentionally leave this bank zero.
    const family4_datagen::loadout::ResolvedItem* acquiredRow = nullptr;
    for (std::size_t index = 0; index < selected.loadout.itemCount; ++index) {
        const auto& item = selected.loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (acquiredRow != nullptr) {
            return report_failure("acquire_refresh_duplicate_row");
        }
        acquiredRow = &item;
    }
    constexpr std::size_t kBitsPerNewItemFlagByte = 8;
    if (acquiredRow == nullptr) {
        return report_failure("acquire_refresh_missing_row");
    }
    auto& characterObject =
        *reinterpret_cast<family4_datagen::character::layout::Object*>(characterBytes.data());
    const std::size_t newItemFlagIndex = acquiredRow->inventoryRow / kBitsPerNewItemFlagByte;
    if (newItemFlagIndex >= characterObject.newItemFlags.size()) {
        return report_failure("acquire_refresh_new_item_row");
    }
    characterObject.newItemFlags[newItemFlagIndex] |=
        std::byte{1U} << (acquiredRow->inventoryRow % kBitsPerNewItemFlagByte);

    // The Shadowkeep character observer does not queue acquisition feedback from the bitmap alone.
    // Mirror the native producer's one-shot change record and point it at the acquired row's exact
    // mutation serial. Canonical snapshots leave this list zero, so the feedback cannot reappear
    // after login or an unrelated inventory refresh.
    constexpr std::uint16_t kAcquisitionChangeWriteSlot = 1;
    constexpr std::uint16_t kAcquisitionChangeNextSequence = 1;
    constexpr std::uint8_t kAcquisitionChangeKind = 1;
    characterObject.inventoryChanges.writeSlot = kAcquisitionChangeWriteSlot;
    characterObject.inventoryChanges.nextSequence = kAcquisitionChangeNextSequence;
    auto& acquisitionChange = characterObject.inventoryChanges.records.front();
    acquisitionChange.sequence = 0;
    acquisitionChange.mutationSerial = acquiredRow->mutationSerial;
    acquisitionChange.kind = kAcquisitionChangeKind;
    acquisitionChange.flags = 0;

    if (!append_object(scratch,
                       characterBytes,
                       selected.characterObjectId,
                       characterSoid,
                       staged.objects[1],
                       compressedExtent)) {
        return report_failure("acquire_refresh_character");
    }

    staged.rawClearSize =
        (std::max)(family4_datagen::instance::layout::kObjectSize,
                   family4_datagen::character::layout::kObjectSize);
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(2),
    };
    return commit(staged, prepared);
}

/** Builds character-after-image then empty-payload release for one native dismantle. */
bool prepare_item_dismantle_from_account(Scratch& scratch,
                                         std::uint64_t familyRootSoid,
                                         std::int32_t version,
                                         const state::AccountState& account,
                                         std::uint64_t characterSoid,
                                         std::uint64_t releasedInstanceSoid,
                                         Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || releasedInstanceSoid == 0
        || family4_datagen::character::layout::kObjectSize > scratch.plaintext.size()
        || !state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("dismantle_refresh_input");
    }
    const std::optional<std::size_t> selectedIndex = find_character_index(account);
    if (!selectedIndex.has_value() || account.characters[*selectedIndex].soid != characterSoid) {
        return report_failure("dismantle_refresh_selection");
    }
    family4_datagen::loadout::ResolvedInstances owned{};
    if (!family4_datagen::loadout::resolve_owned_instances(account, *selectedIndex, owned)) {
        return report_failure("dismantle_refresh_instances");
    }
    for (std::size_t index = 0; index < owned.itemCount; ++index) {
        if (owned.items[index].instance.instanceSoid == releasedInstanceSoid) {
            return report_failure("dismantle_refresh_still_owned");
        }
    }

    Resolved selected{};
    if (!resolve(account, *selectedIndex, selected)) {
        return report_failure("dismantle_refresh_resolve");
    }
    Prepared staged{};
    std::size_t compressedExtent = 0;
    const auto characterBytes =
        std::span(scratch.plaintext).first(family4_datagen::character::layout::kObjectSize);
    const state::CharacterState& character = account.characters[*selectedIndex];
    if (!family4_datagen::character::encode(
            character, selected.loadout, selected.lightEvaluation, characterBytes)
        || !append_object(scratch,
                          characterBytes,
                          selected.characterObjectId,
                          characterSoid,
                          staged.objects[0],
                          compressedExtent)) {
        return report_failure("dismantle_refresh_character");
    }

    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("dismantle_refresh_item_object_id");
    }
    staged.objects[1] = middleware::queuez::Object{
        itemInstanceObjectId,
        releasedInstanceSoid,
        middleware::queuez::Encoding::oodle,
        {},
    };
    staged.rawClearSize = family4_datagen::character::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(2),
    };
    return commit(staged, prepared);
}

/** Builds the Family-4 item additions introduced by one native-created character. */
bool prepare_created_character_items(Scratch& scratch,
                                     std::uint64_t familyRootSoid,
                                     std::int32_t version,
                                     std::uint64_t characterSoid,
                                     Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || family4_datagen::instance::layout::kObjectSize > scratch.plaintext.size()) {
        return report_failure("create_items_input");
    }
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return report_failure("create_items_account");
    }
    std::size_t characterIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].soid == characterSoid) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex == account.characterCount) {
        return report_failure("create_items_character");
    }

    family4_datagen::loadout::ResolvedInstances instances{};
    if (!family4_datagen::loadout::resolve_owned_instances(account, characterIndex, instances)
        || instances.itemCount == 0) {
        return report_failure("create_items_loadout");
    }
    std::uint32_t itemInstanceObjectId = 0;
    if (!middleware::datagen::object_id(
            kAccountFamilyType, kItemDefinitionSlotIndex, itemInstanceObjectId)) {
        return report_failure("create_items_object_id");
    }

    Prepared staged{};
    std::size_t itemCursor = 0;
    std::size_t compressedExtent = 0;
    if (!append_items(scratch,
                      std::span(scratch.plaintext),
                      itemInstanceObjectId,
                      instances,
                      0,
                      staged,
                      itemCursor,
                      compressedExtent)
        || itemCursor == 0) {
        return report_failure("create_items_encode");
    }
    staged.rawClearSize = family4_datagen::instance::layout::kObjectSize;
    staged.compressedClearSize = compressedExtent;
    staged.family = middleware::queuez::Family{
        kAccountFamilyType,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(itemCursor),
    };
    return commit(staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
