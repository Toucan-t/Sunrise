#include "web_service_runtime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "../../client/content/items/packages/build.h"
#include "../../core/logging/log.h"
#include "../../middleware/encoding/bit_reader.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode502.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode601/opcode601_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "opcode_routes.h"

namespace sunrise::server::web_service {

/** One log line carries the opcode and its fixed prefix. */
constexpr std::size_t kOpcodeLineCapacity = 64;
/** Socket-action payload bytes retained on parse failure for protocol-layout discovery. */
constexpr std::size_t kSocketPayloadCaptureBytes = 96;

/** Logs the exact received socket-action body when a strict native codec rejects it. */
void report_socket_payload(const middleware::web_service::Message& message) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=socket stage=payload opcode=%u bytes=%zu hex=",
                                static_cast<unsigned>(message.opcode),
                                message.payload.size());
    if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
        return;
    }
    std::size_t cursor = static_cast<std::size_t>(written);
    const std::size_t count =
        (std::min)(message.payload.size(), kSocketPayloadCaptureBytes);
    for (std::size_t index = 0; index < count; ++index) {
        if (cursor + 2U >= line.size()) {
            break;
        }
        const int added = std::snprintf(line.data() + cursor,
                                        line.size() - cursor,
                                        "%02X",
                                        static_cast<unsigned>(
                                            std::to_integer<std::uint8_t>(message.payload[index])));
        if (added != 2) {
            break;
        }
        cursor += 2U;
    }
    if (count < message.payload.size() && cursor + 3U < line.size()) {
        line[cursor++] = '.';
        line[cursor++] = '.';
        line[cursor++] = '.';
    }
    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     {line.data(), cursor});
}


/**
 * Logs which Web Service opcode arrived. One svc-10 frame looks like any other in the log, and
 * the opcodes the Client sends are what drive its queuez state machine.
 * @param opcode Parsed wire opcode.
 */
void report_opcode(std::uint32_t opcode) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=ws stage=request opcode=%u", opcode);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}


/** Number of payload bytes kept on one create-request diagnostic line. */
constexpr std::size_t kCreateBytesPerLine = 20;
/** One diagnostic line fits the complete observed 17-value 32-bit field group. */
constexpr std::size_t kCreateLineCapacity = 256;

/** Emits the mechanical 97-byte field split while keeping every field semantically unnamed. */
void report_create_layout(std::span<const std::byte> payload, std::uint32_t captureIndex) noexcept {
    middleware::web_service::messages::opcode501::DiagnosticLayout layout{};
    std::array<char, kCreateLineCapacity> line{};
    if (!middleware::web_service::messages::opcode501::parse_diagnostic_layout(payload, layout)) {
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=ws501 stage=layout result=unknown capture=%03u bytes=%zu",
                                          static_cast<unsigned>(captureIndex),
                                          payload.size());
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }

    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=ws501 stage=layout result=ok capture=%03u prefix=0x%X padding=0x%X",
                                static_cast<unsigned>(captureIndex),
                                static_cast<unsigned>(layout.prefix),
                                static_cast<unsigned>(layout.padding));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }

    written = std::snprintf(line.data(),
                            line.size(),
                            "ev=ws501 stage=fields16 capture=%03u values=",
                            static_cast<unsigned>(captureIndex));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        std::size_t cursor = static_cast<std::size_t>(written);
        for (std::size_t index = 0; index < layout.fields16.size(); ++index) {
            const int added = std::snprintf(line.data() + cursor,
                                            line.size() - cursor,
                                            index == 0 ? "%04X" : ",%04X",
                                            static_cast<unsigned>(layout.fields16[index]));
            if (added <= 0 || static_cast<std::size_t>(added) >= line.size() - cursor) {
                return;
            }
            cursor += static_cast<std::size_t>(added);
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), cursor});
    }

    written = std::snprintf(line.data(),
                            line.size(),
                            "ev=ws501 stage=fields32 capture=%03u values=",
                            static_cast<unsigned>(captureIndex));
    if (written > 0 && static_cast<std::size_t>(written) < line.size()) {
        std::size_t cursor = static_cast<std::size_t>(written);
        for (std::size_t index = 0; index < layout.fields32.size(); ++index) {
            const int added = std::snprintf(line.data() + cursor,
                                            line.size() - cursor,
                                            index == 0 ? "%08X" : ",%08X",
                                            static_cast<unsigned>(layout.fields32[index]));
            if (added <= 0 || static_cast<std::size_t>(added) >= line.size() - cursor) {
                return;
            }
            cursor += static_cast<std::size_t>(added);
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), cursor});
    }
}

