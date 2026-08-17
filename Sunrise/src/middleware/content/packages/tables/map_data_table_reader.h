#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of the activity resource reached from a placed object's per-bubble handle. */
inline constexpr std::uint32_t kActivityEntityResourceClass = 0x80809468U;
/** The activity resource names its map-data table at this offset. */
inline constexpr std::size_t kActivityEntityTableTagOffset = 8;

/** Tag class of the supported build's map-data table. */
inline constexpr std::uint32_t kMapDataTableClass = 0x808099D6U;
/** Element class of a map-data table's entry array. */
inline constexpr std::uint32_t kMapDataEntryClass = 0x808099D8U;
/** A map-data table holds its entry-array descriptor immediately after the file-size prefix. */
inline constexpr std::size_t kMapDataTableEntryDescriptor = 8;
/** One map-data entry is 0x90 inline bytes in the supported build. */
inline constexpr std::size_t kMapDataEntryStride = 0x90;

/** Measured map-data-entry field offsets for the supported build. */
inline constexpr std::size_t kMapDataEntryEntityTagOffset = 0x00;
inline constexpr std::size_t kMapDataEntryRotationOffset = 0x10;
inline constexpr std::size_t kMapDataEntryTranslationOffset = 0x20;
inline constexpr std::size_t kMapDataEntryWorldIdOffset = 0x70;
/** A map-data entry carries an inline ResourcePointer to its type-specific data here. */
inline constexpr std::size_t kMapDataEntryDataResourcePointerOffset = 0x78;

/** One four-component float field from a map-data entry. */
struct Float4 {
    float x{};
    float y{};
    float z{};
    float w{};
};

/** The fields needed to identify and place one authored entity entry. */
struct MapDataEntry {
    std::uint32_t entityTag{};
    Float4 rotation{};
    Float4 translation{};
    std::uint64_t worldId{};
    /** Class marker carried by the row's type-specific DataResource pointer, when readable. */
    std::uint32_t dataResourceClass{};
    bool dataResourceClassReadable{};
};

/**
 * Reads the map-data-table tag named by one activity entity resource.
 * @param blob Whole activity-resource bytes.
 * @param tag Receives the table tag.
 * @return True when the field exists and names a definition tag.
 */
[[nodiscard]] bool
activity_entity_table_tag(std::span<const std::byte> blob, std::uint32_t& tag) noexcept;

/**
 * Finds the map-data-entry array in one map-data table.
 * @param blob Whole map-data-table bytes.
 * @param output Receives the entry array.
 * @return True when the descriptor resolves and carries the expected entry class.
 */
[[nodiscard]] bool map_data_entries(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads the entity identity and transform fields of one map-data entry.
 * A valid row may still carry no entity tag; those rows describe other map resources.
 * @param blob Whole map-data-table bytes.
 * @param entries Entry array returned by `map_data_entries`.
 * @param index Entry ordinal.
 * @param output Receives the selected fields.
 * @return True when the complete row and selected fields are inside the blob.
 */
[[nodiscard]] bool map_data_entry_at(std::span<const std::byte> blob,
                                     const Array& entries,
                                     std::uint64_t index,
                                     MapDataEntry& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables
