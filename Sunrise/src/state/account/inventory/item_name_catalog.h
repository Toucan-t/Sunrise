#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "inventory_state.h"

namespace sunrise::state::account::inventory::item_names {

/** One item exposed by d2loadouts as a selectable equipment option. */
struct Option final {
    std::uint32_t definitionHash{};
    EquipmentSlot slot{EquipmentSlot::kinetic};
    /** -1 means the option is not class-restricted. 0/1/2 match Titan/Hunter/Warlock. */
    std::int8_t characterClass{-1};
    std::string name{};
};

/** Runtime load status for the optional d2loadouts catalogue. */
struct Status final {
    bool pathAvailable{};
    bool loaded{};
    std::size_t optionCount{};
    std::size_t nameCount{};
};

/**
 * Loads `Sunrise\\items.js` beside the other Sunrise-generated files.
 * A missing or malformed file is non-fatal; the configured loadout still works normally.
 */
void initialize(void* module) noexcept;

/** Releases catalogue strings and the resolved file path. */
void shutdown() noexcept;

/** @return Current optional catalogue status. */
[[nodiscard]] Status status() noexcept;

/** @return Selectable weapon, armour and subclass options from the loaded catalogue. */
[[nodiscard]] std::span<const Option> options() noexcept;

/** @return Display name for a known hash, or an empty view when the catalogue has none. */
[[nodiscard]] std::string_view name_for_hash(std::uint32_t definitionHash) noexcept;

} // namespace sunrise::state::account::inventory::item_names
