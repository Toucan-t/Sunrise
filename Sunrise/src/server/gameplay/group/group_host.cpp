#include "group_host.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

#include "../../../client/hooks/retail_log/retail_log_enqueue_observer.h"
#include "../../../core/settings/settings.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"
#include "../../../middleware/gameplay/group/member_messages.h"
#include "../../../middleware/gameplay/group/migration_messages.h"
#include "../../../middleware/gameplay/group/parameter_messages.h"
#include "../../../middleware/gameplay/group/parameter_registry.h"
#include "../../../middleware/gameplay/group/session_messages.h"
#include "../../../middleware/gameplay/group/session_state.h"
#include "../../../middleware/gameplay/group/view_message.h"
#include "../../../state/activity/runtime.h"
#include "../dtls/dtls_host.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"
#include "../peer/peer_transport.h"
#include "group_host_sessions.h"
#include "group_migration_receipts.h"

namespace sunrise::server::gameplay::group {

namespace {

namespace wire = middleware::gameplay::group;
namespace bits = middleware::encoding::bits;
namespace descriptor = middleware::gameplay::descriptor;

/** One reliable body staged before it is split into fragments. */
constexpr std::size_t kBodyCapacity = 160;
/** A membership snapshot is far larger, and the peer's reliable send queue bounds it. */
constexpr std::size_t kMembershipBodyCapacity = 512;
/** Only the low 25 bitmap bits name a registry parameter. */
constexpr std::uint64_t kParameterMaskBits = 0x1FFFFFF;
/** Room for every registry name plus its separators. */
constexpr std::size_t kParameterNameCapacity = 640;
/** Member index this host takes, and the index it nominates to succeed it. */
constexpr std::uint32_t kHostMemberIndex = 0;
/** Member index the admitted peer takes. */
constexpr std::size_t kPeerMemberIndex = 1;
/** Members one snapshot names: this host and the admitted peer. */
constexpr std::size_t kSnapshotMemberCount = 2;
/** Registry index the join-latch update names. Any of the 25 would do; none is ever filled. */
constexpr std::uint8_t kJoinLatchParameter = 0;
/** Peers this host tracks at once. The public POC admits one. */
constexpr std::size_t kAdmittedCapacity = 4;
/** Loopback address the BAP listener binds, in host order. */
constexpr std::uint32_t kLoopbackAddress = 0x7F000001;
/** Every member index the `activity-host` parameter covers. The peer needs its own bit set. */
constexpr std::uint32_t kAllMembers = 0xFFFFFFFF;
/** Shortest gap between two retries of an owed publish. */
constexpr std::uint64_t kRetryInterval = 250;
/** Player slot the admitted peer's player takes. */
constexpr std::uint32_t kPeerPlayerSlot = 0;
/** Counter the first player of a session carries. The consumer's own add starts here too. */
constexpr std::uint32_t kFirstAddSequence = 0;
/** Same odd stride used by the citizen advertisement for one region-specific identity. */
constexpr std::uint64_t kRegionIdentityStride = 0x9E3779B97F4A7C15ULL;
/** Recovered host-transition count used for the completed handoff edge. */
constexpr std::uint8_t kHostTransitionComplete = wire::kMaximumHandoffProgress;
/** The transition dword remains opaque. Zero is the controlled baseline for this experiment. */
constexpr std::uint32_t kHostTransitionOpaque = 0;

/** Ordered migration stages. Each reliable edge is allowed to dispatch before the next is queued. */
enum class HostMigrationPhase : std::uint8_t {
    idle,
    settling,
    awaitingBaselineReestablish,
    handoffToPeerReady,
    awaitingPeerHandoff,
    transitionReady,
    awaitingNativeHostReestablish,
    peerReestablishReady,
    complete,
};

/** Exact security/session identity accepted from the native kind-22 migration edge. */
struct NativeHostAckReceipt {
    std::uint64_t sessionId{};
    std::uint64_t machineId{};
    std::array<std::byte, wire::kHostReestablishOpaque16Bytes> opaque16{};
    std::array<std::byte, wire::kHostReestablishOpaque18Bytes> opaque18{};
    bool valid{};
    bool nativeSecurityRegistered{};
};

/** One admitted peer and the player it asked this host to add. */
struct Admitted {
    state::gameplay::Endpoint endpoint{};
    std::uint64_t joinId{};
    std::uint64_t playerId{};
    /** Group-session id the peer named in its join request, which its parameters must echo. */
    std::uint64_t sessionId{};
    bool occupied{};
    bool hasPlayer{};
    /** Set once the first ready-state membership snapshot has entered the reliable queue. */
    bool initialMembershipPublished{};
    /** Set once the join-latch parameter update has entered the reliable queue. */
    bool initialParametersPublished{};
    /** Set once the peer reports its join finished, which is what promotes it to `established`. */
    bool joinComplete{};
    /** Set once a snapshot carrying that promotion is on the peer's reliable channel. */
    bool joinPublished{};
    /** Set once the `activity-host` parameter is on the peer's reliable channel. */
    bool activityHostPublished{};
    /** Set once a snapshot naming the peer's player is on that channel. The queue can refuse it. */
    bool playerPublished{};
    /** Current native host-migration publication edge for this group session. */
    HostMigrationPhase migrationPhase{HostMigrationPhase::idle};
    /** Native kind-22 identity whose security registration authorized the final kind-25 ACK. */
    NativeHostAckReceipt nativeHostAckReceipt{};
    /** Earliest service tick at which the next unacknowledged migration edge may be queued. */
    std::uint64_t migrationDue{};
    /** Tick of the last retry, so a full queue is retried on a timer rather than every packet. */
    std::uint64_t lastRetry{};
    /** Order in which the peer last named this session. The lowest is the least recently used. */
    std::uint64_t lastUse{};
};

/**
 * Public group sessions the peer holds at once: one current and one target.
 * The peer resolves a session through a two-element array, so a third is one it left.
 */
constexpr std::size_t kPublicSessionCapacity = 2;

/** Revision of the last published snapshot. The consumer refuses one that does not increase. */
std::atomic<std::uint32_t> g_membershipRevision{0};
/** Stamps `Admitted::lastUse`. It only has to order the records, so it never has to be a clock. */
std::atomic<std::uint64_t> g_admitClock{0};
/** Guards the admitted table against the worker and the callback pump. */
SRWLOCK g_admittedLock{SRWLOCK_INIT};
/** Admitted peers. A join claims a slot and a leave never reclaims one in this POC. */
std::array<Admitted, kAdmittedCapacity> g_admitted{};

/** Member state this host publishes for every member carrying the join id. */
constexpr wire::MemberState kJoinMemberState = wire::MemberState::ready;

// Three peer checks pin this to exactly `ready`. The joining peer's entry must be at least
// `joined`, must not be `established`, and the request waits until every member carrying the
// join id reads `ready`.
static_assert(static_cast<std::uint8_t>(kJoinMemberState)
                  >= static_cast<std::uint8_t>(wire::MemberState::joined),
              "the published member state must clear the peer's own join bar");
static_assert(kJoinMemberState == wire::MemberState::ready,
              "the request advances only when every member carrying the join id reads ready");

/**
 * Sends one reliable group-session message.
 * @param sessionId Group session whose reliable channel carries it.
 * @param id Registry message id.
 * @param declaredSize Decoded structure size the registry declares.
 * @param write Callback that writes the body.
 * @return True when the message was queued.
 */
template <typename Body>
[[nodiscard]] bool send_reliable(std::uint64_t sessionId,
                                 std::uint8_t id,
                                 std::uint32_t declaredSize,
                                 Body write) noexcept {
    std::array<std::byte, kBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!write(writer) || !writer.finish(size)) {
        return false;
    }
    return peer::enqueue_reliable(
        sessionId, id, declaredSize, {body.data(), size}, writer.bit_count());
}

/** @return A stable nonzero region-specific copy of one endpoint identity. */
[[nodiscard]] std::uint64_t region_identity(std::uint64_t base, std::int32_t regionIndex) noexcept {
    const auto region = static_cast<std::uint64_t>(static_cast<std::uint32_t>(regionIndex));
    const std::uint64_t derived = base ^ (kRegionIdentityStride * (region + 1U));
    return derived == 0 ? kRegionIdentityStride : derived;
}

/** Formats one byte array as uppercase hexadecimal without separators. */
template <std::size_t Size>
void format_hex(const std::array<std::byte, Size>& input,
                std::array<char, (Size * 2) + 1>& output) noexcept {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto value = std::to_integer<unsigned>(input[index]);
        output[index * 2] = kDigits[(value >> 4U) & 0xFU];
        output[(index * 2) + 1] = kDigits[value & 0xFU];
    }
    output[Size * 2] = '\0';
}

