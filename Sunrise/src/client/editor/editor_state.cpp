#include "editor_state.h"

namespace sunrise::client::editor {
namespace {

State g_state{};

} // namespace

/** @return Shared process-lifetime editor state. */
State& state() noexcept {
    return g_state;
}

/** Changes the persistent editor-mode switch. */
void set_edit_mode(bool enabled) noexcept {
    g_state.editMode = enabled;
}

/** Changes the shared asset selection. */
void select_asset(std::uint32_t tag) noexcept {
    g_state.selection = Selection{tag, true};
}

/** Clears only the current selection. */
void clear_selection() noexcept {
    g_state.selection = {};
}

/** Clears every editor-foundation state field. */
void reset() noexcept {
    g_state = {};
}

} // namespace sunrise::client::editor
