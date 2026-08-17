#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::hooks::teleport {

/** One three-lane world-space position or velocity vector. */
using Vector = std::array<float, 3>;

/** Writes the local player's controlled-object handle, or the invalid sentinel. */
using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);
/** Returns the camera pose block array. The pointer in its global is obfuscated, so we call it. */
using CameraSingleton = std::byte* (*)();

void publish_targets(ControlledHandle controlled, CameraSingleton singleton) noexcept;
void clear_targets() noexcept;

/** @return True when a native object is the one currently controlled by the local player. */
[[nodiscard]] bool is_controlled_object(const void* object) noexcept;

/** Reads the local player's current physics position. */
[[nodiscard]] bool current_position(std::array<float, 3>& output) noexcept;

/** Reads the complete local controlled-object handle. */
[[nodiscard]] bool current_controlled_handle(std::uint32_t& output) noexcept;

/** Reads the latest camera position and forward vector captured by the camera hook. */
[[nodiscard]] bool current_camera_pose(std::array<float, 3>& position,
                                       std::array<float, 3>& forward) noexcept;

[[nodiscard]] bool install() noexcept;
void uninstall() noexcept;
void capture_forward(std::uint32_t playerIndex) noexcept;
void poll_request() noexcept;
void force_pending() noexcept;
[[nodiscard]] bool resolve_action_keys() noexcept;
void clear_action_keys() noexcept;
[[nodiscard]] std::uint32_t action_key(std::uint16_t binding) noexcept;
void invoke_sync(void* component) noexcept;
void apply_pending(void* component) noexcept;

/** @return True when this physics component drives the local player. */
[[nodiscard]] bool owns_local_player(void* component) noexcept;

/** Writes the driven rigid body's linear velocity. */
[[nodiscard]] bool write_velocity(void* component, const Vector& velocity) noexcept;

/** Reads the camera forward vector published by the camera hook this frame. */
[[nodiscard]] bool camera_forward(Vector& forward) noexcept;

} // namespace sunrise::client::hooks::teleport
