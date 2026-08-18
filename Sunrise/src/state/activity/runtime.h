#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "definition.h"
#include "entity_slots/runtime.h"
#include "script_state/definition.h"

namespace sunrise::state::activity {

/**
 * Latest joined activity context used by diagnostics that need to correlate authored data with
 * the client's current host region. The highest joined session id wins because offline Sunrise
 * allocates a new monotonic id for each later activity session.
 */
struct CurrentContext {
    std::array<char, destination::kPackageNameCapacity> destination{};
    std::uint8_t destinationLength{};
    std::uint64_t sessionId{};
    std::int32_t regionIndex{membership::kAbsentRegionIndex};
    std::uint32_t regionHash{};
    std::int32_t teleportSliceSetIndex{membership::kAbsentSliceSetIndex};
};

/**
 * Captures the latest joined activity session without mutating State.
 * @param output Cleared, then receives destination and membership-region context.
 * @return True when at least one joined session exists.
 */
[[nodiscard]] bool current_context(CurrentContext& output) noexcept;


/** Complete script-facing view of one joined activity session. */
struct JoinedSessionSnapshot final {
    CurrentContext context{};
    std::int16_t activityIndex{destination::kAbsentActivityIndex};
    std::int16_t previousActivityIndex{destination::kAbsentActivityIndex};
    std::uint32_t scriptState{script_state::kInitialValue};
    std::uint64_t scriptStateRevision{script_state::kInitialRevision};
};

/** Fixed output matching the bounded activity-session State table. */
using JoinedSessionSnapshots = std::array<JoinedSessionSnapshot, kSessionCapacity>;

/**
 * Captures every joined activity session for the server script scheduler.
 * @param output Cleared, then receives joined sessions in State-slot order.
 * @return Number of valid entries at the beginning of output.
 */
[[nodiscard]] std::size_t joined_sessions_snapshot(JoinedSessionSnapshots& output) noexcept;

/**
 * Changes the server-owned mission/script state for one joined activity session.
 * This first scripting patch does not serialize the value to Destiny yet; it creates the exact
 * session-scoped authority point that later activity-script replication will bind to.
 * @param sessionId Joined activity session to mutate.
 * @param value New server mission state.
 * @param previous Receives the state before this request.
 * @param revision Receives the resulting script-state revision.
 */
[[nodiscard]] script_state::MutationResult set_script_state(std::uint64_t sessionId,
                                                            std::uint32_t value,
                                                            std::uint32_t& previous,
                                                            std::uint64_t& revision) noexcept;

/**
 * Prepares one allocation with State's fixed default destination, without changing State.
 * @param sessionId Cleared, then receives the picked nonzero id.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the picked record and allocator revisions can be committed.
 */
[[nodiscard]] bool prepare_session(std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Prepares one allocation with an explicit checked scalar destination.
 * @param selection Caller-owned destination, copied into the read-only allocation plan.
 * @param sessionId Cleared, then receives the picked nonzero id.
 * @param allocation Cleared, then receives the captured allocation data.
 * @return True when the destination and allocator snapshot can be committed together.
 */
[[nodiscard]] bool prepare_session(const destination::DestinationSelection& selection,
                                   std::uint64_t& sessionId,
                                   PendingAllocation& allocation) noexcept;

/**
 * Commits one prepared activity-session allocation when its revisions still match.
 * @param allocation Prepared plan. Always cleared before this function returns.
 * @return True when the allocation committed in one step.
 */
[[nodiscard]] bool commit(PendingAllocation& allocation) noexcept;

/**
 * Tests whether a nonzero activity session id is still in the fixed-size table.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when the id is in a committed record.
 */
[[nodiscard]] bool contains(std::uint64_t sessionId) noexcept;

/**
 * Tests whether a committed activity session id has finished a join.
 * @param sessionId Public activity session id from an earlier allocation.
 * @return True when the current record has a committed join revision.
 */
[[nodiscard]] bool is_joined(std::uint64_t sessionId) noexcept;

/** How far the client has got through the current destination load. */
enum class WorldPhase : std::uint8_t {
    /** No destination load is running. Orbit sits here, and the spawn is never held. */
    idle,
    /** The step that arms the black fade has started and the in-world step has not been reached. */
    transitioning,
    /** The in-world step is entered, so the fade is armed and a spawn now releases it. */
    arrived,
};

/**
 * Records how far the current destination load has got.
 * Entering transitioning from any other phase resets the load's start tick.
 * @param phase Phase the client's own boot-flow step maps to.
 */
void note_world_phase(WorldPhase phase) noexcept;

/** @return How far the client has got through the current destination load. */
[[nodiscard]] WorldPhase world_phase() noexcept;

/** @return Milliseconds since the running load started, or zero when none is running. */
[[nodiscard]] std::uint64_t world_transition_age() noexcept;

/**
 * Read-only activity values useful while correlating native runtime objects with the local host
 * simulation. This is diagnostics only; it never grants, releases, or mutates activity state.
 */
struct RuntimeProbeSnapshot final {
    CurrentContext context{};
    std::uint64_t stateRevision{};
    std::uint64_t recordRevision{};
    std::uint64_t joinedRevision{};
    entity_slots::LeaseMask heldEntitySlotMask{};
    std::size_t heldEntitySlots{};
    std::size_t firstHeldEntitySlot{entity_slots::kSlotCount};
    std::size_t lastHeldEntitySlot{entity_slots::kSlotCount};
    std::size_t grantedBubbles{};
    std::uint16_t currentBubbleGrantToken{};
    WorldPhase phase{WorldPhase::idle};
};

/**
 * Captures the newest joined session plus its slot-lease and bubble-authority summary.
 * @param output Cleared, then receives the current diagnostic snapshot.
 * @return True when a joined activity session exists.
 */
[[nodiscard]] bool runtime_probe_snapshot(RuntimeProbeSnapshot& output) noexcept;

} // namespace sunrise::state::activity
