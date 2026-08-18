#include "../state/build_data/abilities/ability_bucket_catalog.h"

// Keep the complete working item/package implementation in this translation unit.  The wrapper
// only adds the one synchronous ability-row resolver used by native opcode 801.
#include "../client/content/items/packages/package_item_build.cpp"

namespace sunrise::client::content::items::packages {

/**
 * Builds one missing ability-bucket row directly from the installed package before an opcode-801
 * selection is accepted.  The published domain remains live throughout; this never clears the
 * global ability catalog and never wakes the long-running investment extraction worker.
 */
bool ensure_subclass_ability_buckets(std::uint16_t socketEntryListIndex,
                                     const state::build_data::abilities::Selection& selection) noexcept {
    state::build_data::abilities::Definition existing{};
    if (state::build_data::find_ability_buckets(socketEntryListIndex, selection, existing)) {
        return true;
    }
    if (!state::build_data::ability_buckets_ready()
        || !state::build_data::socket_entry_lists_ready()) {
        return false;
    }

    Storage& storage = pass_storage();
    reader::BlockKeys keys{};
    core::path::Buffer directory{};
    if (!collect_keys(keys) || !package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        return false;
    }

    std::array<std::uint32_t, kContainerCandidates> candidates{};
    std::size_t candidateCount = 0;
    if (!investment_globals_tags(candidates, candidateCount)) {
        SecureZeroMemory(&keys, sizeof keys);
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
            || !tables::slot_tag(std::span<const std::byte>{storage.root},
                                 tables::kSocketEntryListTableSlot,
                                 tableTag)
            || tableTag == 0
            || !reader::read_tag(source, storage.scratch, tableTag, storage.abilityTable)) {
            continue;
        }

        tables::Array rows{};
        tables::IndexRow indexRow{};
        if (!tables::find_array_at(std::span<const std::byte>{storage.abilityTable},
                                   tables::kTableArrayDescriptor,
                                   rows)
            || !tables::index_row(std::span<const std::byte>{storage.abilityTable},
                                  rows,
                                  socketEntryListIndex,
                                  indexRow)
            || indexRow.targetTag == 0
            || !reader::read_tag(source, storage.scratch, indexRow.targetTag, storage.definition)) {
            continue;
        }

        state::build_data::abilities::Definition row{};
        row.socketEntryListIndex = socketEntryListIndex;
        row.selection = selection;
        resolved = build_ability_buckets(source,
                                         storage.scratch,
                                         std::span<const std::byte>{storage.definition},
                                         storage.abilityPool,
                                         selection,
                                         row)
                   && state::build_data::abilities::merge(row);
    }

    reader::close_files(storage.scratch);
    SecureZeroMemory(&keys, sizeof keys);

    std::array<char, 192> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=subclass stage=ability_row result=%s list=%u movement=%u grenade=%u super=%u melee=%u class=%u",
                                      resolved ? "ok" : "fail",
                                      static_cast<unsigned>(socketEntryListIndex),
                                      static_cast<unsigned>(selection.movementEntry),
                                      static_cast<unsigned>(selection.grenadeEntry),
                                      static_cast<unsigned>(selection.superEntry),
                                      static_cast<unsigned>(selection.meleeEntry),
                                      static_cast<unsigned>(selection.classEntry));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         resolved ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return resolved;
}

} // namespace sunrise::client::content::items::packages
