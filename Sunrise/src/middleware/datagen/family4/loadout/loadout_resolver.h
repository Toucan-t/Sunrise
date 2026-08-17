#pragma once

#include <cstddef>

#include "../../../../state/account/account_state.h"
#include "definition.h"

namespace sunrise::middleware::datagen::family4::loadout {

/** Resolves one selected character's equipped and unequipped inventory through installed data. */
[[nodiscard]] bool resolve(const state::AccountState& account,
                           std::size_t selectedCharacterIndex,
                           ResolvedLoadout& output) noexcept;

/** Resolves equipped instances only; appearance/roster consumers intentionally use this view. */
[[nodiscard]] bool resolve_instances(const state::AccountState& account,
                                     std::size_t characterIndex,
                                     ResolvedInstances& output) noexcept;

/** Resolves every equipped and unequipped instance for Family-4 resident publication. */
[[nodiscard]] bool resolve_owned_instances(const state::AccountState& account,
                                           std::size_t characterIndex,
                                           ResolvedInstances& output) noexcept;

} // namespace sunrise::middleware::datagen::family4::loadout
