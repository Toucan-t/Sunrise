#pragma once

#include <cstdint>

#include "../../client/network/consumer.h"

namespace sunrise::server::bap {

/** Applies one connection-scoped BAP lifecycle event. */
[[nodiscard]] bool consume(const client::network::BapRequest& request,
                           client::network::BapResponse& response) noexcept;

/**
 * Queues a safe versioned character/item refresh for every authenticated Queuez peer.
 * The deferred path refuses publication if State changed the Family-4 resident manifest. A
 * successful companion-object chain checkpoints runtime characters and mirrors settings.json.
 * @return True when at least one active peer was armed.
 */
[[nodiscard]] bool request_profile_item_refresh(std::uint64_t instanceSoid,
                                                bool addResident) noexcept;

[[nodiscard]] bool request_account_refresh() noexcept;

/**
 * Queues one dependency-ordered equipped-item refresh. The existing item-instance resident is
 * published before the selected-character after-image that references it, then Family 3/0 mirrors
 * follow. Inactive characters publish only the item instance plus their Family-3 roster record.
 * @param characterSoid Stable owner character key.
 * @param instanceSoid Existing equipped item-instance key whose definition changed.
 * @return True when at least one settled Queuez peer was armed.
 */
[[nodiscard]] bool request_equipment_refresh(std::uint64_t characterSoid,
                                             std::uint64_t instanceSoid) noexcept;

/**
 * Queues one manifest-preserving socket/item-body refresh. Equipped edits publish item then
 * selected character and arm Family 3/0 appearance mirrors; unequipped edits publish item only.
 */
[[nodiscard]] bool request_socket_refresh(std::uint64_t characterSoid,
                                          std::uint64_t instanceSoid,
                                          bool equipped) noexcept;

/**
 * Queues publication of one newly debug-created unequipped item. The new item resident is added in
 * one Family-4 version and the selected-character after-image follows in the next version.
 */
[[nodiscard]] bool request_inventory_add_refresh(std::uint64_t characterSoid,
                                                 std::uint64_t instanceSoid) noexcept;

/**
 * Queues a character-metadata refresh without republishing the giant Family-4 account object.
 * Family 4 is touched only when this character is the resident selected character; Family 3 gets
 * only that character record, and Family 0 follows when it names the same character. A complete
 * chain checkpoints runtime characters and mirrors settings.json.
 * @param characterSoid Stable character key that was edited.
 * @return True when at least one settled Queuez peer was armed.
 */
[[nodiscard]] bool request_character_refresh(std::uint64_t characterSoid) noexcept;

/** Wipes every connection-owned nonce and transform buffer. */
void shutdown() noexcept;

} // namespace sunrise::server::bap
