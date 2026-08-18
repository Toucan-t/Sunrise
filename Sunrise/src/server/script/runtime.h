#pragma once

#include <cstdint>

namespace sunrise::server::script {

/**
 * Starts the optional activity-session Lua scheduler.
 * The script root is a sibling `scripts` directory beside the actual loaded shim DLL.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** Reconciles joined activity sessions, hot reloads scripts, dispatches events, and runs timers. */
void service(std::uint64_t now) noexcept;

/** Stops every session script and destroys all session-owned Lua VMs. */
void shutdown() noexcept;

} // namespace sunrise::server::script
