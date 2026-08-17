#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode903 {
inline constexpr std::uint16_t kOpcode = 903;
struct Request {
    std::uint64_t instanceSoid{};
    std::uint32_t socketIndex{};
    std::uint16_t targetDefinitionIndex{};
    std::uint16_t plugDefinitionIndex{};
    bool hasInstance{};
    bool hasTargetDefinition{};
    bool hasPlugDefinition{};
};
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;
} // namespace sunrise::middleware::web_service::messages::opcode903
