#pragma once

namespace sunrise::client::hooks::network::bubble_authority {

/** @return The roster-prefix decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/** @return The content-untracked getter replacement body. */
[[nodiscard]] void* content_untracked_entry_point() noexcept;

/** @return The confirmed native read_bits observer replacement body. */
[[nodiscard]] void* read_bits_entry_point() noexcept;

/** @return The confirmed native read_wide observer replacement body. */
[[nodiscard]] void* read_wide_entry_point() noexcept;

/** @return The confirmed native read_bool observer replacement body. */
[[nodiscard]] void* read_bool_entry_point() noexcept;

/** @return The confirmed native stream_status observer replacement body. */
[[nodiscard]] void* stream_status_entry_point() noexcept;

} // namespace sunrise::client::hooks::network::bubble_authority
