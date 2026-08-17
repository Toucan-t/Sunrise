#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "state.h"

namespace sunrise::state {

/**
 * Loads cached build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @return True when the cached data passes its checks and every secret is generated.
 */
[[nodiscard]] bool initialize(void* module = nullptr,
                              const AccountState& initialAccount = {}) noexcept;

/**
 * Loads cached build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
[[nodiscard]] bool
initialize(void* module,
           const AccountState& initialAccount,
           const activity::defaults::ActivityDefaults& activityDefaults) noexcept;

/** Securely clears State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept;

/** @return Immutable generated SignOn session fields. */
[[nodiscard]] const SignOnState& sign_on() noexcept;

[[nodiscard]] bool publish_bootstrap_token(std::span<const std::byte> token) noexcept;

/** @return Immutable generated BAP session fields. */
[[nodiscard]] const BapState& bap() noexcept;

/**
 * Stores the active nonzero account key when the account remains complete.
 * @param primarySoid Account key selected by the local Client.
 * @return False when the key or resulting account State is invalid.
 */
[[nodiscard]] bool set_primary_soid(std::uint64_t primarySoid) noexcept;

/**
 * Moves the selection to one authored character.
 * The Client names its pick only in the select-character request, so this is where a player's
 * choice enters State.
 * @param characterSoid Picked character key, which must name an authored character.
 * @param changed Receives whether the selection moved to a different character.
 * @return False when no authored character carries that key.
 */
[[nodiscard]] bool set_selected_character(std::uint64_t characterSoid, bool& changed) noexcept;

/** @return A copy of the active account state, read under the lock. */
[[nodiscard]] AccountState account_snapshot() noexcept;

/** Editable character metadata exposed by the temporary Sunrise character manager. */
struct CharacterEdit final {
    CharacterRace race{CharacterRace::human};
    CharacterGender gender{CharacterGender::male};
    CharacterClass characterClass{CharacterClass::titan};
    std::uint8_t level{};
    bool accepted{};
    bool previewAvailable{};
    float appearanceValue{};
    std::uint32_t lastOrbitedDestination{};
    bool contentBypass{};
};

/** Result of one validated runtime character-management transaction. */
enum class CharacterMutationResult : std::uint8_t {
    ok,
    full,
    notFound,
    selected,
    missingTemplate,
    invalid,
    persistenceUnavailable,
    persistenceFailed,
};

/** Result of replacing one equipped item's definition while retaining its instance identity. */
enum class EquipmentMutationResult : std::uint8_t {
    ok,
    notFound,
    emptySlot,
    unsupportedDefinition,
    incompatibleSlot,
    invalid,
    persistenceUnavailable,
    persistenceFailed,
};

/** Result of moving or creating one character-owned inventory item. */
enum class InventoryMutationResult : std::uint8_t {
    ok,
    noSelectedCharacter,
    notFound,
    full,
    unsupportedDefinition,
    incompatibleSlot,
    invalid,
};

/** Result of adding or incrementing one account-wide profile inventory stack. */
enum class ProfileInventoryMutationResult : std::uint8_t {
    ok,
    full,
    unsupportedDefinition,
    notProfileItem,
    invalidQuantity,
    invalid,
    notFound,
};

/** Direction of one native selected-character inventory move. */
enum class InventoryMoveKind : std::uint8_t {
    none,
    equip,
    unequip,
};

/** Result of changing one owned item's ordinary socket plug. */
enum class SocketMutationResult : std::uint8_t {
    ok,
    noSelectedCharacter,
    notFound,
    unsupportedDefinition,
    badLane,
    incompatiblePlug,
    alreadyApplied,
    invalid,
};

/** Checked socket after-image retained until its live item/appearance publication succeeds. */
struct PendingSocketPlug final {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t targetInstanceSoid{};
    std::uint32_t targetDefinitionHash{};
    std::uint32_t plugDefinitionHash{};
    std::size_t characterIndex{};
    std::size_t itemIndex{};
    std::uint16_t targetDefinitionIndex{};
    std::uint16_t plugDefinitionIndex{};
    std::uint8_t socketLane{};
    bool targetEquipped{};
    bool prepared{};
};

/**
 * Checked selected-character inventory move retained until response and Queuez output both fit.
 * State is not changed by preparation; commit revalidates the complete target character.
 */
struct PendingInventoryMove final {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t requestedInstanceSoid{};
    std::size_t characterIndex{};
    std::size_t semanticEquipmentIndex{};
    std::uint8_t nativeEquipmentSlot{};
    InventoryMoveKind kind{InventoryMoveKind::none};
    bool prepared{};
};

/** Prepared Collections pull retained until its response and resident update both fit. */
struct PendingItemAcquisition final {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t acquiredInstanceSoid{};
    std::uint32_t acquiredDefinitionHash{};
    std::size_t characterIndex{};
    std::size_t inventoryIndex{};
    std::uint16_t collectibleIndex{};
    std::uint16_t itemDefinitionIndex{};
    std::uint16_t inventoryRow{};
    std::uint8_t nativeEquipmentSlot{};
    bool prepared{};
};

/** Prepared profile-stack Collections pull retained until resident/account publication succeeds. */
struct PendingProfileItemAcquisition final {
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity> beforeItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity> afterItems{};
    std::uint64_t accountSoid{};
    std::uint64_t acquiredInstanceSoid{};
    std::uint32_t acquiredDefinitionHash{};
    std::size_t expectedItemCount{};
    std::size_t afterItemCount{};
    std::size_t profileIndex{};
    std::uint16_t collectibleIndex{};
    std::uint16_t itemDefinitionIndex{};
    std::uint8_t bucketId{};
    std::int32_t previousQuantity{};
    std::int32_t acquiredQuantity{};
    std::int32_t previousMutationSerial{};
    std::int32_t acquiredMutationSerial{};
    bool actionSource{};
    bool appended{};
    bool prepared{};
};

/** Checked accumulated item-state update (Locked/Tracked/Masterwork) for one owned instance. */
struct PendingItemState final {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t targetInstanceSoid{};
    std::size_t characterIndex{};
    std::size_t itemIndex{};
    std::uint16_t targetDefinitionIndex{};
    std::uint32_t beforeFlags{};
    std::uint32_t afterFlags{};
    bool targetEquipped{};
    bool prepared{};
};

/** Prepared unequipped-item dismantle retained until its release frame fits. */
struct PendingItemDismantle final {
    CharacterState beforeCharacter{};
    CharacterState afterCharacter{};
    account::inventory::Item dismantledItem{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t dismantledInstanceSoid{};
    std::size_t characterIndex{};
    std::size_t inventoryIndex{};
    std::uint16_t itemDefinitionIndex{};
    std::uint16_t inventoryRow{};
    std::uint8_t nativeEquipmentSlot{};
    bool prepared{};
};

/** Prepared account-wide profile stack decrement/removal retained until Family-4 publication. */
struct PendingProfileItemDismantle final {
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity> beforeItems{};
    std::array<account::inventory::ProfileItem, account::inventory::kProfileItemCapacity> afterItems{};
    std::uint64_t accountSoid{};
    /** Optional opcode identity used to target one exact resident profile action-source stack. */
    std::uint64_t requestedInstanceSoid{};
    /** Nonzero only when the final copy owned a resident shader/mod action-source identity. */
    std::uint64_t releasedInstanceSoid{};
    std::uint32_t dismantledDefinitionHash{};
    std::size_t expectedItemCount{};
    std::size_t afterItemCount{};
    std::size_t profileIndex{};
    std::uint16_t itemDefinitionIndex{};
    std::uint8_t bucketId{};
    std::int32_t previousQuantity{};
    std::int32_t requestedQuantity{};
    std::int32_t remainingQuantity{};
    bool actionSource{};
    bool removedRow{};
    bool prepared{};
};

/** Maximum resident item identities owned by one character across equipment and inventory. */
inline constexpr std::size_t kCharacterOwnedItemCapacity =
    account::inventory::kEquipmentSlotCount + account::inventory::kCharacterItemCapacity;

/** Prepared native character deletion retained until Family-4 removal publication succeeds. */
struct PendingCharacterDeletion final {
    std::array<CharacterState, kCharacterCapacity> beforeCharacters{};
    std::array<CharacterState, kCharacterCapacity> afterCharacters{};
    std::array<std::uint64_t, kCharacterOwnedItemCapacity> releasedItemSoids{};
    std::uint64_t accountSoid{};
    std::uint64_t deletedCharacterSoid{};
    std::size_t beforeCharacterCount{};
    std::size_t afterCharacterCount{};
    std::size_t deletedIndex{};
    std::size_t releasedItemCount{};
    bool deletedWasSelected{};
    bool prepared{};
};

/** Current local character-store status for the temporary UI. */
struct CharacterStoreStatus final {
    bool available{};
    bool loaded{};
    bool rejected{};
    bool lastSaveOk{};
    bool lastSettingsMirrorOk{};
};

/**
 * Creates one character from Sunrise's bundled factory loadout for the requested class.
 * All cloned item-instance SOIDs are remapped account-wide before publication.
 * @param race New character race.
 * @param gender New character gender.
 * @param characterClass New character class whose immutable template supplies loadout/abilities.
 * @param createdIndex Receives the dense character row on success.
 */
[[nodiscard]] CharacterMutationResult create_character(CharacterRace race,
                                                       CharacterGender gender,
                                                       CharacterClass characterClass,
                                                       std::size_t& createdIndex) noexcept;

/** Native creator data decoded from opcode 501 and retained without reinterpretation loss. */
struct NativeCharacterCreation final {
    CharacterRace race{CharacterRace::human};
    CharacterGender gender{CharacterGender::male};
    CharacterClass characterClass{CharacterClass::titan};
    std::array<std::byte, kCharacterPresentationHeaderSize> presentationHeader{};
    std::array<std::byte, kCharacterCreationHeaderSize> creationHeader{};
    std::array<std::byte, kCharacterCreationTailSize> creationTail{};
};

/** Creates one template-backed runtime character using the native creator's appearance blocks. */
[[nodiscard]] CharacterMutationResult
create_character_native(const NativeCharacterCreation& creation,
                        std::size_t& createdIndex) noexcept;

/**
 * Replaces editable metadata for one character. Changing class swaps only class-bound gear and
 * ability selections from that class's factory template; general weapons/cosmetics stay owned.
 */
[[nodiscard]] CharacterMutationResult update_character(std::size_t index,
                                                       const CharacterEdit& edit,
                                                       bool& classChanged) noexcept;

/**
 * Deletes one inactive character and compacts the dense roster. Another selected character does
 * not block deletion because character SOIDs remain stable across compaction; only the character
 * currently backing the native player object is protected.
 */
[[nodiscard]] CharacterMutationResult delete_character(std::size_t index) noexcept;

/**
 * Prepares one native character-select deletion by stable SOID without trusting the runtime selected
 * marker. Queuez staging separately proves the peer is in a safe preselection state before commit.
 */
[[nodiscard]] CharacterMutationResult prepare_character_deletion(
    std::uint64_t characterSoid, PendingCharacterDeletion& mutation) noexcept;

/** Builds the exact account after-image while a prepared character deletion remains current. */
[[nodiscard]] bool preview_character_deletion(const PendingCharacterDeletion& mutation,
                                              AccountState& after) noexcept;

/** Commits one prepared native character deletion after exact roster revalidation. */
[[nodiscard]] bool commit_character_deletion(PendingCharacterDeletion& mutation) noexcept;

/** Restores the settings-authored character templates and persists them as the active roster. */
[[nodiscard]] CharacterMutationResult reset_characters_to_settings() noexcept;
/**
 * Replaces one equipped definition while preserving the item's instance SOID, level and quantity.
 * Slot compatibility is learned from the already-working item in that semantic slot: replacements
 * must preserve both its installed inventory bucket and native equipment-slot number. Native socket
 * defaults are restored because the old item's authored sockets cannot safely be reused.
 */
[[nodiscard]] EquipmentMutationResult
update_equipment_definition(std::size_t characterIndex,
                            account::inventory::EquipmentSlot slot,
                            std::uint32_t definitionHash) noexcept;

/** @return Stable text for one equipment mutation result. */
[[nodiscard]] const char* equipment_mutation_name(EquipmentMutationResult result) noexcept;

/**
 * Moves one owned unequipped instance into its learned semantic equipment slot. If that slot is
 * occupied, the previous equipped item replaces the requested item in the dense unequipped array,
 * preserving bucket row order wherever possible. Only the currently selected character may move.
 * @param requestedInstanceSoid Unequipped instance identity selected by the Client/debug UI.
 * @param characterSoid Receives the selected owner on success.
 */
[[nodiscard]] InventoryMutationResult equip_inventory_item(std::uint64_t requestedInstanceSoid,
                                                           std::uint64_t& characterSoid) noexcept;

/**
 * Moves one equipped selected-character instance into the dense unequipped inventory. The item is
 * inserted before existing entries in its native bucket so their physical rows remain stable.
 */
[[nodiscard]] InventoryMutationResult unequip_inventory_item(std::uint64_t requestedInstanceSoid,
                                                             std::uint64_t& characterSoid) noexcept;

/** Prepares one native equip/unequip without changing runtime State. */
[[nodiscard]] InventoryMutationResult prepare_inventory_move(std::uint64_t requestedInstanceSoid,
                                                             InventoryMoveKind kind,
                                                             PendingInventoryMove& mutation) noexcept;

/** Builds the exact account after-image while one prepared inventory move is still current. */
[[nodiscard]] bool preview_inventory_move(const PendingInventoryMove& mutation,
                                          AccountState& after) noexcept;

/** Commits one prepared inventory move after revalidating its full before/after character images. */
[[nodiscard]] bool commit_inventory_move(PendingInventoryMove& mutation) noexcept;

/** Prepares one native Collections acquisition without changing runtime State. */
[[nodiscard]] InventoryMutationResult prepare_item_acquisition(
    std::uint16_t collectibleIndex,
    PendingItemAcquisition& mutation) noexcept;

/** Builds the exact account after-image while a prepared Collections pull remains current. */
[[nodiscard]] bool preview_item_acquisition(const PendingItemAcquisition& mutation,
                                            AccountState& after) noexcept;

/** Commits one prepared Collections pull after exact before-image revalidation. */
[[nodiscard]] bool commit_item_acquisition(PendingItemAcquisition& mutation) noexcept;


/** Prepares a stackable profile-item Collections pull, including resident shader/mod sources. */
[[nodiscard]] ProfileInventoryMutationResult prepare_profile_item_acquisition(
    std::uint16_t collectibleIndex,
    PendingProfileItemAcquisition& mutation) noexcept;

/** Builds the account after-image while a prepared profile Collections pull remains current. */
[[nodiscard]] bool preview_profile_item_acquisition(
    const PendingProfileItemAcquisition& mutation, AccountState& after) noexcept;

/** Commits one prepared profile Collections pull after exact before-image revalidation. */
[[nodiscard]] bool commit_profile_item_acquisition(PendingProfileItemAcquisition& mutation) noexcept;

/** Prepares removal of one unequipped selected-character instance. */
[[nodiscard]] InventoryMutationResult prepare_item_dismantle(
    std::uint64_t instanceSoid, PendingItemDismantle& mutation) noexcept;

/** Builds the exact account after-image while a prepared dismantle remains current. */
[[nodiscard]] bool preview_item_dismantle(const PendingItemDismantle& mutation,
                                          AccountState& after) noexcept;

/** Commits one prepared dismantle after exact before-image revalidation. */
[[nodiscard]] bool commit_item_dismantle(PendingItemDismantle& mutation) noexcept;


/** Prepares one account-wide profile stack decrement/removal selected by native definition index. */
[[nodiscard]] ProfileInventoryMutationResult prepare_profile_item_dismantle(
    std::uint16_t itemDefinitionIndex,
    std::int32_t quantity,
    std::uint64_t requestedInstanceSoid,
    PendingProfileItemDismantle& mutation) noexcept;

/** Builds the account after-image while a prepared profile dismantle remains current. */
[[nodiscard]] bool preview_profile_item_dismantle(
    const PendingProfileItemDismantle& mutation, AccountState& after) noexcept;

/** Commits one prepared profile dismantle after exact before-image revalidation. */
[[nodiscard]] bool commit_profile_item_dismantle(PendingProfileItemDismantle& mutation) noexcept;

/**
 * Debug-only item seeding used by the Inventory/Item Catalog panels. The requested installed item
 * must match the occupied reference slot's native bucket and equipment-slot contract; a fresh SOID,
 * native-default sockets and the reference item's level are assigned.
 */
[[nodiscard]] InventoryMutationResult debug_add_unequipped_item(
    std::size_t characterIndex,
    account::inventory::EquipmentSlot referenceSlot,
    std::uint32_t definitionHash,
    std::uint64_t& createdInstanceSoid) noexcept;

/**
 * Debug-only profile inventory seeding for shaders, mods, consumables, currencies and materials.
 * The installed item must resolve to a stackable profile bucket. Existing non-full stacks are
 * filled first, then additional native bucket rows are appended as needed.
 */
[[nodiscard]] ProfileInventoryMutationResult debug_add_profile_item(
    std::uint32_t definitionHash,
    std::int32_t quantity,
    std::int64_t& totalQuantity,
    std::uint64_t& createdInstanceSoid) noexcept;

/** Assigns/restores stable SOIDs for persisted profile shader/mod action-source stacks. */
[[nodiscard]] bool ensure_profile_item_identities() noexcept;

/** @return Stable text for profile-inventory debug mutation results. */
[[nodiscard]] const char* profile_inventory_mutation_name(
    ProfileInventoryMutationResult result) noexcept;

/** Prepares one compatible ordinary-socket replacement without mutating runtime State. */
[[nodiscard]] SocketMutationResult prepare_socket_plug(std::uint64_t targetInstanceSoid,
                                                       std::uint8_t socketLane,
                                                       std::uint16_t plugDefinitionIndex,
                                                       PendingSocketPlug& mutation) noexcept;

/** Resolves opcode-1901's semantic selector to one exact equipped item and stages the same action. */
[[nodiscard]] SocketMutationResult prepare_equipped_socket_plug(
    std::uint64_t equipmentSelector,
    std::uint8_t socketLane,
    std::uint16_t plugDefinitionIndex,
    PendingSocketPlug& mutation) noexcept;

/** Builds the account after-image while a prepared socket mutation remains current. */
[[nodiscard]] bool preview_socket_plug(const PendingSocketPlug& mutation,
                                       AccountState& after) noexcept;

/** Commits a prepared socket mutation after exact before-image revalidation. */
[[nodiscard]] bool commit_socket_plug(PendingSocketPlug& mutation) noexcept;

/** Debug/UI convenience wrapper around the same prepare/commit socket transaction. */
[[nodiscard]] SocketMutationResult apply_socket_plug(std::uint64_t targetInstanceSoid,
                                                     std::uint8_t socketLane,
                                                     std::uint16_t plugDefinitionIndex,
                                                     std::uint64_t& characterSoid,
                                                     bool& equipped) noexcept;

/** Prepares one accumulated item-state change without mutating runtime State. */
[[nodiscard]] InventoryMutationResult prepare_item_state(std::uint64_t targetInstanceSoid,
                                                         std::uint16_t targetDefinitionIndex,
                                                         std::uint32_t flags,
                                                         PendingItemState& mutation) noexcept;

/** Builds the account after-image while a prepared item-state mutation remains current. */
[[nodiscard]] bool preview_item_state(const PendingItemState& mutation, AccountState& after) noexcept;

/** Commits one prepared item-state mutation after exact before-image revalidation. */
[[nodiscard]] bool commit_item_state(PendingItemState& mutation) noexcept;

/** Debug/UI convenience wrapper for Locked/Tracked/Masterwork flags. */
[[nodiscard]] InventoryMutationResult apply_item_state(std::uint64_t targetInstanceSoid,
                                                       std::uint32_t flags,
                                                       std::uint64_t& characterSoid) noexcept;

/** @return Stable text for one socket mutation result. */
[[nodiscard]] const char* socket_mutation_name(SocketMutationResult result) noexcept;

/** @return Stable text for inventory move/acquisition diagnostics. */
[[nodiscard]] const char* inventory_mutation_name(InventoryMutationResult result) noexcept;


/**
 * Checkpoints current runtime characters/equipment into characters.dat and mirrors them into the
 * settings.json compatibility document. Runtime State remains authoritative if either write fails.
 */
[[nodiscard]] bool checkpoint_characters() noexcept;

/** @return Character persistence status. */
[[nodiscard]] CharacterStoreStatus character_store_status() noexcept;

/** @return Stable text for one mutation result. */
[[nodiscard]] const char* character_mutation_name(CharacterMutationResult result) noexcept;

/**
 * Returns the complete bundled factory template account used by configured item/ability extraction.
 * Runtime character creation/class changes use these stable starter definitions rather than JSON rows.
 */
[[nodiscard]] AccountState configured_account_snapshot() noexcept;

/**
 * Persists the exact bit-packed native opcode-501 request as a diagnostic artifact.
 * The capture does not mutate the active account until its field layout is proven.
 */
[[nodiscard]] bool capture_character_creation_request(std::span<const std::byte> payload,
                                                      std::uint32_t& captureIndex) noexcept;

/** @return A copy of the evaluated content state, read under the lock. */
[[nodiscard]] InvestmentState investment_snapshot() noexcept;

} // namespace sunrise::state