/** Logs and persists an opcode-501 request without assigning meaning to unproven fields. */
void capture_create_character(const middleware::web_service::Message& message) noexcept {
    middleware::web_service::messages::opcode501::RequestCapture capture{};
    if (!middleware::web_service::messages::opcode501::capture_request(message, capture)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws501 stage=capture result=empty");
        return;
    }
    std::uint32_t captureIndex = 0;
    const bool saved = state::capture_character_creation_request(message.payload, captureIndex);
    std::array<char, kCreateLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=ws501 stage=capture result=%s capture=%03u tx=%u bytes=%zu retained=%zu truncated=%u",
                                saved ? "ok" : "disk_fail",
                                static_cast<unsigned>(captureIndex),
                                static_cast<unsigned>(message.transactionId),
                                capture.wireSize,
                                capture.size,
                                static_cast<unsigned>(capture.truncated));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         saved ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    report_create_layout(message.payload, captureIndex);
    for (std::size_t offset = 0; offset < capture.size; offset += kCreateBytesPerLine) {
        const std::size_t count = (std::min)(kCreateBytesPerLine, capture.size - offset);
        written = std::snprintf(line.data(),
                                line.size(),
                                "ev=ws501 stage=payload capture=%03u offset=%zu count=%zu hex=",
                                static_cast<unsigned>(captureIndex),
                                offset,
                                count);
        if (written <= 0 || static_cast<std::size_t>(written) >= line.size()) {
            continue;
        }
        std::size_t cursor = static_cast<std::size_t>(written);
        for (std::size_t index = 0; index < count && cursor + 4 < line.size(); ++index) {
            const unsigned value = std::to_integer<unsigned>(capture.bytes[offset + index]);
            const int added = std::snprintf(line.data() + cursor,
                                            line.size() - cursor,
                                            index == 0 ? "%02X" : " %02X",
                                            value);
            if (added <= 0 || static_cast<std::size_t>(added) >= line.size() - cursor) {
                break;
            }
            cursor += static_cast<std::size_t>(added);
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), cursor});
    }
}

/** Opcode 403/404 carry one big-endian instance SOID followed by one cleared byte. */
constexpr std::size_t kEquipmentActionPayloadSize = middleware::encoding::kU64Size + 1U;

/** Parses and applies one native equip/unequip request through the shared runtime inventory path. */
[[nodiscard]] bool mutate_equipment(const middleware::web_service::Message& message,
                                    bool unequip,
                                    std::int32_t nextFamily4Version,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    Outcome& outcome) noexcept {
    std::uint64_t instanceSoid = 0;
    if (message.payload.size() == kEquipmentActionPayloadSize
        && message.payload[middleware::encoding::kU64Size] == std::byte{}) {
        instanceSoid = middleware::encoding::read_u64_be(
            std::span<const std::byte, middleware::encoding::kU64Size>{
                message.payload.data(), middleware::encoding::kU64Size});
    }
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();
    state::PendingInventoryMove mutation{};
    state::InventoryMutationResult result = state::InventoryMutationResult::invalid;
    if (instanceSoid != 0 && versionReady) {
        result = state::prepare_inventory_move(instanceSoid,
                                               unequip ? state::InventoryMoveKind::unequip
                                                       : state::InventoryMoveKind::equip,
                                               mutation);
    }
    const std::uint64_t characterSoid = mutation.characterSoid;

    std::array<char, 192> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=equipment stage=ws result=%s opcode=%u action=%s "
                                    "instance=0x%llX character=0x%llX family_version=%d reason=%s",
                                    result == state::InventoryMutationResult::ok ? "ok" : "fail",
                                    static_cast<unsigned>(message.opcode),
                                    unequip ? "unequip" : "equip",
                                    static_cast<unsigned long long>(instanceSoid),
                                    static_cast<unsigned long long>(characterSoid),
                                    nextFamily4Version,
                                    versionReady ? state::inventory_mutation_name(result)
                                                 : "family4_not_ready");
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == state::InventoryMutationResult::ok ? core::log::Level::info
                                                                      : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }

    middleware::web_service::StatusResponse status{};
    if (result == state::InventoryMutationResult::ok) {
        status.value = nextFamily4Version;
        outcome.hasEquipmentMutation = true;
        outcome.equipmentMutation = mutation;
    } else {
        status.code = 1;
    }
    return middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
}

