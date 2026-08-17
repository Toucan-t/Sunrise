#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "../account/characters/character_store.h"
#include "../build_data/runtime.h"
#include "../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "runtime.h"
#include "state.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace {

namespace character_store = account::characters;
namespace inventory = account::inventory;

/** Gear whose definition/subclass relationship changes with character class. */
constexpr std::array<inventory::EquipmentSlot, 6> kClassBoundSlots{
    inventory::EquipmentSlot::helmet,
    inventory::EquipmentSlot::gauntlets,
    inventory::EquipmentSlot::chest,
    inventory::EquipmentSlot::legs,
    inventory::EquipmentSlot::classItem,
    inventory::EquipmentSlot::subclass,
};

/** Rebases character keys while preserving each row's stable offset from the prior account key. */
[[nodiscard]] bool rebase_character_soids(AccountState& accountState,
                                           std::uint64_t oldPrimarySoid,
                                           std::uint64_t newPrimarySoid) noexcept {
    if (newPrimarySoid == 0) {
        return false;
    }
    for (std::size_t index = 0; index < accountState.characterCount; ++index) {
        const std::uint64_t current = accountState.characters[index].soid;
        const std::uint64_t offset = oldPrimarySoid != 0 && current > oldPrimarySoid
                                         ? current - oldPrimarySoid
                                         : 1U + index;
        if (offset == 0
            || newPrimarySoid > (std::numeric_limits<std::uint64_t>::max)() - offset) {
            return false;
        }
        accountState.characters[index].soid = newPrimarySoid + offset;
    }
    accountState.primarySoid = newPrimarySoid;
    return true;
}

/** Picks the lowest positive account-relative character id not currently owned. */
[[nodiscard]] bool next_character_offset(const AccountState& accountState,
                                         std::uint64_t& output) noexcept {
    if (accountState.primarySoid == 0) {
        return false;
    }
    for (std::uint64_t candidate = 1; candidate <= kCharacterCapacity + 1U; ++candidate) {
        bool used = false;
        for (std::size_t index = 0; index < accountState.characterCount; ++index) {
            const std::uint64_t soid = accountState.characters[index].soid;
            used = used || (soid > accountState.primarySoid
                            && soid - accountState.primarySoid == candidate);
        }
        if (!used) {
            output = candidate;
            return true;
        }
    }
    return false;
}

/** @return The first SOID above every currently owned equipment instance. */
[[nodiscard]] bool next_instance_soid(const AccountState& accountState, std::uint64_t& next) noexcept {
    std::uint64_t greatest = 0;
    for (std::size_t character = 0; character < accountState.characterCount; ++character) {
        const CharacterState& owned = accountState.characters[character];
        for (const std::optional<inventory::Item>& item : owned.equipment.slots) {
            if (item.has_value()) {
                greatest = (std::max)(greatest, item->instanceSoid);
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < owned.inventory.count; ++itemIndex) {
            greatest = (std::max)(greatest, owned.inventory.values[itemIndex].instanceSoid);
        }
    }
    if (greatest == (std::numeric_limits<std::uint64_t>::max)()) {
        next = 0;
        return false;
    }
    next = greatest + 1U;
    if (next == 0) {
        return false;
    }
    return true;
}

/** Allocates one stable runtime identity for a profile mod/shader action-source stack. */
[[nodiscard]] bool next_profile_instance_soid(const AccountState& accountState,
                                              std::uint64_t& next) noexcept {
    std::uint64_t greatest = inventory::kFirstProfileItemInstanceSoid - 1U;
    std::size_t residentCount = 0;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        const inventory::ProfileItem& item = accountState.profileItems[index];
        if (item.instanceSoid == 0) {
            continue;
        }
        ++residentCount;
        if (item.instanceSoid < inventory::kFirstProfileItemInstanceSoid) {
            return false;
        }
        greatest = (std::max)(greatest, item.instanceSoid);
    }
    if (residentCount >= inventory::kProfileActionSourceCapacity
        || greatest == (std::numeric_limits<std::uint64_t>::max)()) {
        next = 0;
        return false;
    }
    next = greatest + 1U;
    return next >= inventory::kFirstProfileItemInstanceSoid;
}

/** Assigns fresh account-wide item identities to every item present in one equipment block. */
[[nodiscard]] bool remap_equipment_soids(inventory::Equipment& equipment,
                                          std::uint64_t& next) noexcept {
    for (std::optional<inventory::Item>& item : equipment.slots) {
        if (!item.has_value()) {
            continue;
        }
        if (next == 0) {
            return false;
        }
        item->instanceSoid = next;
        if (next == (std::numeric_limits<std::uint64_t>::max)()) {
            next = 0;
        } else {
            ++next;
        }
    }
    return true;
}

/** Assigns fresh identities to any unequipped items carried by a copied template. */
[[nodiscard]] bool remap_inventory_soids(inventory::CharacterItems& items,
                                          std::uint64_t& next) noexcept {
    for (std::size_t index = 0; index < items.count; ++index) {
        if (next == 0) {
            return false;
        }
        items.values[index].instanceSoid = next;
        if (next == (std::numeric_limits<std::uint64_t>::max)()) {
            next = 0;
        } else {
            ++next;
        }
    }
    return true;
}

/** Replaces only class-bound gear with fresh-instance copies from a settings template. */
[[nodiscard]] bool apply_class_template(CharacterState& target,
                                        const CharacterState& source,
                                        std::uint64_t& next) noexcept {
    for (const inventory::EquipmentSlot slot : kClassBoundSlots) {
        const std::size_t index = static_cast<std::size_t>(slot);
        target.equipment.slots[index] = source.equipment.slots[index];
        std::optional<inventory::Item>& item = target.equipment.slots[index];
        if (!item.has_value()) {
            continue;
        }
        if (next == 0) {
            return false;
        }
        item->instanceSoid = next;
        if (next == (std::numeric_limits<std::uint64_t>::max)()) {
            next = 0;
        } else {
            ++next;
        }
    }
    target.movementAbilityEntry = source.movementAbilityEntry;
    target.grenadeAbilityEntry = source.grenadeAbilityEntry;
    target.superAbilityEntry = source.superAbilityEntry;
    target.meleeAbilityEntry = source.meleeAbilityEntry;
    target.classAbilityEntry = source.classAbilityEntry;
    return true;
}

/** Validates editor-owned scalar metadata before it reaches account State. */
[[nodiscard]] bool valid_edit(const CharacterEdit& edit) noexcept {
    return edit.race <= CharacterRace::exo && edit.gender <= CharacterGender::female
           && edit.characterClass <= CharacterClass::warlock && std::isfinite(edit.appearanceValue);
}

/** Converts the character-store result used by the shared commit into the item editor's shape. */
[[nodiscard]] EquipmentMutationResult
equipment_commit_result(CharacterMutationResult result) noexcept {
    switch (result) {
    case CharacterMutationResult::ok:
        return EquipmentMutationResult::ok;
    case CharacterMutationResult::persistenceUnavailable:
        return EquipmentMutationResult::persistenceUnavailable;
    case CharacterMutationResult::persistenceFailed:
        return EquipmentMutationResult::persistenceFailed;
    default:
        return EquipmentMutationResult::invalid;
    }
}

/** Publishes one validated candidate into process runtime State. Caller owns g_stateLock. */
[[nodiscard]] CharacterMutationResult commit_character_account(const AccountState& candidate) noexcept {
    if (!account::valid(candidate)) {
        return CharacterMutationResult::invalid;
    }
    // Live State is authoritative. Disk persistence follows a successful Queuez publication (or an
    // explicit transition-only edit) instead of deciding whether the runtime mutation may happen.
    runtime::storage::g_state.account = candidate;
    return CharacterMutationResult::ok;
}

namespace family4_loadout = middleware::datagen::family4::loadout;
namespace item_details = build_data::items::details;
namespace inventory_buckets = build_data::inventory::buckets;

/** Resolves one installed stackable profile row and whether it needs a resident action identity. */
[[nodiscard]] bool profile_item_contract(std::uint32_t definitionHash,
                                         build_data::items::Definition& definition,
                                         item_details::Definition& detail,
                                         inventory_buckets::Descriptor& bucket,
                                         bool& actionSource) noexcept {
    actionSource = false;
    if (definitionHash == inventory::kNoDefinitionHash
        || !build_data::find_item_definition_hash(definitionHash, definition)
        || definition.definitionHash != definitionHash
        || definition.bucketId == build_data::items::kUnresolvedBucketId
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId || detail.maxStackSize <= 0
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::stackable
        || !build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::profile) {
        return false;
    }
    actionSource = build_data::is_profile_action_source(definition.definitionIndex,
                                                        definition.bucketId);
    return true;
}

/** Returns true when two complete profile inventories match, including their empty tails. */
[[nodiscard]] bool same_profile_inventory(
    const std::array<inventory::ProfileItem, inventory::kProfileItemCapacity>& left,
    std::size_t leftCount,
    const std::array<inventory::ProfileItem, inventory::kProfileItemCapacity>& right,
    std::size_t rightCount) noexcept {
    if (leftCount != rightCount) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const inventory::ProfileItem& a = left[index];
        const inventory::ProfileItem& b = right[index];
        if (a.instanceSoid != b.instanceSoid || a.definitionHash != b.definitionHash
            || a.quantity != b.quantity || a.mutationSerial != b.mutationSerial) {
            return false;
        }
    }
    return true;
}

/** Native placement of one character-owned instance in a resolved Family-4 loadout. */
struct ResolvedInventoryPosition final {
    std::uint16_t inventoryRow{};
    std::uint8_t nativeEquipmentSlot{};
    bool equipped{};
};

/** Resolves the native bucket/equipment contract for one authored item definition. */
[[nodiscard]] bool item_contract(std::uint32_t definitionHash,
                                 std::uint8_t& bucketId,
                                 std::uint8_t& nativeEquipmentSlot) noexcept {
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (definitionHash == inventory::kNoDefinitionHash
        || !build_data::find_item_definition_hash(definitionHash, definition)
        || definition.definitionHash != definitionHash
        || definition.bucketId == build_data::items::kUnresolvedBucketId
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId || !detail.equipmentSlot.has_value()
        || *detail.equipmentSlot < 0
        || static_cast<std::size_t>(*detail.equipmentSlot) >= item_details::kEquipmentSlotCount
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::instanced
        || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::character) {
        return false;
    }
    bucketId = detail.bucketId;
    nativeEquipmentSlot = static_cast<std::uint8_t>(*detail.equipmentSlot);
    return true;
}

/** Learns the stable semantic slot for one native slot from active or factory equipped instances. */
[[nodiscard]] bool semantic_slot_for_native(const AccountState& accountState,
                                            std::uint8_t nativeEquipmentSlot,
                                            std::size_t& semanticIndex) noexcept {
    semanticIndex = inventory::kEquipmentSlotCount;
    const auto scan = [&](const AccountState& source) noexcept {
        for (std::size_t characterIndex = 0; characterIndex < source.characterCount;
             ++characterIndex) {
            const CharacterState& character = source.characters[characterIndex];
            for (std::size_t index = 0; index < character.equipment.slots.size(); ++index) {
                const std::optional<inventory::Item>& item = character.equipment.slots[index];
                if (!item.has_value()) {
                    continue;
                }
                std::uint8_t bucketId = 0;
                std::uint8_t candidateNativeSlot = 0;
                if (!item_contract(item->definitionHash, bucketId, candidateNativeSlot)) {
                    return false;
                }
                if (candidateNativeSlot != nativeEquipmentSlot) {
                    continue;
                }
                if (semanticIndex != inventory::kEquipmentSlotCount && semanticIndex != index) {
                    return false;
                }
                semanticIndex = index;
            }
        }
        return true;
    };
    if (!scan(accountState)) {
        return false;
    }
    // Once the only item in a slot is unequipped, the active account no longer carries that
    // semantic/native link. The immutable factory loadouts retain the measured relationship.
    if (semanticIndex == inventory::kEquipmentSlotCount && !scan(character_store::configured_account())) {
        return false;
    }
    return semanticIndex < inventory::kEquipmentSlotCount;
}

/** Finds one instance exactly once inside one resolved loadout. */
[[nodiscard]] bool find_position(const family4_loadout::ResolvedLoadout& loadout,
                                 std::uint64_t instanceSoid,
                                 ResolvedInventoryPosition& output) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < loadout.itemCount; ++index) {
        const family4_loadout::ResolvedItem& item = loadout.items[index];
        if (item.instance.instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        output.inventoryRow = item.inventoryRow;
        output.nativeEquipmentSlot = item.equipmentSlot;
        output.equipped = item.equipped;
    }
    return found;
}

[[nodiscard]] bool same_position(const ResolvedInventoryPosition& left,
                                 const ResolvedInventoryPosition& right) noexcept {
    return left.inventoryRow == right.inventoryRow
           && left.nativeEquipmentSlot == right.nativeEquipmentSlot
           && left.equipped == right.equipped;
}

[[nodiscard]] bool same_sockets(const inventory::Sockets& left,
                                const inventory::Sockets& right) noexcept {
    return left.policy == right.policy && left.plugCount == right.plugCount
           && left.plugs == right.plugs;
}

[[nodiscard]] bool same_item(const inventory::Item& left, const inventory::Item& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.level == right.level && left.quantity == right.quantity
           && left.mutationSerial == right.mutationSerial && left.flags == right.flags
           && same_sockets(left.sockets, right.sockets);
}

[[nodiscard]] bool same_character(const CharacterState& left, const CharacterState& right) noexcept;

