#include "../runtime.h"
#include "collectible_catalog.h"

namespace sunrise::state::build_data {
namespace {

[[nodiscard]] bool valid_collectible_publication(
    std::span<const collectibles::Definition> definitions) noexcept {
    if (!item_definitions_ready() || !collectibles::valid(definitions)) {
        return false;
    }
    const std::size_t itemCount = item_definition_count();
    for (const collectibles::Definition& definition : definitions) {
        if (definition.itemDefinitionIndex == collectibles::kUnavailableItemDefinitionIndex) {
            continue;
        }
        items::Definition item{};
        if (definition.itemDefinitionIndex >= itemCount
            || !items::find_index(definition.itemDefinitionIndex, item)
            || item.definitionIndex != definition.itemDefinitionIndex) {
            return false;
        }
    }
    return true;
}

} // namespace

bool collectible_definitions_ready() noexcept {
    return collectibles::count() != 0;
}

bool publish_collectible_definitions(
    std::span<const collectibles::Definition> definitions) noexcept {
    // Collections links are intentionally a transient runtime domain in this patch. The existing
    // build-data cache predates them and freezes persisted domains after load, so this table is
    // re-read from the installed investment root each process and protected by its own catalog lock.
    return valid_collectible_publication(definitions) && collectibles::replace(definitions);
}

bool find_collectible_item_definition_index(std::uint16_t collectibleIndex,
                                            std::uint16_t& itemDefinitionIndex) noexcept {
    itemDefinitionIndex = collectibles::kUnavailableItemDefinitionIndex;
    collectibles::Definition definition{};
    if (!collectible_definitions_ready() || !collectibles::find(collectibleIndex, definition)
        || definition.collectibleIndex != collectibleIndex
        || definition.itemDefinitionIndex == collectibles::kUnavailableItemDefinitionIndex) {
        return false;
    }
    itemDefinitionIndex = definition.itemDefinitionIndex;
    return true;
}

} // namespace sunrise::state::build_data
