#pragma once

#include <cstddef>
#include <cstdint>

#include "../details/definition.h"
#include "../item_catalog.h"

namespace sunrise::state::build_data::items::socket_plugs {

inline constexpr std::size_t kLaneCapacity = details::kInitialPlugCapacity;
inline constexpr std::size_t kRuleCapacity = items::kDefinitionCapacity * kLaneCapacity;
inline constexpr std::size_t kPoolCapacity = kRuleCapacity + 1;
inline constexpr std::size_t kMemberDefinitionCapacity = items::kDefinitionCapacity;
inline constexpr std::size_t kMemberCapacity = 1U << 22U;
inline constexpr std::uint32_t kEmptyPoolIndex = 0;

struct Rule {
    std::uint16_t itemDefinitionIndex{};
    std::uint8_t lane{};
    std::uint8_t reserved{};
    std::uint32_t poolIndex{};
};

struct Pool {
    std::uint32_t memberOffset{};
    std::uint32_t memberCount{};
};

using Member = std::uint16_t;

} // namespace sunrise::state::build_data::items::socket_plugs
