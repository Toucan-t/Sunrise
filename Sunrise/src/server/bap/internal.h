#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../client/network/consumer.h"
#include "../../middleware/bap/activity_message/activity_patch_epoch_parser.h"
#include "../../middleware/bap/frame.h"
#include "../../state/build_data/scenarios/definition.h"
#include "../../state/runtime/state.h"
#include "encrypted/queuez/definition.h"

namespace sunrise::server::bap {

/** One session per transport peer slot, so a connection id indexes this array directly. */
inline constexpr std::size_t kSessionCount = client::network::kBapConnectionCount;

/** Fixed scratch storage owned by the lock, kept off the Client thread's stack. */
struct Scratch {
    std::array<std::byte, client::network::kBapFrameCapacity> plaintext{};
    std::array<std::byte, client::network::kBapFrameCapacity> responseBody{};
    std::array<std::byte, client::network::kBapFrameCapacity> responsePayload{};
    std::array<std::byte, client::network::kBapFrameCapacity> sealed{};
    std::array<std::byte, client::network::kBapFrameCapacity> framed{};
    /** Roster groups the outbound body's slot spans point into. */
    std::array<state::build_data::scenarios::RosterGroup,
               state::build_data::scenarios::kDestinationGroupCapacity>
        rosterGroups{};
};

/** Mutable transport state owned by one BAP connection. */
struct Session {
    std::uint32_t id{};
    bool authenticated{};
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::array<std::byte, state::kBapNonceSize> receiveNonce{};
    /** Opaque State handle taken only after the server hello authenticates. */
    state::matchmaking::ContextHandle matchmakingContext{};
    /** Activity capability allocated and published through this authenticated session. */
    std::uint64_t activitySessionId{};
    /** Tick count after which the activity link owes its next keepalive write. */
    std::uint64_t activityKeepaliveDueTick{};
    /** Client member key from the join request. It seeds the membership id. */
    std::uint64_t activityMemberKey{};
    /**
     * Character the join request named, or zero when it carried none.
     * The roster's participation key must be the character the client signed in on. The client
     * binds its player by matching that value.
     */
    std::uint64_t activityCharacterSoid{};
    /** Tick count after which the activity link owes its next roster update. */
    std::uint64_t activityRosterDueTick{};
    /**
     * Tick count until which the client is loading, so the roster runs at its faster cadence.
     * A join and a transition-token change are the only two things that open it.
     */
    std::uint64_t activityTransitionUntilTick{};
    /** The client's own patch epoch, from message 52. The roster body splices it verbatim. */
    middleware::bap::activity_message::patch_epoch::PatchEpoch activityPatchEpoch{};
    /** Group set the last roster update published, folded into one comparable value. */
    std::uint32_t activityRosterGroups{};
    /** Roster updates sent on this connection, capped once the warm-up bumps are spent. */
    std::uint8_t activityRosterSends{};
    /** Per-entry state byte the last roster update carried. */
    std::uint8_t activityRosterState{};
    /** Set once message 52 has arrived, which is what makes a roster update sendable. */
    bool activityPatchEpochSeen{};
    /**
     * Reason code of the last logged roster outcome.
     * The push runs every second, so a refusal is logged only when the reason changes. One flag
     * for every reason hides the second failure behind the first.
     */
    std::uint8_t activityRosterReason{};
    /** Queuez versions and residents published only through this authenticated peer. */
    encrypted::queuez::SessionState queuez{};
    /** Tick count after which the owed Family-4 re-push may go out. */
    std::uint64_t family4RepushDueTick{};
    /** Root the owed re-push must use. */
    std::uint64_t family4RepushRoot{};
    /** True while one Family-4 re-push is still owed to this peer. */
    bool family4RepushArmed{};
    /** Tick count after which the owed banner re-push may go out. */
    std::uint64_t bannerRepushDueTick{};
    /** Root the owed banner re-push must use. */
    std::uint64_t bannerRepushRoot{};
    /** True while one banner re-push is still owed to this peer. */
    bool bannerRepushArmed{};
    /** Newly debug-created item resident that must enter Family 4 before its character references it. */
    std::uint64_t inventoryAddInstanceSoid{};
    /** Selected owner of the newly created unequipped debug item. */
    std::uint64_t inventoryAddCharacterSoid{};
    /** Debug inventory acquisition is waiting for item-addition then character refresh publication. */
    bool inventoryAddRefreshPending{};
    /** Equipped item instance whose body changed but whose resident identity stayed stable. */
    std::uint64_t equipmentRefreshInstanceSoid{};
    /** Character that owns the equipped item awaiting a dependency-ordered refresh. */
    std::uint64_t equipmentRefreshCharacterSoid{};
    /** Equipped definition replacement is waiting for item -> character -> mirror publication. */
    bool equipmentRefreshPending{};
    /** Owned item instance whose sockets changed but whose resident identity stayed stable. */
    std::uint64_t socketRefreshInstanceSoid{};
    /** Character that owns the item awaiting a socket/body refresh. */
    std::uint64_t socketRefreshCharacterSoid{};
    /** Equipped socket edits include the resident character and later appearance mirrors. */
    bool socketRefreshIncludeCharacter{};
    /** Debug socket mutation is waiting for its Family-4 item/body publication. */
    bool socketRefreshPending{};
    /** Newly added profile shader/mod action-source resident, zero for account-only stack edits. */
    std::uint64_t profileRefreshInstanceSoid{};
    /** Profile refresh must add the named resident before publishing the account after-image. */
    bool profileRefreshAddResident{};
    /** Debug profile inventory mutation is waiting for its Family-4 publication. */
    bool profileRefreshPending{};
    /** Editor item mutation owes a manifest-preserving Family-4 object refresh. */
    bool accountRefreshPending{};
    /** Newly created character whose item-instance residents still have to be added to Family 4. */
    std::uint64_t createdCharacterRefreshSoid{};
    /** Opcode-501 post-create Family-4 item publication is waiting for a poll. */
    bool createdCharacterRefreshPending{};
    /** Newly created Guardian that should become the resident selected character after its roster lands. */
    std::uint64_t createdCharacterSelectSoid{};
    /** Post-create Family-4 selection + Family-0 move is waiting for a poll. */
    bool createdCharacterSelectPending{};
    /** Stable character key whose metadata needs companion Queuez object refreshes. */
    std::uint64_t characterRefreshSoid{};
    /** Character metadata mutation is waiting for its Family-4/3/0 refresh chain. */
    bool characterRefreshPending{};
    /** Next editor Family-3 incremental version. Initial subscription is version zero. */
    std::int32_t editorFamily3Version{};
    /** Character key for a metadata-only Family-3 record refresh; zero means broad item refresh. */
    std::uint64_t rosterRefreshCharacterSoid{};
    /** An editor refresh owes a Family-3 update. */
    bool rosterRefreshPending{};
    /** The broad Family-3 refresh was armed by opcode 501 rather than an editor item mutation. */
    bool rosterRefreshFromCreate{};
    /** Character key for a metadata-only Family-0 record refresh; zero means broad item refresh. */
    std::uint64_t bannerRefreshCharacterSoid{};
    /** An editor refresh owes a selected-character Family-0 update. */
    bool bannerRefreshPending{};
    /** Runtime character/equipment State should be checkpointed once this refresh chain completes. */
    bool editorPersistencePending{};
    /** One required object in the current editor refresh chain failed to publish. */
    bool editorRefreshFailed{};
};

namespace plaintext {

/**
 * Handles plaintext bootstrap services, arms encryption after service 25, and routes the rest.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when the service owes no reply, or its response is encoded.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

} // namespace plaintext

namespace encrypted {

/**
 * Authenticates and routes one encrypted post-bootstrap service frame.
 * @param session Auth and nonce state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param outer Validated encrypted outer frame.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded response size in bytes.
 * @return True when routing works, any response fits, State commits and the nonce is published.
 */
[[nodiscard]] bool consume(Session& session,
                           Scratch& scratch,
                           const middleware::bap::OuterFrame& outer,
                           std::span<std::byte> response,
                           std::size_t& written) noexcept;

/**
 * Sends the owed Family-4 re-push once its delay has passed.
 * @param session Auth, nonce and queuez state owned by the connection.
 * @param scratch Transform buffers owned by the lock, kept off the Client thread stack.
 * @param response Whole-frame storage owned by the caller.
 * @param written Gets the encoded notification size in bytes.
 * @param touchesScratch Set before any scratch buffer is used.
 * @return True when a whole Family-4 notification is published.
 */
[[nodiscard]] bool consume_deferred(Session& session,
                                    Scratch& scratch,
                                    std::span<std::byte> response,
                                    std::size_t& written,
                                    bool& touchesScratch) noexcept;

} // namespace encrypted

} // namespace sunrise::server::bap
