#pragma once

namespace sunrise::client::hooks::fly {

inline constexpr float kPublishedSpeedCap = 8.0F;

void poll_toggle() noexcept;
void apply(void* component) noexcept;
[[nodiscard]] bool enabled() noexcept;
void before_step(void* body) noexcept;
void after_step(void* body, bool heldElsewhere) noexcept;
void reset() noexcept;

} // namespace sunrise::client::hooks::fly
