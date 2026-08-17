#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::items::socket_plugs {

void clear() noexcept;
[[nodiscard]] bool valid(std::span<const Rule> rules,
                         std::span<const Pool> pools,
                         std::span<const Member> members) noexcept;
[[nodiscard]] bool replace(std::span<const Rule> rules,
                           std::span<const Pool> pools,
                           std::span<const Member> members) noexcept;
[[nodiscard]] bool allowed(std::uint16_t itemDefinitionIndex,
                           std::uint8_t lane,
                           std::uint16_t plugDefinitionIndex) noexcept;
[[nodiscard]] bool contains(Member plugDefinitionIndex) noexcept;
[[nodiscard]] bool pool_members(std::uint16_t itemDefinitionIndex,
                                std::uint8_t lane,
                                std::span<Member> output,
                                std::size_t& count) noexcept;
[[nodiscard]] std::size_t rule_count() noexcept;

} // namespace sunrise::state::build_data::items::socket_plugs