/** Parses and prepares one ordinary or equipped-appearance socket action. */
[[nodiscard]] bool mutate_socket(const middleware::web_service::Message& message,
                                 bool equippedAppearance,
                                 std::int32_t nextFamily4Version,
                                 std::span<std::byte> response,
                                 std::size_t& written,
                                 Outcome& outcome) noexcept {
    middleware::web_service::StatusResponse status{};
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();
    state::PendingSocketPlug mutation{};
    state::SocketMutationResult result = state::SocketMutationResult::invalid;
    std::uint64_t selectorOrInstance = 0;
    std::uint32_t socketLane = 0;
    std::uint16_t plugDefinitionIndex = 0;
    std::uint8_t modelSocketKind = 0;
    std::uint64_t auxiliary = 0;
    bool parsed = false;

    if (!equippedAppearance) {
        middleware::web_service::messages::opcode903::Request request{};
        parsed = middleware::web_service::messages::opcode903::parse_request(message, request)
                 && request.hasInstance && request.instanceSoid != 0 && request.hasPlugDefinition
                 && request.socketIndex < state::account::inventory::kPlugCapacity;
        selectorOrInstance = request.instanceSoid;
        socketLane = request.socketIndex;
        plugDefinitionIndex = request.plugDefinitionIndex;
        if (parsed && versionReady) {
            result = state::prepare_socket_plug(request.instanceSoid,
                                                static_cast<std::uint8_t>(request.socketIndex),
                                                request.plugDefinitionIndex,
                                                mutation);
            if (result == state::SocketMutationResult::ok && request.hasTargetDefinition
                && request.targetDefinitionIndex != mutation.targetDefinitionIndex) {
                result = state::SocketMutationResult::unsupportedDefinition;
                mutation = {};
            }
        }
    } else {
        middleware::web_service::messages::opcode1901::Request request{};
        parsed = middleware::web_service::messages::opcode1901::parse_request(message, request)
                 && request.equipmentSelector != 0
                 && request.socketIndex < state::account::inventory::kPlugCapacity;
        selectorOrInstance = request.equipmentSelector;
        socketLane = request.socketIndex;
        plugDefinitionIndex = request.plugDefinitionIndex;
        modelSocketKind = request.modelSocketKind;
        auxiliary = request.auxiliary;
        if (parsed && versionReady) {
            result = state::prepare_equipped_socket_plug(request.equipmentSelector,
                                                         static_cast<std::uint8_t>(request.socketIndex),
                                                         request.plugDefinitionIndex,
                                                         mutation);
        }
    }

    // Character-record appearance folding consumes plug detail rows. Resolve the chosen plug now
    // so a shader/ornament applied to equipped gear can update Family 3/0 in the same transaction.
    if (result == state::SocketMutationResult::ok
        && !client::content::items::packages::ensure_socket_plug_details(
            mutation.plugDefinitionHash)) {
        result = state::SocketMutationResult::unsupportedDefinition;
        mutation = {};
    }

    if (result == state::SocketMutationResult::ok) {
        status.value = nextFamily4Version;
        outcome.hasSocketMutation = true;
        outcome.socketMutation = mutation;
    } else {
        status.code = 1;
    }

    if (!parsed) {
        report_socket_payload(message);
    }

    std::array<char, 320> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket stage=ws result=%s opcode=%u target=0x%llX lane=%u plug_definition=%u "
        "plug_hash=0x%08X character=0x%llX equipped=%u model_kind=%u auxiliary=0x%llX "
        "family_version=%d reason=%s",
        result == state::SocketMutationResult::ok ? "ok" : "fail",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned long long>(selectorOrInstance),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        mutation.plugDefinitionHash,
        static_cast<unsigned long long>(mutation.characterSoid),
        mutation.targetEquipped ? 1U : 0U,
        static_cast<unsigned>(modelSocketKind),
        static_cast<unsigned long long>(auxiliary),
        nextFamily4Version,
        !parsed ? "payload"
                : (!versionReady ? "family4_not_ready" : state::socket_mutation_name(result)));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == state::SocketMutationResult::ok ? core::log::Level::info
                                                                   : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
}

/** Exact 15-byte accumulated item-state descriptor used by opcode 406. */
constexpr std::size_t kItemStatePayloadSize = 15;
constexpr std::uint8_t kItemStateDefinitionIndexWidth = 15;
constexpr std::uint8_t kItemStateValueWidth = 32;
constexpr std::uint8_t kItemStatePaddingWidth = 7;
constexpr std::uint64_t kItemStateValueBias = 0x80000000ULL;

/** Parses Locked/Tracked/Masterwork state and stages the character-row after-image atomically. */
[[nodiscard]] bool mutate_item_state(const middleware::web_service::Message& message,
                                     std::int32_t nextFamily4Version,
                                     std::span<std::byte> response,
                                     std::size_t& written,
                                     Outcome& outcome) noexcept {
    middleware::web_service::StatusResponse status{};
    middleware::encoding::bits::Reader reader(message.payload);
    std::uint64_t instancePresent = 0;
    std::uint64_t instanceSoid = 0;
    std::uint64_t definitionPresent = 0;
    std::uint64_t definitionWire = 0;
    std::uint64_t flagsWire = 0;
    std::uint64_t padding = 0;
    constexpr std::uint32_t kSupportedStateMask = 0x7U;
    const bool parsed = message.payload.size() == kItemStatePayloadSize
                        && reader.read(1, instancePresent) && reader.read(64, instanceSoid)
                        && reader.read(1, definitionPresent)
                        && reader.read(kItemStateDefinitionIndexWidth, definitionWire)
                        && reader.read(kItemStateValueWidth, flagsWire)
                        && reader.read(kItemStatePaddingWidth, padding)
                        && reader.remaining_bits() == 0 && instancePresent == 1
                        && instanceSoid != 0 && definitionPresent == 1
                        && definitionWire <= (std::numeric_limits<std::uint16_t>::max)()
                        && flagsWire >= kItemStateValueBias && padding == 0
                        && flagsWire - kItemStateValueBias <= kSupportedStateMask;
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();
    state::PendingItemState mutation{};
    state::InventoryMutationResult result = state::InventoryMutationResult::invalid;
    if (parsed && versionReady) {
        result = state::prepare_item_state(
            instanceSoid,
            static_cast<std::uint16_t>(definitionWire),
            static_cast<std::uint32_t>(flagsWire - kItemStateValueBias),
            mutation);
    }
    if (result == state::InventoryMutationResult::ok) {
        status.value = nextFamily4Version;
        outcome.hasItemStateMutation = true;
        outcome.itemStateMutation = mutation;
    } else {
        status.code = 1;
    }
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=item_state stage=ws result=%s opcode=%u instance=0x%llX definition=%llu "
        "flags=0x%llX family_version=%d reason=%s",
        result == state::InventoryMutationResult::ok ? "ok" : "fail",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned long long>(definitionWire),
        flagsWire >= kItemStateValueBias
            ? static_cast<unsigned long long>(flagsWire - kItemStateValueBias)
            : 0ULL,
        nextFamily4Version,
        !parsed ? "payload" : (!versionReady ? "family4_not_ready"
                                             : state::inventory_mutation_name(result)));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == state::InventoryMutationResult::ok ? core::log::Level::info
                                                                      : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
}

