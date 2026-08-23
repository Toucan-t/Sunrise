/**
 * Host-migration and election bodies. Each one is read into typed fields and nothing more: this
 * host keeps a stable host, and an answer built from a half-read election can leave two peers each
 * believing they carry the group.
 */

#include "migration_messages.h"

#include <algorithm>
#include <array>
#include <span>

#include "../../encoding/bit_raw.h"

namespace sunrise::middleware::gameplay::group {

namespace {

namespace bits = encoding::bits;

/** The successor member index is six bits. */
constexpr std::uint8_t kSuccessorIndexWidth = 6;
/** Handoff progress is seven bits, so the wire can declare more than a percentage. */
constexpr std::uint8_t kProgressWidth = 7;
/** The transition token is a raw 32-bit field. */
constexpr std::size_t kTransitionTokenBytes = 4;
/** Every optional field is introduced by one presence bit. */
constexpr std::uint8_t kPresenceWidth = 1;
/** The candidate count is six bits, so the wire can declare more than the bound allows. */
constexpr std::uint8_t kCandidateCountWidth = 6;
/** The refuse code is four bits. */
constexpr std::uint8_t kRefuseCodeWidth = 4;
/** Raw byte fields are read as whole bytes. */
constexpr std::uint8_t kBitsPerByte = 8;
/** Mask of one raw byte. */
constexpr std::uint32_t kByteMask = 0xFFU;
/** Width of the transport-method selector leading every serialized NetAddr. */
constexpr std::uint8_t kNetAddrMethodWidth = 3;
/** Direct NetAddr methods 0 through 5 serialize the first 41 decoded bytes. */
constexpr std::size_t kShortNetAddrBytes = 41;
/** Relay NetAddr methods 6 and 7 serialize the first 85 decoded bytes. */
constexpr std::size_t kLongNetAddrBytes = 85;
/** First method selecting the long NetAddr form. */
constexpr std::uint8_t kFirstLongNetAddrMethod = 6;
/** The method lives in the last byte of the decoded 86-byte NetAddr. */
constexpr std::size_t kNetAddrMethodOffset = descriptor::kNetAddrSize - 1;
/** Decoded join-descriptor field boundaries reused by host-reestablish. */
constexpr std::size_t kDescriptorMachineBytes = 8;
constexpr std::size_t kDescriptorNetAddrOffset = kDescriptorMachineBytes;
constexpr std::size_t kDescriptorIdentity128Offset =
    kDescriptorNetAddrOffset + descriptor::kNetAddrSize;
constexpr std::size_t kDescriptorIdentity128Bytes = 16;
constexpr std::size_t kDescriptorIdentity144Offset =
    kDescriptorIdentity128Offset + kDescriptorIdentity128Bytes;
constexpr std::size_t kDescriptorIdentity144Bytes = 18;
static_assert(kDescriptorIdentity144Offset + kDescriptorIdentity144Bytes
              == descriptor::kDescriptorSize);

/** Writes the native variable-width NetAddr form used by migration messages. */
[[nodiscard]] bool write_serialized_net_addr(
    bits::Writer& writer,
    const std::array<std::byte, descriptor::kNetAddrSize>& address) noexcept {
    const auto method = static_cast<std::uint8_t>(address[kNetAddrMethodOffset]);
    if (method >= (std::uint8_t{1} << kNetAddrMethodWidth)) {
        return false;
    }
    const std::size_t addressBytes =
        method >= kFirstLongNetAddrMethod ? kLongNetAddrBytes : kShortNetAddrBytes;
    return writer.write(method, kNetAddrMethodWidth)
           && bits::write_raw(writer, std::span(address).first(addressBytes));
}

/** Reads one native variable-width NetAddr into its decoded 86-byte representation. */
[[nodiscard]] bool read_serialized_net_addr(
    bits::Reader& reader,
    std::array<std::byte, descriptor::kNetAddrSize>& address) noexcept {
    std::uint64_t methodValue = 0;
    if (!reader.read(kNetAddrMethodWidth, methodValue)) {
        return false;
    }
    const auto method = static_cast<std::uint8_t>(methodValue);
    const std::size_t addressBytes =
        method >= kFirstLongNetAddrMethod ? kLongNetAddrBytes : kShortNetAddrBytes;
    std::array<std::byte, descriptor::kNetAddrSize> candidate{};
    if (!bits::read_raw(reader, std::span(candidate).first(addressBytes))) {
        return false;
    }
    candidate[kNetAddrMethodOffset] = static_cast<std::byte>(method);
    address = candidate;
    return true;
}

} // namespace

/** Reads a host-handoff or peer-handoff body. */
bool read_host_handoff(bits::Reader& reader, HostHandoff& output) noexcept {
    HostHandoff candidate{};
    std::uint64_t index = 0;
    // Native wire order is session -> six-bit member index -> variable-width NetAddr.
    // The 100-byte registry size describes the decoded C struct, not 100 raw wire bytes.
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !reader.read(kSuccessorIndexWidth, index)
        || !read_serialized_net_addr(reader, candidate.successorAddress)) {
        return false;
    }
    if (index > kMaximumHandoffMemberIndex) {
        return false;
    }
    candidate.successorIndex = static_cast<std::uint8_t>(index);
    output = candidate;
    return true;
}

