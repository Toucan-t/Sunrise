#pragma once

#include <cstdint>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode502 {

/** Web Service opcode used by the character-select Delete Character action. */
inline constexpr std::uint16_t kOpcode = 502;

/** Stable character identity carried by one delete request. */
struct Request {
    std::uint64_t characterSoid{};
};

/** Parses the bare 64-bit character identity used by the native delete action. */
[[nodiscard]] bool parse_request(const Message& message, Request& request) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode502
