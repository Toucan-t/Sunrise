#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode1901 {
inline constexpr std::uint16_t kOpcode = 1901;
struct Request {
    std::uint16_t plugDefinitionIndex{};
    std::uint8_t canonicalSocketKind{};
    std::uint8_t modelSocketKind{};
    std::uint32_t socketIndex{};
    std::uint64_t auxiliary{};
    std::uint64_t equipmentSelector{};
};
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;
} // namespace sunrise::middleware::web_service::messages::opcode1901