/** Writes a host-handoff or peer-handoff body. */
bool write_host_handoff(bits::Writer& writer, const HostHandoff& body) noexcept {
    if (body.successorIndex > kMaximumHandoffMemberIndex) {
        return false;
    }
    // Keep this in the exact native writer order. Writing the decoded 86-byte NetAddr first makes
    // the retail reader consume its leading address bits as the six-bit successor index.
    return bits::write_raw_u64(writer, body.sessionId)
           && writer.write(body.successorIndex, kSuccessorIndexWidth)
           && write_serialized_net_addr(writer, body.successorAddress);
}

/** Reads a host-transition body. */
bool read_host_transition(bits::Reader& reader, HostTransition& output) noexcept {
    HostTransition candidate{};
    std::uint64_t progress = 0;
    std::array<std::byte, kTransitionTokenBytes> token{};
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kProgressWidth, progress)
        || !bits::read_raw(reader, token)) {
        return false;
    }
    if (progress > kMaximumHandoffProgress) {
        return false;
    }
    candidate.progress = static_cast<std::uint8_t>(progress);
    for (std::size_t index = 0; index < token.size(); ++index) {
        candidate.transitionToken |= std::to_integer<std::uint32_t>(token[index])
                                     << (static_cast<unsigned>(index) * kBitsPerByte);
    }
    output = candidate;
    return true;
}

/** Writes a host-transition body. */
bool write_host_transition(bits::Writer& writer, const HostTransition& body) noexcept {
    if (body.progress > kMaximumHandoffProgress) {
        return false;
    }
    std::array<std::byte, kTransitionTokenBytes> token{};
    for (std::size_t index = 0; index < token.size(); ++index) {
        const unsigned shift = static_cast<unsigned>(index) * kBitsPerByte;
        token[index] = static_cast<std::byte>((body.transitionToken >> shift) & kByteMask);
    }
    return bits::write_raw_u64(writer, body.sessionId)
           && writer.write(body.progress, kProgressWidth) && bits::write_raw(writer, token);
}