/** Exact three-byte Collections acquisition selector used by opcode 1820. */
constexpr std::size_t kItemAcquisitionPayloadSize = 3;
constexpr std::uint8_t kItemAcquisitionPresenceWidth = 1;
constexpr std::uint8_t kItemAcquisitionCollectibleIndexWidth = 15;
constexpr std::uint8_t kItemAcquisitionPaddingWidth = 8;

/** Parses and prepares one native Collections pull without committing State early. */
[[nodiscard]] bool acquire_item(const middleware::web_service::Message& message,
                                std::int32_t nextFamily4Version,
                                std::span<std::byte> response,
                                std::size_t& written,
                                Outcome& outcome) noexcept {
    middleware::web_service::StatusResponse status{};
    std::uint64_t present = 0;
    std::uint64_t collectibleWire = 0;
    std::uint64_t padding = 0;
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();
    middleware::encoding::bits::Reader reader(message.payload);
    const bool parsed = message.payload.size() == kItemAcquisitionPayloadSize
                        && reader.read(kItemAcquisitionPresenceWidth, present)
                        && reader.read(kItemAcquisitionCollectibleIndexWidth, collectibleWire)
                        && reader.read(kItemAcquisitionPaddingWidth, padding)
                        && reader.remaining_bits() == 0 && present == 1 && padding == 0
                        && collectibleWire <= (std::numeric_limits<std::uint16_t>::max)();

    bool success = false;
    bool profile = false;
    const char* reason = "inventory transition is invalid";
    state::PendingItemAcquisition itemMutation{};
    state::PendingProfileItemAcquisition profileMutation{};
    std::uint16_t itemDefinitionIndex = 0;
    std::uint32_t definitionHash = 0;
    if (parsed && versionReady) {
        const auto collectibleIndex = static_cast<std::uint16_t>(collectibleWire);
        state::build_data::items::Definition definition{};
        if (state::build_data::find_collectible_item_definition_index(collectibleIndex,
                                                                      itemDefinitionIndex)
            && state::build_data::find_item_definition_index(itemDefinitionIndex, definition)
            && definition.definitionIndex == itemDefinitionIndex) {
            definitionHash = definition.definitionHash;

            // Resolve the base definition without assuming it belongs to character equipment.
            // Profile shaders/mods are stackable action sources and must take the account path.
            if (client::content::items::packages::ensure_socket_plug_details(definitionHash)) {
                state::build_data::items::details::Definition detail{};
                state::build_data::inventory::buckets::Descriptor bucket{};
                if (state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
                    && detail.definitionIndex == itemDefinitionIndex
                    && detail.definitionHash == definitionHash
                    && state::build_data::find_inventory_bucket_descriptor(definition.bucketId,
                                                                           bucket)
                    && bucket.arraySelector
                           == state::build_data::inventory::buckets::ArraySelector::profile
                    && detail.instancedDefinitionState
                           == state::build_data::items::details::InstancedDefinitionState::stackable) {
                    profile = true;
                    const state::ProfileInventoryMutationResult result =
                        state::prepare_profile_item_acquisition(collectibleIndex, profileMutation);
                    success = result == state::ProfileInventoryMutationResult::ok;
                    reason = state::profile_inventory_mutation_name(result);
                } else if (client::content::items::packages::ensure_inventory_item_details(
                               definitionHash)) {
                    const state::InventoryMutationResult result =
                        state::prepare_item_acquisition(collectibleIndex, itemMutation);
                    success = result == state::InventoryMutationResult::ok;
                    reason = state::inventory_mutation_name(result);
                } else {
                    reason = "native item details are unavailable";
                }
            } else {
                reason = "native item details are unavailable";
            }
        } else {
            reason = "collectible definition is unavailable";
        }
    } else {
        reason = !parsed ? "payload" : "family4_not_ready";
    }

    if (success) {
        status.value = nextFamily4Version;
        if (profile) {
            outcome.hasProfileItemAcquisition = true;
            outcome.profileItemAcquisition = profileMutation;
        } else {
            outcome.hasItemAcquisition = true;
            outcome.itemAcquisition = itemMutation;
        }
    } else {
        status.code = 1;
    }

    const std::uint64_t instanceSoid =
        profile ? profileMutation.acquiredInstanceSoid : itemMutation.acquiredInstanceSoid;
    const std::uint64_t characterSoid = profile ? 0 : itemMutation.characterSoid;
    std::array<char, 256> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=ws result=%s opcode=%u kind=%s collectible=%llu definition=%u hash=%u "
        "instance=0x%llX character=0x%llX family_version=%d reason=%s",
        success ? "ok" : "fail",
        static_cast<unsigned>(message.opcode),
        profile ? "profile" : "character",
        static_cast<unsigned long long>(collectibleWire),
        static_cast<unsigned>(itemDefinitionIndex),
        definitionHash,
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned long long>(characterSoid),
        nextFamily4Version,
        reason);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         success ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
}