/** Reads up to eight bytes low-byte-first, mirroring the raw-u64 migration reader. */
template <std::size_t Size>
[[nodiscard]] std::uint64_t read_low_u64(const std::array<std::byte, Size>& input,
                                         std::size_t offset = 0) noexcept {
    std::uint64_t value = 0;
    const std::size_t available = offset < Size ? Size - offset : 0;
    const std::size_t count = available < sizeof(value) ? available : sizeof(value);
    for (std::size_t index = 0; index < count; ++index) {
        value |= std::to_integer<std::uint64_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

/** Recreates the eight raw bytes a little-endian raw-u64 reader consumed. */
[[nodiscard]] std::array<std::byte, sizeof(std::uint64_t)> raw_u64_bytes(
    std::uint64_t value) noexcept {
    std::array<std::byte, sizeof(std::uint64_t)> output{};
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
    return output;
}

/** Human-readable phase labels used only by migration diagnostics. */
[[nodiscard]] const char* migration_phase_name(HostMigrationPhase phase) noexcept {
    switch (phase) {
    case HostMigrationPhase::idle:
        return "idle";
    case HostMigrationPhase::settling:
        return "settling";
    case HostMigrationPhase::awaitingBaselineReestablish:
        return "await_baseline_reestablish";
    case HostMigrationPhase::handoffToPeerReady:
        return "handoff_to_peer_ready";
    case HostMigrationPhase::awaitingPeerHandoff:
        return "await_peer_handoff";
    case HostMigrationPhase::transitionReady:
        return "transition_ready";
    case HostMigrationPhase::awaitingNativeHostReestablish:
        return "await_native_host_reestablish";
    case HostMigrationPhase::peerReestablishReady:
        return "peer_reestablish_ready";
    case HostMigrationPhase::complete:
        return "complete";
    default:
        return "unknown";
    }
}

/**
 * Tests whether a native kind-22 is the exact security identity already accepted for this
 * completed migration. The NetAddr is deliberately excluded: the intentional native resecure can
 * rebuild the channel surface while preserving the accepted session/security identity.
 */
[[nodiscard]] bool same_accepted_native_host(const NativeHostAckReceipt& accepted,
                                             const wire::HostReestablish& receipt) noexcept {
    return accepted.valid && accepted.nativeSecurityRegistered
           && accepted.sessionId == receipt.sessionId && accepted.machineId == receipt.machineId
           && accepted.opaque16 == receipt.opaque16 && accepted.opaque18 == receipt.opaque18;
}

/**
 * Rebuilds the exact 128-byte descriptor image the matching citizen advertisement used.
 * The reestablish writer prefixes only the group session id, preserving the descriptor's 86-byte
 * NetAddr and both opaque identity regions byte-for-byte.
 */
[[nodiscard]] bool build_host_descriptor(
    const Admitted& record,
    std::array<std::byte, descriptor::kDescriptorSize>& output,
    std::uint64_t& onlineSessionId,
    std::int32_t& regionIndex) noexcept {
    HostSessionBinding binding{};
    if (!host_session_for_group(record.sessionId, binding) || binding.regionIndex < 0) {
        return false;
    }
    const endpoint::Identity identity = endpoint::identity();
    const state::gameplay::Endpoint advertised = endpoint::advertised();
    onlineSessionId = region_identity(identity.onlineSessionId, binding.regionIndex);
    regionIndex = binding.regionIndex;

    descriptor::JoinEndpoint join{};
    join.machineId = record.sessionId;
    join.address = advertised.address;
    join.port = advertised.port;
    join.onlineSessionId = onlineSessionId;
    return descriptor::build(join, output);
}

/** Queues one 136-byte host-reestablish body using the proven citizen descriptor image. */
[[nodiscard]] bool publish_host_reestablish(const Admitted& record, const char* phase) noexcept {
    std::array<std::byte, descriptor::kDescriptorSize> descriptorBytes{};
    std::uint64_t onlineSessionId = 0;
    std::int32_t regionIndex = -1;
    if (!build_host_descriptor(record, descriptorBytes, onlineSessionId, regionIndex)) {
        report(core::log::Level::debug,
               "ev=gameplay stage=host_reestablish phase=%s result=deferred session=0x%016llX",
               phase,
               static_cast<unsigned long long>(record.sessionId));
        return false;
    }
    const bool sent = send_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::MigrationMessageId::hostReestablish),
        wire::kHostReestablishSize,
        [&record, &descriptorBytes](bits::Writer& writer) noexcept {
            return wire::write_host_reestablish(writer, record.sessionId, descriptorBytes);
        });
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=host_reestablish phase=%s result=%s session=0x%016llX "
           "machine=0x%016llX region=%d online=0x%016llX bytes=%u",
           phase,
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(record.sessionId),
           static_cast<unsigned long long>(record.sessionId),
           static_cast<int>(regionIndex),
           static_cast<unsigned long long>(onlineSessionId),
           wire::kHostReestablishSize);
    return sent;
}

/** Queues one host-handoff using the exact member NetAddr already published in membership. */
[[nodiscard]] bool publish_host_handoff(const Admitted& record,
                                        std::uint8_t successorIndex,
                                        const char* phase) noexcept {
    wire::HostHandoff body{};
    body.sessionId = record.sessionId;
    body.successorIndex = successorIndex;
    if (successorIndex == kPeerMemberIndex) {
        // A rebuilt endpoint can be logically equal but byte-different. Handoff requires the exact
        // transport-captured peer address used for self-member resolution.
        if (!peer::remote_address(record.sessionId, body.successorAddress)) {
            report(core::log::Level::debug,
                   "ev=gameplay stage=host_handoff phase=%s result=deferred reason=no_peer_addr "
                   "session=0x%016llX successor=%u",
                   phase,
                   static_cast<unsigned long long>(record.sessionId),
                   static_cast<unsigned>(successorIndex));
            return false;
        }
    } else if (successorIndex == kHostMemberIndex) {
        const state::gameplay::Endpoint host = endpoint::advertised();
        descriptor::write_net_addr(host.address, host.port, body.successorAddress);
    } else {
        return false;
    }

    const bool sent = send_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::MigrationMessageId::hostHandoff),
        wire::kHostHandoffSize,
        [&body](bits::Writer& writer) noexcept { return wire::write_host_handoff(writer, body); });
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=host_handoff phase=%s result=%s session=0x%016llX successor=%u "
           "bytes=%u",
           phase,
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(record.sessionId),
           static_cast<unsigned>(successorIndex),
           wire::kHostHandoffSize);
    return sent;
}

