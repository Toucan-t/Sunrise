#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../encoding/bit_reader.h"
#include "../../encoding/bit_writer.h"
#include "../descriptor/join_descriptor.h"

namespace sunrise::middleware::gameplay::group {

/**
 * Registry ids of the host-migration and election messages. Transport authority, not gameplay.
 * A valid inbound body is still read: an unread one leaves the peer's election state and this
 * host's disagreeing, and an echoed one can leave two hosts believing they won.
 */
enum class MigrationMessageId : std::uint8_t {
    hostHandoff = 19,
    peerHandoff = 20,
    hostTransition = 21,
    hostReestablish = 22,
    reestablishPending = 23,
    hostDecline = 24,
    peerReestablish = 25,
    election = 27,
    electionRefuse = 28,
};

/** Declared decoded sizes the registry holds for those ids. */
inline constexpr std::uint32_t kHostHandoffSize = 100;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kPeerHandoffSize = 100;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kHostTransitionSize = 16;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kHostReestablishSize = 136;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kReestablishPendingSize = 8;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kHostDeclineSize = 98;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kPeerReestablishSize = 8;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kElectionSize = 2992;
/** See kHostHandoffSize. */
inline constexpr std::uint32_t kElectionRefuseSize = 100;
/** First opaque trailing region in a decoded host-reestablish body. */
inline constexpr std::size_t kHostReestablishOpaque16Bytes = 16;
/** Second opaque trailing region in a decoded host-reestablish body. */
inline constexpr std::size_t kHostReestablishOpaque18Bytes = 18;

/** Handoff progress is a percentage and never exceeds this. */
inline constexpr std::uint8_t kMaximumHandoffProgress = 100;
/** A group session membership has at most 32 members, indexed zero through 31. */
inline constexpr std::uint8_t kMaximumHandoffMemberIndex = 31;
/** An election names at most this many candidates. */
inline constexpr std::uint8_t kMaximumCandidates = 32;
/** Refuse codes run from one through ten. */
inline constexpr std::uint8_t kMinimumRefuseCode = 1;
/** See kMinimumRefuseCode. */
inline constexpr std::uint8_t kMaximumRefuseCode = 10;

/** Body of a host handoff and of the acknowledgement that mirrors it. */
struct HostHandoff {
    std::uint64_t sessionId{};
    std::array<std::byte, descriptor::kNetAddrSize> successorAddress{};
    /** Member index the old host nominates. */
    std::uint8_t successorIndex{};
};

/** Body of a host transition report. */
struct HostTransition {
    std::uint64_t sessionId{};
    /** Recovered seven-bit count. The client rejects values above 100. */
    std::uint8_t progress{};
    /** Recovered opaque 32-bit value. Do not assign semantics without a capture. */
    std::uint32_t transitionToken{};
};

/**
 * Body of a host reestablishment.
 * The two trailing regions are preserved byte-for-byte for diagnostics. Their ownership and
 * semantics remain unresolved, so callers must not treat them as keys, tokens, or session ids
 * without a matching native capture.
 */
struct HostReestablish {
    std::uint64_t sessionId{};
    std::uint64_t machineId{};
    std::array<std::byte, descriptor::kNetAddrSize> address{};
    /** First opaque trailing region in the decoded 136-byte body. */
    std::array<std::byte, kHostReestablishOpaque16Bytes> opaque16{};
    /** Second opaque trailing region in the decoded 136-byte body. */
    std::array<std::byte, kHostReestablishOpaque18Bytes> opaque18{};
};

/** Body of a host decline. Everything after the session is optional. */
struct HostDecline {
    std::uint64_t sessionId{};
    std::array<std::byte, descriptor::kNetAddrSize> address{};
    bool hasDeclineData{};
    bool declineFlag{};
    bool hasAddress{};
};

/**
 * The recovered prefix of an election.
 * The per-candidate value width is not recovered, so the fields behind the candidate addresses
 * are one bounded tail rather than a sequence this parser can locate.
 */
struct Election {
    std::uint64_t sessionId{};
    std::array<std::byte, descriptor::kNetAddrSize> previousHost{};
    std::uint8_t candidateCount{};
    /** Bits left after the candidate addresses. Their grammar is unresolved. */
    std::uint32_t tailBits{};
};

/** Body of an election refusal. */
struct ElectionRefuse {
    std::uint64_t sessionId{};
    std::array<std::byte, descriptor::kNetAddrSize> candidateAddress{};
    std::uint8_t refuseCode{};
    bool hasCandidateAddress{};
};

/** Reads a host-handoff or peer-handoff body. @return True when every field was present. */
[[nodiscard]] bool read_host_handoff(encoding::bits::Reader& reader, HostHandoff& output) noexcept;

/** Writes a host-handoff or peer-handoff body. @return True when every field fit. */
[[nodiscard]] bool write_host_handoff(encoding::bits::Writer& writer,
                                      const HostHandoff& body) noexcept;

/** Reads a host-transition body. @return True when every field was present and in range. */
[[nodiscard]] bool read_host_transition(encoding::bits::Reader& reader,
                                        HostTransition& output) noexcept;

/** Writes a host-transition body. @return True when every field fit and was in range. */
[[nodiscard]] bool write_host_transition(encoding::bits::Writer& writer,
                                         const HostTransition& body) noexcept;

/** Reads a host-reestablish body. @return True when every field was present. */
[[nodiscard]] bool read_host_reestablish(encoding::bits::Reader& reader,
                                         HostReestablish& output) noexcept;

/**
 * Writes host-reestablish from the decoded 128-byte join descriptor. The decoded NetAddr is
 * converted to its native variable-width wire form while both opaque identity regions are
 * preserved byte-for-byte.
 */
[[nodiscard]] bool write_host_reestablish(
    encoding::bits::Writer& writer,
    std::uint64_t sessionId,
    const std::array<std::byte, descriptor::kDescriptorSize>& descriptorBytes) noexcept;

/** Reads a session-only migration body, shared by ids 23 and 25. */
[[nodiscard]] bool read_migration_session(encoding::bits::Reader& reader,
                                          std::uint64_t& sessionId) noexcept;

/** Writes a session-only migration body. */
[[nodiscard]] bool write_migration_session(encoding::bits::Writer& writer,
                                           std::uint64_t sessionId) noexcept;

/** Reads a host-decline body. @return True when the selected form was complete. */
[[nodiscard]] bool read_host_decline(encoding::bits::Reader& reader, HostDecline& output) noexcept;

/**
 * Reads an election as far as its candidate addresses.
 * @param reader Reader positioned at the body.
 * @param output Receives the prefix and the size of the unread tail.
 * @return True when the prefix was present and the candidate count is in range.
 */
[[nodiscard]] bool read_election(encoding::bits::Reader& reader, Election& output) noexcept;

/** Reads an election refusal. @return True when the selected form was complete and in range. */
[[nodiscard]] bool read_election_refuse(encoding::bits::Reader& reader,
                                        ElectionRefuse& output) noexcept;

} // namespace sunrise::middleware::gameplay::group