/** Reads a host-reestablish body. */
bool read_host_reestablish(bits::Reader& reader, HostReestablish& output) noexcept {
    HostReestablish candidate{};
    // The registry exposes a 136-byte decoded struct, but NetAddr is variable-width on the wire.
    // Preserve both trailing regions exactly. We need the bytes to correlate the native migration
    // identity with the client's security-map diagnostics, but we deliberately assign no semantics.
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw_u64(reader, candidate.machineId)
        || !read_serialized_net_addr(reader, candidate.address)
        || !bits::read_raw(reader, candidate.opaque16)
        || !bits::read_raw(reader, candidate.opaque18)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Writes host-reestablish from the decoded 128-byte join-descriptor image. */
bool write_host_reestablish(
    bits::Writer& writer,
    std::uint64_t sessionId,
    const std::array<std::byte, descriptor::kDescriptorSize>& descriptorBytes) noexcept {
    std::array<std::byte, descriptor::kNetAddrSize> address{};
    std::copy_n(descriptorBytes.begin() + static_cast<std::ptrdiff_t>(kDescriptorNetAddrOffset),
                descriptor::kNetAddrSize,
                address.begin());

    // Descriptor layout mirrors the decoded host-reestablish tail: machine, decoded NetAddr,
    // 16-byte identity, then 18-byte identity. Preserve those opaque blocks exactly while using
    // the native variable-width NetAddr encoder between them.
    const auto machine = std::span(descriptorBytes).first(kDescriptorMachineBytes);
    const auto identity128 =
        std::span(descriptorBytes).subspan(kDescriptorIdentity128Offset, kDescriptorIdentity128Bytes);
    const auto identity144 =
        std::span(descriptorBytes).subspan(kDescriptorIdentity144Offset, kDescriptorIdentity144Bytes);
    return bits::write_raw_u64(writer, sessionId) && bits::write_raw(writer, machine)
           && write_serialized_net_addr(writer, address)
           && bits::write_raw(writer, identity128) && bits::write_raw(writer, identity144);
}

/** Reads a session-only migration body. */
bool read_migration_session(bits::Reader& reader, std::uint64_t& sessionId) noexcept {
    sessionId = 0;
    return bits::read_raw_u64(reader, sessionId);
}

/** Writes a session-only migration body. */
bool write_migration_session(bits::Writer& writer, std::uint64_t sessionId) noexcept {
    return bits::write_raw_u64(writer, sessionId);
}

/** Reads a host-decline body. */
bool read_host_decline(bits::Reader& reader, HostDecline& output) noexcept {
    HostDecline candidate{};
    std::uint64_t present = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kPresenceWidth, present)) {
        return false;
    }
    candidate.hasDeclineData = present != 0;
    if (!candidate.hasDeclineData) {
        output = candidate;
        return true;
    }
    std::uint64_t flag = 0;
    std::uint64_t hasAddress = 0;
    if (!reader.read(kPresenceWidth, flag) || !reader.read(kPresenceWidth, hasAddress)) {
        return false;
    }
    candidate.declineFlag = flag != 0;
    candidate.hasAddress = hasAddress != 0;
    if (candidate.hasAddress && !bits::read_raw(reader, candidate.address)) {
        return false;
    }
    output = candidate;
    return true;
}

/** Reads an election as far as its candidate addresses. */
bool read_election(bits::Reader& reader, Election& output) noexcept {
    Election candidate{};
    std::uint64_t count = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId)
        || !bits::read_raw(reader, candidate.previousHost)
        || !reader.read(kCandidateCountWidth, count)) {
        return false;
    }
    if (count > kMaximumCandidates) {
        return false;
    }
    candidate.candidateCount = static_cast<std::uint8_t>(count);
    for (std::uint8_t index = 0; index < candidate.candidateCount; ++index) {
        if (!bits::skip_raw(reader, descriptor::kNetAddrSize)) {
            return false;
        }
    }
    // The per-candidate value width is unrecovered, so the three bitsets behind it cannot be
    // located and the rest of the body stays one bounded region.
    candidate.tailBits = static_cast<std::uint32_t>(reader.remaining_bits());
    output = candidate;
    return true;
}

/** Reads an election refusal. */
bool read_election_refuse(bits::Reader& reader, ElectionRefuse& output) noexcept {
    ElectionRefuse candidate{};
    std::uint64_t code = 0;
    std::uint64_t present = 0;
    if (!bits::read_raw_u64(reader, candidate.sessionId) || !reader.read(kRefuseCodeWidth, code)
        || !reader.read(kPresenceWidth, present)) {
        return false;
    }
    if (code < kMinimumRefuseCode || code > kMaximumRefuseCode) {
        return false;
    }
    candidate.refuseCode = static_cast<std::uint8_t>(code);
    candidate.hasCandidateAddress = present != 0;
    if (candidate.hasCandidateAddress && !bits::read_raw(reader, candidate.candidateAddress)) {
        return false;
    }
    output = candidate;
    return true;
}

} // namespace sunrise::middleware::gameplay::group