/** Exact fixed-width opcode-402 item/profile-stack dismantle request. */
constexpr std::size_t kItemDismantlePayloadSize = 16;
constexpr std::uint8_t kItemDismantleInstanceWidth = 64;
constexpr std::uint8_t kItemDismantleDefinitionIndexWidth = 15;
constexpr std::uint8_t kItemDismantleQuantityWidth = 32;
constexpr std::uint8_t kItemDismantleRequiredFlagWidth = 1;
constexpr std::uint8_t kItemDismantleNestedPaddingWidth = 6;
constexpr std::uint8_t kItemDismantleOuterTrailerWidth = 2;
constexpr std::uint8_t kItemDismantleFinalPaddingWidth = 6;
constexpr std::uint64_t kItemDismantleQuantityBias = 0x80000000ULL;

/** Parses and prepares one native character-instance or account-profile dismantle transaction. */
[[nodiscard]] bool dismantle_item(const middleware::web_service::Message& message,
                                  std::int32_t nextFamily4Version,
                                  std::span<std::byte> response,
                                  std::size_t& written,
                                  Outcome& outcome) noexcept {
    middleware::web_service::StatusResponse status{};
    std::uint64_t instancePresent = 0;
    std::uint64_t instanceSoid = 0;
    std::uint64_t definitionPresent = 0;
    std::uint64_t definitionWire = 0;
    std::uint64_t quantityWire = 0;
    std::uint64_t requiredFlag = 0;
    std::uint64_t nestedPadding = 0;
    std::uint64_t outerTrailer = 0;
    std::uint64_t finalPadding = 0;
    middleware::encoding::bits::Reader reader(message.payload);
    const bool decoded =
        message.payload.size() == kItemDismantlePayloadSize && reader.read(1, instancePresent)
        && reader.read(kItemDismantleInstanceWidth, instanceSoid) && reader.read(1, definitionPresent)
        && reader.read(kItemDismantleDefinitionIndexWidth, definitionWire)
        && reader.read(kItemDismantleQuantityWidth, quantityWire)
        && reader.read(kItemDismantleRequiredFlagWidth, requiredFlag)
        && reader.read(kItemDismantleNestedPaddingWidth, nestedPadding)
        && reader.read(kItemDismantleOuterTrailerWidth, outerTrailer)
        && reader.read(kItemDismantleFinalPaddingWidth, finalPadding)
        && reader.remaining_bits() == 0;

    // Ordinary profile stacks omit the instance identity. Resident profile action sources may carry
    // one, so a present SOID first gets the existing character-item ownership check and then the
    // exact profile identity check before the request is rejected.
    const bool instanceIdentityPresent = instancePresent == 1 && instanceSoid != 0;
    const bool instanceIdentityAbsent = instancePresent == 0 && instanceSoid == 0;
    const bool identityReady = decoded && definitionPresent == 1
                               && definitionWire <= (std::numeric_limits<std::uint16_t>::max)()
                               && (instanceIdentityPresent || instanceIdentityAbsent);
    const std::uint64_t unbiasedQuantity =
        quantityWire > kItemDismantleQuantityBias ? quantityWire - kItemDismantleQuantityBias : 0;
    const bool quantityReady = unbiasedQuantity != 0
                               && unbiasedQuantity
                                      <= static_cast<std::uint64_t>(
                                          (std::numeric_limits<std::int32_t>::max)());
    const std::int32_t requestedQuantity =
        quantityReady ? static_cast<std::int32_t>(unbiasedQuantity) : 0;
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();

    state::PendingItemDismantle itemMutation{};
    state::PendingProfileItemDismantle profileMutation{};
    state::build_data::items::Definition requestedDefinition{};
    bool definitionReady = false;
    bool success = false;
    bool profileSelected = false;
    const char* reason = !decoded ? "payload_bits"
                                  : (!identityReady ? "identity_fields"
                                                    : (!quantityReady ? "quantity"
                                                                      : (!versionReady
                                                                             ? "family4_not_ready"
                                                                             : "transition")));
    if (identityReady && quantityReady && versionReady) {
        definitionReady = state::build_data::find_item_definition_index(
            static_cast<std::uint16_t>(definitionWire), requestedDefinition);
        if (!definitionReady) {
            reason = "definition";
        } else {
            state::InventoryMutationResult itemResult = state::InventoryMutationResult::invalid;
            bool itemAttempted = false;
            if (instanceIdentityPresent && requestedQuantity == 1) {
                itemAttempted = true;
                itemResult = state::prepare_item_dismantle(instanceSoid, itemMutation);
                if (itemResult == state::InventoryMutationResult::ok) {
                    success = itemMutation.dismantledItem.definitionHash
                                  == requestedDefinition.definitionHash
                              && itemMutation.dismantledItem.quantity == requestedQuantity;
                    reason = success ? "ok" : "character_identity";
                    if (success) {
                        outcome.hasItemDismantle = true;
                        outcome.itemDismantle = itemMutation;
                    }
                }
            }

            if (!success && !(itemResult == state::InventoryMutationResult::ok)) {
                if (!client::content::items::packages::ensure_socket_plug_details(
                        requestedDefinition.definitionHash)) {
                    reason = instanceIdentityAbsent
                                 ? "native profile item details are unavailable"
                                 : (itemAttempted ? state::inventory_mutation_name(itemResult)
                                                  : "native profile item details are unavailable");
                } else {
                    const state::ProfileInventoryMutationResult profileResult =
                        state::prepare_profile_item_dismantle(
                            static_cast<std::uint16_t>(definitionWire),
                            requestedQuantity,
                            instanceIdentityPresent ? instanceSoid : 0,
                            profileMutation);
                    success = profileResult == state::ProfileInventoryMutationResult::ok
                              && profileMutation.dismantledDefinitionHash
                                     == requestedDefinition.definitionHash;
                    profileSelected = success;
                    if (success) {
                        reason = "ok";
                        outcome.hasProfileItemDismantle = true;
                        outcome.profileItemDismantle = profileMutation;
                    } else if (profileResult == state::ProfileInventoryMutationResult::ok) {
                        reason = "profile_identity";
                    } else if (instanceIdentityPresent && itemAttempted
                               && (profileResult == state::ProfileInventoryMutationResult::notFound
                                   || profileResult
                                          == state::ProfileInventoryMutationResult::notProfileItem)) {
                        reason = state::inventory_mutation_name(itemResult);
                    } else {
                        reason = state::profile_inventory_mutation_name(profileResult);
                    }
                }
            }
        }
    }

    if (success) {
        status.value = nextFamily4Version;
    } else {
        status.code = 1;
    }

    const std::uint64_t releasedInstanceSoid =
        profileSelected ? profileMutation.releasedInstanceSoid : itemMutation.dismantledInstanceSoid;
    const std::uint64_t characterSoid = profileSelected ? 0 : itemMutation.characterSoid;
    std::array<char, 288> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=dismantle stage=ws result=%s opcode=%u kind=%s instance=0x%llX definition=%llu "
        "definition_hash=%u quantity_wire=0x%llX quantity=%d flag=%llu nested=0x%llX "
        "outer=0x%llX final=0x%llX release=0x%llX character=0x%llX family_version=%d reason=%s",
        success ? "ok" : "fail",
        static_cast<unsigned>(message.opcode),
        profileSelected ? "profile"
                        : (outcome.hasItemDismantle
                               ? "character"
                               : (instanceIdentityAbsent ? "profile" : "instance")),
        static_cast<unsigned long long>(instanceSoid),
        static_cast<unsigned long long>(definitionWire),
        requestedDefinition.definitionHash,
        static_cast<unsigned long long>(quantityWire),
        requestedQuantity,
        static_cast<unsigned long long>(requiredFlag),
        static_cast<unsigned long long>(nestedPadding),
        static_cast<unsigned long long>(outerTrailer),
        static_cast<unsigned long long>(finalPadding),
        static_cast<unsigned long long>(releasedInstanceSoid),
        static_cast<unsigned long long>(characterSoid),
        nextFamily4Version,
        reason);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         success ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
}

