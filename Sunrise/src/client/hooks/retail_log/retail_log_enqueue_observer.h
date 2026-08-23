#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../hooking/detour.h"

namespace sunrise::client::hooks::retail_log {

extern SRWLOCK g_lock;
extern hooking::detour::Handle g_handle;

/** @return The enqueue observer body itself. */
[[nodiscard]] void* enqueue_entry_point() noexcept;

/** Applies the configured category threshold once the native retail-log block exists. */
void assert_verbosity() noexcept;

/** Compatibility cleanup hook; the retired Warden private-branch experiment no longer patches. */
void restore_warden_private_fast_path() noexcept;

/** @return True while the observed strike_aries authored lifecycle is active. */
[[nodiscard]] bool warden_authored_path_active() noexcept;

/** Result of a native migrated-security registration attempt. */
struct NativeSecurityRegistrationResult {
    bool ready{};
    bool invoked{};
    bool inserted{};
};

/**
 * Registers a native host-migration security pair through Destiny's own learned
 * bdSecurityKeyMap::registerKey call. The owning map is learned only from an exact byte-verified
 * ordinary registration caller; no map internals are written directly.
 */
[[nodiscard]] NativeSecurityRegistrationResult register_migrated_security_key(
    const std::array<std::byte, 8>& securityId,
    const std::array<std::byte, 16>& securityKey) noexcept;

/** Retained ABI for older content diagnostics; currently a no-op compatibility shim. */
void register_schema_marker(std::uint32_t marker) noexcept;

/** Retained ABI for older descriptor diagnostics; currently a no-op compatibility shim. */
void register_gameplay_join_descriptor(const std::byte* descriptor,
                                       std::size_t size,
                                       std::int32_t regionIndex,
                                       std::uint64_t hostSessionId) noexcept;

} // namespace sunrise::client::hooks::retail_log
