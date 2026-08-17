#include "window_focus.h"

#include <Windows.h>

namespace sunrise::client::input {

bool game_focused() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD processId = 0;
    (void)GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

} // namespace sunrise::client::input