/** Parses and prepares one native character-select deletion without committing State early. */
[[nodiscard]] bool delete_character(const middleware::web_service::Message& message,
                                    std::int32_t nextFamily4Version,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    Outcome& outcome) noexcept {
    middleware::web_service::StatusResponse status{};
    middleware::web_service::messages::opcode502::Request request{};
    const bool parsed = middleware::web_service::messages::opcode502::parse_request(message, request);
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();
    state::PendingCharacterDeletion mutation{};
    state::CharacterMutationResult result = state::CharacterMutationResult::invalid;
    const char* layout = parsed ? "direct" : "invalid";
    if (parsed && versionReady) {
        result = state::prepare_character_deletion(request.characterSoid, mutation);
        // Keep one bounded compatibility probe for an optional-presence descriptor. It is only
        // accepted when the decoded SOID names an actually owned character, so arbitrary payload
        // bits cannot select a deletion target.
        if (result == state::CharacterMutationResult::notFound && message.payload.size() >= 9U) {
            middleware::encoding::bits::Reader reader(message.payload);
            std::uint64_t present = 0;
            std::uint64_t alternateSoid = 0;
            if (reader.read(1, present) && reader.read(64, alternateSoid) && present == 1
                && alternateSoid != 0) {
                state::PendingCharacterDeletion alternate{};
                const state::CharacterMutationResult alternateResult =
                    state::prepare_character_deletion(alternateSoid, alternate);
                if (alternateResult == state::CharacterMutationResult::ok) {
                    request.characterSoid = alternateSoid;
                    mutation = alternate;
                    result = alternateResult;
                    layout = "present64";
                }
            }
        }
    }
    if (result == state::CharacterMutationResult::ok) {
        status.value = nextFamily4Version;
        outcome.hasCharacterDeletion = true;
        outcome.characterDeletion = mutation;
    } else {
        status.code = 1;
    }

    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=character_delete stage=ws result=%s opcode=%u character=0x%llX family_version=%d "
        "items=%zu stale_selected=%u layout=%s reason=%s",
        result == state::CharacterMutationResult::ok ? "ok" : "fail",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned long long>(request.characterSoid),
        nextFamily4Version,
        mutation.releasedItemCount,
        mutation.deletedWasSelected ? 1U : 0U,
        layout,
        !parsed ? "payload"
                : (!versionReady ? "character_select_not_ready"
                                 : state::character_mutation_name(result)));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == state::CharacterMutationResult::ok ? core::log::Level::info
                                                                      : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
}

