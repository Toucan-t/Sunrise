#pragma once

#include <array>

namespace sunrise::client::hooks::noclip {

using Vector = std::array<float, 3>;

[[nodiscard]] bool install() noexcept;
void uninstall() noexcept;
void read_body_position(void* body, Vector& position) noexcept;
void write_body_position(void* body, const Vector& position) noexcept;
void read_body_velocity(void* body, Vector& velocity) noexcept;
void write_body_velocity(void* body, const Vector& velocity) noexcept;

} // namespace sunrise::client::hooks::noclip
