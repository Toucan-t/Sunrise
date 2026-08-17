#pragma once

#include <cstdint>

namespace sunrise::client::movement {

/** No key is bound until one is picked, so a fresh install cannot fire a movement feature. */
inline constexpr std::uint32_t kNoKey = 0;

/** Default fly speed, in world units per second. */
inline constexpr float kDefaultFlySpeed = 15.0F;
/** Slowest offered fly speed. Below this a press does not visibly move the player. */
inline constexpr float kMinimumFlySpeed = 1.0F;
/** Fastest offered fly speed. Past this the player outruns what the map streams in. */
inline constexpr float kMaximumFlySpeed = 100.0F;

/** Extra movement features layered on top of the existing Teleport module. */
struct Settings {
    bool noclipEnabled{false};
    std::uint32_t noclipToggleKey{kNoKey};
    bool swordSkateEnabled{false};
    bool flyEnabled{false};
    std::uint32_t flyToggleKey{kNoKey};
    float flySpeed{kDefaultFlySpeed};
};

/** Resolves movement.json beside the other Sunrise runtime files and loads it when present. */
void initialize(void* module) noexcept;

/** Drops the runtime configuration and the resolved file path. */
void shutdown() noexcept;

/** @return One lock-consistent copy of the current configuration. */
[[nodiscard]] Settings get() noexcept;

/** Publishes one configuration and writes it straight to disk. */
bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::movement