/** One line carries the picked id and whether the selection moved. */
constexpr std::size_t kSelectLineCapacity = 96;

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key once the selection has moved in State.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(picked.characterSoid, changed)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterSoid = picked.characterSoid;

    std::array<char, kSelectLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=ws504 stage=select result=ok soid=0x%llX changed=%u",
                                      static_cast<unsigned long long>(picked.characterSoid),
                                      static_cast<unsigned>(changed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Answers a request whose own codec refused with the bare correlated echo.
 * The Client matches on the echoed transaction id. A missing body is worse than a thin one. It
 * under-runs the decoder and takes the BAP connection down.
 * @param message Parsed request whose correlation fields are echoed.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size in bytes.
 * @return True when the echo fits.
 */
bool encode_echo(const middleware::web_service::Message& message,
                 std::span<std::byte> response,
                 std::size_t& written) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=ws stage=body result=echo opcode=%u", message.opcode);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    namespace ws = middleware::web_service;
    return ws::encode_response(
        message, ws::ResponseShape::generic, ws::StatusResponse{}, response, written);
}

/**
 * Parses and answers one Web Service request with its whole descriptor layout.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    Outcome outcome;
    return consume(request, response, written, outcome);
}

/**
 * Parses one request, encodes its response, and publishes checked side effects last.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome,
             std::int32_t nextFamily4Version) noexcept {
    written = 0;
    outcome = {};
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws stage=parse result=fail");
        return false;
    }
    report_opcode(message.opcode);

    if (message.opcode == middleware::web_service::messages::opcode502::kOpcode) {
        return delete_character(message, nextFamily4Version, response, written, outcome);
    }
    if (message.opcode == 406U) {
        return mutate_item_state(message, nextFamily4Version, response, written, outcome);
    }
    if (message.opcode == 402U) {
        return dismantle_item(message, nextFamily4Version, response, written, outcome);
    }
    if (message.opcode == 403U || message.opcode == 404U) {
        return mutate_equipment(
            message, message.opcode == 404U, nextFamily4Version, response, written, outcome);
    }
    if (message.opcode == 1820U) {
        return acquire_item(message, nextFamily4Version, response, written, outcome);
    }
    if (message.opcode == middleware::web_service::messages::opcode903::kOpcode) {
        return mutate_socket(message, false, nextFamily4Version, response, written, outcome);
    }
    if (message.opcode == middleware::web_service::messages::opcode1901::kOpcode) {
        return mutate_socket(message, true, nextFamily4Version, response, written, outcome);
    }

    if (message.opcode == middleware::web_service::messages::opcode205::kOpcode) {
        const auto investment = state::investment_snapshot();
        return middleware::web_service::messages::opcode205::encode_response(
                   message, investment, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode503::kOpcode) {
        middleware::web_service::messages::opcode503::Request bootstrap;
        const bool parsed =
            middleware::web_service::messages::opcode503::parse_request(message, bootstrap);
        // The request's own key is echoed and adopted. An authored id here costs the ship and the
        // banner.
        if (!bootstrap.hasPrimarySoid) {
            bootstrap.primarySoid = state::account_snapshot().primarySoid;
        }
        const auto investment = state::investment_snapshot();
        if (!parsed
            || !middleware::web_service::messages::opcode503::encode_response(
                message, bootstrap, investment, response, written)) {
            return encode_echo(message, response, written);
        }
        if (bootstrap.hasPrimarySoid && !state::set_primary_soid(bootstrap.primarySoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws503 stage=adopt result=fail");
        }
        return true;
    }

    if (message.opcode == middleware::web_service::messages::opcode501::kOpcode) {
        // Keep the raw capture even after decoding so new creator layouts remain diagnosable.
        capture_create_character(message);
        middleware::web_service::messages::opcode501::DecodedRequest decoded{};
        if (!middleware::web_service::messages::opcode501::decode_request(message, decoded)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws501 stage=decode result=fail");
            const std::uint64_t fallback =
                state::account::selected_character_soid(state::account_snapshot());
            return middleware::web_service::messages::opcode501::encode_response(
                       message, fallback, response, written)
                   || encode_echo(message, response, written);
        }

        state::NativeCharacterCreation creation{};
        creation.race = static_cast<state::CharacterRace>(decoded.race);
        creation.gender = static_cast<state::CharacterGender>(decoded.gender);
        creation.characterClass = static_cast<state::CharacterClass>(decoded.characterClass);
        creation.presentationHeader = decoded.presentationHeader;
        creation.creationHeader = decoded.creationHeader;
        creation.creationTail = decoded.creationTail;
        // Preserve the native first-character completion path for later campaign/onboarding
        // work. Creating the first real Guardian is valid, but auto-selecting it immediately makes
        // the Client enter a post-creation transition Sunrise does not implement yet. Additional
        // characters keep the measured create -> select behavior.
        const bool firstRealCharacter = state::account_snapshot().characterCount == 0;
        std::size_t createdIndex = state::kCharacterCapacity;
        const state::CharacterMutationResult createResult =
            state::create_character_native(creation, createdIndex);
        if (createResult != state::CharacterMutationResult::ok) {
            std::array<char, kCreateLineCapacity> line{};
            const int count = std::snprintf(line.data(),
                                            line.size(),
                                            "ev=ws501 stage=create result=fail reason=%s race=%u gender=%u class=%u",
                                            state::character_mutation_name(createResult),
                                            static_cast<unsigned>(decoded.race),
                                            static_cast<unsigned>(decoded.gender),
                                            static_cast<unsigned>(decoded.characterClass));
            if (count > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(count)});
            }
            const std::uint64_t fallback =
                state::account::selected_character_soid(state::account_snapshot());
            return middleware::web_service::messages::opcode501::encode_response(
                       message, fallback, response, written)
                   || encode_echo(message, response, written);
        }

        const state::AccountState account = state::account_snapshot();
        if (createdIndex >= account.characterCount) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws501 stage=create result=fail reason=stale_index");
            return encode_echo(message, response, written);
        }
        const std::uint64_t characterSoid = account.characters[createdIndex].soid;
        if (firstRealCharacter) {
            // Keep the new Guardian unselected. The normal Queuez creation publication can make
            // the real one-character roster visible, but it must not trigger selection/orbit or
            // the unimplemented first-character campaign transition.
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             "ev=ws501 stage=select_new result=skip reason=first_character");
        } else {
            bool selectionChanged = false;
            if (state::set_selected_character(characterSoid, selectionChanged)) {
                outcome.hasSelectedCharacter = true;
                outcome.selectedCharacterSoid = characterSoid;
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 "ev=ws501 stage=select_new result=ok");
            } else {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws501 stage=select_new result=fail");
            }
        }
        std::array<char, kCreateLineCapacity> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=ws501 stage=create result=ok index=%zu soid=0x%llX race=%u gender=%u class=%u",
                                        createdIndex,
                                        static_cast<unsigned long long>(characterSoid),
                                        static_cast<unsigned>(decoded.race),
                                        static_cast<unsigned>(decoded.gender),
                                        static_cast<unsigned>(decoded.characterClass));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        if (!middleware::web_service::messages::opcode501::encode_response(
                message, characterSoid, response, written)) {
            return encode_echo(message, response, written);
        }
        // A successful create response is itself a stable mutation boundary. Persist the native
        // creator blocks now so a later post-create Queuez/transition failure cannot discard the
        // newly authored head/face data. Runtime State remains authoritative if either disk write
        // fails.
        const bool checkpointed = state::checkpoint_characters();
        core::log::write(core::log::Channel::server,
                         checkpointed ? core::log::Level::info : core::log::Level::warn,
                         checkpointed
                             ? "ev=ws501 stage=create_checkpoint result=ok"
                             : "ev=ws501 stage=create_checkpoint result=fail");
        // Queuez publication is connection-owned and therefore cannot happen inside the generic
        // web-service handler. Hand the newly created key back to the BAP transaction so that peer
        // can add only the new item residents and refresh Family 3 after the correlated reply.
        outcome.hasCreatedCharacter = true;
        outcome.createdCharacterSoid = characterSoid;
        return true;
    }

    if (message.opcode == middleware::web_service::messages::opcode601::kOpcode) {
        return middleware::web_service::messages::opcode601::encode_response(
                   message, response, written)
               || encode_echo(message, response, written);
    }

    // A subscribe whose body does not parse is still answered; only the subscription is dropped.
    middleware::queuez::Subscription subscription;
    const bool subscribes =
        message.opcode == middleware::web_service::messages::opcode206::kOpcode
        && middleware::web_service::messages::opcode206::parse_request(message, subscription);

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    if (!middleware::web_service::encode_response(
            message, shape, middleware::web_service::StatusResponse{}, response, written)) {
        return encode_echo(message, response, written);
    }
    if (subscribes) {
        // Publish the subscription only after its correlated response is complete.
        outcome.hasSubscription = true;
        outcome.subscription = subscription;
        return true;
    }
    if (message.opcode == middleware::web_service::messages::opcode504::kOpcode) {
        // The selection is State, not a response field, so it publishes after the reply encodes.
        select_character(message, outcome);
    }
    return true;
}

/** Compatibility path used when no resident Queuez version is available. */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome) noexcept {
    return consume(request,
                   response,
                   written,
                   outcome,
                   (std::numeric_limits<std::int32_t>::min)());
}

} // namespace sunrise::server::web_service
