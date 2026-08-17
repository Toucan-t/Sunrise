#pragma once

#include <array>
#include <cstddef>

#include "../../../../../middleware/datagen/family4/loadout/definition.h"
#include "../../../../../middleware/queuez/queuez_update.h"
#include "../../../../../middleware/queuez/subscription.h"
#include "../../../../../state/account/account_state.h"
#include "../../internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {

/** Reserved presentation-only character key used to initialize the cold creator renderer. */
inline constexpr std::uint64_t kCreatorBootstrapCharacterSoid = 0xFFFFFFFFFFFFF001ULL;

/** Account and selected-character identity take the first two family-four descriptors. */
inline constexpr std::size_t kFamily4IdentityObjectCount = 2;
/**
 * Family four carries both identity objects plus one item record per owned item, for every
 * character. The equip-summary reader looks up an instance with no null check, so no character in
 * the roster may point at a record this snapshot leaves out.
 */
inline constexpr std::size_t kObjectCapacity =
    kFamily4IdentityObjectCount
    + state::kCharacterCapacity * middleware::datagen::family4::loadout::kItemCapacity
    + state::account::inventory::kProfileActionSourceCapacity;

/** Prepared descriptors and scratch extents owned until the update codec copies their bodies. */
struct Prepared {
    std::array<middleware::queuez::Object, kObjectCapacity> objects{};
    middleware::queuez::Family family{};
    std::size_t rawClearSize{};
    std::size_t compressedClearSize{};

    // Default copying would leave family.objects pointing into the source descriptor array.
    Prepared() noexcept = default;
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    Prepared(Prepared&&) = delete;
    Prepared& operator=(Prepared&&) = delete;
};

/**
 * Builds one initial family snapshot from State and build mappings.
 * @param scratch Object and compression storage owned by the lock.
 * @param subscription Family id the Client picked.
 * @param prepared Gets the object descriptors and scratch clear extents.
 * @return True when the asked-for snapshot is valid for the current State and mappings.
 */
[[nodiscard]] bool prepare_initial(Scratch& scratch,
                                   const middleware::queuez::Subscription& subscription,
                                   Prepared& prepared) noexcept;

/**
 * Builds a Family-4 incremental containing only the currently selected character object.
 * This deliberately avoids the account object: selection handling has already shown that a live
 * full account-body replacement can erase client-resident settings that Sunrise does not author.
 * @param scratch Object and compression storage owned by the lock.
 * @param familyRootSoid Active Family-4 root.
 * @param version Next Family-4 incremental version.
 * @param characterSoid Character that must currently be selected.
 * @param prepared Gets the one-object incremental.
 * @return True when the selected character and installed mappings encode completely.
 */
[[nodiscard]] bool prepare_character_refresh(Scratch& scratch,
                                             std::uint64_t familyRootSoid,
                                             std::int32_t version,
                                             std::uint64_t characterSoid,
                                             Prepared& prepared) noexcept;

/**
 * Builds a Family-4 incremental containing only the resident account object. Profile inventory
 * debug edits use this instead of rebuilding the full manifest, because adding a profile row may
 * legitimately change account contents while every resident object identity stays unchanged.
 */
[[nodiscard]] bool prepare_account_refresh(Scratch& scratch,
                                           std::uint64_t familyRootSoid,
                                           std::int32_t version,
                                           Prepared& prepared) noexcept;

/** Builds the same one-account-object incremental from an explicit account after-image. */
[[nodiscard]] bool prepare_account_refresh_from_account(Scratch& scratch,
                                                        std::uint64_t familyRootSoid,
                                                        std::int32_t version,
                                                        const state::AccountState& account,
                                                        Prepared& prepared) noexcept;


/**
 * Builds a profile-stack after-image, optionally adding a new resident shader/mod source first.
 * A positive acquisition mutation serial/quantity pair emits the one-shot native profile-inventory
 * change descriptor used by Collections pickup feedback. Pass zeroes for ordinary/debug refreshes.
 */
[[nodiscard]] bool prepare_profile_item_refresh_from_account(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    const state::AccountState& account,
    std::uint64_t instanceSoid,
    bool addResident,
    std::int32_t acquisitionMutationSerial,
    std::int32_t acquisitionQuantity,
    Prepared& prepared) noexcept;

/**
 * Builds a character-deletion account after-image followed by empty-payload releases for any
 * deleted character/item residents that the current peer actually owns.
 */
[[nodiscard]] bool prepare_character_deletion_from_account(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    const state::AccountState& account,
    std::uint64_t deletedCharacterSoid,
    bool releaseCharacterResident,
    std::span<const std::uint64_t> releasedItemSoids,
    Prepared& prepared) noexcept;

/** Builds a profile-stack after-image and optionally releases its final resident action source. */
[[nodiscard]] bool prepare_profile_item_dismantle_from_account(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    const state::AccountState& account,
    std::uint64_t releasedInstanceSoid,
    Prepared& prepared) noexcept;

/** Builds the same selected-character incremental from a checked uncommitted account after-image. */
[[nodiscard]] bool prepare_character_refresh_from_account(Scratch& scratch,
                                                          std::uint64_t familyRootSoid,
                                                          std::int32_t version,
                                                          const state::AccountState& account,
                                                          std::uint64_t characterSoid,
                                                          Prepared& prepared) noexcept;

