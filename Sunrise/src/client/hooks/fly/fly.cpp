/**
 * Velocity fly. The movement keys set the player's velocity every tick. Noclip's Havok-step hook
 * carries that velocity through collision geometry when both switches are enabled.
 */

#include "fly.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../noclip/runtime.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::fly {
namespace {

namespace bindings = state::account::settings::bindings;

constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
constexpr float kMinimumLengthSquared = 0.000001F;
constexpr std::size_t kLaneX = 0;
constexpr std::size_t kLaneY = 1;
constexpr std::size_t kVerticalLane = 2;
constexpr std::size_t kVectorLanes = 3;

enum class Direction : std::size_t {
    forward,
    backward,
    left,
    right,
    up,
    down,
    count,
};

constexpr std::size_t kDirectionCount = static_cast<std::size_t>(Direction::count);

struct ActionDirection {
    bindings::Action action;
    Direction direction;
};

constexpr std::array<ActionDirection, 7> kActions{{
    {bindings::Action::moveForward, Direction::forward},
    {bindings::Action::moveBackward, Direction::backward},
    {bindings::Action::moveLeft, Direction::left},
    {bindings::Action::moveRight, Direction::right},
    {bindings::Action::jump, Direction::up},
    {bindings::Action::toggleCrouch, Direction::down},
    {bindings::Action::holdCrouch, Direction::down},
}};

std::array<bindings::Binding, kActions.size()> g_bindings{};
bool g_bindingsRead{false};
std::atomic_bool g_toggleDown{false};
float g_heightBeforeStep{0.0F};
bool g_heightValid{false};
bool g_steered{false};

[[nodiscard]] bool read_bindings() noexcept {
    if (g_bindingsRead) {
        return true;
    }
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const std::size_t action = static_cast<std::size_t>(kActions[index].action);
        g_bindings[index] = account.settings.keyBindings.values[action];
    }
    g_bindingsRead = true;
    return true;
}

[[nodiscard]] bool half_down(const std::optional<std::uint16_t>& half) noexcept {
    if (!half.has_value()) {
        return false;
    }
    const std::uint32_t key = teleport::action_key(*half);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0;
}

[[nodiscard]] std::array<bool, kDirectionCount> pressed_directions() noexcept {
    std::array<bool, kDirectionCount> pressed{};
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const bindings::Binding& binding = g_bindings[index];
        if (half_down(binding.primary) || half_down(binding.secondary)) {
            pressed[static_cast<std::size_t>(kActions[index].direction)] = true;
        }
    }
    return pressed;
}

[[nodiscard]] teleport::Vector right_of(const teleport::Vector& forward) noexcept {
    teleport::Vector right{forward[kLaneY], -forward[kLaneX], 0.0F};
    const float lengthSquared = right[kLaneX] * right[kLaneX] + right[kLaneY] * right[kLaneY];
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    const float length = std::sqrt(lengthSquared);
    right[kLaneX] /= length;
    right[kLaneY] /= length;
    return right;
}

[[nodiscard]] teleport::Vector travel(const std::array<bool, kDirectionCount>& pressed,
                                      const teleport::Vector& forward) noexcept {
    const teleport::Vector right = right_of(forward);
    teleport::Vector move{};
    const auto add = [&move](const teleport::Vector& axis, float scale) noexcept {
        for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
            move[lane] += axis[lane] * scale;
        }
    };
    if (pressed[static_cast<std::size_t>(Direction::forward)]) {
        add(forward, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::backward)]) {
        add(forward, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::right)]) {
        add(right, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::left)]) {
        add(right, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::up)]) {
        move[kVerticalLane] += 1.0F;
    }
    if (pressed[static_cast<std::size_t>(Direction::down)]) {
        move[kVerticalLane] -= 1.0F;
    }
    float lengthSquared = 0.0F;
    for (const float lane : move) {
        lengthSquared += lane * lane;
    }
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    const float length = std::sqrt(lengthSquared);
    for (float& lane : move) {
        lane /= length;
    }
    return move;
}

void cap_speed(teleport::Vector& velocity, float limit) noexcept {
    float speedSquared = 0.0F;
    for (const float lane : velocity) {
        speedSquared += lane * lane;
    }
    if (speedSquared <= limit * limit) {
        return;
    }
    const float scale = limit / std::sqrt(speedSquared);
    for (float& lane : velocity) {
        lane *= scale;
    }
}

[[nodiscard]] teleport::Vector desired_velocity(float speed) noexcept {
    std::array<bool, kDirectionCount> pressed{};
    if (!core::ui::runtime::snapshot().visible && input::game_focused()) {
        pressed = pressed_directions();
    }
    teleport::Vector forward{};
    const teleport::Vector move =
        teleport::camera_forward(forward) ? travel(pressed, forward) : teleport::Vector{};
    g_steered = move[kVerticalLane] != 0.0F;
    teleport::Vector velocity{};
    for (std::size_t lane = 0; lane < kVectorLanes; ++lane) {
        velocity[lane] = move[lane] * speed;
    }
    return velocity;
}

} // namespace

void poll_toggle() noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (settings.flyToggleKey == client::movement::kNoKey) {
        g_toggleDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down =
        input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.flyToggleKey)) & kKeyHeldBit) != 0;
    if (core::ui::runtime::snapshot().visible) {
        g_toggleDown.store(down, std::memory_order_relaxed);
        return;
    }
    if (down && !g_toggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::movement::Settings updated = settings;
        updated.flyEnabled = !settings.flyEnabled;
        if (!client::movement::publish(updated)) {
            return;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.flyEnabled ? "ev=fly stage=toggle enabled=1"
                                            : "ev=fly stage=toggle enabled=0");
        return;
    }
    if (!down) {
        g_toggleDown.store(false, std::memory_order_release);
    }
}

void apply(void* component) noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (!settings.flyEnabled || component == nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    if (!read_bindings()) {
        return;
    }
    teleport::Vector velocity = desired_velocity(settings.flySpeed);
    cap_speed(velocity, kPublishedSpeedCap);
    (void)teleport::write_velocity(component, velocity);
}

bool enabled() noexcept {
    return client::movement::get().flyEnabled;
}

void before_step(void* body) noexcept {
    g_heightValid = false;
    if (body == nullptr || !read_bindings()) {
        return;
    }
    noclip::write_body_velocity(body, desired_velocity(client::movement::get().flySpeed));
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    g_heightBeforeStep = position[kVerticalLane];
    g_heightValid = true;
}

void after_step(void* body, bool heldElsewhere) noexcept {
    if (body == nullptr || heldElsewhere || g_steered || !g_heightValid) {
        return;
    }
    noclip::Vector position{};
    noclip::read_body_position(body, position);
    position[kVerticalLane] = g_heightBeforeStep;
    noclip::write_body_position(body, position);

    noclip::Vector velocity{};
    noclip::read_body_velocity(body, velocity);
    velocity[kVerticalLane] = 0.0F;
    noclip::write_body_velocity(body, velocity);
}

void reset() noexcept {
    g_toggleDown.store(false, std::memory_order_release);
    g_heightValid = false;
}

} // namespace sunrise::client::hooks::fly