/** Queues the recovered host-transition grammar with one controlled opaque dword. */
[[nodiscard]] bool publish_host_transition(const Admitted& record) noexcept {
    wire::HostTransition body{};
    body.sessionId = record.sessionId;
    body.progress = kHostTransitionComplete;
    body.transitionToken = kHostTransitionOpaque;
    const bool sent = send_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::MigrationMessageId::hostTransition),
        wire::kHostTransitionSize,
        [&body](bits::Writer& writer) noexcept {
            return wire::write_host_transition(writer, body);
        });
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=host_transition result=%s session=0x%016llX count=%u "
           "opaque=0x%08X bytes=%u",
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(record.sessionId),
           static_cast<unsigned>(body.progress),
           body.transitionToken,
           wire::kHostTransitionSize);
    return sent;
}

/**
 * Acknowledges the native host-reestablish emitted after the local peer accepts our transition.
 * Kind 25 is session-only; queuing it completes Sunrise's side of this migration handshake.
 */
[[nodiscard]] bool publish_peer_reestablish(const Admitted& record, const char* phase) noexcept {
    const bool sent = send_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::MigrationMessageId::peerReestablish),
        wire::kPeerReestablishSize,
        [&record](bits::Writer& writer) noexcept {
            return wire::write_migration_session(writer, record.sessionId);
        });
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=peer_reestablish phase=%s result=%s "
           "session=0x%016llX bytes=%u",
           phase,
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(record.sessionId),
           wire::kPeerReestablishSize);
    return sent;
}

/**
 * Advances only server-originated migration edges. Receipt-driven edges are changed by consume().
 * The first pulse waits until membership, activityHost, and the local player row are all published.
 */
void service_migration(Admitted& record, std::uint64_t now) noexcept {
    const bool publicationReady = record.joinComplete && record.joinPublished
                                  && record.activityHostPublished && record.hasPlayer
                                  && record.playerPublished;
    if (record.migrationPhase == HostMigrationPhase::idle) {
        if (!publicationReady) {
            return;
        }
        // A newly armed migration owns a new acknowledgement identity. Keep the completed
        // receipt alive through resecure, but never carry it into a later migration transaction.
        record.nativeHostAckReceipt = {};
        record.migrationPhase = HostMigrationPhase::settling;
        record.migrationDue = now + kRetryInterval;
        report(core::log::Level::info,
               "ev=gameplay stage=migration result=armed session=0x%016llX delay=%llu",
               static_cast<unsigned long long>(record.sessionId),
               static_cast<unsigned long long>(kRetryInterval));
        return;
    }
    if (record.migrationPhase == HostMigrationPhase::complete || now < record.migrationDue) {
        return;
    }

    bool queued = false;
    switch (record.migrationPhase) {
    case HostMigrationPhase::settling:
        queued = publish_host_reestablish(record, "baseline");
        if (queued) {
            record.migrationPhase = HostMigrationPhase::awaitingBaselineReestablish;
        }
        break;
    case HostMigrationPhase::handoffToPeerReady:
        queued = publish_host_handoff(
            record, static_cast<std::uint8_t>(kPeerMemberIndex), "to_peer");
        if (queued) {
            record.migrationPhase = HostMigrationPhase::awaitingPeerHandoff;
        }
        break;
    case HostMigrationPhase::transitionReady:
        queued = publish_host_transition(record);
        if (queued) {
            // The client becomes the native host after accepting the transition and emits kind 22
            // back to member zero. Do not hand authority back to Sunrise; wait for that native
            // reestablish and answer it with peer-reestablish kind 25.
            record.migrationPhase = HostMigrationPhase::awaitingNativeHostReestablish;
            record.migrationDue = 0;
            return;
        }
        break;
    case HostMigrationPhase::peerReestablishReady:
        queued = publish_peer_reestablish(record, "ack_native_host");
        if (queued) {
            // Commit replay eligibility at the same boundary as the first successful kind-25.
            record.nativeHostAckReceipt.valid =
                record.nativeHostAckReceipt.nativeSecurityRegistered;
            record.migrationPhase = HostMigrationPhase::complete;
            record.migrationDue = 0;
            report(core::log::Level::info,
                   "ev=gameplay stage=migration result=complete session=0x%016llX "
                   "edge=native_host_reestablish_ack",
                   static_cast<unsigned long long>(record.sessionId));
            return;
        }
        break;
    default:
        return;
    }
    if (!queued) {
        record.migrationDue = now + kRetryInterval;
    }
}

/** @return True when two endpoints name the same address and port. */
[[nodiscard]] bool same_endpoint(const state::gameplay::Endpoint& left,
                                 const state::gameplay::Endpoint& right) noexcept {
    return left.address == right.address && left.port == right.port;
}

/**
 * Finds or claims the record for one peer, and binds it to that peer's endpoint.
 * Admission is what establishes ownership, so this rebinds an existing record. A client that
 * rebuilds its channel arrives from a new port and joins the same session again.
 * @param peer Peer endpoint.
 * @param sessionId Group session the record is keyed by. Zero claims nothing.
 * @return Record for that session, or null when the table is full.
 */
[[nodiscard]] Admitted* claim(const state::gameplay::Endpoint& peer,
                              std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    // Keyed by session, not endpoint: one client holds a record per public region and both records
    // name the same endpoint.
    const std::uint64_t use = g_admitClock.fetch_add(1) + 1;
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry.endpoint = peer;
            entry.lastUse = use;
            return &entry;
        }
    }
    for (Admitted& entry : g_admitted) {
        if (!entry.occupied) {
            entry.occupied = true;
            entry.endpoint = peer;
            entry.sessionId = sessionId;
            entry.lastUse = use;
            return &entry;
        }
    }
    return nullptr;
}

/**
 * Finds the record for one session and proves the sender owns it.
 * Every later message names its own session in its body, so without this a peer could name a
 * session another endpoint was admitted for and move that session's state.
 * @param peer Peer endpoint the message arrived from.
 * @param sessionId Group session the message named.
 * @return Record for that session, or null when it is absent or owned by another endpoint.
 */
[[nodiscard]] Admitted* claim_owned(const state::gameplay::Endpoint& peer,
                                    std::uint64_t sessionId) noexcept {
    if (sessionId == 0) {
        return nullptr;
    }
    const std::uint64_t use = g_admitClock.fetch_add(1) + 1;
    for (Admitted& entry : g_admitted) {
        if (!entry.occupied || entry.sessionId != sessionId) {
            continue;
        }
        if (!same_endpoint(entry.endpoint, peer)) {
            return nullptr;
        }
        entry.lastUse = use;
        return &entry;
    }
    return nullptr;
}

/**
 * Tests whether another endpoint was admitted for one session.
 * An absent record is not a conflict: a message may name a session before this host has a record
 * for it, and refusing that would strand the peer. A record held elsewhere is a conflict.
 * @param peer Peer endpoint the message arrived from.
 * @param sessionId Group session the message named.
 * @return True when a record holds that session for a different endpoint.
 */
[[nodiscard]] bool owned_elsewhere(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept {
    AcquireSRWLockShared(&g_admittedLock);
    bool conflict = false;
    for (const Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            conflict = !same_endpoint(entry.endpoint, peer);
            break;
        }
    }
    ReleaseSRWLockShared(&g_admittedLock);
    return conflict;
}

