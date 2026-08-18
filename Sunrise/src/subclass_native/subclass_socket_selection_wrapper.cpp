#include "../middleware/datagen/family4/loadout/subclass_socket_selection.h"
#include "../state/build_data/runtime.h"

#define resolve_socket_states sunrise_base_resolve_socket_states
#include "../middleware/datagen/family4/loadout/subclass_socket_selection.cpp"
#undef resolve_socket_states

namespace sunrise::middleware::datagen::family4::loadout {
namespace {
constexpr std::size_t kNativeBundleSize = 4;
}

void resolve_socket_states(
    const build_socket_lists::Definition& definition,
    const state::CharacterState& character,
    std::array<instance::SocketEntryState, instance::layout::kSocketEntryStateCapacity>& output,
    std::array<instance::SocketSelector, kSelectorBucketCount>& selectors) noexcept {
    output.fill(instance::SocketEntryState::absent);
    selectors.fill(instance::SocketSelector{});
    for (std::size_t index = 0; index < definition.entryCount; ++index) {
        const std::uint64_t bit = std::uint64_t{1} << index;
        if ((definition.readyMask & bit) != 0) {
            output[index] = instance::SocketEntryState::ready;
        }
    }

    build_socket_lists::EntryTable entries{};
    if (!state::build_data::find_socket_entry_table(definition.definitionIndex, entries)) {
        return;
    }
    SubclassSelection selection{};
    subclass_selection(character, selection);

    std::array<std::uint16_t, 256> groupPopulation{};
    for (std::size_t index = 0; index < definition.entryCount; ++index) {
        ++groupPopulation[entries.entries[index].group];
    }

    std::array<std::uint32_t, 256> chosen{};
    std::array<bool, 256> claimed{};
    std::array<bool, build_socket_lists::kEntryCapacity> forcedActive{};
    for (const SelectedEntry& selected : selection.selected) {
        if (selected.entry >= definition.entryCount || selected.bucket >= selectors.size()) {
            continue;
        }
        selectors[selected.bucket] = instance::SocketSelector{selected.entry, 0, 0};
        const build_socket_lists::Entry& entry = entries.entries[selected.entry];
        if (entry.plugSource == build_socket_lists::kNoPlugSource || claimed[entry.group]) {
            continue;
        }
        claimed[entry.group] = true;
        chosen[entry.group] = entry.plugSource;
        if (groupPopulation[entry.group] <= kNativeBundleSize) {
            continue;
        }
        forcedActive[selected.entry] = true;
        for (std::size_t offset = 1;
             offset < kNativeBundleSize && selected.entry + offset < definition.entryCount
             && entries.entries[selected.entry + offset].group == entry.group;
             ++offset) {
            forcedActive[selected.entry + offset] = true;
        }
    }

    for (std::size_t index = 0; index < definition.entryCount; ++index) {
        const build_socket_lists::Entry& entry = entries.entries[index];
        const bool matchesGroup = entry.plugSource != build_socket_lists::kNoPlugSource
                                  && claimed[entry.group]
                                  && chosen[entry.group] == entry.plugSource;
        const bool superLane = entry.plugSource == build_socket_lists::kNoPlugSource
                               && entry.kind == build_socket_lists::kSuperEntryKind;
        if (matchesGroup || superLane || forcedActive[index]) {
            output[index] = instance::SocketEntryState::active;
        }
    }
}

} // namespace sunrise::middleware::datagen::family4::loadout
