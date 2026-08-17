#include <Windows.h>

#include "../runtime/storage/internal.h"
#include "runtime.h"

namespace sunrise::state::activity {

/** Captures the latest joined activity session and the region the client last reported. */
bool current_context(CurrentContext& output) noexcept {
    output = {};
    output.regionIndex = membership::kAbsentRegionIndex;
    output.teleportSliceSetIndex = membership::kAbsentSliceSetIndex;

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const SessionRecord* selected = nullptr;
    for (const SessionRecord& record : state.sessions) {
        if (!record.occupied || !record.joined || record.joinedRevision == kInvalidRevision) {
            continue;
        }
        if (selected == nullptr || record.sessionId > selected->sessionId) {
            selected = &record;
        }
    }
    if (selected != nullptr) {
        output.sessionId = selected->sessionId;
        const std::size_t length = static_cast<std::size_t>(selected->destination.packageNameLength);
        output.destinationLength = selected->destination.packageNameLength;
        for (std::size_t index = 0; index < length && index < output.destination.size(); ++index) {
            output.destination[index] = static_cast<char>(selected->destination.packageName[index]);
        }
        output.regionIndex = selected->membership.region.index;
        output.regionHash = selected->membership.region.hash;
        output.teleportSliceSetIndex = selected->membership.teleport.sliceSetIndex;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return selected != nullptr;
}

/** Tests whether a nonzero activity-session id is still in the bounded table. */
bool contains(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool found = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return found;
}

/** Tests whether a committed activity-session id has finished a join. */
bool is_joined(std::uint64_t sessionId) noexcept {
    if (sessionId == kAbsentSessionId) {
        return false;
    }
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    bool joined = false;
    for (const SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            joined = record.joined && record.joinedRevision != kInvalidRevision;
            break;
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return joined;
}

/** Captures the newest joined session plus its runtime lease/authority diagnostics. */
bool runtime_probe_snapshot(RuntimeProbeSnapshot& output) noexcept {
    output = {};
    output.context.regionIndex = membership::kAbsentRegionIndex;
    output.context.teleportSliceSetIndex = membership::kAbsentSliceSetIndex;
    output.firstHeldEntitySlot = entity_slots::kSlotCount;
    output.lastHeldEntitySlot = entity_slots::kSlotCount;

    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    const SessionRecord* selected = nullptr;
    for (const SessionRecord& record : state.sessions) {
        if (!record.occupied || !record.joined || record.joinedRevision == kInvalidRevision) {
            continue;
        }
        if (selected == nullptr || record.sessionId > selected->sessionId) {
            selected = &record;
        }
    }

    if (selected != nullptr) {
        output.stateRevision = state.stateRevision;
        output.recordRevision = selected->recordRevision;
        output.joinedRevision = selected->joinedRevision;
        output.context.sessionId = selected->sessionId;
        const std::size_t length =
            static_cast<std::size_t>(selected->destination.packageNameLength);
        output.context.destinationLength = selected->destination.packageNameLength;
        for (std::size_t index = 0; index < length && index < output.context.destination.size();
             ++index) {
            output.context.destination[index] =
                static_cast<char>(selected->destination.packageName[index]);
        }
        output.context.regionIndex = selected->membership.region.index;
        output.context.regionHash = selected->membership.region.hash;
        output.context.teleportSliceSetIndex = selected->membership.teleport.sliceSetIndex;

        output.heldEntitySlotMask = selected->heldEntitySlots;
        for (std::size_t byteIndex = 0; byteIndex < selected->heldEntitySlots.size(); ++byteIndex) {
            const auto value = std::to_integer<std::uint8_t>(selected->heldEntitySlots[byteIndex]);
            for (std::size_t bit = 0; bit < entity_slots::kSlotsPerByte; ++bit) {
                if ((value & static_cast<std::uint8_t>(1U << bit)) == 0) {
                    continue;
                }
                const std::size_t slot = byteIndex * entity_slots::kSlotsPerByte + bit;
                if (output.heldEntitySlots == 0) {
                    output.firstHeldEntitySlot = slot;
                }
                output.lastHeldEntitySlot = slot;
                ++output.heldEntitySlots;
            }
        }

        for (const std::uint16_t token : selected->bubbleAuthority.grantTokens) {
            if (token != 0) {
                ++output.grantedBubbles;
            }
        }
        if (output.context.regionIndex >= 0
            && output.context.regionIndex <= bubble_authority::kMaximumGrantSliceSetIndex) {
            const auto bubble = static_cast<std::size_t>(
                output.context.regionIndex >> bubble_authority::kSliceSetToBubbleShift);
            if (bubble < selected->bubbleAuthority.grantTokens.size()) {
                output.currentBubbleGrantToken =
                    selected->bubbleAuthority.grantTokens[bubble];
            }
        }
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    output.phase = world_phase();
    return selected != nullptr;
}

} // namespace sunrise::state::activity
