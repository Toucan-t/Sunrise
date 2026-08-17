#pragma once

namespace sunrise::client::input {

/** @return True while a window owned by the game process is in the foreground. */
[[nodiscard]] bool game_focused() noexcept;

} // namespace sunrise::client::input
