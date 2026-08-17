#include <cstddef>
#include <limits>

#include "../../encoding/bit_reader.h"
#include "opcode903.h"

namespace sunrise::middleware::web_service::messages::opcode903 {
namespace {
constexpr std::size_t kPayloadSize = 18;
constexpr std::uint8_t kInstanceWidth = 64;
constexpr std::uint8_t kDefinitionIndexWidth = 15;
constexpr std::uint8_t kSocketIndexWidth = 32;
constexpr std::uint8_t kInnerPaddingWidth = 7;
constexpr std::uint8_t kOuterTrailerWidth = 2;
constexpr std::uint8_t kFinalPaddingWidth = 6;
constexpr std::uint64_t kSocketIndexBias = 0x80000000ULL;
constexpr std::uint64_t kAbsentDefinitionTail = 0x7FFFULL;
}

bool parse_request(const Message& message, Request& request) noexcept {
    request = {};
    if (message.opcode != kOpcode || message.payload.size() != kPayloadSize) {
        return false;
    }
    encoding::bits::Reader reader(message.payload);
    std::uint64_t instancePresent = 0;
    std::uint64_t targetDefinitionPresent = 0;
    std::uint64_t encodedTargetDefinition = 0;
    std::uint64_t encodedSocketIndex = 0;
    std::uint64_t plugDefinitionPresent = 0;
    std::uint64_t encodedPlugDefinition = 0;
    std::uint64_t innerPadding = 0;
    std::uint64_t outerTrailer = 0;
    std::uint64_t finalPadding = 0;
    if (!reader.read(1, instancePresent) || !reader.read(kInstanceWidth, request.instanceSoid)
        || !reader.read(1, targetDefinitionPresent)
        || !reader.read(kDefinitionIndexWidth, encodedTargetDefinition)
        || !reader.read(kSocketIndexWidth, encodedSocketIndex)
        || !reader.read(1, plugDefinitionPresent)
        || !reader.read(kDefinitionIndexWidth, encodedPlugDefinition)
        || !reader.read(kInnerPaddingWidth, innerPadding)
        || !reader.read(kOuterTrailerWidth, outerTrailer)
        || !reader.read(kFinalPaddingWidth, finalPadding) || reader.remaining_bits() != 0
        || innerPadding != 0 || outerTrailer != 0 || finalPadding != 0
        || encodedSocketIndex < kSocketIndexBias
        || encodedSocketIndex - kSocketIndexBias > (std::numeric_limits<std::uint32_t>::max)()) {
        request = {};
        return false;
    }
    request.hasInstance = instancePresent != 0;
    request.hasTargetDefinition = targetDefinitionPresent != 0;
    request.hasPlugDefinition = plugDefinitionPresent != 0;
    request.targetDefinitionIndex = static_cast<std::uint16_t>(encodedTargetDefinition);
    request.plugDefinitionIndex = static_cast<std::uint16_t>(encodedPlugDefinition);
    request.socketIndex = static_cast<std::uint32_t>(encodedSocketIndex - kSocketIndexBias);
    if ((!request.hasTargetDefinition && encodedTargetDefinition != kAbsentDefinitionTail)
        || (!request.hasPlugDefinition && encodedPlugDefinition != kAbsentDefinitionTail)) {
        request = {};
        return false;
    }
    return true;
}
} // namespace sunrise::middleware::web_service::messages::opcode903
