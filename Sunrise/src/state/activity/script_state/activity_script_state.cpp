#include <Windows.h>

#include <cstddef>
#include <cstdint>

#include "../../runtime/storage/internal.h"
#include "../runtime.h"

namespace sunrise::state::activity {
namespace {

/** Copies one joined record into the script-facing scalar snapshot. */
void copy_record(const SessionRecord& record, JoinedSessionSnapshot& output) noexcept {
    output = {};
    output.context.regionIndex = membership::kAbsentRegionIndex;
    output.context.teleportSliceSetIndex = membership::kAbsentSliceSetIndex;
    output.context.sessionId = record.sessionId;
    const std::size_t length = static_cast<std::size_t>(record.destination.packageNameLength);
    output.context.destinationLength = record.destination.packageNameLength;
    for (std::size_t index = 0; index < length && index < output.context.destination.size(); ++index) {
        output.context.destination[index] = static_cast<char>(record.destination.packageName[index]);
    }
    output.context.regionIndex = record.membership.region.index;
    output.context.regionHash = record.membership.region.hash;
    output.context.teleportSliceSetIndex = record.membership.teleport.sliceSetIndex;
    output.activityIndex = record.destination.activityIndex;
    output.previousActivityIndex = record.destination.previousActivityIndex;
    output.scriptState = record.scriptState.value;
    output.scriptStateRevision = record.scriptState.revision;
}

} // namespace

/** Captures every currently joined activity session without exposing mutable State storage. */
std::size_t joined_sessions_snapshot(JoinedSessionSnapshots& output) noexcept {
    output = {};
    std::size_t count = 0;
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const ActivityState& state = runtime::storage::g_state.activity;
    for (const SessionRecord& record : state.sessions) {
        if (!record.occupied || !record.joined || record.joinedRevision == kInvalidRevision) {
            continue;
        }
        if (count >= output.size()) {
            break;
        }
        copy_record(record, output[count]);
        ++count;
    }
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return count;
}

/** Changes only Sunrise's server-owned script state; Destiny publication is intentionally pending. */
script_state::MutationResult set_script_state(std::uint64_t sessionId,
                                              std::uint32_t value,
                                              std::uint32_t& previous,
                                              std::uint64_t& revision) noexcept {
    previous = script_state::kInitialValue;
    revision = 0;
    if (sessionId == kAbsentSessionId) {
        return script_state::MutationResult::notFound;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    ActivityState& state = runtime::storage::g_state.activity;
    SessionRecord* selected = nullptr;
    for (SessionRecord& record : state.sessions) {
        if (record.occupied && record.sessionId == sessionId) {
            selected = &record;
            break;
        }
    }
    if (selected == nullptr) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return script_state::MutationResult::notFound;
    }
    previous = selected->scriptState.value;
    revision = selected->scriptState.revision;
    if (!selected->joined || selected->joinedRevision == kInvalidRevision) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return script_state::MutationResult::notJoined;
    }
    if (selected->scriptState.value == value) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return script_state::MutationResult::unchanged;
    }
    if (state.stateRevision == kMaximumRevision
        || selected->scriptState.revision == kMaximumRevision) {
        ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
        return script_state::MutationResult::exhausted;
    }

    ++state.stateRevision;
    selected->recordRevision = state.stateRevision;
    selected->scriptState.value = value;
    ++selected->scriptState.revision;
    revision = selected->scriptState.revision;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return script_state::MutationResult::ok;
}

} // namespace sunrise::state::activity
