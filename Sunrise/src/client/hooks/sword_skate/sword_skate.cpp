/**
 * A sword's air attack sets one movement-state bit that refuses a glide. Clearing only that bit on
 * the tick jump is pressed retains the sword throw but lets the client start its ordinary glide.
 */

#include "sword_skate.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::sword_skate {
namespace {

constexpr std::size_t kMovementStateOffset = 15492;
constexpr std::uint32_t kGlideRefusedBit = 0x00000800U;
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
constexpr std::uint16_t kJumpAction =
    static_cast<std::uint16_t>(state::account::settings::bindings::Action::jump);

bool g_jumpHeld{false};
std::array<std::optional<std::uint16_t>, 2> g_jumpBinding{};
bool g_bindingRead{false};

[[nodiscard]] bool read_at(const std::byte* address, std::uint32_t& value) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

[[nodiscard]] bool write_at(std::byte* address, std::uint32_t value) noexcept {
    SIZE_T written = 0;
    return WriteProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &written) != FALSE
           && written == sizeof value;
}

[[nodiscard]] bool read_jump_binding() noexcept {
    if (g_bindingRead) {
        return true;
    }
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    const auto& binding = account.settings.keyBindings.values[kJumpAction];
    g_jumpBinding = {binding.primary, binding.secondary};
    g_bindingRead = true;
    return true;
}

[[nodiscard]] bool jump_down() noexcept {
    for (const std::optional<std::uint16_t>& half : g_jumpBinding) {
        if (!half.has_value()) {
            continue;
        }
        const std::uint32_t key = teleport::action_key(*half);
        if (key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

void apply(void* component) noexcept {
    const client::movement::Settings settings = client::movement::get();
    const bool usable = settings.swordSkateEnabled && !core::ui::runtime::snapshot().visible
                        && input::game_focused();
    if (!usable) {
        g_jumpHeld = false;
        return;
    }
    if (component == nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    if (!read_jump_binding()) {
        return;
    }
    const bool held = jump_down();
    const bool wasHeld = g_jumpHeld;
    g_jumpHeld = held;
    if (!held || wasHeld) {
        return;
    }
    auto* const bytes = static_cast<std::byte*>(component);
    std::uint32_t state = 0;
    if (!read_at(bytes + kMovementStateOffset, state) || (state & kGlideRefusedBit) == 0) {
        return;
    }
    (void)write_at(bytes + kMovementStateOffset, state & ~kGlideRefusedBit);
}

} // namespace sunrise::client::hooks::sword_skate