struct OwnedItemLocation final {
    bool equipped{};
    std::size_t index{};
};

[[nodiscard]] const inventory::Item*
owned_item_at(const CharacterState& character, const OwnedItemLocation& location) noexcept {
    if (location.equipped) {
        if (location.index >= character.equipment.slots.size()
            || !character.equipment.slots[location.index].has_value()) {
            return nullptr;
        }
        return &*character.equipment.slots[location.index];
    }
    return location.index < character.inventory.count ? &character.inventory.values[location.index]
                                                       : nullptr;
}

[[nodiscard]] inventory::Item*
owned_item_at(CharacterState& character, const OwnedItemLocation& location) noexcept {
    if (location.equipped) {
        if (location.index >= character.equipment.slots.size()
            || !character.equipment.slots[location.index].has_value()) {
            return nullptr;
        }
        return &*character.equipment.slots[location.index];
    }
    return location.index < character.inventory.count ? &character.inventory.values[location.index]
                                                       : nullptr;
}

[[nodiscard]] bool find_owned_item(const CharacterState& character,
                                   std::uint64_t instanceSoid,
                                   OwnedItemLocation& location) noexcept {
    bool found = false;
    for (std::size_t index = 0; index < character.equipment.slots.size(); ++index) {
        const auto& item = character.equipment.slots[index];
        if (!item.has_value() || item->instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        location = {true, index};
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (character.inventory.values[index].instanceSoid != instanceSoid) {
            continue;
        }
        if (found) {
            return false;
        }
        found = true;
        location = {false, index};
    }
    return found;
}

[[nodiscard]] bool materialize_native_sockets(const item_details::Definition& detail,
                                              inventory::Sockets& sockets) noexcept {
    sockets = {};
    if (detail.ordinarySocketState != item_details::OrdinarySocketState::present
        || detail.ordinarySocketCount > sockets.plugs.size()) {
        return false;
    }
    sockets.policy = inventory::SocketPolicy::authored;
    sockets.plugCount = detail.ordinarySocketCount;
    for (std::size_t lane = 0; lane < sockets.plugCount; ++lane) {
        const std::uint16_t plugIndex = detail.initialPlugIndices[lane];
        if (plugIndex == item_details::kUnavailableItemIndex) {
            continue;
        }
        build_data::items::Definition plug{};
        if (!build_data::find_item_definition_index(plugIndex, plug)
            || plug.definitionIndex != plugIndex
            || plug.definitionHash == inventory::kNoDefinitionHash) {
            return false;
        }
        sockets.plugs[lane] = plug.definitionHash;
    }
    return inventory::valid(sockets);
}

[[nodiscard]] SocketMutationResult stage_socket_plug(const AccountState& snapshot,
                                                     std::size_t characterIndex,
                                                     std::uint64_t targetInstanceSoid,
                                                     std::uint8_t socketLane,
                                                     std::uint16_t plugDefinitionIndex,
                                                     PendingSocketPlug& mutation) noexcept {
    mutation = {};
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || targetInstanceSoid == 0 || socketLane >= inventory::kPlugCapacity
        || !build_data::socket_plug_rules_ready()) {
        return SocketMutationResult::invalid;
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return SocketMutationResult::noSelectedCharacter;
    }

    OwnedItemLocation location{};
    if (!find_owned_item(before, targetInstanceSoid, location)) {
        return SocketMutationResult::notFound;
    }
    const inventory::Item* target = owned_item_at(before, location);
    if (target == nullptr) {
        return SocketMutationResult::notFound;
    }

    build_data::items::Definition targetDefinition{};
    build_data::items::Definition plugDefinition{};
    item_details::Definition targetDetail{};
    if (!build_data::find_item_definition_hash(target->definitionHash, targetDefinition)
        || targetDefinition.definitionHash != target->definitionHash
        || !build_data::find_configured_item_detail(targetDefinition.definitionIndex, targetDetail)
        || targetDetail.definitionIndex != targetDefinition.definitionIndex
        || targetDetail.definitionHash != targetDefinition.definitionHash
        || targetDetail.ordinarySocketState != item_details::OrdinarySocketState::present) {
        return SocketMutationResult::unsupportedDefinition;
    }
    if (socketLane >= targetDetail.ordinarySocketCount) {
        return SocketMutationResult::badLane;
    }
    if (!build_data::find_item_definition_index(plugDefinitionIndex, plugDefinition)
        || plugDefinition.definitionIndex != plugDefinitionIndex
        || plugDefinition.definitionHash == inventory::kNoDefinitionHash) {
        return SocketMutationResult::unsupportedDefinition;
    }
    if (!build_data::is_socket_plug_allowed(
            targetDefinition.definitionIndex, socketLane, plugDefinitionIndex)) {
        return SocketMutationResult::incompatiblePlug;
    }

    inventory::Sockets sockets{};
    if (target->sockets.policy == inventory::SocketPolicy::nativeDefaults) {
        if (!materialize_native_sockets(targetDetail, sockets)) {
            return SocketMutationResult::unsupportedDefinition;
        }
    } else {
        sockets = target->sockets;
        if (sockets.policy != inventory::SocketPolicy::authored
            || sockets.plugCount != targetDetail.ordinarySocketCount || !inventory::valid(sockets)) {
            return SocketMutationResult::invalid;
        }
    }
    if (sockets.plugs[socketLane].has_value()
        && *sockets.plugs[socketLane] == plugDefinition.definitionHash) {
        return SocketMutationResult::alreadyApplied;
    }
    sockets.plugs[socketLane] = plugDefinition.definitionHash;

    CharacterState after = before;
    inventory::Item* changed = owned_item_at(after, location);
    if (changed == nullptr || !same_item(*changed, *target)) {
        return SocketMutationResult::invalid;
    }
    changed->sockets = sockets;

    AccountState candidate = snapshot;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout beforeLoadout{};
    family4_loadout::ResolvedLoadout afterLoadout{};
    ResolvedInventoryPosition beforePosition{};
    ResolvedInventoryPosition afterPosition{};
    if (!family4_loadout::resolve(snapshot, characterIndex, beforeLoadout)
        || !account::valid(candidate)
        || !family4_loadout::resolve(candidate, characterIndex, afterLoadout)
        || !find_position(beforeLoadout, targetInstanceSoid, beforePosition)
        || !find_position(afterLoadout, targetInstanceSoid, afterPosition)
        || !same_position(beforePosition, afterPosition)) {
        return SocketMutationResult::invalid;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.targetDefinitionHash = targetDefinition.definitionHash;
    mutation.plugDefinitionHash = plugDefinition.definitionHash;
    mutation.characterIndex = characterIndex;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = targetDefinition.definitionIndex;
    mutation.plugDefinitionIndex = plugDefinitionIndex;
    mutation.socketLane = socketLane;
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return SocketMutationResult::ok;
}

[[nodiscard]] bool same_socket_mutation(const PendingSocketPlug& left,
                                        const PendingSocketPlug& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.targetInstanceSoid == right.targetInstanceSoid
           && left.targetDefinitionHash == right.targetDefinitionHash
           && left.plugDefinitionHash == right.plugDefinitionHash
           && left.characterIndex == right.characterIndex && left.itemIndex == right.itemIndex
           && left.targetDefinitionIndex == right.targetDefinitionIndex
           && left.plugDefinitionIndex == right.plugDefinitionIndex
           && left.socketLane == right.socketLane && left.targetEquipped == right.targetEquipped
           && same_character(left.beforeCharacter, right.beforeCharacter)
           && same_character(left.afterCharacter, right.afterCharacter);
}

/** Builds one accumulated item-state transition for Locked/Tracked/Masterwork bits. */
[[nodiscard]] InventoryMutationResult stage_item_state(const AccountState& snapshot,
                                                       std::size_t characterIndex,
                                                       std::uint64_t targetInstanceSoid,
                                                       std::uint16_t targetDefinitionIndex,
                                                       std::uint32_t flags,
                                                       PendingItemState& mutation) noexcept {
    mutation = {};
    constexpr std::uint32_t kSupportedItemStateMask = 0x7U;
    if (!account::valid(snapshot) || characterIndex >= snapshot.characterCount
        || targetInstanceSoid == 0 || (flags & ~kSupportedItemStateMask) != 0) {
        return InventoryMutationResult::invalid;
    }
    const CharacterState& before = snapshot.characters[characterIndex];
    if (!before.selected || before.soid == 0) {
        return InventoryMutationResult::noSelectedCharacter;
    }
    OwnedItemLocation location{};
    if (!find_owned_item(before, targetInstanceSoid, location)) {
        return InventoryMutationResult::notFound;
    }
    const inventory::Item* target = owned_item_at(before, location);
    if (target == nullptr) {
        return InventoryMutationResult::notFound;
    }
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(target->definitionHash, definition)
        || definition.definitionHash != target->definitionHash
        || definition.definitionIndex != targetDefinitionIndex) {
        return InventoryMutationResult::unsupportedDefinition;
    }

    CharacterState after = before;
    inventory::Item* changed = owned_item_at(after, location);
    if (changed == nullptr || !same_item(*changed, *target)) {
        return InventoryMutationResult::invalid;
    }
    changed->flags = flags;
    AccountState candidate = snapshot;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout checked{};
    if (!account::valid(candidate) || !family4_loadout::resolve(candidate, characterIndex, checked)) {
        return InventoryMutationResult::invalid;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.accountSoid = snapshot.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.targetInstanceSoid = targetInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.itemIndex = location.index;
    mutation.targetDefinitionIndex = targetDefinitionIndex;
    mutation.beforeFlags = target->flags;
    mutation.afterFlags = flags;
    mutation.targetEquipped = location.equipped;
    mutation.prepared = true;
    return InventoryMutationResult::ok;
}

[[nodiscard]] bool same_item_state(const PendingItemState& left,
                                   const PendingItemState& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.targetInstanceSoid == right.targetInstanceSoid
           && left.characterIndex == right.characterIndex && left.itemIndex == right.itemIndex
           && left.targetDefinitionIndex == right.targetDefinitionIndex
           && left.beforeFlags == right.beforeFlags && left.afterFlags == right.afterFlags
           && left.targetEquipped == right.targetEquipped
           && same_character(left.beforeCharacter, right.beforeCharacter)
           && same_character(left.afterCharacter, right.afterCharacter);
}

[[nodiscard]] bool same_character(const CharacterState& left, const CharacterState& right) noexcept {
    if (left.soid != right.soid || left.selected != right.selected || left.race != right.race
        || left.gender != right.gender || left.characterClass != right.characterClass
        || left.level != right.level || left.accepted != right.accepted
        || left.previewAvailable != right.previewAvailable
        || left.appearanceValue != right.appearanceValue
        || left.presentationHeader != right.presentationHeader
        || left.creationHeader != right.creationHeader || left.creationTail != right.creationTail
        || left.lastOrbitedDestination != right.lastOrbitedDestination
        || left.contentBypass != right.contentBypass
        || left.movementAbilityEntry != right.movementAbilityEntry
        || left.grenadeAbilityEntry != right.grenadeAbilityEntry
        || left.superAbilityEntry != right.superAbilityEntry
        || left.meleeAbilityEntry != right.meleeAbilityEntry
        || left.classAbilityEntry != right.classAbilityEntry
        || left.inventory.count != right.inventory.count
        || left.nextInventorySerial != right.nextInventorySerial) {
        return false;
    }
    for (std::size_t index = 0; index < left.equipment.slots.size(); ++index) {
        const auto& first = left.equipment.slots[index];
        const auto& second = right.equipment.slots[index];
        if (first.has_value() != second.has_value()
            || (first.has_value() && !same_item(*first, *second))) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.inventory.values.size(); ++index) {
        const inventory::Item& first = left.inventory.values[index];
        const inventory::Item& second = right.inventory.values[index];
        const bool occupied = index < left.inventory.count;
        if (occupied ? !same_item(first, second)
                     : (first.instanceSoid != second.instanceSoid
                        || first.definitionHash != second.definitionHash
                        || first.level != second.level || first.quantity != second.quantity
                        || first.mutationSerial != second.mutationSerial
                        || first.flags != second.flags || !same_sockets(first.sockets, second.sockets))) {
            return false;
        }
    }
    return true;
}

/**
 * Assigns fresh native row generations only to items whose resolved row/equipped marker changed.
 * The shape-only transition is resolved once before stamping and again afterwards so serialization
 * cannot silently move a different item while State is committed.
 */
[[nodiscard]] bool finalize_inventory_transition(const AccountState& beforeAccount,
                                                 std::size_t characterIndex,
                                                 const family4_loadout::ResolvedLoadout& beforeLoadout,
                                                 CharacterState& afterCharacter) noexcept {
    if (characterIndex >= beforeAccount.characterCount
        || afterCharacter.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    AccountState placedAccount = beforeAccount;
    placedAccount.characters[characterIndex] = afterCharacter;
    family4_loadout::ResolvedLoadout placedAfter{};
    if (!account::valid(placedAccount)
        || !family4_loadout::resolve(placedAccount, characterIndex, placedAfter)
        || placedAfter.itemCount != beforeLoadout.itemCount) {
        return false;
    }

    std::size_t movedCount = 0;
    const auto count_move = [&](const inventory::Item& item) noexcept {
        ResolvedInventoryPosition before{};
        ResolvedInventoryPosition after{};
        if (!find_position(beforeLoadout, item.instanceSoid, before)
            || !find_position(placedAfter, item.instanceSoid, after)
            || before.nativeEquipmentSlot != after.nativeEquipmentSlot) {
            return false;
        }
        movedCount += static_cast<std::size_t>(!same_position(before, after));
        return true;
    };
    for (const std::optional<inventory::Item>& item : afterCharacter.equipment.slots) {
        if (item.has_value() && !count_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < afterCharacter.inventory.count; ++index) {
        if (!count_move(afterCharacter.inventory.values[index])) {
            return false;
        }
    }
    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    if (movedCount == 0 || movedCount > kMaximumInventorySerial - afterCharacter.nextInventorySerial) {
        return false;
    }

    const auto stamp_move = [&](inventory::Item& item) noexcept {
        ResolvedInventoryPosition before{};
        ResolvedInventoryPosition after{};
        if (!find_position(beforeLoadout, item.instanceSoid, before)
            || !find_position(placedAfter, item.instanceSoid, after)
            || before.nativeEquipmentSlot != after.nativeEquipmentSlot) {
            return false;
        }
        if (!same_position(before, after)) {
            item.mutationSerial = static_cast<std::int32_t>(afterCharacter.nextInventorySerial++);
        }
        return true;
    };
    for (std::optional<inventory::Item>& item : afterCharacter.equipment.slots) {
        if (item.has_value() && !stamp_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < afterCharacter.inventory.count; ++index) {
        if (!stamp_move(afterCharacter.inventory.values[index])) {
            return false;
        }
    }

    AccountState checkedAccount = beforeAccount;
    checkedAccount.characters[characterIndex] = afterCharacter;
    family4_loadout::ResolvedLoadout checkedAfter{};
    if (!account::valid(checkedAccount)
        || !family4_loadout::resolve(checkedAccount, characterIndex, checkedAfter)
        || checkedAfter.itemCount != placedAfter.itemCount) {
        return false;
    }
    for (const std::optional<inventory::Item>& item : afterCharacter.equipment.slots) {
        if (!item.has_value()) {
            continue;
        }
        ResolvedInventoryPosition placed{};
        ResolvedInventoryPosition checked{};
        if (!find_position(placedAfter, item->instanceSoid, placed)
            || !find_position(checkedAfter, item->instanceSoid, checked)
            || !same_position(placed, checked)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < afterCharacter.inventory.count; ++index) {
        const inventory::Item& item = afterCharacter.inventory.values[index];
        ResolvedInventoryPosition placed{};
        ResolvedInventoryPosition checked{};
        if (!find_position(placedAfter, item.instanceSoid, placed)
            || !find_position(checkedAfter, item.instanceSoid, checked)
            || !same_position(placed, checked)) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Stores the active account key without publishing an incomplete account. */
bool set_primary_soid(std::uint64_t primarySoid) noexcept {
    if (primarySoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    const std::uint64_t previousPrimarySoid = candidate.primarySoid;
    // Preserve each character's account-relative identity when the Client replaces the reference
    // account key with its live one. This also keeps identities stable across character deletion.
    if (!rebase_character_soids(candidate, previousPrimarySoid, primarySoid)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the settings and identity rules hold together.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return true;
}

/** Moves the selection to one authored character. */
bool set_selected_character(std::uint64_t characterSoid, bool& changed) noexcept {
    changed = false;
    if (characterSoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    std::size_t picked = candidate.characterCount;
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        if (candidate.characters[index].soid == characterSoid) {
            picked = index;
        }
    }
    if (picked == candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }

    const bool alreadySelected = candidate.characters[picked].selected;
    for (CharacterState& character : candidate.characters) {
        character.selected = false;
    }
    candidate.characters[picked].selected = true;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    // Publish only after the whole account still meets its identity rules.
    runtime::storage::g_state.account = candidate;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    changed = !alreadySelected;
    return true;
}

/** @return A copy of the active account state, read under the lock. */
AccountState account_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const AccountState snapshot = runtime::storage::g_state.account;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

/** Creates one template-backed character directly in runtime State. */
CharacterMutationResult create_character(CharacterRace race,
                                         CharacterGender gender,
                                         CharacterClass characterClass,
                                         std::size_t& createdIndex) noexcept {
    createdIndex = kCharacterCapacity;
    if (race > CharacterRace::exo || gender > CharacterGender::female
        || characterClass > CharacterClass::warlock) {
        return CharacterMutationResult::invalid;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (candidate.characterCount >= candidate.characters.size()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::full;
    }
    CharacterState templateCharacter{};
    if (!character_store::template_for_class(characterClass, templateCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::missingTemplate;
    }
    std::uint64_t next = 0;
    if (!next_instance_soid(candidate, next)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }

    CharacterState created = templateCharacter;
    created.selected = false;
    created.race = race;
    created.gender = gender;
    created.characterClass = characterClass;
    if (!remap_equipment_soids(created.equipment, next)
        || !remap_inventory_soids(created.inventory, next)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }
    std::uint64_t characterOffset = 0;
    if (!next_character_offset(candidate, characterOffset)
        || candidate.primarySoid > (std::numeric_limits<std::uint64_t>::max)() - characterOffset) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }
    const std::size_t index = candidate.characterCount;
    created.soid = candidate.primarySoid + characterOffset;
    candidate.characters[index] = created;
    ++candidate.characterCount;
    const CharacterMutationResult result = commit_character_account(candidate);
    if (result == CharacterMutationResult::ok) {
        createdIndex = index;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

/** Creates one template-backed runtime character with native presentation/creation bytes. */
CharacterMutationResult create_character_native(const NativeCharacterCreation& creation,
                                                std::size_t& createdIndex) noexcept {
    createdIndex = kCharacterCapacity;
    if (creation.race > CharacterRace::exo || creation.gender > CharacterGender::female
        || creation.characterClass > CharacterClass::warlock) {
        return CharacterMutationResult::invalid;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (candidate.characterCount >= candidate.characters.size()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::full;
    }
    CharacterState templateCharacter{};
    if (!character_store::template_for_class(creation.characterClass, templateCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::missingTemplate;
    }
    std::uint64_t next = 0;
    if (!next_instance_soid(candidate, next)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }

    CharacterState created = templateCharacter;
    created.selected = false;
    created.race = creation.race;
    created.gender = creation.gender;
    created.characterClass = creation.characterClass;
    created.presentationHeader = creation.presentationHeader;
    created.creationHeader = creation.creationHeader;
    created.creationTail = creation.creationTail;
    if (!remap_equipment_soids(created.equipment, next)
        || !remap_inventory_soids(created.inventory, next)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }
    std::uint64_t characterOffset = 0;
    if (!next_character_offset(candidate, characterOffset)
        || candidate.primarySoid > (std::numeric_limits<std::uint64_t>::max)() - characterOffset) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }
    const std::size_t index = candidate.characterCount;
    created.soid = candidate.primarySoid + characterOffset;
    candidate.characters[index] = created;
    ++candidate.characterCount;
    const CharacterMutationResult result = commit_character_account(candidate);
    if (result == CharacterMutationResult::ok) {
        createdIndex = index;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

/** Updates editable character metadata, swapping only class-bound template gear on class changes. */
CharacterMutationResult update_character(std::size_t index,
                                         const CharacterEdit& edit,
                                         bool& classChanged) noexcept {
    classChanged = false;
    if (!valid_edit(edit)) {
        return CharacterMutationResult::invalid;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (index >= candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::notFound;
    }
    CharacterState& character = candidate.characters[index];
    if (character.characterClass != edit.characterClass) {
        CharacterState templateCharacter{};
        if (!character_store::template_for_class(edit.characterClass, templateCharacter)) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return CharacterMutationResult::missingTemplate;
        }
        std::uint64_t next = 0;
        if (!next_instance_soid(candidate, next)
            || !apply_class_template(character, templateCharacter, next)) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return CharacterMutationResult::invalid;
        }
        character.characterClass = edit.characterClass;
        classChanged = true;
    }
    character.race = edit.race;
    character.gender = edit.gender;
    character.level = edit.level;
    character.accepted = edit.accepted;
    character.previewAvailable = edit.previewAvailable;
    character.appearanceValue = edit.appearanceValue;
    character.lastOrbitedDestination = edit.lastOrbitedDestination;
    character.contentBypass = edit.contentBypass;
    const CharacterMutationResult result = commit_character_account(candidate);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

/** Replaces one equipped definition after validating it against the working slot contract. */
EquipmentMutationResult update_equipment_definition(std::size_t characterIndex,
                                                    inventory::EquipmentSlot slot,
                                                    std::uint32_t definitionHash) noexcept {
    const std::size_t slotIndex = static_cast<std::size_t>(slot);
    if (slotIndex >= inventory::kEquipmentSlotCount
        || definitionHash == inventory::kNoDefinitionHash) {
        return EquipmentMutationResult::invalid;
    }

    // Semantic slot names are Sunrise-owned. Native equipment-slot numbers are not assumed here:
    // the currently equipped, already-working item supplies both the inventory bucket and native
    // equipment slot that any replacement must preserve.
    const AccountState observed = account_snapshot();
    if (characterIndex >= observed.characterCount) {
        return EquipmentMutationResult::notFound;
    }
    const std::optional<inventory::Item>& observedItem =
        observed.characters[characterIndex].equipment.slots[slotIndex];
    if (!observedItem.has_value()) {
        return EquipmentMutationResult::emptySlot;
    }
    const std::uint32_t referenceHash = observedItem->definitionHash;

    build_data::items::Definition referenceDefinition{};
    build_data::items::details::Definition referenceDetail{};
    build_data::items::Definition itemDefinition{};
    build_data::items::details::Definition itemDetail{};
    if (!build_data::find_item_definition_hash(referenceHash, referenceDefinition)
        || !build_data::find_configured_item_detail(referenceDefinition.definitionIndex,
                                                     referenceDetail)
        || referenceDetail.definitionHash != referenceHash
        || !referenceDetail.equipmentSlot.has_value()
        || !build_data::find_item_definition_hash(definitionHash, itemDefinition)
        || !build_data::find_configured_item_detail(itemDefinition.definitionIndex, itemDetail)
        || itemDetail.definitionHash != definitionHash
        || !itemDetail.equipmentSlot.has_value()) {
        return EquipmentMutationResult::unsupportedDefinition;
    }
    if (referenceDefinition.bucketId == build_data::items::kUnresolvedBucketId
        || itemDefinition.bucketId != referenceDefinition.bucketId
        || referenceDetail.bucketId != referenceDefinition.bucketId
        || itemDetail.bucketId != itemDefinition.bucketId
        || itemDetail.equipmentSlot != referenceDetail.equipmentSlot) {
        return EquipmentMutationResult::incompatibleSlot;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (characterIndex >= candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EquipmentMutationResult::notFound;
    }
    CharacterState& character = candidate.characters[characterIndex];
    std::optional<inventory::Item>& item = character.equipment.slots[slotIndex];
    if (!item.has_value()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EquipmentMutationResult::emptySlot;
    }
    // Refuse a stale UI action if another edit changed this slot while native details were read.
    if (item->definitionHash != referenceHash) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return EquipmentMutationResult::invalid;
    }
    if (slot == inventory::EquipmentSlot::subclass) {
        const build_data::abilities::Selection selection{character.movementAbilityEntry,
                                                         character.grenadeAbilityEntry,
                                                         character.superAbilityEntry,
                                                         character.meleeAbilityEntry,
                                                         character.classAbilityEntry};
        build_data::abilities::Definition abilityBuckets{};
        if (!build_data::find_ability_buckets(
                itemDetail.socketEntryListIndex, selection, abilityBuckets)) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return EquipmentMutationResult::unsupportedDefinition;
        }
    }
    item->definitionHash = definitionHash;
    item->sockets = {};
    const EquipmentMutationResult result =
        equipment_commit_result(commit_character_account(candidate));
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

/** Stages one selected-character equip/unequip transition without publishing runtime State. */
[[nodiscard]] InventoryMutationResult stage_inventory_move(const AccountState& accountState,
                                                           std::uint64_t requestedInstanceSoid,
                                                           InventoryMoveKind kind,
                                                           PendingInventoryMove& mutation) noexcept {
    mutation = {};
    if (requestedInstanceSoid == 0
        || (kind != InventoryMoveKind::equip && kind != InventoryMoveKind::unequip)
        || !account::valid(accountState)) {
        return InventoryMutationResult::invalid;
    }

    std::size_t characterIndex = accountState.characterCount;
    for (std::size_t index = 0; index < accountState.characterCount; ++index) {
        if (!accountState.characters[index].selected) {
            continue;
        }
        if (characterIndex != accountState.characterCount) {
            return InventoryMutationResult::invalid;
        }
        characterIndex = index;
    }
    if (characterIndex >= accountState.characterCount) {
        return InventoryMutationResult::noSelectedCharacter;
    }

    const CharacterState& before = accountState.characters[characterIndex];
    family4_loadout::ResolvedLoadout beforeLoadout{};
    if (!family4_loadout::resolve(accountState, characterIndex, beforeLoadout)) {
        return InventoryMutationResult::unsupportedDefinition;
    }

    std::size_t semanticIndex = before.equipment.slots.size();
    std::uint8_t requestedBucket = 0;
    std::uint8_t requestedNativeSlot = 0;
    CharacterState after = before;

    if (kind == InventoryMoveKind::equip) {
        std::size_t inventoryIndex = before.inventory.count;
        for (std::size_t index = 0; index < before.inventory.count; ++index) {
            if (before.inventory.values[index].instanceSoid != requestedInstanceSoid) {
                continue;
            }
            if (inventoryIndex != before.inventory.count) {
                return InventoryMutationResult::invalid;
            }
            inventoryIndex = index;
        }
        if (inventoryIndex >= before.inventory.count) {
            return InventoryMutationResult::notFound;
        }

        const inventory::Item requested = before.inventory.values[inventoryIndex];
        if (!item_contract(requested.definitionHash, requestedBucket, requestedNativeSlot)
            || !semantic_slot_for_native(accountState, requestedNativeSlot, semanticIndex)
            || semanticIndex >= before.equipment.slots.size()) {
            return InventoryMutationResult::unsupportedDefinition;
        }

        const std::optional<inventory::Item>& previous = before.equipment.slots[semanticIndex];
        if (previous.has_value()) {
            std::uint8_t previousBucket = 0;
            std::uint8_t previousNativeSlot = 0;
            if (!item_contract(previous->definitionHash, previousBucket, previousNativeSlot)
                || previousBucket != requestedBucket || previousNativeSlot != requestedNativeSlot) {
                return InventoryMutationResult::incompatibleSlot;
            }
            // Replace the requested backpack row with the old equipped item. The new item then
            // occupies the bucket's equipped row without shifting the rest of that bucket.
            after.inventory.values[inventoryIndex] = *previous;
        } else {
            for (std::size_t index = inventoryIndex; index + 1U < after.inventory.count; ++index) {
                after.inventory.values[index] = after.inventory.values[index + 1U];
            }
            --after.inventory.count;
            after.inventory.values[after.inventory.count] = {};
        }
        after.equipment.slots[semanticIndex] = requested;
    } else {
        for (std::size_t index = 0; index < before.equipment.slots.size(); ++index) {
            const std::optional<inventory::Item>& item = before.equipment.slots[index];
            if (!item.has_value() || item->instanceSoid != requestedInstanceSoid) {
                continue;
            }
            if (semanticIndex != before.equipment.slots.size()) {
                return InventoryMutationResult::invalid;
            }
            semanticIndex = index;
        }
        if (semanticIndex >= before.equipment.slots.size()) {
            return InventoryMutationResult::notFound;
        }
        if (before.inventory.count >= before.inventory.values.size()) {
            return InventoryMutationResult::full;
        }

        const inventory::Item requested = *before.equipment.slots[semanticIndex];
        if (!item_contract(requested.definitionHash, requestedBucket, requestedNativeSlot)) {
            return InventoryMutationResult::unsupportedDefinition;
        }
        std::size_t learnedSemantic = inventory::kEquipmentSlotCount;
        if (!semantic_slot_for_native(accountState, requestedNativeSlot, learnedSemantic)
            || learnedSemantic != semanticIndex) {
            return InventoryMutationResult::incompatibleSlot;
        }

        // Insert immediately before existing unequipped rows in this native bucket. Equipment
        // previously occupied its first physical row, so existing backpack rows stay stationary.
        std::size_t insertIndex = before.inventory.count;
        for (std::size_t index = 0; index < before.inventory.count; ++index) {
            std::uint8_t bucketId = 0;
            std::uint8_t nativeSlot = 0;
            if (!item_contract(before.inventory.values[index].definitionHash, bucketId, nativeSlot)) {
                return InventoryMutationResult::unsupportedDefinition;
            }
            if (bucketId == requestedBucket) {
                insertIndex = index;
                break;
            }
        }
        for (std::size_t index = after.inventory.count; index > insertIndex; --index) {
            after.inventory.values[index] = after.inventory.values[index - 1U];
        }
        after.inventory.values[insertIndex] = requested;
        ++after.inventory.count;
        after.equipment.slots[semanticIndex].reset();
    }

    if (!finalize_inventory_transition(accountState, characterIndex, beforeLoadout, after)) {
        return InventoryMutationResult::invalid;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.accountSoid = accountState.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.requestedInstanceSoid = requestedInstanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.semanticEquipmentIndex = semanticIndex;
    mutation.nativeEquipmentSlot = requestedNativeSlot;
    mutation.kind = kind;
    mutation.prepared = true;
    return InventoryMutationResult::ok;
}

/** Confirms a restaged inventory move is byte-for-byte the same authored transition. */
[[nodiscard]] bool same_inventory_move(const PendingInventoryMove& left,
                                       const PendingInventoryMove& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.requestedInstanceSoid == right.requestedInstanceSoid
           && left.characterIndex == right.characterIndex
           && left.semanticEquipmentIndex == right.semanticEquipmentIndex
           && left.nativeEquipmentSlot == right.nativeEquipmentSlot && left.kind == right.kind
           && same_character(left.beforeCharacter, right.beforeCharacter)
           && same_character(left.afterCharacter, right.afterCharacter);
}

/** Prepares one native equip/unequip without changing runtime State. */
InventoryMutationResult prepare_inventory_move(std::uint64_t requestedInstanceSoid,
                                               InventoryMoveKind kind,
                                               PendingInventoryMove& mutation) noexcept {
    return stage_inventory_move(account_snapshot(), requestedInstanceSoid, kind, mutation);
}

/** Builds the exact account after-image while a prepared inventory move remains current. */
bool preview_inventory_move(const PendingInventoryMove& mutation, AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.requestedInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity
        || mutation.semanticEquipmentIndex >= inventory::kEquipmentSlotCount
        || (mutation.kind != InventoryMoveKind::equip
            && mutation.kind != InventoryMoveKind::unequip)) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    PendingInventoryMove canonical{};
    if (stage_inventory_move(current,
                             mutation.requestedInstanceSoid,
                             mutation.kind,
                             canonical)
        != InventoryMutationResult::ok
        || !same_inventory_move(canonical, mutation)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    return account::valid(after) && family4_loadout::resolve(after, mutation.characterIndex, checked);
}

/** Commits one prepared move only while its complete target-character view is unchanged. */
bool commit_inventory_move(PendingInventoryMove& mutation) noexcept {
    const PendingInventoryMove prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.requestedInstanceSoid == 0 || prepared.characterIndex >= kCharacterCapacity
        || prepared.semanticEquipmentIndex >= inventory::kEquipmentSlotCount
        || (prepared.kind != InventoryMoveKind::equip
            && prepared.kind != InventoryMoveKind::unequip)) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || prepared.characterIndex >= current.characterCount
        || !same_character(current.characters[prepared.characterIndex], prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingInventoryMove canonical{};
    if (stage_inventory_move(current,
                             prepared.requestedInstanceSoid,
                             prepared.kind,
                             canonical)
            != InventoryMutationResult::ok
        || !same_inventory_move(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.characters[prepared.characterIndex] = canonical.afterCharacter;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

/** Finds the selected character once and rejects malformed multi-selection State. */
[[nodiscard]] bool selected_character_index(const AccountState& accountState,
                                            std::size_t& characterIndex) noexcept {
    characterIndex = accountState.characterCount;
    for (std::size_t index = 0; index < accountState.characterCount; ++index) {
        if (!accountState.characters[index].selected) {
            continue;
        }
        if (characterIndex != accountState.characterCount) {
            return false;
        }
        characterIndex = index;
    }
    return characterIndex < accountState.characterCount;
}

/** Uses the selected Guardian's current gear generation for a newly pulled item. */
[[nodiscard]] std::int32_t current_item_level(const CharacterState& character) noexcept {
    std::int32_t level = static_cast<std::int32_t>(character.level);
    for (const std::optional<inventory::Item>& item : character.equipment.slots) {
        if (item.has_value()) {
            level = (std::max)(level, item->level);
        }
    }
    return level;
}

/** Builds one canonical Collections acquisition over an exact account snapshot. */
[[nodiscard]] InventoryMutationResult stage_item_acquisition(
    const AccountState& accountState,
    std::uint16_t collectibleIndex,
    PendingItemAcquisition& mutation) noexcept {
    mutation = {};
    if (!account::valid(accountState)) {
        return InventoryMutationResult::invalid;
    }
    std::size_t characterIndex = accountState.characterCount;
    if (!selected_character_index(accountState, characterIndex)) {
        return InventoryMutationResult::noSelectedCharacter;
    }
    const CharacterState& before = accountState.characters[characterIndex];
    if (before.inventory.count >= before.inventory.values.size()) {
        return InventoryMutationResult::full;
    }

    std::uint16_t itemDefinitionIndex = 0;
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    if (!build_data::find_collectible_item_definition_index(collectibleIndex, itemDefinitionIndex)
        || !build_data::find_item_definition_index(itemDefinitionIndex, definition)
        || definition.definitionIndex != itemDefinitionIndex
        || !build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || detail.instancedDefinitionState != item_details::InstancedDefinitionState::instanced
        || !detail.equipmentSlot.has_value() || *detail.equipmentSlot < 0
        || !build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)
        || bucket.arraySelector != inventory_buckets::ArraySelector::character) {
        return InventoryMutationResult::unsupportedDefinition;
    }
    const auto nativeSlot = static_cast<std::uint8_t>(*detail.equipmentSlot);
    std::size_t semanticIndex = inventory::kEquipmentSlotCount;
    if (!semantic_slot_for_native(accountState, nativeSlot, semanticIndex)) {
        return InventoryMutationResult::incompatibleSlot;
    }

    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    std::uint64_t nextSoid = 0;
    if (before.nextInventorySerial >= kMaximumInventorySerial
        || !next_instance_soid(accountState, nextSoid)) {
        return InventoryMutationResult::invalid;
    }

    CharacterState after = before;
    inventory::Item created{};
    created.instanceSoid = nextSoid;
    created.definitionHash = definition.definitionHash;
    created.level = current_item_level(before);
    created.quantity = 1;
    created.mutationSerial = static_cast<std::int32_t>(after.nextInventorySerial++);
    created.flags = 0;
    created.sockets = {};
    const std::size_t inventoryIndex = after.inventory.count;
    after.inventory.values[after.inventory.count++] = created;

    AccountState candidate = accountState;
    candidate.characters[characterIndex] = after;
    family4_loadout::ResolvedLoadout resolved{};
    ResolvedInventoryPosition position{};
    if (!account::valid(candidate) || !family4_loadout::resolve(candidate, characterIndex, resolved)
        || !find_position(resolved, nextSoid, position) || position.equipped
        || position.nativeEquipmentSlot != nativeSlot) {
        return InventoryMutationResult::invalid;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.accountSoid = accountState.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.acquiredInstanceSoid = nextSoid;
    mutation.acquiredDefinitionHash = definition.definitionHash;
    mutation.characterIndex = characterIndex;
    mutation.inventoryIndex = inventoryIndex;
    mutation.collectibleIndex = collectibleIndex;
    mutation.itemDefinitionIndex = itemDefinitionIndex;
    mutation.inventoryRow = position.inventoryRow;
    mutation.nativeEquipmentSlot = nativeSlot;
    mutation.prepared = true;
    return InventoryMutationResult::ok;
}

[[nodiscard]] bool same_item_acquisition(const PendingItemAcquisition& left,
                                         const PendingItemAcquisition& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.acquiredInstanceSoid == right.acquiredInstanceSoid
           && left.acquiredDefinitionHash == right.acquiredDefinitionHash
           && left.characterIndex == right.characterIndex
           && left.inventoryIndex == right.inventoryIndex
           && left.collectibleIndex == right.collectibleIndex
           && left.itemDefinitionIndex == right.itemDefinitionIndex
           && left.inventoryRow == right.inventoryRow
           && left.nativeEquipmentSlot == right.nativeEquipmentSlot
           && same_character(left.beforeCharacter, right.beforeCharacter)
           && same_character(left.afterCharacter, right.afterCharacter);
}

InventoryMutationResult prepare_item_acquisition(std::uint16_t collectibleIndex,
                                                 PendingItemAcquisition& mutation) noexcept {
    return stage_item_acquisition(account_snapshot(), collectibleIndex, mutation);
}

bool preview_item_acquisition(const PendingItemAcquisition& mutation, AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.acquiredInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    PendingItemAcquisition canonical{};
    if (stage_item_acquisition(current, mutation.collectibleIndex, canonical)
            != InventoryMutationResult::ok
        || !same_item_acquisition(canonical, mutation)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    return account::valid(after) && family4_loadout::resolve(after, mutation.characterIndex, checked);
}

bool commit_item_acquisition(PendingItemAcquisition& mutation) noexcept {
    const PendingItemAcquisition prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.acquiredInstanceSoid == 0 || prepared.characterIndex >= kCharacterCapacity) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || prepared.characterIndex >= current.characterCount
        || !same_character(current.characters[prepared.characterIndex], prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingItemAcquisition canonical{};
    if (stage_item_acquisition(current, prepared.collectibleIndex, canonical)
            != InventoryMutationResult::ok
        || !same_item_acquisition(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.characters[prepared.characterIndex] = canonical.afterCharacter;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

/** Builds one exact profile-stack acquisition transition without publishing runtime State. */
[[nodiscard]] ProfileInventoryMutationResult stage_profile_item_acquisition(
    const AccountState& accountState,
    std::uint16_t collectibleIndex,
    PendingProfileItemAcquisition& mutation) noexcept {
    mutation = {};
    if (!account::valid(accountState)) {
        return ProfileInventoryMutationResult::invalid;
    }

    std::uint16_t itemDefinitionIndex = 0;
    build_data::items::Definition definition{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    bool actionSource = false;
    if (!build_data::find_collectible_item_definition_index(collectibleIndex, itemDefinitionIndex)
        || !build_data::find_item_definition_index(itemDefinitionIndex, definition)
        || definition.definitionIndex != itemDefinitionIndex
        || !profile_item_contract(
            definition.definitionHash, definition, detail, bucket, actionSource)) {
        return ProfileInventoryMutationResult::notProfileItem;
    }

    std::size_t usedBucketRows = 0;
    std::size_t profileIndex = accountState.profileItemCount;
    std::int32_t greatestSerial = 0;
    std::int32_t previousQuantity = 0;
    std::int32_t previousMutationSerial = 0;
    bool appended = true;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        const inventory::ProfileItem& item = accountState.profileItems[index];
        greatestSerial = (std::max)(greatestSerial, item.mutationSerial);
        build_data::items::Definition existingDefinition{};
        if (!build_data::find_item_definition_hash(item.definitionHash, existingDefinition)
            || existingDefinition.definitionHash != item.definitionHash
            || existingDefinition.bucketId == build_data::items::kUnresolvedBucketId) {
            return ProfileInventoryMutationResult::invalid;
        }
        usedBucketRows += static_cast<std::size_t>(existingDefinition.bucketId == definition.bucketId);
        if (item.definitionHash != definition.definitionHash || item.quantity >= detail.maxStackSize) {
            continue;
        }
        const bool identityMatches = actionSource ? item.instanceSoid != 0 : item.instanceSoid == 0;
        if (!identityMatches) {
            return ProfileInventoryMutationResult::invalid;
        }
        if (appended) {
            profileIndex = index;
            previousQuantity = item.quantity;
            previousMutationSerial = item.mutationSerial;
            appended = false;
        }
    }
    if (greatestSerial == (std::numeric_limits<std::int32_t>::max)()) {
        return ProfileInventoryMutationResult::invalid;
    }
    if (appended
        && (accountState.profileItemCount >= accountState.profileItems.size()
            || usedBucketRows >= bucket.slotCount)) {
        return ProfileInventoryMutationResult::full;
    }

    AccountState after = accountState;
    std::uint64_t instanceSoid = appended ? 0 : after.profileItems[profileIndex].instanceSoid;
    if (appended && actionSource && !next_profile_instance_soid(accountState, instanceSoid)) {
        return ProfileInventoryMutationResult::full;
    }
    const std::int32_t mutationSerial = greatestSerial + 1;
    if (appended) {
        after.profileItems[profileIndex] = {
            instanceSoid, definition.definitionHash, 1, mutationSerial};
        ++after.profileItemCount;
    } else {
        ++after.profileItems[profileIndex].quantity;
        after.profileItems[profileIndex].mutationSerial = mutationSerial;
    }
    if (after.profileItems[profileIndex].quantity <= previousQuantity
        || after.profileItems[profileIndex].quantity > detail.maxStackSize
        || (actionSource != (after.profileItems[profileIndex].instanceSoid != 0))
        || !account::valid(after)) {
        return ProfileInventoryMutationResult::invalid;
    }

    mutation.beforeItems = accountState.profileItems;
    mutation.afterItems = after.profileItems;
    mutation.accountSoid = accountState.primarySoid;
    mutation.acquiredInstanceSoid = instanceSoid;
    mutation.acquiredDefinitionHash = definition.definitionHash;
    mutation.expectedItemCount = accountState.profileItemCount;
    mutation.afterItemCount = after.profileItemCount;
    mutation.profileIndex = profileIndex;
    mutation.collectibleIndex = collectibleIndex;
    mutation.itemDefinitionIndex = itemDefinitionIndex;
    mutation.bucketId = definition.bucketId;
    mutation.previousQuantity = previousQuantity;
    mutation.acquiredQuantity = after.profileItems[profileIndex].quantity;
    mutation.previousMutationSerial = previousMutationSerial;
    mutation.acquiredMutationSerial = mutationSerial;
    mutation.actionSource = actionSource;
    mutation.appended = appended;
    mutation.prepared = true;
    return ProfileInventoryMutationResult::ok;
}

[[nodiscard]] bool same_profile_acquisition(const PendingProfileItemAcquisition& left,
                                            const PendingProfileItemAcquisition& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.acquiredInstanceSoid == right.acquiredInstanceSoid
           && left.acquiredDefinitionHash == right.acquiredDefinitionHash
           && left.expectedItemCount == right.expectedItemCount
           && left.afterItemCount == right.afterItemCount && left.profileIndex == right.profileIndex
           && left.collectibleIndex == right.collectibleIndex
           && left.itemDefinitionIndex == right.itemDefinitionIndex && left.bucketId == right.bucketId
           && left.previousQuantity == right.previousQuantity
           && left.acquiredQuantity == right.acquiredQuantity
           && left.previousMutationSerial == right.previousMutationSerial
           && left.acquiredMutationSerial == right.acquiredMutationSerial
           && left.actionSource == right.actionSource && left.appended == right.appended
           && same_profile_inventory(left.beforeItems,
                                     left.expectedItemCount,
                                     right.beforeItems,
                                     right.expectedItemCount)
           && same_profile_inventory(left.afterItems,
                                     left.afterItemCount,
                                     right.afterItems,
                                     right.afterItemCount);
}

ProfileInventoryMutationResult prepare_profile_item_acquisition(
    std::uint16_t collectibleIndex,
    PendingProfileItemAcquisition& mutation) noexcept {
    return stage_profile_item_acquisition(account_snapshot(), collectibleIndex, mutation);
}

bool preview_profile_item_acquisition(const PendingProfileItemAcquisition& mutation,
                                      AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0
        || mutation.expectedItemCount > inventory::kProfileItemCapacity
        || mutation.afterItemCount > inventory::kProfileItemCapacity) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(current.profileItems,
                                   current.profileItemCount,
                                   mutation.beforeItems,
                                   mutation.expectedItemCount)) {
        return false;
    }
    PendingProfileItemAcquisition canonical{};
    if (stage_profile_item_acquisition(current, mutation.collectibleIndex, canonical)
            != ProfileInventoryMutationResult::ok
        || !same_profile_acquisition(canonical, mutation)) {
        return false;
    }
    after = current;
    after.profileItems = canonical.afterItems;
    after.profileItemCount = canonical.afterItemCount;
    return account::valid(after);
}

bool commit_profile_item_acquisition(PendingProfileItemAcquisition& mutation) noexcept {
    const PendingProfileItemAcquisition prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.accountSoid == 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || !same_profile_inventory(current.profileItems,
                                   current.profileItemCount,
                                   prepared.beforeItems,
                                   prepared.expectedItemCount)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingProfileItemAcquisition canonical{};
    if (stage_profile_item_acquisition(current, prepared.collectibleIndex, canonical)
            != ProfileInventoryMutationResult::ok
        || !same_profile_acquisition(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.profileItems = canonical.afterItems;
    current.profileItemCount = canonical.afterItemCount;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

/** Builds one canonical profile-stack decrement/removal over an exact account snapshot. */
[[nodiscard]] ProfileInventoryMutationResult stage_profile_item_dismantle(
    const AccountState& accountState,
    std::uint16_t itemDefinitionIndex,
    std::int32_t quantity,
    std::uint64_t requestedInstanceSoid,
    PendingProfileItemDismantle& mutation) noexcept {
    mutation = {};
    if (!account::valid(accountState)) {
        return ProfileInventoryMutationResult::invalid;
    }
    if (quantity <= 0) {
        return ProfileInventoryMutationResult::invalidQuantity;
    }

    build_data::items::Definition definition{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    bool actionSource = false;
    if (!build_data::find_item_definition_index(itemDefinitionIndex, definition)
        || definition.definitionIndex != itemDefinitionIndex
        || !profile_item_contract(
            definition.definitionHash, definition, detail, bucket, actionSource)) {
        return ProfileInventoryMutationResult::notProfileItem;
    }

    std::size_t profileIndex = accountState.profileItemCount;
    std::int32_t greatestSerial = 0;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        const inventory::ProfileItem& item = accountState.profileItems[index];
        greatestSerial = (std::max)(greatestSerial, item.mutationSerial);
        if (profileIndex == accountState.profileItemCount
            && item.definitionHash == definition.definitionHash
            && (requestedInstanceSoid == 0 || item.instanceSoid == requestedInstanceSoid)) {
            profileIndex = index;
        }
    }
    if (profileIndex >= accountState.profileItemCount) {
        return ProfileInventoryMutationResult::notFound;
    }

    const inventory::ProfileItem target = accountState.profileItems[profileIndex];
    if (quantity > target.quantity || actionSource != (target.instanceSoid != 0)) {
        return quantity > target.quantity ? ProfileInventoryMutationResult::invalidQuantity
                                          : ProfileInventoryMutationResult::invalid;
    }
    if (greatestSerial == (std::numeric_limits<std::int32_t>::max)()) {
        return ProfileInventoryMutationResult::invalid;
    }

    AccountState after = accountState;
    const std::int32_t remainingQuantity = target.quantity - quantity;
    std::uint64_t releasedInstanceSoid = 0;
    const bool removedRow = remainingQuantity == 0;
    if (!removedRow) {
        after.profileItems[profileIndex].quantity = remainingQuantity;
        after.profileItems[profileIndex].mutationSerial = ++greatestSerial;
    } else {
        releasedInstanceSoid = target.instanceSoid;
        for (std::size_t index = profileIndex; index + 1U < after.profileItemCount; ++index) {
            after.profileItems[index] = after.profileItems[index + 1U];
        }
        --after.profileItemCount;
        after.profileItems[after.profileItemCount] = {};

        // The account encoder assigns native rows by per-bucket encounter order. Removing one dense
        // row shifts only later items in the same bucket, so stamp exactly those survivors with new
        // mutation serials while leaving unrelated native rows untouched.
        for (std::size_t index = profileIndex; index < after.profileItemCount; ++index) {
            inventory::ProfileItem& moved = after.profileItems[index];
            build_data::items::Definition movedDefinition{};
            if (!build_data::find_item_definition_hash(moved.definitionHash, movedDefinition)
                || movedDefinition.definitionHash != moved.definitionHash
                || movedDefinition.bucketId == build_data::items::kUnresolvedBucketId) {
                return ProfileInventoryMutationResult::invalid;
            }
            if (movedDefinition.bucketId != definition.bucketId) {
                continue;
            }
            if (greatestSerial == (std::numeric_limits<std::int32_t>::max)()) {
                return ProfileInventoryMutationResult::invalid;
            }
            moved.mutationSerial = ++greatestSerial;
        }
    }
    if (!account::valid(after)) {
        return ProfileInventoryMutationResult::invalid;
    }

    mutation.beforeItems = accountState.profileItems;
    mutation.afterItems = after.profileItems;
    mutation.accountSoid = accountState.primarySoid;
    mutation.requestedInstanceSoid = requestedInstanceSoid;
    mutation.releasedInstanceSoid = releasedInstanceSoid;
    mutation.dismantledDefinitionHash = definition.definitionHash;
    mutation.expectedItemCount = accountState.profileItemCount;
    mutation.afterItemCount = after.profileItemCount;
    mutation.profileIndex = profileIndex;
    mutation.itemDefinitionIndex = itemDefinitionIndex;
    mutation.bucketId = definition.bucketId;
    mutation.previousQuantity = target.quantity;
    mutation.requestedQuantity = quantity;
    mutation.remainingQuantity = remainingQuantity;
    mutation.actionSource = actionSource;
    mutation.removedRow = removedRow;
    mutation.prepared = true;
    return ProfileInventoryMutationResult::ok;
}

[[nodiscard]] bool same_profile_dismantle(const PendingProfileItemDismantle& left,
                                          const PendingProfileItemDismantle& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.requestedInstanceSoid == right.requestedInstanceSoid
           && left.releasedInstanceSoid == right.releasedInstanceSoid
           && left.dismantledDefinitionHash == right.dismantledDefinitionHash
           && left.expectedItemCount == right.expectedItemCount
           && left.afterItemCount == right.afterItemCount && left.profileIndex == right.profileIndex
           && left.itemDefinitionIndex == right.itemDefinitionIndex && left.bucketId == right.bucketId
           && left.previousQuantity == right.previousQuantity
           && left.requestedQuantity == right.requestedQuantity
           && left.remainingQuantity == right.remainingQuantity
           && left.actionSource == right.actionSource && left.removedRow == right.removedRow
           && same_profile_inventory(left.beforeItems,
                                     left.expectedItemCount,
                                     right.beforeItems,
                                     right.expectedItemCount)
           && same_profile_inventory(left.afterItems,
                                     left.afterItemCount,
                                     right.afterItems,
                                     right.afterItemCount);
}

ProfileInventoryMutationResult prepare_profile_item_dismantle(
    std::uint16_t itemDefinitionIndex,
    std::int32_t quantity,
    std::uint64_t requestedInstanceSoid,
    PendingProfileItemDismantle& mutation) noexcept {
    return stage_profile_item_dismantle(
        account_snapshot(), itemDefinitionIndex, quantity, requestedInstanceSoid, mutation);
}

bool preview_profile_item_dismantle(const PendingProfileItemDismantle& mutation,
                                    AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.requestedQuantity <= 0
        || mutation.expectedItemCount > inventory::kProfileItemCapacity
        || mutation.afterItemCount > inventory::kProfileItemCapacity) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || !same_profile_inventory(current.profileItems,
                                   current.profileItemCount,
                                   mutation.beforeItems,
                                   mutation.expectedItemCount)) {
        return false;
    }
    PendingProfileItemDismantle canonical{};
    if (stage_profile_item_dismantle(current,
                                     mutation.itemDefinitionIndex,
                                     mutation.requestedQuantity,
                                     mutation.requestedInstanceSoid,
                                     canonical)
            != ProfileInventoryMutationResult::ok
        || !same_profile_dismantle(canonical, mutation)) {
        return false;
    }
    after = current;
    after.profileItems = canonical.afterItems;
    after.profileItemCount = canonical.afterItemCount;
    return account::valid(after);
}

bool commit_profile_item_dismantle(PendingProfileItemDismantle& mutation) noexcept {
    const PendingProfileItemDismantle prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.requestedQuantity <= 0) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || !same_profile_inventory(current.profileItems,
                                   current.profileItemCount,
                                   prepared.beforeItems,
                                   prepared.expectedItemCount)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingProfileItemDismantle canonical{};
    if (stage_profile_item_dismantle(current,
                                     prepared.itemDefinitionIndex,
                                     prepared.requestedQuantity,
                                     prepared.requestedInstanceSoid,
                                     canonical)
            != ProfileInventoryMutationResult::ok
        || !same_profile_dismantle(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.profileItems = canonical.afterItems;
    current.profileItemCount = canonical.afterItemCount;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

/** Stamps surviving items whose native row moves after one removal. */
[[nodiscard]] bool finalize_inventory_removal(
    const AccountState& beforeAccount,
    std::size_t characterIndex,
    const family4_loadout::ResolvedLoadout& beforeLoadout,
    std::uint64_t removedInstanceSoid,
    CharacterState& afterCharacter) noexcept {
    if (characterIndex >= beforeAccount.characterCount || beforeLoadout.itemCount == 0
        || afterCharacter.nextInventorySerial
               > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
        return false;
    }
    AccountState placedAccount = beforeAccount;
    placedAccount.characters[characterIndex] = afterCharacter;
    family4_loadout::ResolvedLoadout placedAfter{};
    ResolvedInventoryPosition removed{};
    if (!account::valid(placedAccount)
        || !family4_loadout::resolve(placedAccount, characterIndex, placedAfter)
        || placedAfter.itemCount + 1U != beforeLoadout.itemCount
        || !find_position(beforeLoadout, removedInstanceSoid, removed)) {
        return false;
    }
    ResolvedInventoryPosition shouldBeGone{};
    if (find_position(placedAfter, removedInstanceSoid, shouldBeGone)) {
        return false;
    }

    std::size_t movedCount = 0;
    const auto count_move = [&](const inventory::Item& item) noexcept {
        ResolvedInventoryPosition before{};
        ResolvedInventoryPosition after{};
        if (!find_position(beforeLoadout, item.instanceSoid, before)
            || !find_position(placedAfter, item.instanceSoid, after)
            || before.nativeEquipmentSlot != after.nativeEquipmentSlot) {
            return false;
        }
        movedCount += static_cast<std::size_t>(!same_position(before, after));
        return true;
    };
    for (const std::optional<inventory::Item>& item : afterCharacter.equipment.slots) {
        if (item.has_value() && !count_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < afterCharacter.inventory.count; ++index) {
        if (!count_move(afterCharacter.inventory.values[index])) {
            return false;
        }
    }
    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    if (movedCount > kMaximumInventorySerial - afterCharacter.nextInventorySerial) {
        return false;
    }

    const auto stamp_move = [&](inventory::Item& item) noexcept {
        ResolvedInventoryPosition before{};
        ResolvedInventoryPosition after{};
        if (!find_position(beforeLoadout, item.instanceSoid, before)
            || !find_position(placedAfter, item.instanceSoid, after)
            || before.nativeEquipmentSlot != after.nativeEquipmentSlot) {
            return false;
        }
        if (!same_position(before, after)) {
            item.mutationSerial = static_cast<std::int32_t>(afterCharacter.nextInventorySerial++);
        }
        return true;
    };
    for (std::optional<inventory::Item>& item : afterCharacter.equipment.slots) {
        if (item.has_value() && !stamp_move(*item)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < afterCharacter.inventory.count; ++index) {
        if (!stamp_move(afterCharacter.inventory.values[index])) {
            return false;
        }
    }

    AccountState checkedAccount = beforeAccount;
    checkedAccount.characters[characterIndex] = afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    return account::valid(checkedAccount)
           && family4_loadout::resolve(checkedAccount, characterIndex, checked)
           && checked.itemCount == placedAfter.itemCount
           && !find_position(checked, removedInstanceSoid, shouldBeGone);
}

/** Builds one canonical unequipped-item removal over an exact account snapshot. */
[[nodiscard]] InventoryMutationResult stage_item_dismantle(
    const AccountState& accountState,
    std::uint64_t instanceSoid,
    PendingItemDismantle& mutation) noexcept {
    mutation = {};
    if (instanceSoid == 0 || !account::valid(accountState)) {
        return InventoryMutationResult::invalid;
    }
    std::size_t characterIndex = accountState.characterCount;
    if (!selected_character_index(accountState, characterIndex)) {
        return InventoryMutationResult::noSelectedCharacter;
    }
    const CharacterState& before = accountState.characters[characterIndex];
    std::size_t inventoryIndex = before.inventory.count;
    for (std::size_t index = 0; index < before.inventory.count; ++index) {
        if (before.inventory.values[index].instanceSoid == instanceSoid) {
            inventoryIndex = index;
            break;
        }
    }
    if (inventoryIndex >= before.inventory.count) {
        return InventoryMutationResult::notFound;
    }

    const inventory::Item dismantled = before.inventory.values[inventoryIndex];
    build_data::items::Definition definition{};
    std::uint8_t bucketId = 0;
    std::uint8_t nativeSlot = 0;
    if (!build_data::find_item_definition_hash(dismantled.definitionHash, definition)
        || !item_contract(dismantled.definitionHash, bucketId, nativeSlot)) {
        return InventoryMutationResult::unsupportedDefinition;
    }
    family4_loadout::ResolvedLoadout beforeLoadout{};
    ResolvedInventoryPosition targetPosition{};
    if (!family4_loadout::resolve(accountState, characterIndex, beforeLoadout)
        || !find_position(beforeLoadout, instanceSoid, targetPosition) || targetPosition.equipped
        || targetPosition.nativeEquipmentSlot != nativeSlot) {
        return InventoryMutationResult::invalid;
    }

    CharacterState after = before;
    for (std::size_t index = inventoryIndex; index + 1U < after.inventory.count; ++index) {
        after.inventory.values[index] = after.inventory.values[index + 1U];
    }
    --after.inventory.count;
    after.inventory.values[after.inventory.count] = {};
    if (!finalize_inventory_removal(
            accountState, characterIndex, beforeLoadout, instanceSoid, after)) {
        return InventoryMutationResult::invalid;
    }

    mutation.beforeCharacter = before;
    mutation.afterCharacter = after;
    mutation.dismantledItem = dismantled;
    mutation.accountSoid = accountState.primarySoid;
    mutation.characterSoid = before.soid;
    mutation.dismantledInstanceSoid = instanceSoid;
    mutation.characterIndex = characterIndex;
    mutation.inventoryIndex = inventoryIndex;
    mutation.itemDefinitionIndex = definition.definitionIndex;
    mutation.inventoryRow = targetPosition.inventoryRow;
    mutation.nativeEquipmentSlot = nativeSlot;
    mutation.prepared = true;
    return InventoryMutationResult::ok;
}

[[nodiscard]] bool same_item_dismantle(const PendingItemDismantle& left,
                                       const PendingItemDismantle& right) noexcept {
    return left.prepared == right.prepared && left.accountSoid == right.accountSoid
           && left.characterSoid == right.characterSoid
           && left.dismantledInstanceSoid == right.dismantledInstanceSoid
           && left.characterIndex == right.characterIndex
           && left.inventoryIndex == right.inventoryIndex
           && left.itemDefinitionIndex == right.itemDefinitionIndex
           && left.inventoryRow == right.inventoryRow
           && left.nativeEquipmentSlot == right.nativeEquipmentSlot
           && same_item(left.dismantledItem, right.dismantledItem)
           && same_character(left.beforeCharacter, right.beforeCharacter)
           && same_character(left.afterCharacter, right.afterCharacter);
}

InventoryMutationResult prepare_item_dismantle(std::uint64_t instanceSoid,
                                               PendingItemDismantle& mutation) noexcept {
    return stage_item_dismantle(account_snapshot(), instanceSoid, mutation);
}

bool preview_item_dismantle(const PendingItemDismantle& mutation, AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.dismantledInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    PendingItemDismantle canonical{};
    if (stage_item_dismantle(current, mutation.dismantledInstanceSoid, canonical)
            != InventoryMutationResult::ok
        || !same_item_dismantle(canonical, mutation)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    return account::valid(after) && family4_loadout::resolve(after, mutation.characterIndex, checked);
}

bool commit_item_dismantle(PendingItemDismantle& mutation) noexcept {
    const PendingItemDismantle prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.accountSoid == 0 || prepared.characterSoid == 0
        || prepared.dismantledInstanceSoid == 0 || prepared.characterIndex >= kCharacterCapacity) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || prepared.characterIndex >= current.characterCount
        || !same_character(current.characters[prepared.characterIndex], prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingItemDismantle canonical{};
    if (stage_item_dismantle(current, prepared.dismantledInstanceSoid, canonical)
            != InventoryMutationResult::ok
        || !same_item_dismantle(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.characters[prepared.characterIndex] = canonical.afterCharacter;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

/** Convenience debug/UI path: prepare and commit through the same transaction used by opcode 403. */
InventoryMutationResult equip_inventory_item(std::uint64_t requestedInstanceSoid,
                                             std::uint64_t& characterSoid) noexcept {
    characterSoid = 0;
    PendingInventoryMove mutation{};
    const InventoryMutationResult result =
        prepare_inventory_move(requestedInstanceSoid, InventoryMoveKind::equip, mutation);
    if (result != InventoryMutationResult::ok) {
        return result;
    }
    characterSoid = mutation.characterSoid;
    return commit_inventory_move(mutation) ? InventoryMutationResult::ok
                                           : InventoryMutationResult::invalid;
}

/** Convenience debug/UI path: prepare and commit through the same transaction used by opcode 404. */
InventoryMutationResult unequip_inventory_item(std::uint64_t requestedInstanceSoid,
                                               std::uint64_t& characterSoid) noexcept {
    characterSoid = 0;
    PendingInventoryMove mutation{};
    const InventoryMutationResult result =
        prepare_inventory_move(requestedInstanceSoid, InventoryMoveKind::unequip, mutation);
    if (result != InventoryMutationResult::ok) {
        return result;
    }
    characterSoid = mutation.characterSoid;
    return commit_inventory_move(mutation) ? InventoryMutationResult::ok
                                           : InventoryMutationResult::invalid;
}

SocketMutationResult prepare_socket_plug(std::uint64_t targetInstanceSoid,
                                                std::uint8_t socketLane,
                                                std::uint16_t plugDefinitionIndex,
                                                PendingSocketPlug& mutation) noexcept {
    mutation = {};
    const AccountState snapshot = account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
        if (snapshot.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= snapshot.characterCount) {
        return SocketMutationResult::noSelectedCharacter;
    }
    return stage_socket_plug(
        snapshot, characterIndex, targetInstanceSoid, socketLane, plugDefinitionIndex, mutation);
}

SocketMutationResult prepare_equipped_socket_plug(std::uint64_t equipmentSelector,
                                                  std::uint8_t socketLane,
                                                  std::uint16_t plugDefinitionIndex,
                                                  PendingSocketPlug& mutation) noexcept {
    mutation = {};
    constexpr std::uint64_t kSelectorStride = 4;
    constexpr std::uint64_t kInstanceIdentityMask = 0x3FFFFFFFFFFFFFFFULL;
    if (equipmentSelector == 0 || equipmentSelector % kSelectorStride != 0) {
        return SocketMutationResult::invalid;
    }
    const std::uint64_t identity = equipmentSelector / kSelectorStride;
    const AccountState snapshot = account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
        if (snapshot.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= snapshot.characterCount) {
        return SocketMutationResult::noSelectedCharacter;
    }
    const CharacterState& character = snapshot.characters[characterIndex];
    const inventory::Item* target = nullptr;
    for (const auto& item : character.equipment.slots) {
        if (!item.has_value() || (item->instanceSoid & kInstanceIdentityMask) != identity) {
            continue;
        }
        if (target != nullptr) {
            return SocketMutationResult::invalid;
        }
        target = &*item;
    }
    if (target == nullptr || target->instanceSoid == 0) {
        return SocketMutationResult::notFound;
    }

    build_data::items::Definition definition{};
    item_details::Definition detail{};
    if (!build_data::find_item_definition_hash(target->definitionHash, definition)
        || definition.definitionHash != target->definitionHash
        || !build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.ordinarySocketState != item_details::OrdinarySocketState::present
        || detail.ordinarySocketCount > inventory::kPlugCapacity) {
        return SocketMutationResult::unsupportedDefinition;
    }

    // 1901 sometimes carries a semantic appearance action kind instead of the physical ordinary
    // socket lane (shaders are the known example). Prefer an exact compatible lane, otherwise map
    // the semantic kind to the one and only physical lane whose installed pool accepts the plug.
    std::uint8_t resolvedLane = static_cast<std::uint8_t>(inventory::kPlugCapacity);
    if (socketLane < detail.ordinarySocketCount
        && build_data::is_socket_plug_allowed(
            definition.definitionIndex, socketLane, plugDefinitionIndex)) {
        resolvedLane = socketLane;
    } else {
        for (std::uint8_t lane = 0; lane < detail.ordinarySocketCount; ++lane) {
            if (!build_data::is_socket_plug_allowed(
                    definition.definitionIndex, lane, plugDefinitionIndex)) {
                continue;
            }
            if (resolvedLane < inventory::kPlugCapacity) {
                return SocketMutationResult::incompatiblePlug;
            }
            resolvedLane = lane;
        }
    }
    if (resolvedLane >= inventory::kPlugCapacity) {
        return SocketMutationResult::incompatiblePlug;
    }

    const SocketMutationResult result = stage_socket_plug(snapshot,
                                                          characterIndex,
                                                          target->instanceSoid,
                                                          resolvedLane,
                                                          plugDefinitionIndex,
                                                          mutation);
    if (result != SocketMutationResult::ok) {
        return result;
    }
    if (!mutation.targetEquipped) {
        mutation = {};
        return SocketMutationResult::notFound;
    }
    return SocketMutationResult::ok;
}

bool preview_socket_plug(const PendingSocketPlug& mutation, AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.characterIndex >= kCharacterCapacity
        || mutation.characterSoid == 0 || mutation.targetInstanceSoid == 0) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    PendingSocketPlug canonical{};
    if (stage_socket_plug(current,
                          mutation.characterIndex,
                          mutation.targetInstanceSoid,
                          mutation.socketLane,
                          mutation.plugDefinitionIndex,
                          canonical)
        != SocketMutationResult::ok
        || !same_socket_mutation(canonical, mutation)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    return account::valid(after) && family4_loadout::resolve(after, mutation.characterIndex, checked);
}

bool commit_socket_plug(PendingSocketPlug& mutation) noexcept {
    const PendingSocketPlug prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.characterIndex >= kCharacterCapacity) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || prepared.characterIndex >= current.characterCount
        || !same_character(current.characters[prepared.characterIndex], prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingSocketPlug canonical{};
    if (stage_socket_plug(current,
                          prepared.characterIndex,
                          prepared.targetInstanceSoid,
                          prepared.socketLane,
                          prepared.plugDefinitionIndex,
                          canonical)
            != SocketMutationResult::ok
        || !same_socket_mutation(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.characters[prepared.characterIndex] = canonical.afterCharacter;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

InventoryMutationResult prepare_item_state(std::uint64_t targetInstanceSoid,
                                                   std::uint16_t targetDefinitionIndex,
                                                   std::uint32_t flags,
                                                   PendingItemState& mutation) noexcept {
    mutation = {};
    const AccountState snapshot = account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
        if (snapshot.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= snapshot.characterCount) {
        return InventoryMutationResult::noSelectedCharacter;
    }
    return stage_item_state(snapshot,
                            characterIndex,
                            targetInstanceSoid,
                            targetDefinitionIndex,
                            flags,
                            mutation);
}

bool preview_item_state(const PendingItemState& mutation, AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.characterSoid == 0
        || mutation.targetInstanceSoid == 0 || mutation.characterIndex >= kCharacterCapacity) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (current.primarySoid != mutation.accountSoid
        || mutation.characterIndex >= current.characterCount
        || !same_character(current.characters[mutation.characterIndex], mutation.beforeCharacter)) {
        return false;
    }
    PendingItemState canonical{};
    if (stage_item_state(current,
                         mutation.characterIndex,
                         mutation.targetInstanceSoid,
                         mutation.targetDefinitionIndex,
                         mutation.afterFlags,
                         canonical)
            != InventoryMutationResult::ok
        || !same_item_state(canonical, mutation)) {
        return false;
    }
    after = current;
    after.characters[mutation.characterIndex] = canonical.afterCharacter;
    family4_loadout::ResolvedLoadout checked{};
    return account::valid(after) && family4_loadout::resolve(after, mutation.characterIndex, checked);
}

bool commit_item_state(PendingItemState& mutation) noexcept {
    const PendingItemState prepared = mutation;
    mutation = {};
    if (!prepared.prepared || prepared.characterIndex >= kCharacterCapacity) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (current.primarySoid != prepared.accountSoid
        || prepared.characterIndex >= current.characterCount
        || !same_character(current.characters[prepared.characterIndex], prepared.beforeCharacter)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    PendingItemState canonical{};
    if (stage_item_state(current,
                         prepared.characterIndex,
                         prepared.targetInstanceSoid,
                         prepared.targetDefinitionIndex,
                         prepared.afterFlags,
                         canonical)
            != InventoryMutationResult::ok
        || !same_item_state(canonical, prepared)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.characters[prepared.characterIndex] = canonical.afterCharacter;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

InventoryMutationResult apply_item_state(std::uint64_t targetInstanceSoid,
                                         std::uint32_t flags,
                                         std::uint64_t& characterSoid) noexcept {
    characterSoid = 0;
    const AccountState snapshot = account_snapshot();
    std::size_t characterIndex = snapshot.characterCount;
    for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
        if (snapshot.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= snapshot.characterCount) {
        return InventoryMutationResult::noSelectedCharacter;
    }
    OwnedItemLocation location{};
    if (!find_owned_item(snapshot.characters[characterIndex], targetInstanceSoid, location)) {
        return InventoryMutationResult::notFound;
    }
    const inventory::Item* item = owned_item_at(snapshot.characters[characterIndex], location);
    build_data::items::Definition definition{};
    if (item == nullptr || !build_data::find_item_definition_hash(item->definitionHash, definition)
        || definition.definitionHash != item->definitionHash) {
        return InventoryMutationResult::unsupportedDefinition;
    }
    PendingItemState mutation{};
    const InventoryMutationResult result = prepare_item_state(
        targetInstanceSoid, definition.definitionIndex, flags, mutation);
    if (result != InventoryMutationResult::ok) {
        return result;
    }
    characterSoid = mutation.characterSoid;
    return commit_item_state(mutation) ? InventoryMutationResult::ok : InventoryMutationResult::invalid;
}

SocketMutationResult apply_socket_plug(std::uint64_t targetInstanceSoid,
                                       std::uint8_t socketLane,
                                       std::uint16_t plugDefinitionIndex,
                                       std::uint64_t& characterSoid,
                                       bool& equipped) noexcept {
    characterSoid = 0;
    equipped = false;
    PendingSocketPlug mutation{};
    const SocketMutationResult result =
        prepare_socket_plug(targetInstanceSoid, socketLane, plugDefinitionIndex, mutation);
    if (result != SocketMutationResult::ok) {
        return result;
    }
    characterSoid = mutation.characterSoid;
    equipped = mutation.targetEquipped;
    return commit_socket_plug(mutation) ? SocketMutationResult::ok : SocketMutationResult::invalid;
}

const char* socket_mutation_name(SocketMutationResult result) noexcept {
    switch (result) {
    case SocketMutationResult::ok:
        return "ok";
    case SocketMutationResult::noSelectedCharacter:
        return "select this character in Destiny first";
    case SocketMutationResult::notFound:
        return "owned item no longer exists";
    case SocketMutationResult::unsupportedDefinition:
        return "item or plug definition is unavailable";
    case SocketMutationResult::badLane:
        return "socket lane is not present on this item";
    case SocketMutationResult::incompatiblePlug:
        return "plug is not allowed in this socket";
    case SocketMutationResult::alreadyApplied:
        return "plug is already applied";
    case SocketMutationResult::invalid:
        return "socket transition is invalid";
    }
    return "unknown";
}

/** Adds one same-slot installed item as a new unequipped debug instance. */
InventoryMutationResult debug_add_unequipped_item(std::size_t characterIndex,
                                                  inventory::EquipmentSlot referenceSlot,
                                                  std::uint32_t definitionHash,
                                                  std::uint64_t& createdInstanceSoid) noexcept {
    createdInstanceSoid = 0;
    const std::size_t semanticIndex = static_cast<std::size_t>(referenceSlot);
    if (semanticIndex >= inventory::kEquipmentSlotCount
        || definitionHash == inventory::kNoDefinitionHash) {
        return InventoryMutationResult::invalid;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (characterIndex >= candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::notFound;
    }
    CharacterState& character = candidate.characters[characterIndex];
    if (!character.selected) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::noSelectedCharacter;
    }
    if (character.inventory.count >= character.inventory.values.size()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::full;
    }
    const std::optional<inventory::Item>& reference = character.equipment.slots[semanticIndex];
    if (!reference.has_value()) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::incompatibleSlot;
    }

    std::uint8_t referenceBucket = 0;
    std::uint8_t referenceNativeSlot = 0;
    std::uint8_t requestedBucket = 0;
    std::uint8_t requestedNativeSlot = 0;
    if (!item_contract(reference->definitionHash, referenceBucket, referenceNativeSlot)
        || !item_contract(definitionHash, requestedBucket, requestedNativeSlot)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::unsupportedDefinition;
    }
    if (requestedBucket != referenceBucket || requestedNativeSlot != referenceNativeSlot) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::incompatibleSlot;
    }
    constexpr std::uint32_t kMaximumInventorySerial =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    std::uint64_t nextSoid = 0;
    if (character.nextInventorySerial > kMaximumInventorySerial
        || character.nextInventorySerial == kMaximumInventorySerial
        || !next_instance_soid(candidate, nextSoid)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::invalid;
    }

    inventory::Item created{};
    created.instanceSoid = nextSoid;
    created.definitionHash = definitionHash;
    created.level = reference->level;
    created.quantity = 1;
    created.mutationSerial = static_cast<std::int32_t>(character.nextInventorySerial++);
    created.flags = 0;
    created.sockets = {};
    character.inventory.values[character.inventory.count++] = created;

    family4_loadout::ResolvedLoadout checked{};
    if (!account::valid(candidate) || !family4_loadout::resolve(candidate, characterIndex, checked)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::invalid;
    }
    if (commit_character_account(candidate) != CharacterMutationResult::ok) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return InventoryMutationResult::invalid;
    }
    createdInstanceSoid = nextSoid;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return InventoryMutationResult::ok;
}

/** Adds one installed stackable row to account-wide profile inventory for debug testing. */
ProfileInventoryMutationResult debug_add_profile_item(std::uint32_t definitionHash,
                                                       std::int32_t quantity,
                                                       std::int64_t& totalQuantity,
                                                       std::uint64_t& createdInstanceSoid) noexcept {
    totalQuantity = 0;
    createdInstanceSoid = 0;
    if (definitionHash == inventory::kNoDefinitionHash || quantity <= 0) {
        return ProfileInventoryMutationResult::invalidQuantity;
    }

    build_data::items::Definition definition{};
    item_details::Definition detail{};
    inventory_buckets::Descriptor bucket{};
    bool actionSource = false;
    if (!profile_item_contract(definitionHash, definition, detail, bucket, actionSource)) {
        return ProfileInventoryMutationResult::notProfileItem;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return ProfileInventoryMutationResult::invalid;
    }

    std::size_t usedBucketRows = 0;
    std::int32_t greatestSerial = 0;
    for (std::size_t index = 0; index < candidate.profileItemCount; ++index) {
        const inventory::ProfileItem& item = candidate.profileItems[index];
        greatestSerial = (std::max)(greatestSerial, item.mutationSerial);
        build_data::items::Definition existingDefinition{};
        if (!build_data::find_item_definition_hash(item.definitionHash, existingDefinition)
            || existingDefinition.definitionHash != item.definitionHash
            || existingDefinition.bucketId == build_data::items::kUnresolvedBucketId) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return ProfileInventoryMutationResult::invalid;
        }
        usedBucketRows += static_cast<std::size_t>(existingDefinition.bucketId == definition.bucketId);
    }

    std::int64_t remaining = quantity;
    constexpr std::int32_t kMaximumSerial = (std::numeric_limits<std::int32_t>::max)();
    for (std::size_t index = 0; index < candidate.profileItemCount && remaining != 0; ++index) {
        inventory::ProfileItem& item = candidate.profileItems[index];
        if (item.definitionHash != definitionHash || item.quantity >= detail.maxStackSize
            || (actionSource != (item.instanceSoid != 0))) {
            continue;
        }
        const std::int32_t room = detail.maxStackSize - item.quantity;
        const std::int32_t added = static_cast<std::int32_t>(
            (std::min)(remaining, static_cast<std::int64_t>(room)));
        if (added <= 0 || greatestSerial == kMaximumSerial) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return ProfileInventoryMutationResult::invalid;
        }
        item.quantity += added;
        item.mutationSerial = ++greatestSerial;
        remaining -= added;
    }

    // A single debug request may create at most one resident action-source stack. This keeps the
    // deferred Family-4 publication atomic (one new resident followed by the account after-image).
    if (actionSource && remaining > detail.maxStackSize) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return ProfileInventoryMutationResult::full;
    }

    while (remaining != 0) {
        if (candidate.profileItemCount >= candidate.profileItems.size()
            || usedBucketRows >= bucket.slotCount || greatestSerial == kMaximumSerial) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return ProfileInventoryMutationResult::full;
        }
        std::uint64_t instanceSoid = 0;
        if (actionSource && !next_profile_instance_soid(candidate, instanceSoid)) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return ProfileInventoryMutationResult::full;
        }
        const std::int32_t stackQuantity = static_cast<std::int32_t>(
            (std::min)(remaining, static_cast<std::int64_t>(detail.maxStackSize)));
        candidate.profileItems[candidate.profileItemCount++] = {
            instanceSoid, definitionHash, stackQuantity, ++greatestSerial};
        if (instanceSoid != 0) {
            createdInstanceSoid = instanceSoid;
        }
        ++usedBucketRows;
        remaining -= stackQuantity;
    }

    if (!account::valid(candidate)
        || commit_character_account(candidate) != CharacterMutationResult::ok) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return ProfileInventoryMutationResult::invalid;
    }
    for (std::size_t index = 0; index < candidate.profileItemCount; ++index) {
        if (candidate.profileItems[index].definitionHash == definitionHash) {
            totalQuantity += candidate.profileItems[index].quantity;
        }
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return ProfileInventoryMutationResult::ok;
}

bool ensure_profile_item_identities() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (!account::valid(candidate)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    std::int32_t greatestSerial = 0;
    for (std::size_t index = 0; index < candidate.profileItemCount; ++index) {
        greatestSerial = (std::max)(greatestSerial, candidate.profileItems[index].mutationSerial);
    }
    bool changed = false;
    for (std::size_t index = 0; index < candidate.profileItemCount; ++index) {
        inventory::ProfileItem& item = candidate.profileItems[index];
        build_data::items::Definition definition{};
        if (!build_data::find_item_definition_hash(item.definitionHash, definition)
            || definition.definitionHash != item.definitionHash
            || definition.bucketId == build_data::items::kUnresolvedBucketId) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        constexpr std::uint8_t kModBucketId = 13;
        constexpr std::uint8_t kShaderBucketId = 14;
        const bool possibleActionSource =
            definition.bucketId == kModBucketId || definition.bucketId == kShaderBucketId;
        if (!possibleActionSource) {
            if (item.instanceSoid != 0) {
                ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
                return false;
            }
            continue;
        }

        item_details::Definition detail{};
        inventory_buckets::Descriptor bucket{};
        bool actionSource = false;
        if (!profile_item_contract(item.definitionHash, definition, detail, bucket, actionSource)
            || !actionSource) {
            ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
            return false;
        }
        if (item.instanceSoid == 0) {
            std::uint64_t soid = 0;
            if (greatestSerial == (std::numeric_limits<std::int32_t>::max)()
                || !next_profile_instance_soid(candidate, soid)) {
                ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
                return false;
            }
            item.instanceSoid = soid;
            item.mutationSerial = ++greatestSerial;
            changed = true;
        }
    }
    const bool valid = account::valid(candidate);
    if (valid && changed) {
        runtime::storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return valid;
}

/** Stable text for account-wide profile inventory debug mutations. */
const char* profile_inventory_mutation_name(ProfileInventoryMutationResult result) noexcept {
    switch (result) {
    case ProfileInventoryMutationResult::ok:
        return "ok";
    case ProfileInventoryMutationResult::notFound:
        return "profile item is not owned";
    case ProfileInventoryMutationResult::full:
        return "native profile bucket is full";
    case ProfileInventoryMutationResult::unsupportedDefinition:
        return "installed profile item details are unavailable";
    case ProfileInventoryMutationResult::notProfileItem:
        return "item is not a stackable profile-inventory definition";
    case ProfileInventoryMutationResult::invalidQuantity:
        return "quantity must be positive";
    case ProfileInventoryMutationResult::invalid:
        return "profile inventory transition is invalid";
    }
    return "unknown";
}

/** Stable text for inventory action status. */
const char* inventory_mutation_name(InventoryMutationResult result) noexcept {
    switch (result) {
    case InventoryMutationResult::ok:
        return "ok";
    case InventoryMutationResult::noSelectedCharacter:
        return "select this character in Destiny first";
    case InventoryMutationResult::notFound:
        return "item or character no longer exists";
    case InventoryMutationResult::full:
        return "unequipped inventory is full";
    case InventoryMutationResult::unsupportedDefinition:
        return "installed item details are incomplete or unsupported";
    case InventoryMutationResult::incompatibleSlot:
        return "item does not match the learned equipment slot";
    case InventoryMutationResult::invalid:
        return "inventory transition is invalid";
    }
    return "unknown";
}

/** Stable text for equipment-editor status. */
const char* equipment_mutation_name(EquipmentMutationResult result) noexcept {
    switch (result) {
    case EquipmentMutationResult::ok:
        return "ok";
    case EquipmentMutationResult::notFound:
        return "character no longer exists";
    case EquipmentMutationResult::emptySlot:
        return "that equipment slot is empty";
    case EquipmentMutationResult::unsupportedDefinition:
        return "item is not available in the installed detail cache";
    case EquipmentMutationResult::incompatibleSlot:
        return "item does not belong in that equipment slot";
    case EquipmentMutationResult::invalid:
        return "equipment state is invalid";
    case EquipmentMutationResult::persistenceUnavailable:
        return "character persistence is unavailable";
    case EquipmentMutationResult::persistenceFailed:
        return "character file could not be saved";
    }
    return "unknown";
}

/** @return True when one runtime roster exactly matches a retained dense character bank. */
[[nodiscard]] bool same_character_bank(
    const AccountState& account,
    std::size_t count,
    const std::array<CharacterState, kCharacterCapacity>& characters) noexcept {
    if (account.characterCount != count || count > characters.size()) {
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (!same_character(account.characters[index], characters[index])) {
            return false;
        }
    }
    for (std::size_t index = count; index < characters.size(); ++index) {
        if (!same_character(account.characters[index], characters[index])) {
            return false;
        }
    }
    return true;
}

/** Adds one nonzero deleted-item identity to the bounded native release list. */
[[nodiscard]] bool append_deleted_item_soid(PendingCharacterDeletion& mutation,
                                            std::uint64_t instanceSoid) noexcept {
    if (instanceSoid == 0) {
        return true;
    }
    if (mutation.releasedItemCount >= mutation.releasedItemSoids.size()) {
        return false;
    }
    for (std::size_t index = 0; index < mutation.releasedItemCount; ++index) {
        if (mutation.releasedItemSoids[index] == instanceSoid) {
            return false;
        }
    }
    mutation.releasedItemSoids[mutation.releasedItemCount++] = instanceSoid;
    return true;
}

/** Prepares one character-select deletion without treating the stale runtime selection as a lock. */
CharacterMutationResult prepare_character_deletion(std::uint64_t characterSoid,
                                                   PendingCharacterDeletion& mutation) noexcept {
    mutation = {};
    if (characterSoid == 0) {
        return CharacterMutationResult::notFound;
    }
    const AccountState snapshot = account_snapshot();
    if (!account::valid(snapshot) || snapshot.primarySoid == 0) {
        return CharacterMutationResult::invalid;
    }

    std::size_t deletedIndex = snapshot.characterCount;
    for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
        if (snapshot.characters[index].soid == characterSoid) {
            deletedIndex = index;
            break;
        }
    }
    if (deletedIndex == snapshot.characterCount) {
        return CharacterMutationResult::notFound;
    }

    mutation.accountSoid = snapshot.primarySoid;
    mutation.deletedCharacterSoid = characterSoid;
    mutation.beforeCharacterCount = snapshot.characterCount;
    mutation.afterCharacterCount = snapshot.characterCount - 1U;
    mutation.deletedIndex = deletedIndex;
    mutation.deletedWasSelected = snapshot.characters[deletedIndex].selected;
    mutation.beforeCharacters = snapshot.characters;

    const CharacterState& deleted = snapshot.characters[deletedIndex];
    for (const auto& equipped : deleted.equipment.slots) {
        if (equipped.has_value()
            && !append_deleted_item_soid(mutation, equipped->instanceSoid)) {
            mutation = {};
            return CharacterMutationResult::invalid;
        }
    }
    for (std::size_t index = 0; index < deleted.inventory.count; ++index) {
        if (!append_deleted_item_soid(mutation, deleted.inventory.values[index].instanceSoid)) {
            mutation = {};
            return CharacterMutationResult::invalid;
        }
    }

    AccountState candidate = snapshot;
    for (std::size_t row = deletedIndex; row + 1U < candidate.characterCount; ++row) {
        candidate.characters[row] = candidate.characters[row + 1U];
    }
    candidate.characters[candidate.characterCount - 1U] = {};
    --candidate.characterCount;
    // Native deletion is a character-select operation. Runtime State may still retain the Guardian
    // that was active before opcode 505, so normalize every surviving row back to preselection
    // instead of treating that stale marker as an ownership lock.
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        candidate.characters[index].selected = false;
    }
    if (!account::valid(candidate) || account::selected_character_soid(candidate) != 0) {
        mutation = {};
        return CharacterMutationResult::invalid;
    }
    mutation.afterCharacters = candidate.characters;
    mutation.prepared = true;
    return CharacterMutationResult::ok;
}

/** Revalidates and materializes the account after-image for one pending native deletion. */
bool preview_character_deletion(const PendingCharacterDeletion& mutation,
                                AccountState& after) noexcept {
    after = {};
    if (!mutation.prepared || mutation.accountSoid == 0 || mutation.deletedCharacterSoid == 0
        || mutation.beforeCharacterCount == 0
        || mutation.beforeCharacterCount > kCharacterCapacity
        || mutation.afterCharacterCount + 1U != mutation.beforeCharacterCount
        || mutation.deletedIndex >= mutation.beforeCharacterCount
        || mutation.releasedItemCount > mutation.releasedItemSoids.size()) {
        return false;
    }
    const AccountState current = account_snapshot();
    if (!account::valid(current) || current.primarySoid != mutation.accountSoid
        || !same_character_bank(current, mutation.beforeCharacterCount, mutation.beforeCharacters)
        || current.characters[mutation.deletedIndex].soid != mutation.deletedCharacterSoid) {
        return false;
    }
    after = current;
    after.characters = mutation.afterCharacters;
    after.characterCount = mutation.afterCharacterCount;
    return account::valid(after) && account::selected_character_soid(after) == 0;
}

/** Commits one staged native character deletion after exact roster before-image revalidation. */
bool commit_character_deletion(PendingCharacterDeletion& mutation) noexcept {
    if (!mutation.prepared) {
        return false;
    }
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState current = runtime::storage::g_state.account;
    if (!account::valid(current) || current.primarySoid != mutation.accountSoid
        || !same_character_bank(current, mutation.beforeCharacterCount, mutation.beforeCharacters)
        || mutation.deletedIndex >= current.characterCount
        || current.characters[mutation.deletedIndex].soid != mutation.deletedCharacterSoid) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return false;
    }
    current.characters = mutation.afterCharacters;
    current.characterCount = mutation.afterCharacterCount;
    const bool committed = commit_character_account(current) == CharacterMutationResult::ok;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    if (committed) {
        mutation = {};
    }
    return committed;
}

/** Deletes one inactive character while preserving any other active selection. */
CharacterMutationResult delete_character(std::size_t index) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (index >= candidate.characterCount) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::notFound;
    }
    if (candidate.characters[index].selected) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::selected;
    }
    for (std::size_t row = index; row + 1U < candidate.characterCount; ++row) {
        candidate.characters[row] = candidate.characters[row + 1U];
    }
    candidate.characters[candidate.characterCount - 1U] = {};
    --candidate.characterCount;
    const CharacterMutationResult result = commit_character_account(candidate);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

/** Restores the boot-authored settings roster while retaining the live account identity/settings. */
CharacterMutationResult reset_characters_to_settings() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    if (account::selected_character_soid(candidate) != 0) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::selected;
    }
    const AccountState configured = character_store::authored_account();
    candidate.characters = configured.characters;
    candidate.characterCount = configured.characterCount;
    const std::uint64_t configuredPrimarySoid = configured.primarySoid;
    for (std::size_t index = 0; index < candidate.characterCount; ++index) {
        candidate.characters[index].selected = false;
    }
    if (!rebase_character_soids(candidate, configuredPrimarySoid, candidate.primarySoid)) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return CharacterMutationResult::invalid;
    }
    const CharacterMutationResult result = commit_character_account(candidate);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return result;
}

/** Returns the character store status in a State-layer stable shape. */
CharacterStoreStatus character_store_status() noexcept {
    const character_store::StoreStatus store = character_store::status();
    return {store.available,
            store.loaded,
            store.rejected,
            store.lastSaveOk,
            store.lastSettingsMirrorOk};
}

/** Checkpoints the current runtime roster to the binary store and settings compatibility mirror. */
bool checkpoint_characters() noexcept {
    const AccountState snapshot = account_snapshot();
    const bool stored = character_store::persist(snapshot);
    const bool mirrored = character_store::mirror_settings(snapshot);
    return stored && mirrored;
}

/** Stable text for UI/log display. */
const char* character_mutation_name(CharacterMutationResult result) noexcept {
    switch (result) {
    case CharacterMutationResult::ok:
        return "ok";
    case CharacterMutationResult::full:
        return "all 3 character slots are occupied";
    case CharacterMutationResult::notFound:
        return "character no longer exists";
    case CharacterMutationResult::selected:
        return "that character is currently active";
    case CharacterMutationResult::missingTemplate:
        return "no Sunrise class factory template is available";
    case CharacterMutationResult::invalid:
        return "character state is invalid";
    case CharacterMutationResult::persistenceUnavailable:
        return "character persistence is unavailable";
    case CharacterMutationResult::persistenceFailed:
        return "character file could not be saved";
    }
    return "unknown";
}

/** Returns the immutable factory equipment/ability template account. */
AccountState configured_account_snapshot() noexcept {
    return character_store::configured_account();
}

/** Stores the exact native character-creation request for offline layout comparison. */
bool capture_character_creation_request(std::span<const std::byte> payload,
                                        std::uint32_t& captureIndex) noexcept {
    return character_store::capture_creation_request(payload, captureIndex);
}

} // namespace sunrise::state
