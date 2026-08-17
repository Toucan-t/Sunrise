#pragma once

namespace sunrise::client::player {

struct Settings {
    bool infiniteAmmoEnabled{false};
};

void initialize(void* module) noexcept;
void shutdown() noexcept;
[[nodiscard]] Settings get() noexcept;
bool publish(const Settings& settings) noexcept;

} // namespace sunrise::client::player
