#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/filesystem/path.h"
#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/reader/reader.h"
#include "../../../../middleware/content/packages/tables/definition_index_table.h"
#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/build_data/abilities/definition.h"
#include "../../../../state/build_data/inventory/buckets/definition.h"
#include "../../../../state/build_data/items/details/definition.h"
#include "../../../../state/build_data/progressions/definition.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/socket_entry_lists/definition.h"
#include "../../../../state/content/content_catalog.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../memory/current_process_memory.h"
#include "../../../targets/game.h"
#include "../../hash_names/hash_name_build.h"
#include "../../scenarios/scenario_build.h"
#include "../../spawn_sets/spawn_set_build.h"
#include "build.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** @return True when every domain owned by the package pass is published. */
[[nodiscard]] bool package_domains_ready() noexcept {
    return state::build_data::item_definitions_ready()
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
 * @return True when the native domains required by on-demand editor item resolution are ready.
 * Ability, progression, scenario, spawn-set and hash-name failures must not disable the item editor.
 */
[[nodiscard]] bool editor_item_domains_ready() noexcept {
    return state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready();
}

/** @return True when every item and investment-root domain is published. */
[[nodiscard]] bool root_domains_ready() noexcept {
    return state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::inventory_bucket_descriptors_ready()
           && state::build_data::socket_entry_lists_ready()
           && state::build_data::ability_buckets_ready()
           && state::build_data::progression_definitions_ready()
           && state::build_data::investment_constants_ready();
}

/** Logs one explicit editor detail-resolution result without making normal package boot noisy. */
void report_editor_resolution(std::uint32_t definitionHash,
                              bool success,
                              const char* reason) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=item_editor stage=resolve result=%s hash=0x%08X reason=%s",
                                      success ? "ok" : "fail",
                                      definitionHash,
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         success ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Shared package-pass storage. It is several megabytes and must never live on the stack. */
Storage& pass_storage() noexcept {
    static Storage storage{};
    return storage;
}

/** True when one native definition index is already staged in the on-demand prefix. */
[[nodiscard]] bool staged_detail(std::span<const state::build_data::items::details::Definition> rows,
                                 std::uint16_t definitionIndex) noexcept {
    for (const auto& row : rows) {
        if (row.definitionIndex == definitionIndex) {
            return true;
        }
    }
    return false;
}

} // namespace

