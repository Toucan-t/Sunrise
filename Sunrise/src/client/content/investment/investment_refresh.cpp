#include <Windows.h>

#include <array>
#include <cstdio>
#include <optional>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/busy/busy.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/account/characters/character_store.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../../state/runtime/storage/internal.h"
#include "../../process/freeze/client_process_freeze.h"
#include "../items/packages/build.h"
#include "internal.h"
#include "runtime.h"

namespace sunrise::client::content::investment {
namespace {

SRWLOCK g_refreshLock{SRWLOCK_INIT};

/** One line reports the freeze, so a run that could not hold the game is visible. */
constexpr std::size_t kLineLimit = 96;
/** Legacy retail-collection item injected by the temporary emote-wheel experiment. */
constexpr std::uint32_t kLegacyEmoteCollectionHash = 3183180185U;
/** Semantic equipment slot occupied by the normal single equipped emote. */
constexpr std::size_t kEmoteSlot =
    static_cast<std::size_t>(state::account::inventory::EquipmentSlot::emote);

/**
 * Removes the persisted temporary emote-wheel item without discarding the user's character store.
 * The experiment replaced the old emote in-place and could subsequently be checkpointed into
 * characters.dat. Once the experiment is removed, that collection item has no ordinary equipment
 * mapping and blocks account-item rehydration forever. Restore the class factory emote while
 * retaining the persisted instance identity and mutation serial, then checkpoint the corrected
 * runtime state so later boots no longer need the migration.
 */
[[nodiscard]] bool repair_legacy_emote_collection() noexcept {
    namespace characters = state::account::characters;
    namespace inventory = state::account::inventory;
    namespace storage = state::runtime::storage;

    state::CharacterState titanTemplate{};
    state::CharacterState hunterTemplate{};
    state::CharacterState warlockTemplate{};
    const bool haveTitan =
        characters::template_for_class(state::CharacterClass::titan, titanTemplate);
    const bool haveHunter =
        characters::template_for_class(state::CharacterClass::hunter, hunterTemplate);
    const bool haveWarlock =
        characters::template_for_class(state::CharacterClass::warlock, warlockTemplate);

    const auto template_for_class = [&](state::CharacterClass characterClass) noexcept
        -> const state::CharacterState* {
        switch (characterClass) {
        case state::CharacterClass::titan:
            return haveTitan ? &titanTemplate : nullptr;
        case state::CharacterClass::hunter:
            return haveHunter ? &hunterTemplate : nullptr;
        case state::CharacterClass::warlock:
            return haveWarlock ? &warlockTemplate : nullptr;
        default:
            return nullptr;
        }
    };

    bool changed = false;
    AcquireSRWLockExclusive(&storage::g_stateLock);
    state::AccountState candidate = storage::g_state.account;
    if (!state::account::valid(candidate)) {
        ReleaseSRWLockExclusive(&storage::g_stateLock);
        return false;
    }

    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount;
         ++characterIndex) {
        state::CharacterState& character = candidate.characters[characterIndex];
        std::optional<inventory::Item>& slot = character.equipment.slots[kEmoteSlot];
        if (!slot.has_value() || slot->definitionHash != kLegacyEmoteCollectionHash) {
            continue;
        }

        const state::CharacterState* factory = template_for_class(character.characterClass);
        if (factory == nullptr) {
            ReleaseSRWLockExclusive(&storage::g_stateLock);
            return false;
        }
        const std::optional<inventory::Item>& factoryEmote = factory->equipment.slots[kEmoteSlot];
        if (!factoryEmote.has_value()
            || factoryEmote->definitionHash == kLegacyEmoteCollectionHash) {
            ReleaseSRWLockExclusive(&storage::g_stateLock);
            return false;
        }

        inventory::Item replacement = *factoryEmote;
        replacement.instanceSoid = slot->instanceSoid;
        replacement.mutationSerial = slot->mutationSerial;
        slot = replacement;
        changed = true;
    }

    if (changed && !state::account::valid(candidate)) {
        ReleaseSRWLockExclusive(&storage::g_stateLock);
        return false;
    }
    if (changed) {
        storage::g_state.account = candidate;
    }
    ReleaseSRWLockExclusive(&storage::g_stateLock);

    if (!changed) {
        return true;
    }