/**
 * Publishes one snapshot naming this host, one admitted peer, and that peer's player if it has
 * one. The caller holds the admitted lock.
 * @param record Admitted peer the snapshot names.
 * @return True when the snapshot was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_snapshot(const Admitted& record) noexcept {
    const state::gameplay::Endpoint host = endpoint::advertised();
    std::array<wire::MembershipMember, kSnapshotMemberCount> members{};
    descriptor::write_net_addr(host.address, host.port, members[kHostMemberIndex].address);
    // The session id is the machine id this region's descriptor advertised, and the client joined
    // through it. The whole-process identity would name a host this session never saw.
    members[kHostMemberIndex].machineId = record.sessionId;
    // The consumer refuses a table with no entry it recognises as itself, so the peer's own blob is
    // echoed. A blob rebuilt from the endpoint it arrived from is not the same bytes.
    if (!peer::remote_address(record.sessionId, members[kPeerMemberIndex].address)) {
        descriptor::write_net_addr(
            record.endpoint.address, record.endpoint.port, members[kPeerMemberIndex].address);
    }
    // The peer refuses a snapshot whose entry for itself carries another join id. Its real
    // machine id sits in the address table this host does not decode, so the join id stands in.
    members[kPeerMemberIndex].machineId = record.joinId;
    members[kPeerMemberIndex].joinId = record.joinId;
    // The peer ends its join request once no session holds more than one member with that id, so
    // both entries carry it. A table naming it once says the join is over.
    members[kHostMemberIndex].joinId = record.joinId;
    for (wire::MembershipMember& member : members) {
        // The connection group is what makes the consumer resolve the member's peer link. This
        // host has no value for join compatibility or the join timestamp, so both stay cleared.
        member.connectionPresent = true;
    }
    // Both entries carry the join id, so both take the same state. Once the peer reports its join
    // finished they move to `established`, which is what stops it re-sending that report.
    const wire::MemberState state =
        record.joinComplete ? wire::MemberState::established : kJoinMemberState;
    members[kHostMemberIndex].state = state;
    members[kPeerMemberIndex].state = state;

    std::array<wire::MembershipPlayer, 1> players{};
    players[0].slot = kPeerPlayerSlot;
    players[0].playerId = record.playerId;
    players[0].memberIndex = static_cast<std::uint32_t>(kPeerMemberIndex);
    players[0].addSequence = kFirstAddSequence;
    if (record.hasPlayer) {
        members[kPeerMemberIndex].ownsPlayerSlot = true;
        members[kPeerMemberIndex].playerSlot = kPeerPlayerSlot;
    }

    wire::MembershipUpdate update{};
    // The same per-region machine id the member table carries.
    update.hostMachineId = record.sessionId;
    update.revision = g_membershipRevision.fetch_add(1) + 1;
    update.hostMemberIndex = kHostMemberIndex;
    update.successionIndex = kHostMemberIndex;
    update.members = members;
    if (record.hasPlayer) {
        update.players = players;
    }

    std::array<std::byte, kMembershipBodyCapacity> body{};
    bits::Writer writer(body);
    std::size_t size = 0;
    if (!wire::write_membership_update(writer, update) || !writer.finish(size)) {
        return false;
    }
    // The peer logs the hash it wanted, so ours has to be logged next to it to read a mismatch.
    report(core::log::Level::info,
           "ev=gameplay stage=membership result=built revision=%u members=%zu players=%zu "
           "hash=0x%08X",
           update.revision,
           update.members.size(),
           update.players.size(),
           wire::session_state_hash(update));
    return peer::enqueue_reliable(
        record.sessionId,
        static_cast<std::uint8_t>(wire::SessionMessageId::membershipUpdate),
        wire::kMembershipUpdateSize,
        {body.data(), size},
        writer.bit_count());
}

/**
 * Publishes the parameter update that releases the joining peer's initial application latch.
 * The caller holds the admitted lock. A queued update is sticky for this join transaction: client
 * retries must not append duplicate copies and crowd later membership/activity-host publications.
 * @param record Admitted join transaction.
 * @return True once this transaction has queued the update.
 */
[[nodiscard]] bool publish_initial_parameters(Admitted& record) noexcept {
    if (record.initialParametersPublished) {
        return true;
    }
    // Keep the retail ordering observed by the client: membership first, then at least one named
    // group parameter. If the membership queue was full, service() retries it before this update.
    if (!record.initialMembershipPublished) {
        return false;
    }
    wire::ParameterUpdate update{};
    update.sessionId = record.sessionId;
    update.releasedMask = std::uint64_t{1} << kJoinLatchParameter;

    const bool sent = send_reliable(
        record.sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    if (sent) {
        record.initialParametersPublished = true;
    }
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s released=0x%08X names=%s",
           sent ? "queued" : "fail",
           static_cast<unsigned>(update.releasedMask),
           wire::parameter_names(update.releasedMask, names.data(), names.size()));
    return sent;
}

/**
 * Fills the `activity-host` body this host publishes.
 * The peer creates no activity client until it holds this parameter, and the public-region
 * slice-set switch waits behind that client.
 * @param body Cleared body to fill.
 * @param binding Retained host row used for this whole parameter body.
 */
void fill_activity_host(wire::ActivityHostParameter& body,
                        const HostSessionBinding& binding) noexcept {
    // The peer's `current-activity` carries this host's empty delta, so its nonce is the
    // descriptor default and the comparand is the empty id.
    body.selectionId = 0;
    // The peer addresses its activity join request to this id, and the activity route refuses one
    // that names no committed activity session. A gameplay identity is not one.
    body.hostId = binding.target.sessionId;
    // The peer tests only the bit for its own member index, and this host does not decode which
    // index that is, so every bit is set.
    body.memberMask = kAllMembers;
    body.address = kLoopbackAddress;
    body.port = core::settings::get().server.bapPort;
}

/**
 * Publishes the `activity-host` parameter for one admitted peer. The caller holds the lock.
 * @param record Admitted peer the parameter is published to.
 * @return True when the update was queued on the peer's reliable channel.
 */
[[nodiscard]] bool publish_activity_host(const Admitted& record) noexcept {
    // The body is built from this copy, so no retain is needed: `host_session_for_group` already
    // returns only a ready row whose State bindings still match, and nothing below reads the table.
    HostSessionBinding binding{};
    if (!host_session_for_group(record.sessionId, binding)) {
        // Publishing a zero host id latches an unusable parameter on the peer, and the peer only
        // reads it once. The region's advertisement allocates and this retries.
        report(core::log::Level::debug, "ev=gameplay stage=activityhost result=nosession");
        return false;
    }
    wire::ParameterUpdate update{};
    update.sessionId = record.sessionId;
    // Both go in one update, so the peer never holds the host without the activity it belongs to.
    // `current-activity` carries an empty delta, which leaves the peer's own descriptor defaults.
    update.carriedMask =
        (std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::activityHost))
        | (std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::currentActivity));
    fill_activity_host(update.activityHost, binding);

    const bool sent = send_reliable(
        record.sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::debug,
           "ev=gameplay stage=activityhost result=%s host=0x%llX address=0x%08X port=%u names=%s",
           sent ? "queued" : "deferred",
           static_cast<unsigned long long>(update.activityHost.hostId),
           update.activityHost.address,
           static_cast<unsigned>(update.activityHost.port),
           wire::parameter_names(update.carriedMask, names.data(), names.size()));
    return sent;
}

/**
 * Answers one view establishment by binding and echoing the peer's own signature.
 * What a host's own view should hold is unknown. Echoing is the only answer that cannot produce
 * a signature mismatch. The binding is keyed by the link, because the body names no session.
 * @param from Peer endpoint the view arrived from.
 * @param sessionId Session the reply rides back on, or zero when the link carries several.
 * @param view Decoded view body.
 */
