#include "collectible_catalog.h"

#include <array>

#include "../table.h"

namespace sunrise::state::build_data::collectibles {
namespace {

Lock g_lock;
Table<Definition, kDefinitionCapacity> g_definitions;

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_definitions.clear();
}

bool valid(std::span<const Definition> definitions) noexcept {
    if (definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    std::array<bool, kDefinitionCapacity> occupied{};
    for (const Definition& definition : definitions) {
        if (definition.collectibleIndex >= definitions.size()
            || occupied[definition.collectibleIndex]) {
            return false;
        }
        occupied[definition.collectibleIndex] = true;
    }
    return true;
}

bool replace(std::span<const Definition> definitions) noexcept {
    if (!valid(definitions)) {
        return false;
    }
    const Lock::Exclusive guard(g_lock);
    const std::span<Definition> rows = g_definitions.reset(definitions.size());
    if (rows.size() != definitions.size()) {
        return false;
    }
    for (const Definition& definition : definitions) {
        rows[definition.collectibleIndex] = definition;
    }
    return true;
}

bool find(std::uint16_t collectibleIndex, Definition& definition) noexcept {
    definition = {};
    definition.itemDefinitionIndex = kUnavailableItemDefinitionIndex;
    const Lock::Shared guard(g_lock);
    const std::span<const Definition> rows = g_definitions.rows();
    if (static_cast<std::size_t>(collectibleIndex) >= rows.size()) {
        return false;
    }
    definition = rows[collectibleIndex];
    return true;
}

std::size_t count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_definitions.count();
}

} // namespace sunrise::state::build_data::collectibles
