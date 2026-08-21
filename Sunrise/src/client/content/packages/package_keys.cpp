#include "package_keys.h"

#include <Windows.h>

#include <cstddef>

#include "../../../state/runtime/runtime.h"
#include "../../targets/game/packages.h"

namespace sunrise::client::content::packages {

/** Derives package block keys exactly as the working content-extraction path does. */
bool collect_block_keys(
    sunrise::middleware::content::packages::reader::BlockKeys& keys) noexcept {
    keys = {};
    const state::SignOnState& signOn = state::sign_on();
    targets::game::packages::KeyTable table{};
    if (!signOn.bootstrapTokenPresent) {
        return false;
    }
    if (!targets::game::packages::read(table)) {
        SecureZeroMemory(&table, sizeof table);
        return false;
    }

    keys.alternate = table.alternateKey;
    keys.nonceBase = table.nonceBase;
    // The installed table carries an identity constant, not the already-derived primary key.
    // Match the proven package extraction path: bootstrap-token byte + identity-constant byte.
    for (std::size_t index = 0; index < keys.primary.size(); ++index) {
        const auto tokenByte = static_cast<unsigned char>(signOn.bootstrapToken[index]);
        const auto constantByte = static_cast<unsigned char>(table.identityConstant[index]);
        keys.primary[index] = static_cast<std::byte>(tokenByte + constantByte);
    }

    SecureZeroMemory(&table, sizeof table);
    return true;
}

} // namespace sunrise::client::content::packages
