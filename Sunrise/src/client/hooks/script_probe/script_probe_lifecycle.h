#pragma once

namespace sunrise::client::hooks::script_probe {

/**
 * Attaches ABI-neutral observation thunks to safe function-entry points in the Season of Arrivals
 * client activity-launch and authored-script paths. Pattern-resolved launch probes never hook
 * interior basic blocks, and the runtime never calls any provisional native target directly.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every installed launch/script probe before the Client runtime shuts down. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True when at least one client launch/script diagnostic probe is attached. */
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::script_probe