void bind_view(const state::gameplay::Endpoint& from,
               std::uint64_t sessionId,
               const wire::ViewEstablishment& view) noexcept {
    state::gameplay::ViewSignature signature{};
    signature.token = view.sessionToken;
    signature.kind = view.kind;
    signature.listCount = view.listCount;
    signature.hasList = view.hasList;
    signature.list = view.list;
    // Kept unread. Its meaning is unrecovered, and dropping it would lose a field the peer sent.
    signature.optionalValue = view.optionalValue;
    signature.hasOptionalValue = view.hasOptionalValue;
    signature.bound = true;
    peer::bind_view(from, signature);

    const bool sent = send_reliable(
        sessionId,
        wire::kViewMessageId,
        wire::kViewMessageSize,
        [&view](bits::Writer& writer) noexcept { return wire::write_view(writer, view); });
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=view result=%s kind=%u token=0x%llX list=%u",
           sent ? "bound" : "fail",
           static_cast<unsigned>(view.kind),
           static_cast<unsigned long long>(view.sessionToken),
           static_cast<unsigned>(view.listCount));
}

/**
 * Answers one parameter request with the parameters this host can encode.
 * An empty answer leaves the peer waiting, so the answer carries every requested parameter that
 * has an encoder and names the rest as unheld.
 * @param sessionId Session the request named, which is also the link it goes back on.
 * @param requested Requested parameter mask, already reduced to its meaningful bits.
 */
void answer_parameters(std::uint64_t sessionId, std::uint64_t requested) noexcept {
    std::uint64_t carried = requested & wire::kEncodableParameters;
    const std::uint64_t activityHostMask =
        std::uint64_t{1} << static_cast<std::uint8_t>(wire::Parameter::activityHost);
    // The body is built from this copy, so no retain is needed. See publish_activity_host.
    HostSessionBinding binding{};
    const bool hasHost =
        (carried & activityHostMask) != 0 && host_session_for_group(sessionId, binding);
    if ((carried & activityHostMask) != 0 && !hasHost) {
        // A zero host id is worse than no answer for this one.
        carried &= ~activityHostMask;
    }
    if (carried == 0) {
        report(core::log::Level::debug,
               "ev=gameplay stage=parameters result=unheld mask=0x%08X",
               static_cast<unsigned>(requested));
        return;
    }

    wire::ParameterUpdate update{};
    update.sessionId = sessionId;
    update.carriedMask = carried;
    // A zero host id latches an unusable parameter on the peer, so the answer carries the same
    // body the unsolicited publish does.
    if (hasHost) {
        fill_activity_host(update.activityHost, binding);
    }

    const bool sent = send_reliable(
        sessionId,
        wire::kParameterUpdateId,
        wire::kParameterUpdateSize,
        [&update](bits::Writer& writer) { return wire::write_parameter_update(writer, update); });
    std::array<char, kParameterNameCapacity> names{};
    report(sent ? core::log::Level::info : core::log::Level::warn,
           "ev=gameplay stage=parameters result=%s carried=0x%08X names=%s",
           sent ? "answered" : "fail",
           static_cast<unsigned>(carried),
           wire::parameter_names(carried, names.data(), names.size()));
}

/**
 * Answers one time-synchronize probe with the same form it arrived in.
 * @param from Peer endpoint.
 * @param probe Decoded probe.
 */
void answer_time(const state::gameplay::Endpoint& from,
                 const wire::TimeSynchronize& probe) noexcept {
    // The exchange must never block the event loop, so the samples are echoed unchanged.
    if (!peer::send_out_of_band(from,
                                static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize),
                                wire::kTimeSynchronizeSize,
                                [&probe](bits::Writer& writer) noexcept {
                                    return wire::write_time_synchronize(writer, probe);
                                })) {
        report(core::log::Level::debug, "ev=gameplay stage=time result=fail");
    }
}

/**
 * Drops one session's link and its admitted record together.
 * A leave names one region's session, and the client's other region must keep its own link.
 * @param sessionId Session the peer is leaving.
 */
void release(std::uint64_t sessionId) noexcept {
    peer::drop(sessionId);
    // The region's activity host stays. A leave is also how the peer fast travels to the region it
    // is already in, and a fresh id there is `public_activity_host_mismatch`.
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace

/** Frees every admitted record at one endpoint. */
void release_endpoint(const state::gameplay::Endpoint& endpoint) noexcept {
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && same_endpoint(entry.endpoint, endpoint)) {
            ++count;
            entry = {};
        }
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    if (count != 0) {
        report(core::log::Level::info,
               "ev=gameplay stage=admitted result=dropped endpoint=0x%08X:%u sessions=%zu",
               endpoint.address,
               static_cast<unsigned>(endpoint.port),
               count);
    }
}

