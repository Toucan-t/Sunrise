#include "../client/content/items/packages/internal.h"
#include "../middleware/content/packages/tables/ability_pool_reader.h"
#include "../state/account/account_state.h"
#include "../state/build_data/runtime.h"
#include "../state/runtime/runtime.h"
#include "subclass_runtime_bridge.h"

// Compile the current working implementation into this translation unit, but keep its exported
// builder under a private name so the wrapper below can add bundle support and entry routing.
#define build_ability_buckets sunrise_base_build_ability_buckets
#include "../client/content/items/packages/package_ability_build.cpp"
#undef build_ability_buckets

namespace sunrise::client::content::items::packages {
namespace {

constexpr std::size_t kNativeBundleSize = 4;

void chosen_sources_native(const Walk& walk,
                           std::array<std::uint32_t, 256>& sources,
                           std::array<bool, pool::kEntryCapacity>& forcedActive) noexcept {
    sources.fill(pool::kNoPlugSource);
    forcedActive.fill(false);
    std::array<std::uint16_t, 256> population{};
    for (std::size_t index = 0; index < walk.entryCount; ++index) {
        ++population[walk.entries[index].group];
    }
    for (const std::uint8_t entryIndex : walk.selected) {
        if (entryIndex >= walk.entryCount) {
            continue;
        }
        const pool::Entry& entry = walk.entries[entryIndex];
        if (entry.plugSource == pool::kNoPlugSource || sources[entry.group] != pool::kNoPlugSource) {
            continue;
        }
        sources[entry.group] = entry.plugSource;
        if (population[entry.group] <= kNativeBundleSize) {
            continue;
        }
        forcedActive[entryIndex] = true;
        for (std::size_t offset = 1;
             offset < kNativeBundleSize && entryIndex + offset < walk.entryCount
             && walk.entries[entryIndex + offset].group == entry.group;
             ++offset) {
            forcedActive[entryIndex + offset] = true;
        }
    }
}

[[nodiscard]] bool active_native(
    const pool::Entry& entry,
    std::size_t entryIndex,
    const std::array<std::uint32_t, 256>& sources,
    const std::array<bool, pool::kEntryCapacity>& forcedActive) noexcept {
    if (entryIndex < forcedActive.size() && forcedActive[entryIndex]) {
        return true;
    }
    if (entry.plugSource == pool::kNoPlugSource) {
        return entry.kind == kSuperKind;
    }
    return sources[entry.group] == entry.plugSource;
}

void claim_bundle_kinds_native(
    const Walk& walk,
    const std::array<bool, pool::kEntryCapacity>& forcedActive,
    domain::Definition& output) noexcept {
    for (std::size_t entryIndex = 0; entryIndex < walk.entryCount; ++entryIndex) {
        if (!forcedActive[entryIndex]) {
            continue;
        }
        std::array<pool::PoolRecord, pool::kPoolRecordCapacity> records{};
        std::uint8_t bucket = 0;
        if (records_of(walk, walk.entries[entryIndex], 0, records) == 0
            || records[0].kind == pool::kEmptyByte
            || !selector_destination(walk, static_cast<std::uint8_t>(entryIndex), bucket)
            || output.buckets[bucket].kind != domain::kEmptyBucketKind) {
            continue;
        }
        output.buckets[bucket].kind = records[0].kind;
    }
}

} // namespace

bool build_ability_buckets(const reader::Source& source,
                           reader::Scratch& scratch,
                           std::span<const std::byte> listDefinition,
                           std::vector<std::byte>& blob,
                           const state::build_data::abilities::Selection& selection,
                           state::build_data::abilities::Definition& output) noexcept {
    Walk walk{};
    walk.source = &source;
    walk.scratch = &scratch;
    walk.blob = &blob;
    walk.entryCount = pool::read_entries(listDefinition, walk.entries);
    if (walk.entryCount == 0) {
        return false;
    }
    walk.selected = summary_entries(selection);

    std::array<std::uint8_t, state::build_data::socket_entry_lists::kEntryCapacity> entryBuckets{};
    entryBuckets.fill(subclass_native::kNoDestinationBucket);
    for (std::size_t entryIndex = 0;
         entryIndex < walk.entryCount && entryIndex < entryBuckets.size();
         ++entryIndex) {
        std::uint8_t bucket = 0;
        if (selector_destination(walk, static_cast<std::uint8_t>(entryIndex), bucket)) {
            entryBuckets[entryIndex] = bucket;
        }
    }
    subclass_native::publish_entry_buckets(
        output.socketEntryListIndex, entryBuckets, walk.entryCount);

    for (domain::Bucket& bucket : output.buckets) {
        bucket = {};
    }
    output.overflowCount = 0;
    if (!assign_kinds(walk, output)) {
        return false;
    }

    std::array<std::uint32_t, 256> sources{};
    std::array<bool, pool::kEntryCapacity> forcedActive{};
    chosen_sources_native(walk, sources, forcedActive);
    claim_bundle_kinds_native(walk, forcedActive, output);
    for (std::size_t entryIndex = 0; entryIndex < walk.entryCount; ++entryIndex) {
        if (!active_native(walk.entries[entryIndex], entryIndex, sources, forcedActive)) {
            continue;
        }
        std::array<pool::PoolRecord, pool::kPoolRecordCapacity> records{};
        const std::size_t count = records_of(walk, walk.entries[entryIndex], 0, records);
        for (std::size_t entry = 0; entry < count; ++entry) {
            file_hash(records[entry], output);
        }
    }
    return true;
}

} // namespace sunrise::client::content::items::packages
