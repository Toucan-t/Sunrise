#include <cstring>

#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {

/** Reads the fixed collectible table and publishes its native-index item links. */
bool build_collectibles(const reader::Source& source,
                        Storage& storage,
                        std::span<const std::byte> root,
                        std::uint64_t itemDefinitionCount) noexcept {
    namespace domain = state::build_data::collectibles;
    if (state::build_data::collectible_definitions_ready()) {
        return true;
    }
    if (itemDefinitionCount == 0
        || itemDefinitionCount > state::build_data::items::kDefinitionCapacity) {
        return false;
    }

    std::uint32_t tableTag = 0;
    std::uint32_t tableClass = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kCollectibleTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, storage.scratch, tableTag, storage.child, tableClass)
        || tableClass != tables::kCollectibleTableClass
        || !tables::find_array_at(
            std::span<const std::byte>{storage.child}, tables::kTableArrayDescriptor, rows)
        || rows.elementClass != tables::kCollectibleRowClass || rows.count == 0
        || rows.count > storage.collectibleRows.size()) {
        return false;
    }

    const std::span<const std::byte> table{storage.child};
    if (rows.dataOffset > table.size()
        || rows.count > (table.size() - rows.dataOffset) / tables::kCollectibleRowStride) {
        return false;
    }
    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kCollectibleRowStride;
        std::uint32_t collectibleHash = 0;
        std::uint16_t itemDefinitionIndex = domain::kUnavailableItemDefinitionIndex;
        std::memcpy(&collectibleHash,
                    table.data() + at + tables::kCollectibleHashOffset,
                    sizeof collectibleHash);
        std::memcpy(&itemDefinitionIndex,
                    table.data() + at + tables::kCollectibleItemIndexOffset,
                    sizeof itemDefinitionIndex);
        if (itemDefinitionIndex != domain::kUnavailableItemDefinitionIndex
            && itemDefinitionIndex >= itemDefinitionCount) {
            return false;
        }
        storage.collectibleRows[static_cast<std::size_t>(row)] = {
            collectibleHash,
            static_cast<std::uint16_t>(row),
            itemDefinitionIndex,
        };
    }
    return state::build_data::publish_collectible_definitions(
        std::span(storage.collectibleRows).first(static_cast<std::size_t>(rows.count)));
}

} // namespace sunrise::client::content::items::packages