/** Publishes the dense item table from the installed packages, once. */
bool build() noexcept {
    if (package_domains_ready() && state::build_data::collectible_definitions_ready()) {
        return true;
    }
    Storage& storage = pass_storage();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!collect_keys(keys)) {
        report(0, "keys");
        return false;
    }
    std::size_t rowCount = 0;
    const char* reason = "directory";
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report(0, reason);
        return false;
    }
    // The destination layouts and the spawn sets share this pass's directory, keys, and block
    // storage. Both are independent of the item table, so a failure here leaves it alone.
    {
        const reader::Source packageSource{directory.chars.data(), &keys};
        (void)content::scenarios::build(packageSource, storage.scratch);
        (void)content::spawn_sets::build(packageSource, storage.scratch);
        (void)content::hash_names::build(packageSource, storage.scratch);
    }
    if (root_domains_ready() && state::build_data::collectible_definitions_ready()) {
        SecureZeroMemory(&keys, sizeof keys);
        return true;
    }
    reason = "tag";
    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;
    const bool named = investment_globals_tags(candidates, candidateCount);
    if (named) {
        const reader::Source source{directory.chars.data(), &keys};
        tables::Array table{};
        bool located = false;
        reason = "read";
        for (std::size_t candidate = 0; candidate < candidateCount && !located; ++candidate) {
            if (!reader::read_tag(
                    source, storage.scratch, candidates[candidate], storage.container)) {
                continue;
            }
            // Fixed navigation: globals child zero is the investment root, whose slot holds the
            // item table, whose array descriptor sits at a fixed offset.
            std::uint32_t rootTag = 0;
            std::uint32_t tableTag = 0;
            reason = "root";
            if (!tables::child_tag(std::span<const std::byte>{storage.container},
                                   tables::kInvestmentRootChild,
                                   rootTag)
                || rootTag == 0
                || !reader::read_tag(source, storage.scratch, rootTag, storage.child)) {
                continue;
            }
            // The same root names the bucket and socket-list tables.
            storage.root = storage.child;
            (void)build_buckets(source, storage, std::span<const std::byte>{storage.root});
            (void)build_socket_entry_lists(
                source, storage, std::span<const std::byte>{storage.root});
            if (!state::build_data::progression_definitions_ready()) {
                std::size_t progressionCount = 0;
                if (build_progressions(source,
                                       storage.scratch,
                                       std::span<const std::byte>{storage.root},
                                       storage.child,
                                       storage.progressionRows,
                                       progressionCount)) {
                    (void)state::build_data::publish_progression_definitions(
                        std::span(storage.progressionRows).first(progressionCount));
                }
            }
            if (!state::build_data::investment_constants_ready()) {
                state::build_data::constants::InvestmentConstants extracted{};
                if (read_investment_constants(source,
                                              storage.scratch,
                                              std::span<const std::byte>{storage.root},
                                              storage.child,
                                              extracted)) {
                    (void)state::build_data::publish_investment_constants(extracted);
                }
            }
            reason = "slot";
            if (!tables::slot_tag(
                    std::span<const std::byte>{storage.root}, tables::kItemTableSlot, tableTag)
                || tableTag == 0
                || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
                continue;
            }
            reason = "table";
            located = tables::find_array_at(std::span<const std::byte>{storage.child},
                                            tables::kTableArrayDescriptor,
                                            table)
                      && table.elementClass == tables::kItemIndexTableClass;
        }
        if (located) {
            (void)build_item_rows(source, storage, table, rowCount, reason);
            if (state::build_data::item_definitions_ready()
                && !state::build_data::collectible_definitions_ready()) {
                reason = "collectibles";
                (void)build_collectibles(source,
                                         storage,
                                         std::span<const std::byte>{storage.root},
                                         state::build_data::item_definition_count());
            }
        }
    }
    SecureZeroMemory(&keys, sizeof keys);
    const bool complete = package_domains_ready();
    if (complete) {
        // Nothing reads a package again until the next boot, so this reader's files go back now.
        reader::close_files(storage.scratch);
    }
    report(complete ? state::build_data::item_definition_count() : 0, reason);
    return complete;
}

/** Resolves one editor-selected base item and its default plugs without a process freeze. */
bool ensure_editor_item_details(std::uint32_t definitionHash,
                                std::uint32_t referenceDefinitionHash) noexcept {
    state::build_data::items::Definition itemDefinition{};
    state::build_data::items::Definition referenceDefinition{};
    state::build_data::items::details::Definition referenceDetail{};
    state::build_data::items::details::Definition existing{};
    if (!state::build_data::find_item_definition_hash(definitionHash, itemDefinition)
        || !state::build_data::find_item_definition_hash(referenceDefinitionHash,
                                                         referenceDefinition)
        || itemDefinition.bucketId == state::build_data::items::kUnresolvedBucketId
        || referenceDefinition.bucketId == state::build_data::items::kUnresolvedBucketId
        || itemDefinition.bucketId != referenceDefinition.bucketId
        || !state::build_data::find_configured_item_detail(referenceDefinition.definitionIndex,
                                                           referenceDetail)
        || referenceDetail.definitionHash != referenceDefinitionHash
        || referenceDetail.bucketId != referenceDefinition.bucketId
        || !referenceDetail.equipmentSlot.has_value()) {
        report_editor_resolution(definitionHash, false, "native_contract");
        return false;
    }
    if (state::build_data::find_configured_item_detail(itemDefinition.definitionIndex, existing)) {
        const bool compatible = existing.definitionHash == definitionHash
                                && existing.bucketId == itemDefinition.bucketId
                                && existing.equipmentSlot == referenceDetail.equipmentSlot;
        report_editor_resolution(definitionHash, compatible, compatible ? "already_loaded" : "slot_mismatch");
        return compatible;
    }
    // On-demand expansion is only safe after the boot snapshot has finished publishing the
    // numeric item/socket domains. It reads package files only and never enters process::freeze.
    if (!editor_item_domains_ready()) {
        report_editor_resolution(definitionHash, false, "item_domains_not_ready");
        return false;
    }

    Storage& storage = pass_storage();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!collect_keys(keys) || !package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_editor_resolution(definitionHash, false, "package_source");
        return false;
    }

    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;
    if (!investment_globals_tags(candidates, candidateCount)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_editor_resolution(definitionHash, false, "investment_root");
        return false;
    }

    bool resolved = false;
    const reader::Source source{directory.chars.data(), &keys};
    for (std::size_t candidate = 0; candidate < candidateCount && !resolved; ++candidate) {
        if (!reader::read_tag(source, storage.scratch, candidates[candidate], storage.container)) {
            continue;
        }
        std::uint32_t rootTag = 0;
        std::uint32_t tableTag = 0;
        if (!tables::child_tag(std::span<const std::byte>{storage.container},
                               tables::kInvestmentRootChild,
                               rootTag)
            || rootTag == 0
            || !reader::read_tag(source, storage.scratch, rootTag, storage.root)
            || !tables::slot_tag(
                std::span<const std::byte>{storage.root}, tables::kItemTableSlot, tableTag)
            || tableTag == 0
            || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
            continue;
        }

        tables::Array table{};
        if (!tables::find_array_at(std::span<const std::byte>{storage.child},
                                   tables::kTableArrayDescriptor,
                                   table)
            || table.elementClass != tables::kItemIndexTableClass) {
            continue;
        }

        const DetailSource detailSource{
            &source, &storage.scratch, std::span<const std::byte>{storage.child}, table,
            &storage.definition};
        std::size_t detailCount = 0;
        if (!build_detail(
                detailSource, itemDefinition.definitionIndex, storage.details[detailCount])) {
            continue;
        }
        if (storage.details[detailCount].definitionHash != definitionHash
            || storage.details[detailCount].bucketId != itemDefinition.bucketId) {
            continue;
        }
        // The package row gives us the stable inventory bucket, but the native equipment-slot
        // number is learned from the already-working item in this semantic slot. That contract is
        // safer than assuming native slot numbers from the external catalogue or a hard-coded map.
        storage.details[detailCount].equipmentSlot = referenceDetail.equipmentSlot;
        ++detailCount;

        const state::build_data::items::details::Definition base = storage.details[0];
        for (std::size_t lane = 0; lane < base.ordinarySocketCount; ++lane) {
            const std::uint16_t plugIndex = base.initialPlugIndices[lane];
            if (plugIndex == state::build_data::items::details::kUnavailableItemIndex) {
                continue;
            }
            state::build_data::items::details::Definition plug{};
            if (state::build_data::find_configured_item_detail(plugIndex, plug)
                || staged_detail(std::span(storage.details).first(detailCount), plugIndex)) {
                continue;
            }
            if (detailCount >= storage.details.size()
                || !build_detail(detailSource, plugIndex, storage.details[detailCount])) {
                detailCount = 0;
                break;
            }
            ++detailCount;
        }
        resolved = detailCount != 0
                   && state::build_data::extend_configured_item_details_runtime(
                       std::span(storage.details).first(detailCount));
    }

    reader::close_files(storage.scratch);
    SecureZeroMemory(&keys, sizeof keys);
    report_editor_resolution(definitionHash, resolved, resolved ? "published" : "detail_decode");
    return resolved;
}

