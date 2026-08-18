#pragma once

#include <cstddef>
#include <span>

#include "definition.h"

namespace sunrise::state::build_data::abilities {

/** Clears every generated ability bucket row. */
void clear() noexcept;

/**
 * Checks that no two rows share a subclass and ability selection.
 * @param definitions Candidate rows.
 * @return True when the rows fit storage and every key is unique.
 */
[[nodiscard]] bool valid(std::span<const Definition> definitions) noexcept;

/**
 * Replaces the generated ability bucket rows in one step.
 * @param definitions Complete rows in any order.
 * @return True when the rows pass the checks and fit fixed State storage.
 */
[[nodiscard]] bool replace(std::span<const Definition> definitions) noexcept;

/**
 * Adds or replaces one exact subclass-selection row without withdrawing the published domain.
 * Native opcode 801 uses this for a live selection after its package row is resolved.
 * @param definition Complete generated row for one socket-entry-list/selection key.
 * @return True when the row is valid and fixed storage has room.
 */
[[nodiscard]] bool merge(const Definition& definition) noexcept;

/**
 * Finds the buckets one subclass publishes under one ability selection.
 * @param socketEntryListIndex Native socket-entry-list index of the subclass.
 * @param selection The character's 5 selected socket entries.
 * @param definition Receives the matching row.
 * @return True when a row carries that exact key.
 */
[[nodiscard]] bool find(std::uint16_t socketEntryListIndex,
                        const Selection& selection,
                        Definition& definition) noexcept;

/**
 * Copies every row in publication order.
 * @param output Caller-owned fixed row storage.
 * @param count Receives the copied row count, or zero when output is too small.
 * @return True when output can hold every row.
 */
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;

/** @return Number of generated ability bucket rows, read under the lock. */
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::abilities