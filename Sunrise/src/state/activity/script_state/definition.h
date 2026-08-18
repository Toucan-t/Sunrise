#pragma once

#include <cstdint>

namespace sunrise::state::activity::script_state {

/** A cleared activity session begins at server script state zero. */
inline constexpr std::uint32_t kInitialValue = 0;
/** Revision zero is reserved for an invalid/uninitialized state snapshot. */
inline constexpr std::uint64_t kInitialRevision = 1;

/** Server-owned mission/script progression attached to one activity session. */
struct State final {
    std::uint32_t value{kInitialValue};
    std::uint64_t revision{kInitialRevision};
};

/** Result of changing the server-owned script state for one joined activity session. */
enum class MutationResult : std::uint8_t {
    ok,
    unchanged,
    notFound,
    notJoined,
    exhausted,
};

} // namespace sunrise::state::activity::script_state
