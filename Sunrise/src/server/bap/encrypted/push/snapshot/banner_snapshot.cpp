#include <algorithm>
#include <array>
#include <span>

#include "../../../../../middleware/datagen/character_record/character_record_encoder.h"
#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "../../../../../state/runtime/runtime.h"
#include "internal.h"
#include "snapshot_storage.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace character_record = middleware::datagen::character_record;

/** The anchor and the record it names are the two objects every family-zero frame upserts. */
constexpr std::size_t kBannerUpsertCount = 2;

/**
 * Native Exo-male Titan presentation header captured from a completely default creator run.
 * The synthetic creator Guardian never enters AccountState; this block only gives the cold
 * presentation renderer a valid authored head while no real character is selected.
 */
constexpr std::array<std::byte, state::kCharacterPresentationHeaderSize>
    kCreatorBootstrapPresentationHeader{
        std::byte{0x16}, std::byte{0x00}, std::byte{0x02}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0xD5}, std::byte{0x01},
        std::byte{0xFC}, std::byte{0x01}, std::byte{0x8E}, std::byte{0x01},
        std::byte{0x02}, std::byte{0x02}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xB9}, std::byte{0x01}, std::byte{0x01}, std::byte{0x00},
        std::byte{0xC5}, std::byte{0x9D}, std::byte{0x1C}, std::byte{0x81},
        std::byte{0x7E}, std::byte{0x38}, std::byte{0x3B}, std::byte{0x74},
    };

/** Builds a one-row, non-persistent factory account for the cold creator banner. */
[[nodiscard]] bool creator_bootstrap_account(std::uint64_t familyRootSoid,
                                             state::AccountState& account) noexcept {
    account = {};
    if (familyRootSoid == 0) {
        return false;
    }
    const state::AccountState factory = state::configured_account_snapshot();
    std::size_t titanIndex = factory.characterCount;
    for (std::size_t index = 0; index < factory.characterCount; ++index) {
        if (factory.characters[index].characterClass == state::CharacterClass::titan) {
            titanIndex = index;
            break;
        }
    }
    if (titanIndex >= factory.characterCount) {
        return false;
    }
    account.primarySoid = familyRootSoid;
    account.characterCount = 1;
    account.characters[0] = factory.characters[titanIndex];
    state::CharacterState& character = account.characters[0];
    character.soid = kCreatorBootstrapCharacterSoid;
    character.selected = false;
    character.race = state::CharacterRace::exo;
    character.gender = state::CharacterGender::male;
    character.characterClass = state::CharacterClass::titan;
    character.previewAvailable = true;
    character.presentationHeader = kCreatorBootstrapPresentationHeader;
    return true;
}

} // namespace

