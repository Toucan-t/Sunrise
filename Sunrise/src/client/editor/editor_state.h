#pragma once

#include <cstdint>

namespace sunrise::client::editor {

/** Stable package identity shared by every editor view. */
struct Selection {
    std::uint32_t tag{};
    bool valid{};
};

/** Shared editor state. Gameplay manipulation is added by later patches. */
struct State {
    bool editMode{};
    Selection selection{};
};

/** @return Process-lifetime editor state shared by all editor views. */
[[nodiscard]] State& state() noexcept;

/** Enables or disables editor mode without coupling it to one ImGui page. */
void set_edit_mode(bool enabled) noexcept;

/** Selects one package-backed asset by its stable TagHash identity. */
void select_asset(std::uint32_t tag) noexcept;

/** Clears the shared selection. */
void clear_selection() noexcept;

/** Clears transient editor state. */
void reset() noexcept;

} // namespace sunrise::client::editor
