#include "entity_definition_reader.h"

#include <limits>

#include "internal.h"

namespace sunrise::middleware::content::packages::tables {

bool entity_resources(std::span<const std::byte> blob, Array& output) noexcept {
    output = {};
    if (!find_array_at(blob, kEntityResourceArrayDescriptor, output)
        || output.elementClass != kEntityResourceEntryClass) {
        output = {};
        return false;
    }
    return true;
}

bool entity_resource_at(std::span<const std::byte> blob,
                        const Array& resources,
                        std::uint64_t index,
                        std::uint32_t& tag) noexcept {
    tag = 0;
    if (resources.elementClass != kEntityResourceEntryClass) {
        return false;
    }
    std::size_t offset = 0;
    if (!element_offset(resources.dataOffset,
                        resources.count,
                        kEntityResourceEntryStride,
                        index,
                        offset)
        || offset > blob.size() || blob.size() - offset < kEntityResourceEntryStride
        || !read(blob, offset + kEntityResourceTagOffset, tag)
        || package_of(tag) == kAbsentPackageId) {
        tag = 0;
        return false;
    }
    return true;
}

bool resource_pointer_class(std::span<const std::byte> blob,
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

} // namespace sunrise::middleware::content::packages::tables