/** Resolves one arbitrary installed equippable item and its default plugs without a slot reference. */
bool ensure_inventory_item_details(std::uint32_t definitionHash) noexcept {
    state::build_data::items::Definition itemDefinition{};
    state::build_data::items::details::Definition existing{};
    if (!state::build_data::find_item_definition_hash(definitionHash, itemDefinition)
        || itemDefinition.bucketId == state::build_data::items::kUnresolvedBucketId) {
        report_editor_resolution(definitionHash, false, "native_definition");
        return false;
    }
    if (state::build_data::find_configured_item_detail(itemDefinition.definitionIndex, existing)) {
        const bool usable = existing.definitionHash == definitionHash
                            && existing.bucketId == itemDefinition.bucketId
                            && existing.equipmentSlot.has_value();
        report_editor_resolution(definitionHash, usable, usable ? "already_loaded" : "not_equippable");
        return usable;
    }
    if (!editor_item_domains_ready()) {
        report_editor_resolution(definitionHash, false, "item_domains_not_ready");
        return false;
    }

    Storage& storage = pass_storage();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!collect_keys(keys) || !package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_editor_resolution(definitionHash, false, "package_source");
        return false;
    }

    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;
    if (!investment_globals_tags(candidates, candidateCount)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_editor_resolution(definitionHash, false, "investment_root");
        return false;
    }

    bool resolved = false;
    const reader::Source source{directory.chars.data(), &keys};
    for (std::size_t candidate = 0; candidate < candidateCount && !resolved; ++candidate) {
        if (!reader::read_tag(source, storage.scratch, candidates[candidate], storage.container)) {
            continue;
        }
        std::uint32_t rootTag = 0;
        std::uint32_t tableTag = 0;
        if (!tables::child_tag(std::span<const std::byte>{storage.container},
                               tables::kInvestmentRootChild,
                               rootTag)
            || rootTag == 0
            || !reader::read_tag(source, storage.scratch, rootTag, storage.root)
            || !tables::slot_tag(
                std::span<const std::byte>{storage.root}, tables::kItemTableSlot, tableTag)
            || tableTag == 0
            || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
            continue;
        }

        tables::Array table{};
        if (!tables::find_array_at(std::span<const std::byte>{storage.child},
                                   tables::kTableArrayDescriptor,
                                   table)
            || table.elementClass != tables::kItemIndexTableClass) {
            continue;
        }

        const DetailSource detailSource{
            &source, &storage.scratch, std::span<const std::byte>{storage.child}, table,
            &storage.definition};
        std::size_t detailCount = 0;
        if (!build_detail(detailSource, itemDefinition.definitionIndex, storage.details[detailCount])
            || storage.details[detailCount].definitionHash != definitionHash
            || storage.details[detailCount].bucketId != itemDefinition.bucketId
            || !storage.details[detailCount].equipmentSlot.has_value()) {
            continue;
        }
        ++detailCount;

        const state::build_data::items::details::Definition base = storage.details[0];
        for (std::size_t lane = 0; lane < base.ordinarySocketCount; ++lane) {
            const std::uint16_t plugIndex = base.initialPlugIndices[lane];
            if (plugIndex == state::build_data::items::details::kUnavailableItemIndex) {
                continue;
            }
            state::build_data::items::details::Definition plug{};
            if (state::build_data::find_configured_item_detail(plugIndex, plug)
                || staged_detail(std::span(storage.details).first(detailCount), plugIndex)) {
                continue;
            }
            if (detailCount >= storage.details.size()
                || !build_detail(detailSource, plugIndex, storage.details[detailCount])) {
                detailCount = 0;
                break;
            }
            ++detailCount;
        }
        resolved = detailCount != 0
                   && state::build_data::extend_configured_item_details_runtime(
                       std::span(storage.details).first(detailCount));
    }

    reader::close_files(storage.scratch);
    SecureZeroMemory(&keys, sizeof keys);
    report_editor_resolution(definitionHash, resolved, resolved ? "published" : "detail_decode");
    return resolved;
}