/** Consumes one group-session message. */
bool consume(const state::gameplay::Endpoint& from,
             std::uint64_t sessionId,
             std::uint8_t id,
             bits::Reader& reader,
             std::uint64_t now) noexcept {
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::timeSynchronize)) {
        wire::TimeSynchronize probe{};
        if (!wire::read_time_synchronize(reader, probe)) {
            return false;
        }
        answer_time(from, probe);
        return true;
    }
    if (id == wire::kViewMessageId) {
        wire::ViewEstablishment view{};
        if (!wire::read_view(reader, view)) {
            return false;
        }
        bind_view(from, sessionId, view);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::leaveSession)) {
        std::uint64_t leaving = 0;
        if (!wire::read_session_only(reader, leaving)) {
            return false;
        }
        if (owned_elsewhere(from, leaving)) {
            // A leave tears the session's link down, so a peer must not be able to send one for a
            // session another endpoint was admitted for.
            report(core::log::Level::warn,
                   "ev=gameplay stage=leave result=unowned session=0x%016llX",
                   static_cast<unsigned long long>(leaving));
            return true;
        }
        const bool sent = peer::send_out_of_band(
            from,
            static_cast<std::uint8_t>(wire::SessionMessageId::leaveAcknowledge),
            wire::kLeaveAcknowledgeSize,
            [leaving](bits::Writer& writer) noexcept {
                return wire::write_session_only(writer, leaving);
            });
        report(core::log::Level::info,
               "ev=gameplay stage=leave result=%s session=0x%016llX",
               sent ? "acknowledged" : "fail",
               static_cast<unsigned long long>(leaving));
        release(leaving);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::peerEstablish)) {
        std::uint64_t established = 0;
        if (!wire::read_session_only(reader, established)) {
            return false;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=establish result=ok session=0x%016llX",
               static_cast<unsigned long long>(established));
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinComplete)) {
        wire::JoinComplete body{};
        if (!wire::read_join_complete(reader, body)) {
            return false;
        }
        // The peer repeats this until its membership shows every member of the join at
        // `established`, so the answer is a snapshot that promotes them. Keyed by the body's
        // session, not the link's: one link carries every region the client joined over it.
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, body.sessionId);
        bool queued = false;
        const bool owed = record != nullptr && !record->joinPublished;
        if (record != nullptr) {
            record->joinComplete = true;
            if (owed) {
                queued = publish_snapshot(*record);
                record->joinPublished = queued;
            }
            // The peer only reads the parameter once its join is finished, and the queue is at its
            // fullest right here, so a refusal is expected and the service slice retries it.
            if (record->joinPublished && !record->activityHostPublished) {
                record->activityHostPublished = publish_activity_host(*record);
                record->lastRetry = now;
            }
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(queued ? core::log::Level::info : core::log::Level::debug,
               "ev=gameplay stage=join result=%s session=0x%llX machine=0x%llX update=%u",
               queued              ? "completed"
               : record == nullptr ? "fail"
               : owed              ? "deferred"
                                   : "repeat",
               static_cast<unsigned long long>(body.sessionId),
               static_cast<unsigned long long>(body.machineId),
               body.joinSequence);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::SessionMessageId::joinAbort)) {
        wire::SessionNotice notice{};
        if (!wire::read_join_abort(reader, notice)) {
            return false;
        }
        if (owned_elsewhere(from, notice.sessionId)) {
            report(core::log::Level::warn,
                   "ev=gameplay stage=join result=unowned_abort session=0x%016llX",
                   static_cast<unsigned long long>(notice.sessionId));
            return true;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=join result=abort session=0x%016llX",
               static_cast<unsigned long long>(notice.sessionId));
        release(notice.sessionId);
        return true;
    }
    if (id == wire::kParameterRequestId) {
        wire::ParameterRequestHeader header{};
        if (!wire::read_parameter_request(reader, header)) {
            return false;
        }
        const std::uint64_t mask = header.requestedMask & kParameterMaskBits;
        std::array<char, kParameterNameCapacity> names{};
        report(core::log::Level::info,
               "ev=gameplay stage=parameters result=request mask=0x%08X mode=%u names=%s",
               static_cast<unsigned>(mask),
               static_cast<unsigned>(header.modeFlag ? 1U : 0U),
               wire::parameter_names(mask, names.data(), names.size()));
        // The selected bodies are walked before the answer goes out, so nothing is answered from
        // a request that was only read as far as its header.
        wire::ParameterRequestWalk walk{};
        const bool intact = wire::walk_parameter_request(reader, mask, walk);
        report(walk.complete ? core::log::Level::debug : core::log::Level::info,
               "ev=gameplay stage=parameters result=%s walked=0x%08X stopped=%u tail=%u",
               walk.complete ? "framed"
               : intact      ? "ambiguous"
                             : "truncated",
               static_cast<unsigned>(walk.walkedMask),
               static_cast<unsigned>(walk.ambiguousParameter),
               walk.tailBits);
        // The peer builds no activity client until it holds the host parameter, so the answer goes
        // out even when a later body could not be located. The tail above is what is unread, not
        // the answer's own inputs. A session another endpoint holds is answered by that endpoint.
        if (!owned_elsewhere(from, header.sessionId)) {
            answer_parameters(header.sessionId, mask);
        }
        // Only a fully located request leaves the container readable behind it.
        return walk.complete;
    }
    if (id == wire::kPeerPropertiesId) {
        wire::PeerPropertiesHeader header{};
        if (!wire::read_peer_properties_header(reader, header)) {
            return false;
        }
        // The 304-byte property block behind the address is not decoded, so the body is
        // reported and not consumed.
        report(core::log::Level::info,
               "ev=gameplay stage=properties result=read session=0x%llX method=%u",
               static_cast<unsigned long long>(header.sessionId),
               static_cast<unsigned>(header.addressMethod));
        return false;
    }
    if (id == wire::kPlayerAddId) {
        wire::PlayerAddRequest request{};
        if (!wire::read_player_add(reader, request)) {
            return false;
        }
        // The published row carries the identity group only. The profile block behind it has no
        // encoder here, and the peer's clear-flag arm accepts a row without one.
        AcquireSRWLockExclusive(&g_admittedLock);
        // The body's session, for the same reason join-complete uses its own.
        Admitted* const record = claim_owned(from, request.sessionId);
        bool published = false;
        if (record != nullptr) {
            record->hasPlayer = true;
            record->playerId = request.playerId;
            published = publish_snapshot(*record);
            // The queue is at its fullest here, right after the join promotion, so a refusal is
            // ordinary and the service slice retries it.
            record->playerPublished = published;
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        // The player block and its tail are not decoded, so the body is reported and not consumed.
        report(core::log::Level::info,
               "ev=gameplay stage=player result=%s session=0x%llX player=0x%llX seq=%u kind=%u",
               published ? "added" : "fail",
               static_cast<unsigned long long>(request.sessionId),
               static_cast<unsigned long long>(request.playerId),
               request.sequence,
               static_cast<unsigned>(request.kind));
        return false;
    }
    if (id == wire::kPlayerRemoveId) {
        wire::PlayerRemoveRequest request{};
        if (!wire::read_player_remove(reader, request)) {
            return false;
        }
        // The message names no player. The one to drop is the player the bound record holds.
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, request.sessionId);
        bool published = false;
        if (record != nullptr && record->hasPlayer) {
            record->hasPlayer = false;
            record->playerId = 0;
            published = publish_snapshot(*record);
            record->playerPublished = published;
        }
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(core::log::Level::info,
               "ev=gameplay stage=player result=%s session=0x%llX",
               published           ? "removed"
               : record == nullptr ? "fail"
                                   : "absent",
               static_cast<unsigned long long>(request.sessionId));
        // The whole body is two fields, so the container stays readable behind it.
        return true;
    }
    if (id == wire::kPlayerPropertiesId) {
        wire::PlayerPropertiesRequest request{};
        if (!wire::read_player_properties_header(reader, request)) {
            return false;
        }
        // The sparse record behind the header is not decoded, so nothing is merged from it. A
        // merge from the header alone would reset every field the record carries.
        report(core::log::Level::info,
               "ev=gameplay stage=player result=properties session=0x%llX seq=%u kind=%u",
               static_cast<unsigned long long>(request.sessionId),
               request.sequence,
               static_cast<unsigned>(request.kind));
        return false;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::hostReestablish)) {
        wire::HostReestablish receipt{};
        if (!wire::read_host_reestablish(reader, receipt)) {
            return false;
        }

        // Keep the exact native identity material visible beside the retail security-map trace.
        // `machine_wire` is the byte order the native raw-u64 reader consumed, which can be
        // compared directly with retail "bdSecurityID" diagnostics without guessing endianness.
        const auto machineBytes = raw_u64_bytes(receipt.machineId);
        std::array<char, (sizeof(std::uint64_t) * 2) + 1> machineHex{};
        std::array<char, (wire::kHostReestablishOpaque16Bytes * 2) + 1> opaque16Hex{};
        std::array<char, (wire::kHostReestablishOpaque18Bytes * 2) + 1> opaque18Hex{};
        format_hex(machineBytes, machineHex);
        format_hex(receipt.opaque16, opaque16Hex);
        format_hex(receipt.opaque18, opaque18Hex);
        const std::uint64_t opaque16Lo = read_low_u64(receipt.opaque16);
        const std::uint64_t opaque16Hi = read_low_u64(receipt.opaque16, 8);
        const std::uint64_t opaque18Lo = read_low_u64(receipt.opaque18);
        const std::uint64_t opaque18Hi = read_low_u64(receipt.opaque18, 8);
        const unsigned opaque18Tail =
            std::to_integer<unsigned>(receipt.opaque18[16])
            | (std::to_integer<unsigned>(receipt.opaque18[17]) << 8U);
        const unsigned method =
            std::to_integer<unsigned>(receipt.address[descriptor::kNetAddrSize - 1]);

        report(core::log::Level::info,
               "ev=gameplay stage=migration_identity result=native_host_reestablish "
               "session=0x%016llX machine=0x%016llX machine_wire=%s method=%u "
               "opaque16=%s opaque16_lo=0x%016llX opaque16_hi=0x%016llX "
               "opaque18=%s opaque18_lo=0x%016llX opaque18_hi=0x%016llX "
               "opaque18_tail=0x%04X",
               static_cast<unsigned long long>(receipt.sessionId),
               static_cast<unsigned long long>(receipt.machineId),
               machineHex.data(),
               method,
               opaque16Hex.data(),
               static_cast<unsigned long long>(opaque16Lo),
               static_cast<unsigned long long>(opaque16Hi),
               opaque18Hex.data(),
               static_cast<unsigned long long>(opaque18Lo),
               static_cast<unsigned long long>(opaque18Hi),
               opaque18Tail);

        // Do not acknowledge the native host's reestablish until both sides can use the migrated
        // security material. The server DTLS host already has a bounded migration-key registry;
        // the client side uses Destiny's own learned bdSecurityKeyMap::registerKey path.
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* completedRecord = nullptr;
        for (Admitted& entry : g_admitted) {
            if (entry.occupied && entry.sessionId == receipt.sessionId
                && entry.migrationPhase == HostMigrationPhase::complete) {
                completedRecord = &entry;
                break;
            }
        }

        // A completed replay is keyed by the accepted migration identity rather than the old UDP
        // source endpoint. The owner-resecure can rebuild that endpoint before Destiny retransmits
        // kind 22. Normal first acceptance still requires claim_owned() below.
        const bool completedMigration = completedRecord != nullptr;
        const bool replayAccepted =
            completedMigration
            && same_accepted_native_host(completedRecord->nativeHostAckReceipt, receipt);
        Admitted replayRecord{};
        if (replayAccepted) {
            completedRecord->lastUse = g_admitClock.fetch_add(1) + 1;
            replayRecord = *completedRecord;
        }

        Admitted* const initialRecord =
            completedMigration ? nullptr : claim_owned(from, receipt.sessionId);
        const HostMigrationPhase before =
            completedMigration
                ? HostMigrationPhase::complete
                : (initialRecord != nullptr ? initialRecord->migrationPhase
                                            : HostMigrationPhase::idle);
        const bool expectedNativeEdge =
            initialRecord != nullptr
            && initialRecord->migrationPhase == HostMigrationPhase::awaitingNativeHostReestablish;
        ReleaseSRWLockExclusive(&g_admittedLock);

        // The first kind-25 can be queued on the channel that the intentional owner-resecure is
        // about to tear down. Destiny then retransmits the same native kind-22 after the migrated
        // channel secures. A completed migration must ACK that exact accepted identity again, but
        // must not re-register either security map, invoke owner-resecure again, or reopen state.
        if (completedMigration) {
            if (replayAccepted) {
                const bool replayQueued =
                    publish_peer_reestablish(replayRecord, "ack_native_host_replay");
                report(replayQueued ? core::log::Level::info : core::log::Level::debug,
                       "ev=gameplay stage=migration result=native_host_reestablish_replay "
                       "session=0x%016llX machine=0x%016llX phase=%s->%s advanced=0 ack=%s",
                       static_cast<unsigned long long>(receipt.sessionId),
                       static_cast<unsigned long long>(receipt.machineId),
                       migration_phase_name(before),
                       migration_phase_name(before),
                       replayQueued ? "queued" : "deferred");
            } else {
                report(core::log::Level::warn,
                       "ev=gameplay stage=migration result=native_host_reestablish_ignored "
                       "reason=completed_identity_mismatch session=0x%016llX machine=0x%016llX "
                       "phase=%s",
                       static_cast<unsigned long long>(receipt.sessionId),
                       static_cast<unsigned long long>(receipt.machineId),
                       migration_phase_name(before));
            }
            return true;
        }

        sunrise::client::hooks::retail_log::NativeSecurityRegistrationResult nativeRegistration{};
        if (expectedNativeEdge) {
            dtls::register_security_key(
                from,
                receipt.machineId,
                std::span<const std::byte>{receipt.opaque16.data(), receipt.opaque16.size()});
            dtls::prefer_security_id(from, receipt.machineId);
            nativeRegistration =
                sunrise::client::hooks::retail_log::register_migrated_security_key(
                    machineBytes, receipt.opaque16);
            const bool nativeRegistered = nativeRegistration.invoked && nativeRegistration.inserted;
            report(nativeRegistered ? core::log::Level::info : core::log::Level::warn,
                   "ev=gameplay stage=migration_security result=%s session=0x%016llX "
                   "security=0x%016llX native_ready=%u native_invoked=%u native_inserted=%u",
                   nativeRegistered ? "registered" : "held",
                   static_cast<unsigned long long>(receipt.sessionId),
                   static_cast<unsigned long long>(receipt.machineId),
                   nativeRegistration.ready ? 1U : 0U,
                   nativeRegistration.invoked ? 1U : 0U,
                   nativeRegistration.inserted ? 1U : 0U);
        }

        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, receipt.sessionId);
        bool advanced = false;
        if (record != nullptr
            && record->migrationPhase == HostMigrationPhase::awaitingNativeHostReestablish
            && nativeRegistration.invoked && nativeRegistration.inserted) {
            // Retain only the identity material whose native registration succeeded. The phase
            // does not become complete until service_migration() successfully queues kind 25, so
            // this receipt cannot authorize replay before the first ACK commit boundary.
            record->nativeHostAckReceipt.sessionId = receipt.sessionId;
            record->nativeHostAckReceipt.machineId = receipt.machineId;
            record->nativeHostAckReceipt.opaque16 = receipt.opaque16;
            record->nativeHostAckReceipt.opaque18 = receipt.opaque18;
            record->nativeHostAckReceipt.nativeSecurityRegistered = true;
            // The receipt becomes replay-valid only when the first kind-25 is actually queued.
            record->nativeHostAckReceipt.valid = false;
            record->migrationPhase = HostMigrationPhase::peerReestablishReady;
            record->migrationDue = now;
            advanced = true;
        }
        const HostMigrationPhase after =
            record != nullptr ? record->migrationPhase : HostMigrationPhase::idle;
        ReleaseSRWLockExclusive(&g_admittedLock);

        report(advanced ? core::log::Level::info : core::log::Level::debug,
               "ev=gameplay stage=migration result=native_host_reestablish "
               "session=0x%016llX machine=0x%016llX phase=%s->%s advanced=%u",
               static_cast<unsigned long long>(receipt.sessionId),
               static_cast<unsigned long long>(receipt.machineId),
               migration_phase_name(before),
               migration_phase_name(after),
               advanced ? 1U : 0U);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::peerReestablish)) {
        std::uint64_t receiptSession = 0;
        if (!wire::read_migration_session(reader, receiptSession)) {
            return false;
        }
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, receiptSession);
        const HostMigrationPhase before =
            record != nullptr ? record->migrationPhase : HostMigrationPhase::idle;
        bool advanced = false;
        if (record != nullptr
            && record->migrationPhase == HostMigrationPhase::awaitingBaselineReestablish) {
            record->migrationPhase = HostMigrationPhase::handoffToPeerReady;
            record->migrationDue = now;
            advanced = true;
        }
        const HostMigrationPhase after =
            record != nullptr ? record->migrationPhase : HostMigrationPhase::idle;
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(advanced ? core::log::Level::info : core::log::Level::debug,
               "ev=gameplay stage=migration result=peer_reestablish session=0x%016llX "
               "phase=%s->%s advanced=%u",
               static_cast<unsigned long long>(receiptSession),
               migration_phase_name(before),
               migration_phase_name(after),
               advanced ? 1U : 0U);
        return true;
    }
    if (id == static_cast<std::uint8_t>(wire::MigrationMessageId::peerHandoff)) {
        wire::HostHandoff receipt{};
        if (!wire::read_host_handoff(reader, receipt)) {
            return false;
        }
        AcquireSRWLockExclusive(&g_admittedLock);
        Admitted* const record = claim_owned(from, receipt.sessionId);
        const HostMigrationPhase before =
            record != nullptr ? record->migrationPhase : HostMigrationPhase::idle;
        bool advanced = false;
        if (record != nullptr && record->migrationPhase == HostMigrationPhase::awaitingPeerHandoff
            && receipt.successorIndex == kPeerMemberIndex) {
            record->migrationPhase = HostMigrationPhase::transitionReady;
            record->migrationDue = now;
            advanced = true;
        }
        const HostMigrationPhase after =
            record != nullptr ? record->migrationPhase : HostMigrationPhase::idle;
        ReleaseSRWLockExclusive(&g_admittedLock);
        report(advanced ? core::log::Level::info : core::log::Level::debug,
               "ev=gameplay stage=migration result=peer_handoff session=0x%016llX successor=%u "
               "phase=%s->%s advanced=%u",
               static_cast<unsigned long long>(receipt.sessionId),
               static_cast<unsigned>(receipt.successorIndex),
               migration_phase_name(before),
               migration_phase_name(after),
               advanced ? 1U : 0U);
        return true;
    }
    // Other migration/election bodies remain observation-only. The synchronization bodies above
    // are consumed here because they drive this host's bounded migration sequence.
    return migration::consume(id, reader);
}