/** Builds the family-zero banner anchor and the record for the character it names. */
bool prepare_banner(Scratch& scratch,
                    std::uint64_t familyRootSoid,
                    std::int32_t version,
                    std::uint64_t previousCharacter,
                    Prepared& prepared) noexcept {
    const Reservation reservation = reserve_prior(scratch, prepared);
    state::AccountState account = state::account_snapshot();
    if (reservation.rawWriteOffset > scratch.plaintext.size()) {
        return false;
    }
    std::size_t selectedIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            selectedIndex = index;
            break;
        }
    }
    if (selectedIndex == account.characterCount) {
        // Family zero is also the creator's presentation bootstrap. Use a completely synthetic
        // factory Guardian when the account is unselected so a zero-character account still has a
        // valid renderer seed. This local account is never stored or exposed through roster APIs.
        if (!creator_bootstrap_account(familyRootSoid, account)) {
            return false;
        }
        selectedIndex = 0;
    }

    middleware::datagen::family4::loadout::ResolvedInstances instances{};
    std::int32_t light = 0;
    if (!middleware::datagen::family4::loadout::resolve_instances(account, selectedIndex, instances)
        || !state::equipment::light::resolution::character_light(account, selectedIndex, light)) {
        return false;
    }

    /** Both family-zero objects are staged together, so raw storage must hold the pair. */
    constexpr std::size_t kTotalSize =
        character_record::kFamily0AnchorSize + character_record::kFamily0RecordSize;
    if (scratch.plaintext.size() - reservation.rawWriteOffset < kTotalSize) {
        return false;
    }
    const auto anchor =
        std::span(scratch.plaintext)
            .subspan(reservation.rawWriteOffset, character_record::kFamily0AnchorSize);
    const auto record =
        std::span(scratch.plaintext)
            .subspan(reservation.rawWriteOffset + character_record::kFamily0AnchorSize,
                     character_record::kFamily0RecordSize);
    const state::CharacterState& character = account.characters[selectedIndex];
    if (!character_record::encode_family0_anchor(account.primarySoid, character.soid, anchor)
        || !character_record::encode_family0(character, instances, light, record)) {
        return false;
    }
    Prepared staged{};
    staged.rawClearSize =
        (std::max)(reservation.rawClearSize, reservation.rawWriteOffset + kTotalSize);
    std::size_t objectCount = 0;
    if (previousCharacter != 0) {
        // The Client holds an objIdx-1 buffer for one character at a time, allocated from the
        // character the anchor names. The record it already holds is released first, or the
        // replacement finds no buffer and the Client tears the whole family down.
        staged.objects[objectCount] = middleware::queuez::Object{
            middleware::datagen::kBannerCharacterObjectId,
            previousCharacter,
            middleware::queuez::Encoding::raw,
            {},
        };
        ++objectCount;
    }
    std::size_t compressedExtent = reservation.compressedWriteOffset;
    std::size_t compressedSize = 0;
    if (!compress_object(scratch,
                         anchor,
                         middleware::datagen::kBannerAnchorObjectId,
                         account.primarySoid,
                         compressedExtent,
                         staged.objects[objectCount],
                         compressedSize)) {
        return false;
    }
    ++objectCount;
    compressedExtent += compressedSize;
    if (!compress_object(scratch,
                         record,
                         middleware::datagen::kBannerCharacterObjectId,
                         character.soid,
                         compressedExtent,
                         staged.objects[objectCount],
                         compressedSize)) {
        return false;
    }
    ++objectCount;
    compressedExtent += compressedSize;
    staged.compressedClearSize = (std::max)(reservation.compressedClearSize, compressedExtent);
    staged.family = middleware::queuez::Family{
        middleware::datagen::kBannerFamily,
        familyRootSoid,
        version,
        previousCharacter != 0 ? std::uint8_t{0} : middleware::queuez::kFullSnapshotFlag,
        std::span(staged.objects).first(objectCount),
    };
    static_assert(kBannerUpsertCount == 2);
    return commit(staged, prepared);
}

/** Builds a Family-0 incremental containing only the resident selected-character record. */
bool prepare_banner_character_refresh(Scratch& scratch,
                                      std::uint64_t familyRootSoid,
                                      std::int32_t version,
                                      std::uint64_t characterSoid,
                                      Prepared& prepared) noexcept {
    if (familyRootSoid == 0 || version <= kInitialFamilyVersion || characterSoid == 0
        || character_record::kFamily0RecordSize > scratch.plaintext.size()) {
        return false;
    }
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account) || account.primarySoid != familyRootSoid) {
        return false;
    }
    std::size_t selectedIndex = account.characterCount;
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected
            && account.characters[index].soid == characterSoid) {
            selectedIndex = index;
            break;
        }
    }
    if (selectedIndex == account.characterCount) {
        return false;
    }

    middleware::datagen::family4::loadout::ResolvedInstances instances{};
    std::int32_t light = 0;
    if (!middleware::datagen::family4::loadout::resolve_instances(account, selectedIndex, instances)
        || !state::equipment::light::resolution::character_light(account, selectedIndex, light)) {
        return false;
    }
    const auto record =
        std::span(scratch.plaintext).first(character_record::kFamily0RecordSize);
    if (!character_record::encode_family0(account.characters[selectedIndex], instances, light, record)) {
        return false;
    }

    Prepared staged{};
    std::size_t compressedSize = 0;
    if (!compress_object(scratch,
                         record,
                         middleware::datagen::kBannerCharacterObjectId,
                         characterSoid,
                         0,
                         staged.objects.front(),
                         compressedSize)) {
        return false;
    }
    staged.rawClearSize = character_record::kFamily0RecordSize;
    staged.compressedClearSize = compressedSize;
    staged.family = middleware::queuez::Family{
        middleware::datagen::kBannerFamily,
        familyRootSoid,
        version,
        0,
        std::span(staged.objects).first(kSingleObjectCount),
    };
    return commit(staged, prepared);
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
