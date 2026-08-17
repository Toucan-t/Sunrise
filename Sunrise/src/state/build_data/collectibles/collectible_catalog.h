#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::collectibles {

/** Collections requests carry one presence bit and a 15-bit native collectible row. */
inline constexpr std::size_t kDefinitionCapacity = 1U << 15U;
/** Some collectible rows deliberately grant no inventory item. */
inline constexpr std::uint16_t kUnavailableItemDefinitionIndex = 0xFFFFU;

/** Installed collectible row mapped to the dense installed item-definition table. */
struct Definition final {
    std::uint32_t collectibleHash{};
    std::uint16_t collectibleIndex{};
    std::uint16_t itemDefinitionIndex{kUnavailableItemDefinitionIndex};
};

void clear() noexcept;
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;
[[nodiscard]] bool find(std::uint16_t collectibleIndex, Definition& definition) noexcept;
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::collectibles
