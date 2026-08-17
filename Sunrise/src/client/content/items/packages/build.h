#pragma once

#include <cstdint>

namespace sunrise::client::content::items::packages {

/**
 * Publishes the dense item table from the installed packages, once.
 * @return True when State already holds the table or a full pass publishes it.
 */
[[nodiscard]] bool build() noexcept;

/**
 * Reports whether the pass can read anything yet.
 * It needs the bootstrap token and the installed key table, both published before the Client
 * worker starts. Until both are there, every call returns at once.
 * @return True when the block keys the pass borrows are there.
 */
[[nodiscard]] bool readable() noexcept;

/**
 * Resolves one editor-selected item and its native-default plugs from installed packages.
 * The result extends only the process-local detail table; it does not trigger the global
 * process-freeze path or preload the rest of the external item catalogue.
 * @param definitionHash Selected item definition hash.
 * @param referenceDefinitionHash Currently equipped definition in the semantic slot. Its installed
 * native equipment slot is the contract the replacement must preserve.
 * @return True when the base item and every required default plug are ready for live generation.
 */
[[nodiscard]] bool ensure_editor_item_details(std::uint32_t definitionHash,
                                              std::uint32_t referenceDefinitionHash) noexcept;

/**
 * Resolves one installed equippable item and its native-default plugs without requiring an
 * already-equipped reference item. This is used by native Collections acquisition, where the
 * Client identifies the item by collectible row rather than by one of Sunrise's semantic slots.
 * @param definitionHash Installed item definition selected by the native request.
 * @return True when the item detail and every required default plug are ready for generation.
 */
[[nodiscard]] bool ensure_inventory_item_details(std::uint32_t definitionHash) noexcept;

/**
 * Resolves one arbitrary installed socket plug detail without requiring an equipment slot or
 * matching the target item bucket. Shaders, ornaments, mods, and perk plugs are item definitions
 * in their own native buckets, but equipped appearance generation still needs their package art
 * and material metadata after a live socket action.
 * @param definitionHash Installed plug item definition hash.
 * @return True when that exact plug detail is available in the process-local detail catalog.
 */
[[nodiscard]] bool ensure_socket_plug_details(std::uint32_t definitionHash) noexcept;

/**
 * Rehydrates process-local native details for every item persisted in AccountState.
 * Dynamic debug/Collections items survive in characters.dat, while their package details are
 * intentionally not part of the immutable boot cache. This pass restores those details before
 * Family 4 resolves the persisted owned-item graph.
 * @return True when every equipped and unequipped persisted item can resolve natively.
 */
[[nodiscard]] bool ensure_account_inventory_item_details() noexcept;

} // namespace sunrise::client::content::items::packages
