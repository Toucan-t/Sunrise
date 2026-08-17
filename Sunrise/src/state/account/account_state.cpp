#include "account_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace sunrise::state::account {
namespace {

/** Enough fixed storage for every account, character, profile-stack, and character-item key. */
inline constexpr std::size_t kIdentityCapacity =
    1 + inventory::kProfileItemCapacity
    + kCharacterCapacity * (1 + inventory::kEquipmentSlotCount + inventory::kCharacterItemCapacity);

/** @return True when a profile row carries no authored or runtime-owned value. */
[[nodiscard]] bool empty_profile_item(const inventory::ProfileItem& item) noexcept {
    return item.instanceSoid == 0 && item.definitionHash == 0 && item.quantity == 0
           && item.mutationSerial == 0;
}

/** Adds one nonzero globally unique key to a bounded identity set. */
[[nodiscard]] bool append_identity(std::array<std::uint64_t, kIdentityCapacity>& identities,
                                   std::size_t& count,
                                   std::uint64_t soid) noexcept {
    if (soid == 0 || count >= identities.size()) {
        return false;
    }
    const auto end = identities.cbegin() + static_cast<std::ptrdiff_t>(count);
    if (std::find(identities.cbegin(), end, soid) != end) {
        return false;
    }
    identities[count++] = soid;
    return true;
}

} // namespace

/**
 * Checks bounded ids and whole settings for every nonzero account.
 * @param state Account State to check.
 * @return True for empty State, or a whole account with unique ids.
 */
bool valid(const AccountState& state) noexcept {
    if (state.profileItemCount > state.profileItems.size()
        || state.characterCount > state.characters.size()) {
        return false;
    }
    if (state.primarySoid == 0) {
        return state.profileItemCount == 0 && state.characterCount == 0 && !state.settings.configured
               && !state.settings.keyBindings.configured
               && std::all_of(
                   state.profileItems.cbegin(), state.profileItems.cend(), empty_profile_item);
    }
    if (!settings::valid(state.settings)) {
        return false;
    }

    std::array<std::uint64_t, kIdentityCapacity> identities{};
    std::size_t identityCount = 0;
    if (!append_identity(identities, identityCount, state.primarySoid)) {
        return false;
    }

    for (std::size_t index = 0; index < state.profileItems.size(); ++index) {
        const inventory::ProfileItem& item = state.profileItems[index];
        if (index >= state.profileItemCount) {
            if (!empty_profile_item(item)) {
                return false;
            }
            continue;
        }
        if (item.definitionHash == 0 || item.definitionHash == inventory::kNoDefinitionHash
            || item.quantity <= 0 || item.mutationSerial < 0
            || (item.instanceSoid != 0
                && !append_identity(identities, identityCount, item.instanceSoid))) {
            return false;
        }
    }

    bool selected = false;
    for (std::size_t index = 0; index < state.characterCount; ++index) {
        const CharacterState& character = state.characters[index];
        if (!append_identity(identities, identityCount, character.soid)
            || (character.selected && selected) || character.race > CharacterRace::exo
            || character.gender > CharacterGender::female
            || character.characterClass > CharacterClass::warlock
            || !std::isfinite(character.appearanceValue) || !inventory::valid(character.equipment)
            || !inventory::valid(character.inventory)) {
            return false;
        }
        selected = selected || character.selected;
        for (const std::optional<inventory::Item>& item : character.equipment.slots) {
            if (item.has_value()
                && !append_identity(identities, identityCount, item->instanceSoid)) {
                return false;
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            if (!append_identity(identities,
                                 identityCount,
                                 character.inventory.values[itemIndex].instanceSoid)) {
                return false;
            }
        }
    }
    return true;
}

/**
 * Finds the selected character without storing a second account-level key.
 * @param state Account snapshot read under the lock.
 * @return The selected character's nonzero SOID, or zero when none is selected.
 */
std::uint64_t selected_character_soid(const AccountState& state) noexcept {
    const std::size_t count = (std::min)(state.characterCount, state.characters.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (state.characters[index].selected) {
            return state.characters[index].soid;
        }
    }
    return 0;
}

} // namespace sunrise::state::account
