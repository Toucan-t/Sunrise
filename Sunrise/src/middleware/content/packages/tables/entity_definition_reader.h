#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "definition_index_table.h"

namespace sunrise::middleware::content::packages::tables {

/** Tag class of the supported build's entity definition. */
inline constexpr std::uint32_t kEntityDefinitionClass = 0x80809C0FU;
/** Entity definitions hold their resource array at this descriptor offset. */
inline constexpr std::size_t kEntityResourceArrayDescriptor = 0x10;
/** Element class of one entity-resource reference row. */
inline constexpr std::uint32_t kEntityResourceEntryClass = 0x80809C04U;
/** One entity-resource reference row is 12 inline bytes. */
inline constexpr std::size_t kEntityResourceEntryStride = 0x0C;
/** Resource tag is the first field of one entity-resource reference row. */
inline constexpr std::size_t kEntityResourceTagOffset = 0x00;
/** Tag class of the supported build's ordinary entity resource. */
inline constexpr std::uint32_t kEntityResourceClass = 0x80809C36U;
/** Resource pointer that identifies the entity-resource kind. */
inline constexpr std::size_t kEntityResourceKindPointerOffset = 0x10;
/** Resource pointer that identifies the entity-resource payload kind. */
inline constexpr std::size_t kEntityResourcePayloadPointerOffset = 0x18;

/**
 * Finds one entity definition's resource-reference array.
 * @param blob Whole entity-definition bytes.
 * @param output Receives the resource-reference array.
 * @return True when the descriptor resolves and carries the expected element class.
 */
[[nodiscard]] bool entity_resources(std::span<const std::byte> blob, Array& output) noexcept;

/**
 * Reads one resource tag from an entity definition's resource-reference array.
 * @param blob Whole entity-definition bytes.
 * @param resources Array returned by `entity_resources`.
 * @param index Resource-reference ordinal.
 * @param tag Receives the referenced resource tag.
 * @return True when the row is complete and the value is a definition tag.
 */
[[nodiscard]] bool entity_resource_at(std::span<const std::byte> blob,
                                      const Array& resources,
                                      std::uint64_t index,
                                      std::uint32_t& tag) noexcept;

/**
 * Reads the class marker associated with one relative ResourcePointer.
 * A null pointer is valid and returns class id zero.
 * @param blob Whole resource bytes.
 * @param pointerOffset Offset of the signed relative pointer field.
 * @param classId Receives the pointed resource class, or zero for a null pointer.
 * @return True when the pointer and any non-null class marker stay inside the blob.
 */
[[nodiscard]] bool resource_pointer_class(std::span<const std::byte> blob,
                                          std::size_t pointerOffset,
                                          std::uint32_t& classId) noexcept;

} // namespace sunrise::middleware::content::packages::tables