    const bool checkpointed = state::checkpoint_characters();
    core::log::write(core::log::Channel::state,
                     checkpointed ? core::log::Level::info : core::log::Level::warn,
                     checkpointed
                         ? "ev=characters stage=legacy_emote_repair result=ok"
                         : "ev=characters stage=legacy_emote_repair result=runtime_only");
    // A disk failure must not put the current process back into the boot loop. The repaired runtime
    // state is already authoritative; at worst the same migration runs again on the next launch.
    return true;
}

/**
 * @return True when every persistent mapping domain is fully published.
 * The destination layouts and spawn sets belong here even though they are not equipment mappings.
 * This is the only caller of the package pass, so a domain left out of this test stops being
 * extracted once the others finish, and the cache can then never be written.
 */
[[nodiscard]] bool ready() noexcept {
    return state::build_data::named_catalog_ready() && state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::scenario_layouts_ready() && state::build_data::spawn_sets_ready()
           && state::build_data::hash_names_ready()
           && state::build_data::investment_constants_ready();
}

/**
 * Reports the outcome of one freeze attempt.
 * @param frozen True when the game was held.
 * @param threadCount Threads that were suspended.
 */
void report_freeze(bool frozen, std::size_t threadCount) noexcept {
    std::array<char, kLineLimit> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=extract stage=freeze result=%s threads=%zu",
                                      frozen ? "ok" : "fail",
                                      threadCount);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    // The pass runs in slices, so a working freeze reports at debug and only a failure is loud.
    core::log::write(core::log::Channel::client,
                     frozen ? core::log::Level::debug : core::log::Level::warn,
                     std::string_view(line.data(), length));
}

} // namespace

/** @return True when the next refresh slice must hold the process for a package sweep. */
bool requires_process_freeze() noexcept {
    return !state::build_data::item_definitions_ready() && items::packages::readable();
}

/** Publishes every installed equipment mapping domain. */
bool refresh() noexcept {
    if (ready()) {
        // The same lock as the extraction path. A cache write holds its own lock across file
        // calls, so a held thread stopped inside one would deadlock the freeze below.
        AcquireSRWLockExclusive(&g_refreshLock);
        // Collections links are transient support for opcode 1820. Rebuild them opportunistically
        // on cached boots, but never let their absence block the normal account/character service.
        if (!state::build_data::collectible_definitions_ready()) {
            (void)items::packages::build();
        }
        const bool accountStateReady = repair_legacy_emote_collection();
        const bool persisted = accountStateReady && state::build_data::persist();
        // characters.dat can contain debug/native-acquired item hashes whose detail rows were
        // merged only for the prior process. Rehydrate those rows before Family 4 is allowed to
        // resolve the persisted owned-item graph.
        const bool accountItemsReady =
            accountStateReady && items::packages::ensure_account_inventory_item_details();
        // Nothing reads a package again until the next boot, so the open files and the held
        // tables go back now rather than at process exit.
        middleware::content::packages::reader::release_caches();
        ReleaseSRWLockExclusive(&g_refreshLock);
        if (persisted && accountItemsReady) {
            core::ui::busy::end(core::ui::busy::Task::contentExtraction);
        }
        return persisted && accountItemsReady;
    }

    AcquireSRWLockExclusive(&g_refreshLock);
    // Only the item sweep holds the game, and only once the block keys exist. The destination and
    // spawn-set passes after it are tens of thousands of tag reads over many slices, so the
    // overlay covers the whole pass: without it the longest stall of the boot has nothing on
    // screen.
    const bool sweeping = requires_process_freeze();
    process::freeze::Held held{};
    bool frozen = false;
    std::size_t heldThreads = 0;
    if (sweeping) {
        // The overlay reaches the screen before the freeze stops the frame loop. A held game
        // cannot time its connection out, which a slow disk otherwise causes here.
        core::ui::busy::begin(core::ui::busy::Task::contentExtraction);
        frozen = process::freeze::hold(held);
        heldThreads = held.count;
    } else {
        core::ui::busy::raise(core::ui::busy::Task::contentExtraction);
    }
    // The package pass owns the item table and must not wait on runtime content lookups.
    (void)items::packages::build();
    // Family 4 only needs the item domains below. Do not wait for unrelated ability/scenario/hash
    // retries before restoring details for items that survived in characters.dat.
    const bool itemDomainsReady = state::build_data::item_definitions_ready()
                                  && state::build_data::configured_item_details_ready()
                                  && state::build_data::inventory_bucket_descriptors_ready()
                                  && state::build_data::socket_entry_lists_ready();
    const bool accountStateReady = itemDomainsReady && repair_legacy_emote_collection();
    const bool accountItemsReady =
        accountStateReady && items::packages::ensure_account_inventory_item_details();
    const bool domainsReady = ready();
    const bool complete =
        domainsReady && accountStateReady && accountItemsReady && state::build_data::persist();
    process::freeze::release(held);
    // The overlay ends with the work, not with the slice, so it spans every retry the pass needs.
    if (complete) {
        core::ui::busy::end(core::ui::busy::Task::contentExtraction);
    }
    ReleaseSRWLockExclusive(&g_refreshLock);
    if (sweeping) {
        report_freeze(frozen, heldThreads);
    }
    return complete;
}

} // namespace sunrise::client::content::investment
