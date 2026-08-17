#include <cstddef>

#include "../../encoding/bit_reader.h"
#include "opcode1901.h"

namespace sunrise::middleware::web_service::messages::opcode1901 {
namespace {
constexpr std::size_t kPayloadSize = 24;
constexpr std::uint8_t kReplacementCountWidth = 4;
constexpr std::uint64_t kCanonicalReplacementCount = 1;
constexpr std::uint8_t kDefinitionIndexWidth = 15;
constexpr std::uint8_t kCanonicalSocketKindWidth = 8;
constexpr std::uint8_t kModelSocketKindWidth = 2;
constexpr std::uint8_t kSocketIndexWidth = 32;
constexpr std::uint8_t kOptionalIdentityWidth = 64;
/** Shadowkeep leaves one legacy optional-identity marker clear before the raw equipment selector. */
constexpr std::uint8_t kLegacySelectorMarkerWidth = 1;
constexpr std::uint64_t kCanonicalSocketKindBias = 0x80ULL;
constexpr std::uint64_t kSocketIndexBias = 0x80000000ULL;
constexpr std::uint64_t kModelSocketKindBias = 1;
}

bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t replacementCount = 0;
    std::uint64_t plugDefinitionPresent = 0;
    std::uint64_t encodedPlugDefinition = 0;
    std::uint64_t encodedCanonicalSocketKind = 0;
    std::uint64_t modelSocketKind = 0;
    std::uint64_t encodedSocketIndex = 0;
    std::uint64_t auxiliaryPresent = 0;
    std::uint64_t legacySelectorMarker = 0;
    if (!reader.read(kReplacementCountWidth, replacementCount)
        || !reader.read(1, plugDefinitionPresent)
        || !reader.read(kDefinitionIndexWidth, encodedPlugDefinition)
        || !reader.read(kCanonicalSocketKindWidth, encodedCanonicalSocketKind)
        || !reader.read(kModelSocketKindWidth, modelSocketKind)
        || !reader.read(kSocketIndexWidth, encodedSocketIndex) || !reader.read(1, auxiliaryPresent)
        || !reader.read(kOptionalIdentityWidth, request.auxiliary)
        || !reader.read(kLegacySelectorMarkerWidth, legacySelectorMarker)
        || !reader.read(kOptionalIdentityWidth, request.equipmentSelector)
        || reader.remaining_bits() != 0 || replacementCount != kCanonicalReplacementCount
        || plugDefinitionPresent == 0 || auxiliaryPresent == 0 || legacySelectorMarker != 0
        || request.equipmentSelector == 0
        || encodedCanonicalSocketKind < kCanonicalSocketKindBias
        || modelSocketKind < kModelSocketKindBias || encodedSocketIndex < kSocketIndexBias) {
        request = {};
        return false;
    }
    request.plugDefinitionIndex = static_cast<std::uint16_t>(encodedPlugDefinition);
    request.canonicalSocketKind =
        static_cast<std::uint8_t>(encodedCanonicalSocketKind - kCanonicalSocketKindBias);
    request.modelSocketKind = static_cast<std::uint8_t>(modelSocketKind - kModelSocketKindBias);
    request.socketIndex = static_cast<std::uint32_t>(encodedSocketIndex - kSocketIndexBias);
    return request.canonicalSocketKind == request.socketIndex;
}
} // namespace sunrise::middleware::web_service::messages::opcode1901
