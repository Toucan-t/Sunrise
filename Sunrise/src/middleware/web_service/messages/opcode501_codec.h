#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../web_service_envelope.h"

namespace sunrise::middleware::web_service::messages::opcode501 {

/** Web Service opcode for the create-character request. */
inline constexpr std::uint16_t kOpcode = 501;
/** Bounded diagnostic copy retained while the request layout is being mapped. */
inline constexpr std::size_t kCaptureCapacity = 512;

/** Observed Shadowkeep create-character request shape; still diagnostic until field semantics are proven. */
inline constexpr std::size_t kDiagnosticWireSize = 97;
inline constexpr std::uint8_t kDiagnosticPrefixWidth = 3;
inline constexpr std::size_t kDiagnosticU16Count = 14;
inline constexpr std::size_t kDiagnosticU32Count = 17;
inline constexpr std::uint8_t kDiagnosticPaddingWidth = 5;
/** Confirmed creator identity masks from Human/Awoken/Exo and all three class captures. */
inline constexpr std::uint16_t kIdentityFixedBits = 0x0301U;
inline constexpr std::uint16_t kRaceMask = 0x0C00U;
inline constexpr std::uint16_t kGenderMask = 0x0002U;
inline constexpr std::uint16_t kClassLowByte = 0x0080U;
inline constexpr std::uint8_t kObservedPrefix = 6;
static_assert(kDiagnosticPrefixWidth + kDiagnosticU16Count * 16 + kDiagnosticU32Count * 32
              + kDiagnosticPaddingWidth
              == kDiagnosticWireSize * 8);

/** Exact bytes of one bit-packed native create request, before field interpretation. */
struct RequestCapture {
    std::array<std::byte, kCaptureCapacity> bytes{};
    std::size_t size{};
    std::size_t wireSize{};
    bool truncated{};
};

/** Mechanical split of the one 97-byte request shape observed in native creator captures. */

/** Decoded stable identity plus losslessly preserved native creator blocks. */
struct DecodedRequest {
    std::uint8_t race{};
    std::uint8_t gender{};
    std::uint8_t characterClass{};
    std::array<std::byte, 36> presentationHeader{};
    std::array<std::byte, 36> creationHeader{};
    std::array<std::byte, 24> creationTail{};
};

struct DiagnosticLayout {
    std::uint8_t prefix{};
    std::array<std::uint16_t, kDiagnosticU16Count> fields16{};
    std::array<std::uint32_t, kDiagnosticU32Count> fields32{};
    std::uint8_t padding{};
};

/**
 * Captures the native request body without guessing its descriptor layout.
 * @param message Parsed opcode-501 envelope.
 * @param capture Receives up to kCaptureCapacity exact payload bytes and the full wire size.
 * @return True when the opcode matches and the request contains a payload.
 */
[[nodiscard]] bool capture_request(const Message& message, RequestCapture& capture) noexcept;

/**
 * Splits the currently observed 97-byte request shape without assigning semantics to any field.
 * Requests with any other size are deliberately left raw rather than guessed.
 */
[[nodiscard]] bool parse_diagnostic_layout(std::span<const std::byte> payload,
                                           DiagnosticLayout& layout) noexcept;

/** Decodes the confirmed identity and native-width blocks from the observed 97-byte request. */
[[nodiscard]] bool decode_request(const Message& message, DecodedRequest& request) noexcept;

/**
 * Encodes the create-character response: the status pair then the character object id.
 * @param message Parsed request whose envelope fields are echoed.
 * @param characterSoid Character object id; must be published in family 3.
 * @param output Caller-owned svc-11 response-body storage.
 * @param written Receives encoded response-body bytes.
 * @return True when the fixed response fits the output buffer.
 */
[[nodiscard]] bool encode_response(const Message& message,
                                   std::uint64_t characterSoid,
                                   std::span<std::byte> output,
                                   std::size_t& written) noexcept;

} // namespace sunrise::middleware::web_service::messages::opcode501
