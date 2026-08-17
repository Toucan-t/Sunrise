#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/web_service/messages/opcode206.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::web_service {

/** Optional Server action produced while answering one Web Service request. */
struct Outcome {
    bool hasSubscription{};
    middleware::queuez::Subscription subscription{};
    /** An opcode-504 pick moved the selection and its Family-4 object still has to follow. */
    bool hasSelectedCharacter{};
    std::uint64_t selectedCharacterSoid{};
    /** Opcode 501 created a new runtime character whose Queuez residents still need publishing. */
    bool hasCreatedCharacter{};
    std::uint64_t createdCharacterSoid{};
    /** Opcode 502 prepared one character-select roster/member deletion. */
    bool hasCharacterDeletion{};
    state::PendingCharacterDeletion characterDeletion{};
    /** Opcode 403/404 prepared a selected-character inventory move for atomic Queuez publication. */
    bool hasEquipmentMutation{};
    state::PendingInventoryMove equipmentMutation{};
    /** Opcode 1820 prepared one selected-character Collections acquisition. */
    bool hasItemAcquisition{};
    state::PendingItemAcquisition itemAcquisition{};
    /** Opcode 1820 prepared one account-wide profile-stack acquisition. */
    bool hasProfileItemAcquisition{};
    state::PendingProfileItemAcquisition profileItemAcquisition{};
    /** Opcode 402 prepared removal of one unequipped selected-character item. */
    bool hasItemDismantle{};
    state::PendingItemDismantle itemDismantle{};
    /** Opcode 402 prepared an account-wide profile-stack decrement/removal. */
    bool hasProfileItemDismantle{};
    state::PendingProfileItemDismantle profileItemDismantle{};
    /** Opcode 903/1901 prepared one owned item socket/appearance plug replacement. */
    bool hasSocketMutation{};
    state::PendingSocketPlug socketMutation{};
    /** Opcode 406 prepared one accumulated owned-item state change. */
    bool hasItemStateMutation{};
    state::PendingItemState itemStateMutation{};
};

/**
 * Answers one whole supported Web Service request body.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Answers one request and reports any subscription side effect.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written,
                           Outcome& outcome,
                           std::int32_t nextFamily4Version) noexcept;

/** Compatibility wrapper for callers without a resident Family-4 version. */
[[nodiscard]] bool consume(std::span<const std::byte> request,
                           std::span<std::byte> response,
                           std::size_t& written,
                           Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