/**
 * Builds a manifest-preserving equipped-definition refresh in dependency order. The changed item
 * instance is always first. When its owner is the selected resident character, that character
 * after-image follows in the same Family-4 incremental so the Client re-evaluates the new body.
 * Inactive owners have no resident character object and therefore publish only the item instance.
 * @param scratch Lock-owned encoding/compression storage.
 * @param familyRootSoid Active Family-4 account root.
 * @param version Next Family-4 incremental version.
 * @param characterSoid Character that owns the edited equipped instance.
 * @param instanceSoid Existing equipped item-instance SOID whose definition changed.
 * @param prepared Gets one or two dependency-ordered object descriptors.
 */
[[nodiscard]] bool prepare_equipment_refresh(Scratch& scratch,
                                             std::uint64_t familyRootSoid,
                                             std::int32_t version,
                                             std::uint64_t characterSoid,
                                             std::uint64_t instanceSoid,
                                             Prepared& prepared) noexcept;

/**
 * Builds a manifest-preserving socket/item-body refresh from an explicit account after-image.
 * The changed item instance is first. Equipped appearance actions may request the selected
 * character after-image as the second descriptor; unequipped socket edits publish the item only.
 */
[[nodiscard]] bool prepare_socket_refresh_from_account(Scratch& scratch,
                                                       std::uint64_t familyRootSoid,
                                                       std::int32_t version,
                                                       const state::AccountState& account,
                                                       std::uint64_t characterSoid,
                                                       std::uint64_t instanceSoid,
                                                       bool includeCharacter,
                                                       Prepared& prepared) noexcept;

/** Builds the same socket refresh from the currently committed runtime account. */
[[nodiscard]] bool prepare_socket_refresh(Scratch& scratch,
                                          std::uint64_t familyRootSoid,
                                          std::int32_t version,
                                          std::uint64_t characterSoid,
                                          std::uint64_t instanceSoid,
                                          bool includeCharacter,
                                          Prepared& prepared) noexcept;

/**
 * Builds one new item-instance resident for the debug inventory seeding path. The character object
 * deliberately follows in the next Family-4 version, after the new dependency has become resident.
 */
[[nodiscard]] bool prepare_inventory_item_addition(Scratch& scratch,
                                                   std::uint64_t familyRootSoid,
                                                   std::int32_t version,
                                                   std::uint64_t characterSoid,
                                                   std::uint64_t instanceSoid,
                                                   Prepared& prepared) noexcept;

/** Builds a native acquisition incremental from one uncommitted account after-image. */
[[nodiscard]] bool prepare_item_acquisition_from_account(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    const state::AccountState& account,
    std::uint64_t characterSoid,
    std::uint64_t instanceSoid,
    Prepared& prepared) noexcept;

/** Builds a native dismantle incremental: character after-image first, then item release. */
[[nodiscard]] bool prepare_item_dismantle_from_account(
    Scratch& scratch,
    std::uint64_t familyRootSoid,
    std::int32_t version,
    const state::AccountState& account,
    std::uint64_t characterSoid,
    std::uint64_t releasedInstanceSoid,
    Prepared& prepared) noexcept;

/**
 * Builds a Family-4 incremental containing only one newly created character's item instances.
 * The account body is intentionally not republished: live full-account replacement can erase
 * client-owned settings, while the Family-3 roster update carries the new roster row separately.
 */
[[nodiscard]] bool prepare_created_character_items(Scratch& scratch,
                                                   std::uint64_t familyRootSoid,
                                                   std::int32_t version,
                                                   std::uint64_t characterSoid,
                                                   Prepared& prepared) noexcept;

/**
 * Builds a Family-3 incremental containing only one character-select record.
 * @param scratch Object and compression storage owned by the lock.
 * @param familyRootSoid Active account root.
 * @param version Next Family-3 incremental version.
 * @param characterSoid Character record to rebuild.
 * @param prepared Gets the one-object incremental.
 * @return True when the character exists and its render/stat/ability record encodes completely.
 */
[[nodiscard]] bool prepare_roster_character_refresh(Scratch& scratch,
                                                    std::uint64_t familyRootSoid,
                                                    std::int32_t version,
                                                    std::uint64_t characterSoid,
                                                    Prepared& prepared) noexcept;

/**
 * Builds a Family-0 incremental containing only the selected character record. The anchor is
 * unchanged when the selected SOID does not change, so it is deliberately not republished.
 * @param scratch Object and compression storage owned by the lock.
 * @param familyRootSoid Active account root.
 * @param version Next Family-0 incremental version.
 * @param characterSoid Character that must still be selected.
 * @param prepared Gets the one-object incremental.
 * @return True when the selected banner record encodes completely.
 */
[[nodiscard]] bool prepare_banner_character_refresh(Scratch& scratch,
                                                    std::uint64_t familyRootSoid,
                                                    std::int32_t version,
                                                    std::uint64_t characterSoid,
                                                    Prepared& prepared) noexcept;

/**
 * Builds the family-zero banner anchor and the record for the character it names.
 * @param scratch Raw object storage owned by the lock.
 * @param familyRootSoid Root the Client subscribed for the roster.
 * @param version Family version this frame carries.
 * @param previousCharacter Character whose record this frame releases, or zero for the full
 *        snapshot. Nonzero also clears the full-snapshot flag, which retail sets once.
 * @param prepared Gets the descriptors and the scratch clear extent.
 * @return True when a character is selected and every object fits raw storage.
 */
[[nodiscard]] bool prepare_banner(Scratch& scratch,
                                  std::uint64_t familyRootSoid,
                                  std::int32_t version,
                                  std::uint64_t previousCharacter,
                                  Prepared& prepared) noexcept;

} // namespace sunrise::server::bap::encrypted::push::snapshot
