#pragma once

#include <cstddef>
#include <span>

#include "../account_state.h"

namespace sunrise::state::account::characters {

/** Process view of the local character override store. */
struct StoreStatus final {
    bool available{};
    bool loaded{};
    bool rejected{};
    bool lastSaveOk{true};
    bool lastSettingsMirrorOk{true};
};

/**
 * Captures the settings-authored roster, loads the DLL-bundled three-class factory templates, and
 * overlays the persistent character store. A missing/rejected store or factory resource never
 * prevents boot.
 * @param module Loaded Sunrise module used for the owned artifact directory and factory resource.
 * @param authored Complete account parsed from settings.json. It remains the compatibility/reset
 * source.
 * @param active Receives authored or persisted character rows while retaining account/profile data.
 */
void initialize(void* module, const AccountState& authored, AccountState& active) noexcept;

/** Drops the stored path and immutable process-lifetime templates. */
void shutdown() noexcept;

/**
 * Saves the exact bit-packed opcode-501 payload beside the character store for protocol analysis.
 * This diagnostic artifact never becomes authoritative character state by itself.
 * @param captureIndex Receives the numbered capture suffix (001..9999) on success.
 * @return True only after a unique capture file is completely written and flushed.
 */
[[nodiscard]] bool capture_creation_request(std::span<const std::byte> payload,
                                             std::uint32_t& captureIndex) noexcept;

/**
 * Atomically replaces the character override file with the account's dense character rows.
 * Selection and character SOIDs are runtime identities and are deliberately not persisted.
 * @return True only after the staged file is flushed and renamed over the previous store.
 */
[[nodiscard]] bool persist(const AccountState& account) noexcept;

/**
 * Mirrors the runtime character/equipment rows into settings.json without making that file the
 * live source of truth. The rest of the user's document is retained byte-for-byte.
 * Runtime character SOIDs are rebased back onto the authored account key before serialization.
 * @return True only after the replacement document parses and is atomically installed.
 */
[[nodiscard]] bool mirror_settings(const AccountState& account) noexcept;

/**
 * Finds one immutable class starter template. The DLL-bundled three-class factory set is preferred
 * so runtime creation never depends on which classes happen to exist in settings.json.
 */
[[nodiscard]] bool template_for_class(CharacterClass characterClass,
                                      CharacterState& output) noexcept;

/**
 * Returns the complete factory template account used to prefetch configured item details and
 * ability rows. Falls back to the boot-authored account if the bundled factory resource is absent.
 */
[[nodiscard]] AccountState configured_account() noexcept;

/** Returns the boot-authored account used only by the explicit reset-to-settings action. */
[[nodiscard]] AccountState authored_account() noexcept;

/** @return Current persistence availability/load/save status. */
[[nodiscard]] StoreStatus status() noexcept;

} // namespace sunrise::state::account::characters
