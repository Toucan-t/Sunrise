#pragma once

namespace sunrise::client::hooks::network::investment {

/** @return True when freshness and both real-arrival rebuild arms are attached. */
[[nodiscard]] bool install() noexcept;

/** @return True when every investment rebuild detour is absent. */
[[nodiscard]] bool uninstall() noexcept;

/** @return True while freshness and both real-arrival rebuild arms are attached. */
[[nodiscard]] bool is_installed() noexcept;

/** @return True while any investment rebuild detour still needs cleanup. */
[[nodiscard]] bool has_ownership() noexcept;

/** Logs a bounded sample of the next native Family-4 lookup calls for presentation diagnostics. */
void arm_family4_lookup_trace() noexcept;

} // namespace sunrise::client::hooks::network::investment
