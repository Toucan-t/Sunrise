#include "map_data_table_reader.h"

#include <limits>

#include "internal.h"

namespace sunrise::middleware::content::packages::tables {
namespace {

/**
 * Reads the class marker immediately before one non-null relative ResourcePointer target.
 * A null pointer is valid and reports class zero.
 */
[[nodiscard]] bool resource_pointer_class(std::span<const std::byte> blob,
                                          std::size_t pointerOffset,
                                          std::uint32_t& classId) noexcept {
    classId = 0;
    std::int64_t relative = 0;
    if (!read(blob, pointerOffset, relative)) {
        return false;
    }
    if (relative == 0) {
        return true;
    }

    constexpr std::int64_t kMaximum = (std::numeric_limits<std::int64_t>::max)();
    constexpr std::int64_t kMinimum = (std::numeric_limits<std::int64_t>::min)();
    if (pointerOffset > static_cast<std::size_t>(kMaximum)) {
        return false;
    }
    const std::int64_t base = static_cast<std::int64_t>(pointerOffset);
    if ((relative > 0 && base > kMaximum - relative)
        || (relative < 0 && base < kMinimum - relative)) {
        return false;
    }
    const std::int64_t target = base + relative;
    if (target < static_cast<std::int64_t>(sizeof(std::uint32_t))) {
        return false;
    }
    const auto classOffset = static_cast<std::size_t>(target) - sizeof(std::uint32_t);
    return read(blob, classOffset, classId);
}

} // namespace

/** Reads the map-data-table tag named by one activity entity resource. */
bool activity_entity_table_tag(std::span<const std::byte> blob, std::uint32_t& tag) noexcept {
    tag = 0;
    return read(blob, kActivityEntityTableTagOffset, tag)
           && package_of(tag) != kAbsentPackageId;
}

/** Finds the map-data-entry array in one map-data table. */
bool map_data_entries(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    if (!find_array_at(blob, kMapDataTableEntryDescriptor, output)
        || output.elementClass != kMapDataEntryClass) {
        output = {};
        return false;
    }
    return true;
}

/** Reads the entity identity and transform fields of one map-data entry. */
bool map_data_entry_at(std::span<const std::byte> blob,
                       const Array& entries,
                       std::uint64_t index,
                       MapDataEntry& output) noexcept {
    output = {};
    if (entries.elementClass != kMapDataEntryClass) {
        return false;
    }

    std::size_t offset = 0;
    if (!element_offset(entries.dataOffset, entries.count, kMapDataEntryStride, index, offset)
        || offset > blob.size() || blob.size() - offset < kMapDataEntryStride
        || !read(blob, offset + kMapDataEntryEntityTagOffset, output.entityTag)
        || !read(blob, offset + kMapDataEntryRotationOffset, output.rotation)
        || !read(blob, offset + kMapDataEntryTranslationOffset, output.translation)
        || !read(blob, offset + kMapDataEntryWorldIdOffset, output.worldId)) {
        output = {};
        return false;
    }

    output.dataResourceClassReadable =
        resource_pointer_class(blob,
                               offset + kMapDataEntryDataResourcePointerOffset,
                               output.dataResourceClass);
    return true;
}

} // namespace sunrise::middleware::content::packages::tables