/** Publishes the membership snapshot that completes one peer's join. */
bool publish_membership(const state::gameplay::Endpoint& peer,
                        std::uint64_t peerJoinId,
                        std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* const record = claim(peer, sessionId);
    bool published = false;
    if (record != nullptr) {
        const bool newTransaction = record->joinId != peerJoinId;
        if (newTransaction) {
            // Only a different join id starts a different admission transaction. The client
            // retransmits the same join while waiting for initial updates; resetting state on
            // those retries used to discard successful publications and migration state.
            record->joinId = peerJoinId;
            record->sessionId = sessionId;
            record->hasPlayer = false;
            record->playerId = 0;
            record->initialMembershipPublished = false;
            record->initialParametersPublished = false;
            record->joinComplete = false;
            record->joinPublished = false;
            record->activityHostPublished = false;
            record->playerPublished = false;
            record->migrationPhase = HostMigrationPhase::idle;
            record->nativeHostAckReceipt = {};
            record->migrationDue = 0;
            record->lastRetry = 0;
        }
        if (record->initialMembershipPublished) {
            published = true;
        } else {
            published = publish_snapshot(*record);
            record->initialMembershipPublished = published;
            if (published) {
                record->lastRetry = 0;
            }
        }
        report(core::log::Level::debug,
               "ev=gameplay stage=join_publication result=%s session=0x%016llX join=0x%016llX "
               "transaction=%s membership=%u parameters=%u",
               published ? "ready" : "deferred",
               static_cast<unsigned long long>(sessionId),
               static_cast<unsigned long long>(peerJoinId),
               newTransaction ? "new" : "retry",
               record->initialMembershipPublished ? 1U : 0U,
               record->initialParametersPublished ? 1U : 0U);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    return published;
}

/** Retries any publish a full reliable queue refused. */
void service(std::uint64_t now) noexcept {
    // Outside any staged push, so the state revision it advances cannot fail a transaction guard.
    allocate_claimed_host_sessions();
    // The peer drops a stale target locally and sends no leave for it. Such a record shows up
    // only as the least recently named one over the capacity.
    std::uint64_t retired = 0;
    AcquireSRWLockExclusive(&g_admittedLock);
    std::size_t occupied = 0;
    Admitted* oldest = nullptr;
    for (Admitted& record : g_admitted) {
        if (!record.occupied) {
            continue;
        }
        ++occupied;
        if (oldest == nullptr || record.lastUse < oldest->lastUse) {
            oldest = &record;
        }
    }
    if (occupied > kPublicSessionCapacity && oldest != nullptr) {
        retired = oldest->sessionId;
        *oldest = {};
    }
    for (Admitted& record : g_admitted) {
        if (!record.occupied) {
            continue;
        }
        const bool owed = !record.initialMembershipPublished
                          || !record.initialParametersPublished
                          || (record.joinComplete && !record.joinPublished)
                          || (record.joinPublished && !record.activityHostPublished)
                          || (record.joinPublished && record.hasPlayer && !record.playerPublished);
        if (owed) {
            if (now - record.lastRetry < kRetryInterval) {
                continue;
            }
            record.lastRetry = now;
            // Initial application establishment is ordered and independently retryable. A full
            // queue must not turn a one-time join callback into a permanent missing update.
            if (!record.initialMembershipPublished) {
                record.initialMembershipPublished = publish_snapshot(record);
                continue;
            }
            if (!record.initialParametersPublished) {
                static_cast<void>(publish_initial_parameters(record));
                continue;
            }
            if (!record.joinPublished) {
                record.joinPublished = publish_snapshot(record);
                continue;
            }
            if (!record.activityHostPublished) {
                record.activityHostPublished = publish_activity_host(record);
                continue;
            }
            // Last, so the join order is unchanged. Migration cannot arm until this succeeds.
            record.playerPublished = publish_snapshot(record);
            continue;
        }
        service_migration(record, now);
    }
    ReleaseSRWLockExclusive(&g_admittedLock);
    // Outside the lock, in the order `release` already uses. The region's activity host stays: the
    // peer rotates back into a region it has not left, and a fresh id there is a hard error.
    if (retired != 0) {
        peer::drop(retired);
        report(core::log::Level::info,
               "ev=gameplay stage=admitted result=retired session=0x%016llX held=%zu",
               static_cast<unsigned long long>(retired),
               occupied - 1);
    }
}

/** Publishes the parameter update a joining peer needs before it will finish its join. */
bool publish_join_parameters(std::uint64_t sessionId) noexcept {
    AcquireSRWLockExclusive(&g_admittedLock);
    Admitted* record = nullptr;
    for (Admitted& entry : g_admitted) {
        if (entry.occupied && entry.sessionId == sessionId) {
            record = &entry;
            break;
        }
    }
    const bool sent = record != nullptr && publish_initial_parameters(*record);
    ReleaseSRWLockExclusive(&g_admittedLock);
    return sent;
}

/** Reports whether replication may produce entity output for one peer. */
bool view_accepted(std::uint64_t sessionId) noexcept {
    return peer::view_bound(sessionId);
}

/** Copies every admitted group-session record. */
void snapshot_admitted(std::span<AdmittedRow> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_admittedLock);
    for (const Admitted& entry : g_admitted) {
        if (!entry.occupied || count >= output.size()) {
            continue;
        }
        output[count] = {entry.sessionId,
                         entry.endpoint,
                         entry.joinComplete,
                         entry.activityHostPublished,
                         entry.hasPlayer,
                         entry.playerPublished,
                         entry.joinId};
        ++count;
    }
    ReleaseSRWLockShared(&g_admittedLock);
}

/** Clears every group-session record. */
void reset() noexcept {
    g_membershipRevision.store(0);
    // Every host session goes back to State as well, or its records are stranded there.
    reset_host_sessions();
    AcquireSRWLockExclusive(&g_admittedLock);
    g_admitted = {};
    ReleaseSRWLockExclusive(&g_admittedLock);
}

} // namespace sunrise::server::gameplay::group