/** Resolves one arbitrary socket plug detail without imposing the target gear's bucket/slot. */
bool ensure_socket_plug_details(std::uint32_t definitionHash) noexcept {
    state::build_data::items::Definition itemDefinition{};
    state::build_data::items::details::Definition existing{};
    if (!state::build_data::find_item_definition_hash(definitionHash, itemDefinition)
        || itemDefinition.bucketId == state::build_data::items::kUnresolvedBucketId) {
        report_editor_resolution(definitionHash, false, "plug_definition");
        return false;
    }
    if (state::build_data::find_configured_item_detail(itemDefinition.definitionIndex, existing)) {
        const bool usable = existing.definitionHash == definitionHash
                            && existing.bucketId == itemDefinition.bucketId;
        report_editor_resolution(definitionHash,
                                 usable,
                                 usable ? "plug_already_loaded" : "plug_detail_mismatch");
        return usable;
    }
    if (!editor_item_domains_ready()) {
        report_editor_resolution(definitionHash, false, "item_domains_not_ready");
        return false;
    }

    Storage& storage = pass_storage();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!collect_keys(keys) || !package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_editor_resolution(definitionHash, false, "package_source");
        return false;
    }

    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;
    if (!investment_globals_tags(candidates, candidateCount)) {
        SecureZeroMemory(&keys, sizeof keys);
        report_editor_resolution(definitionHash, false, "investment_root");
        return false;
    }

    bool resolved = false;
    const reader::Source source{directory.chars.data(), &keys};
    for (std::size_t candidate = 0; candidate < candidateCount && !resolved; ++candidate) {
        if (!reader::read_tag(source, storage.scratch, candidates[candidate], storage.container)) {
            continue;
        }
        std::uint32_t rootTag = 0;
        std::uint32_t tableTag = 0;
        if (!tables::child_tag(std::span<const std::byte>{storage.container},
                               tables::kInvestmentRootChild,
                               rootTag)
            || rootTag == 0
            || !reader::read_tag(source, storage.scratch, rootTag, storage.root)
            || !tables::slot_tag(
                std::span<const std::byte>{storage.root}, tables::kItemTableSlot, tableTag)
            || tableTag == 0
            || !reader::read_tag(source, storage.scratch, tableTag, storage.child)) {
            continue;
        }

        tables::Array table{};
        if (!tables::find_array_at(std::span<const std::byte>{storage.child},
                                   tables::kTableArrayDescriptor,
                                   table)
            || table.elementClass != tables::kItemIndexTableClass) {
            continue;
        }

        const DetailSource detailSource{
            &source, &storage.scratch, std::span<const std::byte>{storage.child}, table,
            &storage.definition};
        if (!build_detail(detailSource, itemDefinition.definitionIndex, storage.details[0])
            || storage.details[0].definitionIndex != itemDefinition.definitionIndex
            || storage.details[0].definitionHash != definitionHash
            || storage.details[0].bucketId != itemDefinition.bucketId) {
            continue;
        }
        resolved = state::build_data::extend_configured_item_details_runtime(
            std::span(storage.details).first(1));
    }

    reader::close_files(storage.scratch);
    SecureZeroMemory(&keys, sizeof keys);
    report_editor_resolution(definitionHash,
                             resolved,
                             resolved ? "plug_published" : "plug_detail_decode");
    return resolved;
}

