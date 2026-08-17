#pragma once

#include <cstdint>

#include "../../account/account_state.h"

namespace sunrise::state::runtime::equipment {

/**
 * Builds a nonsecret cache identity from the settings templates and mutable active roster.
 * The external item-name catalogue is deliberately excluded: changing names/search data must not
 * invalidate native investment data. Persisted equipment does participate so a newly selected
 * item is extracted normally on the next boot.
 * @param configuredAccount Settings-authored template account.
 * @param activeAccount Character-store account currently presented to Destiny.
 * @return Stable build-relevant equipment fingerprint.
 */
[[nodiscard]] std::uint64_t configured_hash(const AccountState& configuredAccount,
                                            const AccountState& activeAccount) noexcept;

} // namespace sunrise::state::runtime::equipment
