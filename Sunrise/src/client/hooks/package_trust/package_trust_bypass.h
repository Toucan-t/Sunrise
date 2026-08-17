#pragma once

namespace sunrise::client::hooks::package_trust {

/**
 * Accepts package RSA, extended-header hash and cached-data hash authentication. Native package
 * parsing, decompression, file-size, table and bounds validation remain active.
 */
[[nodiscard]] bool install() noexcept;
[[nodiscard]] bool uninstall() noexcept;
[[nodiscard]] bool is_installed() noexcept;

} // namespace sunrise::client::hooks::package_trust