/** Restores package details for every item identity persisted in the runtime account. */
bool ensure_account_inventory_item_details() noexcept {
    static std::atomic_bool successReported{};
    static std::atomic_bool failureReported{};
    const state::AccountState account = state::account_snapshot();
    if (!state::account::valid(account)) {
        if (!failureReported.exchange(true, std::memory_order_relaxed)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=item_rehydrate stage=account result=fail reason=invalid_state");
        }
        return false;
    }

    const auto ensure_item = [](std::uint64_t characterSoid,
                                const state::account::inventory::Item& item) noexcept {
        state::build_data::items::Definition definition{};
        state::build_data::items::details::Definition detail{};
        bool ready = state::build_data::find_item_definition_hash(item.definitionHash, definition)
                     && definition.definitionHash == item.definitionHash
                     && state::build_data::find_configured_item_detail(definition.definitionIndex,
                                                                       detail)
                     && detail.definitionIndex == definition.definitionIndex
                     && detail.definitionHash == definition.definitionHash
                     && detail.bucketId == definition.bucketId
                     && detail.equipmentSlot.has_value();
        if (!ready) {
            ready = ensure_inventory_item_details(item.definitionHash);
        }
        if (ready && item.sockets.policy == state::account::inventory::SocketPolicy::authored) {
            for (std::size_t lane = 0; lane < item.sockets.plugCount; ++lane) {
                if (!item.sockets.plugs[lane].has_value()) {
                    continue;
                }
                const std::uint32_t plugHash = *item.sockets.plugs[lane];
                state::build_data::items::Definition plugDefinition{};
                state::build_data::items::details::Definition plugDetail{};
                const bool plugReady =
                    state::build_data::find_item_definition_hash(plugHash, plugDefinition)
                    && plugDefinition.definitionHash == plugHash
                    && state::build_data::find_configured_item_detail(
                        plugDefinition.definitionIndex, plugDetail)
                    && plugDetail.definitionIndex == plugDefinition.definitionIndex
                    && plugDetail.definitionHash == plugHash
                    && plugDetail.bucketId == plugDefinition.bucketId;
                if (!plugReady && !ensure_socket_plug_details(plugHash)) {
                    ready = false;
                    if (!failureReported.exchange(true, std::memory_order_relaxed)) {
                        std::array<char, 256> line{};
                        const int written = std::snprintf(
                            line.data(),
                            line.size(),
                            "ev=item_rehydrate stage=plug result=fail character=0x%llX "
                            "instance=0x%llX lane=%zu hash=0x%08X",
                            static_cast<unsigned long long>(characterSoid),
                            static_cast<unsigned long long>(item.instanceSoid),
                            lane,
                            plugHash);
                        if (written > 0) {
                            core::log::write(core::log::Channel::client,
                                             core::log::Level::warn,
                                             {line.data(), static_cast<std::size_t>(written)});
                        }
                    }
                    break;
                }
            }
        }
        if (!ready && !failureReported.exchange(true, std::memory_order_relaxed)) {
            std::array<char, 224> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=item_rehydrate stage=item result=fail character=0x%llX instance=0x%llX "
                "hash=0x%08X",
                static_cast<unsigned long long>(characterSoid),
                static_cast<unsigned long long>(item.instanceSoid),
                item.definitionHash);
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
        }
        return ready;
    };

    std::size_t checked = 0;
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount; ++characterIndex) {
        const state::CharacterState& character = account.characters[characterIndex];
        for (const auto& item : character.equipment.slots) {
            if (!item.has_value()) {
                continue;
            }
            ++checked;
            if (!ensure_item(character.soid, *item)) {
                return false;
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            ++checked;
            if (!ensure_item(character.soid, character.inventory.values[itemIndex])) {
                return false;
            }
        }
    }

    for (std::size_t profileIndex = 0; profileIndex < account.profileItemCount; ++profileIndex) {
        const auto& profileItem = account.profileItems[profileIndex];
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_hash(profileItem.definitionHash, definition)
            || definition.definitionHash != profileItem.definitionHash) {
            return false;
        }
        constexpr std::uint8_t kModBucketId = 13;
        constexpr std::uint8_t kShaderBucketId = 14;
        if (definition.bucketId != kModBucketId && definition.bucketId != kShaderBucketId
            && profileItem.instanceSoid == 0) {
            continue;
        }
        ++checked;
        state::build_data::items::details::Definition detail{};
        const bool ready =
            state::build_data::find_configured_item_detail(definition.definitionIndex, detail)
            && detail.definitionIndex == definition.definitionIndex
            && detail.definitionHash == definition.definitionHash
            && detail.bucketId == definition.bucketId;
        if (!ready && !ensure_socket_plug_details(profileItem.definitionHash)) {
            if (!failureReported.exchange(true, std::memory_order_relaxed)) {
                std::array<char, 192> line{};
                const int written = std::snprintf(
                    line.data(),
                    line.size(),
                    "ev=item_rehydrate stage=profile result=fail index=%zu instance=0x%llX hash=0x%08X",
                    profileIndex,
                    static_cast<unsigned long long>(profileItem.instanceSoid),
                    profileItem.definitionHash);
                if (written > 0) {
                    core::log::write(core::log::Channel::client,
                                     core::log::Level::warn,
                                     {line.data(), static_cast<std::size_t>(written)});
                }
            }
            return false;
        }
    }
    if (!state::ensure_profile_item_identities()) {
        if (!failureReported.exchange(true, std::memory_order_relaxed)) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=item_rehydrate stage=profile result=fail reason=identity");
        }
        return false;
    }

    if (!successReported.exchange(true, std::memory_order_relaxed)) {
        std::array<char, 112> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=item_rehydrate stage=account result=ok items=%zu",
                                          checked);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
