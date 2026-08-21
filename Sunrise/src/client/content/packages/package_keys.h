#pragma once

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::packages {

/**
 * Derives the package block keys from the live sign-on bootstrap token and installed key table.
 * The caller owns the returned bytes and must clear them after the bounded package operation.
 * @param keys Receives the primary, alternate and nonce material.
 * @return True when the bootstrap token and installed key table are both available.
 */
[[nodiscard]] bool
collect_block_keys(sunrise::middleware::content::packages::reader::BlockKeys& keys) noexcept;

} // namespace sunrise::client::content::packages
