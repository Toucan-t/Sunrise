#include "entity_destination_scan.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/entity_definition_reader.h"
#include "../../../middleware/content/packages/tables/map_data_table_reader.h"
#include "../../../middleware/content/packages/tables/scenario_reader.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../../targets/game/packages.h"

namespace sunrise::client::content::entities {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;
namespace layouts = state::build_data::scenarios;

/** Installed packages sit beside the main executable. */
constexpr std::wstring_view kPackageDirectory = L"packages";
/** The three object-registry arrays are independent and all can carry activity resources. */
constexpr std::array<std::size_t, 3> kRegistryDescriptors{
    tables::kRegistryFirstDescriptor,
    tables::kRegistrySecondDescriptor,
    tables::kRegistryThirdDescriptor,
};

/** Shadowkeep entity-resource kind classes used only as diagnostic feature labels. */
constexpr std::uint32_t kModelKindClass = 0x808072B8U;
constexpr std::uint32_t kPhysicsModelKindClass = 0x80806D5BU;
constexpr std::uint32_t kSkeletonKindClass = 0x80808545U;
constexpr std::uint32_t kControlRigKindClass = 0x80808B66U;

/** Heuristic nested-tag inspection is deliberately bounded to keep one click responsive. */
constexpr std::size_t kNestedTagReadAttemptLimit = 192;
constexpr std::size_t kNestedTagRetainLimit = 64;

/** Selected branch inspection keeps the chosen immediate resource at depth zero. */
constexpr std::uint16_t kBranchInspectMaximumDepth = 5;
/** Hard cap on package reads issued by one explicit selected-branch inspection click. */
constexpr std::size_t kBranchInspectReadAttemptLimit = 640;
/** Hard cap on retained selected-branch rows, including repeated references. */
constexpr std::size_t kBranchInspectNodeRetainLimit = 224;

/** Cross-actor nested-class analysis reads only immediate actor resources and their verified tags. */
constexpr std::size_t kNestedAnalysisReadAttemptLimit = 6144;
/** Per-actor cap prevents one unusually dense resource graph from consuming the whole analysis. */
constexpr std::size_t kNestedAnalysisPerActorReadAttemptLimit = 448;

/** Resource signature currently observed to carry nested entity definitions in actor graphs. */
constexpr std::uint32_t kNestedEntityCarrierKindClass = 0x808084D7U;
constexpr std::uint32_t kNestedEntityCarrierPayloadClass = 0x808084E9U;
/** Exact entity graph is intentionally small: it is a structural diagnostic, not a package crawler. */
constexpr std::uint16_t kEntityGraphMaximumDepth = 5;
constexpr std::size_t kEntityGraphEntityLimit = 64;
constexpr std::size_t kEntityGraphEdgeLimit = 128;
constexpr std::size_t kEntityGraphReadAttemptLimit = 1536;
/** One carrier scan may spend this many package reads looking only for entity-definition tags. */
constexpr std::size_t kEntityGraphPerCarrierReadAttemptLimit = 512;

struct HandleRef {
    std::int32_t bubbleIndex{};
    std::uint32_t resourceTag{};
};

struct Range {
    std::size_t first{};
    std::size_t count{};
    bool readable{};
};

struct ResourceRecord {
    std::uint32_t tableTag{};
    bool readable{};
};

struct ResourceKindProbe {
    std::uint32_t tagClass{};
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    bool readable{};
    bool kindReadable{};
    bool payloadReadable{};
};

struct DeepTagMetadata {
    std::uint32_t classId{};
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    std::size_t bytes{};
    bool readable{};
    bool entityResource{};
};

struct PlacementKey {
    std::uint32_t entityTag{};
    std::uint32_t tableTag{};
    std::uint64_t worldId{};
    std::array<std::uint32_t, 4> rotation{};
    std::array<std::uint32_t, 4> translation{};

    [[nodiscard]] bool operator==(const PlacementKey&) const noexcept = default;
};

struct PlacementKeyHash {
    [[nodiscard]] std::size_t operator()(const PlacementKey& key) const noexcept {
        std::size_t value = static_cast<std::size_t>(key.entityTag)
                            ^ (static_cast<std::size_t>(key.tableTag) << 1U);
        const auto mix = [&value](std::uint64_t part) noexcept {
            constexpr std::size_t kGolden =
                sizeof(std::size_t) == 8 ? static_cast<std::size_t>(0x9E3779B97F4A7C15ULL)
                                         : static_cast<std::size_t>(0x9E3779B9U);
            value ^= static_cast<std::size_t>(part) + kGolden + (value << 6U) + (value >> 2U);
        };
        mix(key.worldId);
        for (const std::uint32_t lane : key.rotation) {
            mix(lane);
        }
        for (const std::uint32_t lane : key.translation) {
            mix(lane);
        }
        return value;
    }
};

/** Large read scratch and all transient caches live off the render-thread stack. */
struct WorkStorage {
    reader::Scratch scratch{};
    std::vector<std::byte> scenario;
    std::vector<std::byte> entry;
    std::vector<std::byte> registry;
    std::vector<std::byte> object;
    std::vector<std::byte> resource;
    std::vector<std::byte> table;
    std::vector<std::byte> definition;
    std::vector<std::byte> definitionResource;
    std::vector<std::byte> nestedTag;
    std::vector<HandleRef> handles;
    std::vector<tables::MapDataEntry> tableEntries;
    std::unordered_map<std::uint32_t, Range> objectCache;
    std::unordered_map<std::uint32_t, ResourceRecord> resourceCache;
    std::unordered_map<std::uint32_t, Range> tableCache;
    std::unordered_set<PlacementKey, PlacementKeyHash> placementKeys;
};

SRWLOCK g_lock{SRWLOCK_INIT};
WorkStorage g_work;
Summary g_summary;
std::vector<Placement> g_placements;
DefinitionSummary g_definitionSummary;
std::vector<DefinitionResource> g_definitionResources;

ClassificationSummary g_classificationSummary;
std::vector<DefinitionSignature> g_definitionSignatures;
std::vector<ActorResourceFrequency> g_actorResourceFrequencies;
ResourceInspectSummary g_resourceInspectSummary;
std::vector<NestedTagReference> g_nestedTagReferences;
BranchInspectSummary g_branchInspectSummary;
std::vector<BranchInspectNode> g_branchInspectNodes;
NestedAnalysisSummary g_nestedAnalysisSummary;
std::vector<ActorNestedClassFrequency> g_actorNestedClassFrequencies;
std::vector<ActorNestedProfile> g_actorNestedProfiles;
std::vector<ActorNestedCluster> g_actorNestedClusters;
EntityGraphSummary g_entityGraphSummary;
std::vector<EntityGraphNode> g_entityGraphNodes;
std::vector<EntityGraphEdge> g_entityGraphEdges;

/** Copies the installed package block keys for one bounded scan. */
[[nodiscard]] bool collect_keys(reader::BlockKeys& keys) noexcept {
    keys = {};
    const state::SignOnState& signOn = state::sign_on();
    targets::game::packages::KeyTable table{};
    if (!signOn.bootstrapTokenPresent || !targets::game::packages::read(table)) {
        SecureZeroMemory(&table, sizeof table);
        return false;
    }
    keys.alternate = table.alternateKey;
    keys.nonceBase = table.nonceBase;
    for (std::size_t index = 0; index < keys.primary.size(); ++index) {
        const auto tokenByte = static_cast<unsigned char>(signOn.bootstrapToken[index]);
        const auto constantByte = static_cast<unsigned char>(table.identityConstant[index]);
        keys.primary[index] = static_cast<std::byte>(tokenByte + constantByte);
    }
    SecureZeroMemory(&table, sizeof table);
    return true;
}

/** Finds the installed package directory beside the game executable. */
[[nodiscard]] bool package_directory(core::path::Buffer& directory) noexcept {
    if (!core::path::module_directory(GetModuleHandleW(nullptr), directory)
        || !core::path::append(directory, kPackageDirectory)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.chars.data());
    return attributes != INVALID_FILE_ATTRIBUTES
           && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** Clears only the transient caches while keeping their allocated capacity for the next scan. */
void reset_work() noexcept {
    reader::close_files(g_work.scratch);
    g_work.scenario.clear();
    g_work.entry.clear();
    g_work.registry.clear();
    g_work.object.clear();
    g_work.resource.clear();
    g_work.table.clear();
    g_work.definition.clear();
    g_work.definitionResource.clear();
    g_work.nestedTag.clear();
    g_work.handles.clear();
    g_work.tableEntries.clear();
    g_work.objectCache.clear();
    g_work.resourceCache.clear();
    g_work.tableCache.clear();
    g_work.placementKeys.clear();
}

/** Publishes a failure without leaving rows from a previous destination visible. */
void fail(ScanResult result, std::string_view destination) noexcept {
    g_summary = {};
    g_summary.result = result;
    const std::size_t copy = std::min(destination.size(), g_summary.destination.size());
    if (copy != 0) {
        std::memcpy(g_summary.destination.data(), destination.data(), copy);
    }
    g_summary.destinationLength = static_cast<std::uint8_t>(copy);
    g_placements.clear();
}

/** Reads and memoizes every per-bubble resource handle one placed object declares. */
[[nodiscard]] Range object_handles(const reader::Source& source,
                                   std::uint32_t objectTag) noexcept {
    const auto known = g_work.objectCache.find(objectTag);
    if (known != g_work.objectCache.end()) {
        return known->second;
    }

    Range range{g_work.handles.size(), 0, false};
    std::uint32_t objectClass = 0;
    if (!reader::read_tag(source, g_work.scratch, objectTag, g_work.object, objectClass)
        || objectClass != tables::kObjectClass) {
        ++g_summary.objectReadFailures;
        g_work.objectCache.emplace(objectTag, range);
        return range;
    }
    ++g_summary.objects;

    tables::Array bubbles{};
    if (tables::object_bubbles(g_work.object, bubbles)) {
        for (std::uint64_t index = 0; index < bubbles.count; ++index) {
            tables::ObjectBubble bubble{};
            if (!tables::object_bubble_at(g_work.object, bubbles, index, bubble)) {
                continue;
            }
            for (std::uint64_t handleIndex = 0; handleIndex < bubble.handleCount; ++handleIndex) {
                std::uint32_t resourceTag = 0;
                if (!tables::object_placed_handle_at(
                        g_work.object, bubble, handleIndex, resourceTag)) {
                    continue;
                }
                g_work.handles.push_back(HandleRef{bubble.bubbleIndex, resourceTag});
            }
        }
    }
    range.count = g_work.handles.size() - range.first;
    range.readable = true;
    g_work.objectCache.emplace(objectTag, range);
    return range;
}

/** Resolves one activity-resource tag to its map-data-table tag, once per scan. */
[[nodiscard]] ResourceRecord resource_table(const reader::Source& source,
                                            std::uint32_t resourceTag) noexcept {
    const auto known = g_work.resourceCache.find(resourceTag);
    if (known != g_work.resourceCache.end()) {
        return known->second;
    }

    ResourceRecord record{};
    std::uint32_t classId = 0;
    if (!reader::read_tag(source, g_work.scratch, resourceTag, g_work.resource, classId)) {
        ++g_summary.resourceReadFailures;
        g_work.resourceCache.emplace(resourceTag, record);
        return record;
    }
    if (classId != tables::kActivityEntityResourceClass) {
        ++g_summary.unexpectedResourceClasses;
        g_work.resourceCache.emplace(resourceTag, record);
        return record;
    }
    ++g_summary.resources;
    record.readable = tables::activity_entity_table_tag(g_work.resource, record.tableTag);
    g_work.resourceCache.emplace(resourceTag, record);
    return record;
}

/** Reads and memoizes every entry from one map-data table. */
[[nodiscard]] Range table_entries(const reader::Source& source, std::uint32_t tableTag) noexcept {
    const auto known = g_work.tableCache.find(tableTag);
    if (known != g_work.tableCache.end()) {
        return known->second;
    }

    Range range{g_work.tableEntries.size(), 0, false};
    std::uint32_t classId = 0;
    if (!reader::read_tag(source, g_work.scratch, tableTag, g_work.table, classId)) {
        ++g_summary.tableReadFailures;
        g_work.tableCache.emplace(tableTag, range);
        return range;
    }
    if (classId != tables::kMapDataTableClass) {
        ++g_summary.unexpectedTableClasses;
        g_work.tableCache.emplace(tableTag, range);
        return range;
    }
    ++g_summary.tables;

    tables::Array entries{};
    if (!tables::map_data_entries(g_work.table, entries)) {
        ++g_summary.malformedTables;
        g_work.tableCache.emplace(tableTag, range);
        return range;
    }
    g_summary.entries += static_cast<std::size_t>(entries.count);
    for (std::uint64_t index = 0; index < entries.count; ++index) {
        tables::MapDataEntry entry{};
        if (!tables::map_data_entry_at(g_work.table, entries, index, entry)) {
            ++g_summary.malformedEntries;
            continue;
        }
        g_work.tableEntries.push_back(entry);
    }
    range.count = g_work.tableEntries.size() - range.first;
    range.readable = true;
    g_work.tableCache.emplace(tableTag, range);
    return range;
}

/** Builds the exact authored-row identity used only to remove repeated provenance paths. */
[[nodiscard]] PlacementKey placement_key(std::uint32_t tableTag,
                                         const tables::MapDataEntry& entry) noexcept {
    PlacementKey key{};
    key.entityTag = entry.entityTag;
    key.tableTag = tableTag;
    key.worldId = entry.worldId;
    key.rotation = {std::bit_cast<std::uint32_t>(entry.rotation.x),
                    std::bit_cast<std::uint32_t>(entry.rotation.y),
                    std::bit_cast<std::uint32_t>(entry.rotation.z),
                    std::bit_cast<std::uint32_t>(entry.rotation.w)};
    key.translation = {std::bit_cast<std::uint32_t>(entry.translation.x),
                       std::bit_cast<std::uint32_t>(entry.translation.y),
                       std::bit_cast<std::uint32_t>(entry.translation.z),
                       std::bit_cast<std::uint32_t>(entry.translation.w)};
    return key;
}

/** Appends every entity row reached through one placed object in one scenario context. */
void append_object(const reader::Source& source,
                   std::uint32_t scenarioTag,
                   std::uint16_t bubbleOrdinal,
                   std::uint16_t stateOrdinal,
                   std::uint32_t bubbleNameHash,
                   std::uint32_t sliceSetIndex,
                   std::uint32_t objectTag) noexcept {
    const Range handles = object_handles(source, objectTag);
    if (!handles.readable) {
        return;
    }
    for (std::size_t index = 0; index < handles.count; ++index) {
        const HandleRef& handle = g_work.handles[handles.first + index];
        if (handle.bubbleIndex != tables::kGlobalBubbleIndex
            && handle.bubbleIndex != static_cast<std::int32_t>(bubbleOrdinal)) {
            continue;
        }
        const ResourceRecord resource = resource_table(source, handle.resourceTag);
        if (!resource.readable) {
            continue;
        }
        const Range entries = table_entries(source, resource.tableTag);
        if (!entries.readable) {
            continue;
        }
        for (std::size_t entryIndex = 0; entryIndex < entries.count; ++entryIndex) {
            const tables::MapDataEntry& entry = g_work.tableEntries[entries.first + entryIndex];
            if (tables::package_of(entry.entityTag) == tables::kAbsentPackageId) {
                continue;
            }
            ++g_summary.rawEntities;
            const auto [ignored, inserted] =
                g_work.placementKeys.emplace(placement_key(resource.tableTag, entry));
            (void)ignored;
            if (!inserted) {
                ++g_summary.duplicateEntities;
                continue;
            }
            Placement placement{};
            placement.scenarioTag = scenarioTag;
            placement.bubbleNameHash = bubbleNameHash;
            placement.sliceSetIndex = sliceSetIndex;
            placement.objectTag = objectTag;
            placement.resourceTag = handle.resourceTag;
            placement.tableTag = resource.tableTag;
            placement.entityTag = entry.entityTag;
            placement.worldId = entry.worldId;
            placement.objectBubbleIndex = handle.bubbleIndex;
            placement.bubbleOrdinal = bubbleOrdinal;
            placement.stateOrdinal = stateOrdinal;
            placement.rotation = {entry.rotation.x, entry.rotation.y, entry.rotation.z, entry.rotation.w};
            placement.translation = {
                entry.translation.x, entry.translation.y, entry.translation.z, entry.translation.w};
            placement.dataResourceClass = entry.dataResourceClass;
            placement.dataResourceClassReadable = entry.dataResourceClassReadable;
            g_placements.push_back(placement);
            ++g_summary.entities;
            if (placement.worldId == ~std::uint64_t{0}) {
                ++g_summary.sentinelEntities;
            }
        }
    }
}

/** Walks one loaded object registry. */
[[nodiscard]] bool walk_registry(const reader::Source& source,
                                 std::uint32_t scenarioTag,
                                 std::uint16_t bubbleOrdinal,
                                 std::uint16_t stateOrdinal,
                                 std::uint32_t bubbleNameHash,
                                 std::uint32_t sliceSetIndex) noexcept {
    for (const std::size_t descriptor : kRegistryDescriptors) {
        tables::Array objects{};
        if (!tables::registry_objects(g_work.registry, descriptor, objects)) {
            continue;
        }
        for (std::uint64_t index = 0; index < objects.count; ++index) {
            std::uint32_t objectTag = 0;
            if (!tables::registry_object_at(g_work.registry, objects, index, objectTag)) {
                return false;
            }
            append_object(source,
                          scenarioTag,
                          bubbleOrdinal,
                          stateOrdinal,
                          bubbleNameHash,
                          sliceSetIndex,
                          objectTag);
        }
    }
    return true;
}

/** Logs one concise diagnostic line for an explicit scan. */
void report() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_entities stage=destination_scan result=%s dest=%.*s bubbles=%zu "
        "slices=%zu objects=%zu resources=%zu tables=%zu entries=%zu raw=%zu unique=%zu "
        "duplicates=%zu sentinel=%zu object_fail=%zu resource_fail=%zu resource_class=%zu table_fail=%zu "
        "table_class=%zu malformed_tables=%zu malformed_entries=%zu",
        result_label(g_summary.result),
        static_cast<int>(g_summary.destinationLength),
        g_summary.destination.data(),
        g_summary.bubbles,
        g_summary.sliceSets,
        g_summary.objects,
        g_summary.resources,
        g_summary.tables,
        g_summary.entries,
        g_summary.rawEntities,
        g_summary.entities,
        g_summary.duplicateEntities,
        g_summary.sentinelEntities,
        g_summary.objectReadFailures,
        g_summary.resourceReadFailures,
        g_summary.unexpectedResourceClasses,
        g_summary.tableReadFailures,
        g_summary.unexpectedTableClasses,
        g_summary.malformedTables,
        g_summary.malformedEntries);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         g_summary.result == ScanResult::ok ? core::log::Level::info
                                                           : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Clears the selected-definition result without disturbing the destination catalog. */
void reset_definition() noexcept {
    g_definitionSummary = {};
    g_definitionResources.clear();
    g_work.definition.clear();
    g_work.definitionResource.clear();
}

/** Logs one concise line for an explicit entity-definition inspection. */
void report_definition() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_entity_definition result=%s entity=0x%08X class=0x%08X bytes=%zu "
        "declared=%zu readable=%zu read_fail=%zu malformed_ptr=%zu",
        inspect_result_label(g_definitionSummary.result),
        static_cast<unsigned>(g_definitionSummary.entityTag),
        static_cast<unsigned>(g_definitionSummary.definitionClass),
        g_definitionSummary.bytes,
        g_definitionSummary.declaredResources,
        g_definitionSummary.readableResources,
        g_definitionSummary.resourceReadFailures,
        g_definitionSummary.malformedPointers);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         g_definitionSummary.result == InspectResult::ok ? core::log::Level::info
                                                                        : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Applies one known Shadowkeep entity-resource kind to a definition signature. */
void note_known_kind(DefinitionSignature& signature, std::uint32_t kindClass) noexcept {
    switch (kindClass) {
    case kModelKindClass:
        signature.hasModel = true;
        return;
    case kPhysicsModelKindClass:
        signature.hasPhysicsModel = true;
        return;
    case kSkeletonKindClass:
        signature.hasSkeleton = true;
        return;
    case kControlRigKindClass:
        signature.hasControlRig = true;
        return;
    default:
        ++signature.unknownKinds;
        return;
    }
}

/** Reads one resource's tag and kind class, memoized for a bulk classification pass. */
[[nodiscard]] ResourceKindProbe probe_resource_kind(
    const reader::Source& source,
    std::uint32_t tag,
    std::unordered_map<std::uint32_t, ResourceKindProbe>& cache) noexcept {
    const auto known = cache.find(tag);
    if (known != cache.end()) {
        return known->second;
    }

    ResourceKindProbe probe{};
    if (!reader::read_tag(source,
                          g_work.scratch,
                          tag,
                          g_work.definitionResource,
                          probe.tagClass)) {
        ++g_classificationSummary.resourceReadFailures;
        cache.emplace(tag, probe);
        return probe;
    }
    probe.readable = true;
    probe.kindReadable = tables::resource_pointer_class(g_work.definitionResource,
                                                        tables::kEntityResourceKindPointerOffset,
                                                        probe.kindClass);
    probe.payloadReadable = tables::resource_pointer_class(
        g_work.definitionResource, tables::kEntityResourcePayloadPointerOffset, probe.payloadClass);
    cache.emplace(tag, probe);
    return probe;
}

/** @return Actor-priority score based on the resource signals that are present in this build. */
[[nodiscard]] std::uint8_t actor_score(const DefinitionSignature& signature) noexcept {
    if (signature.hasModel && signature.hasSkeleton && signature.hasPhysicsModel) {
        return 4;
    }
    if (signature.hasModel && signature.hasSkeleton) {
        return 3;
    }
    if (signature.hasSkeleton) {
        return 2;
    }
    if (signature.hasModel && signature.hasPhysicsModel) {
        return 1;
    }
    return 0;
}

/** @return Stable sort score that puts skeleton-bearing definitions first. */
[[nodiscard]] unsigned signature_score(const DefinitionSignature& signature) noexcept {
    return static_cast<unsigned>(signature.actorScore);
}

/** Logs one concise line for the explicit bulk classification pass. */
void report_classification() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_entity_classification result=%s definitions=%zu readable=%zu model=%zu "
        "physics=%zu skeleton=%zu rig=%zu actor_candidates=%zu actor_signatures=%zu "
        "definition_fail=%zu malformed=%zu resource_fail=%zu",
        classification_result_label(g_classificationSummary.result),
        g_classificationSummary.definitions,
        g_classificationSummary.readableDefinitions,
        g_classificationSummary.modelDefinitions,
        g_classificationSummary.physicsDefinitions,
        g_classificationSummary.skeletonDefinitions,
        g_classificationSummary.controlRigDefinitions,
        g_classificationSummary.actorCandidates,
        g_actorResourceFrequencies.size(),
        g_classificationSummary.definitionReadFailures,
        g_classificationSummary.malformedDefinitions,
        g_classificationSummary.resourceReadFailures);
    if (written > 0) {
        core::log::write(
            core::log::Channel::state,
            g_classificationSummary.result == ClassificationResult::ok ? core::log::Level::info
                                                                       : core::log::Level::warn,
            {line.data(), static_cast<std::size_t>(written)});
    }
}


/** Logs one concise line for a bounded selected-resource heuristic inspection. */
void report_resource_inspection() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_entity_resource result=%s resource=0x%08X class=0x%08X kind=0x%08X "
        "payload=0x%08X bytes=%zu candidates=%zu attempts=%zu nested=%zu read_fail=%zu truncated=%u",
        resource_inspect_result_label(g_resourceInspectSummary.result),
        static_cast<unsigned>(g_resourceInspectSummary.resourceTag),
        static_cast<unsigned>(g_resourceInspectSummary.resourceClass),
        static_cast<unsigned>(g_resourceInspectSummary.kindClass),
        static_cast<unsigned>(g_resourceInspectSummary.payloadClass),
        g_resourceInspectSummary.bytes,
        g_resourceInspectSummary.candidateWords,
        g_resourceInspectSummary.readAttempts,
        g_resourceInspectSummary.nestedTags,
        g_resourceInspectSummary.readFailures,
        g_resourceInspectSummary.truncated ? 1U : 0U);
    if (written > 0) {
        core::log::write(
            core::log::Channel::state,
            g_resourceInspectSummary.result == ResourceInspectResult::ok ? core::log::Level::info
                                                                         : core::log::Level::warn,
            {line.data(), static_cast<std::size_t>(written)});
    }
}


/** Queue row for one retained tag whose readable blob may expose more verified nested tags. */
struct BranchQueueItem {
    std::uint32_t tag{};
    std::size_t nodeIndex{};
    std::uint16_t depth{};
};

/** Per-click state for the bounded selected-resource branch inspection. */
struct BranchWalkRuntime {
    const reader::Source* source{};
    std::unordered_map<std::uint32_t, DeepTagMetadata> metadata;
    std::unordered_set<std::uint32_t> attempted;
    std::unordered_set<std::uint32_t> expanded;
    std::unordered_set<std::uint32_t> queued;
    std::vector<BranchQueueItem> queue;
};

/** Reads one tag once for selected-branch metadata, bounded by the click budget. */
[[nodiscard]] bool branch_read_metadata(BranchWalkRuntime& walk,
                                        std::uint32_t tag,
                                        DeepTagMetadata& output) noexcept {
    output = {};
    const auto known = walk.metadata.find(tag);
    if (known != walk.metadata.end()) {
        output = known->second;
        return output.readable;
    }
    if (!walk.attempted.insert(tag).second) {
        return false;
    }
    if (g_branchInspectSummary.tagReadAttempts >= kBranchInspectReadAttemptLimit) {
        g_branchInspectSummary.truncated = true;
        return false;
    }

    ++g_branchInspectSummary.tagReadAttempts;
    std::vector<std::byte> blob;
    std::uint32_t classId = 0;
    if (walk.source == nullptr
        || !reader::read_tag(*walk.source, g_work.scratch, tag, blob, classId)) {
        ++g_branchInspectSummary.tagReadFailures;
        return false;
    }

    output.classId = classId;
    output.bytes = blob.size();
    output.readable = true;
    output.entityResource = classId == tables::kEntityResourceClass;
    if (output.entityResource) {
        (void)tables::resource_pointer_class(
            blob, tables::kEntityResourceKindPointerOffset, output.kindClass);
        (void)tables::resource_pointer_class(
            blob, tables::kEntityResourcePayloadPointerOffset, output.payloadClass);
    }
    walk.metadata.emplace(tag, output);
    return true;
}

/** Retains one selected-branch row and queues it when it can still be expanded. */
[[nodiscard]] std::size_t branch_retain_node(BranchWalkRuntime& walk,
                                             std::uint32_t tag,
                                             const DeepTagMetadata& metadata,
                                             std::size_t parentIndex,
                                             std::size_t sourceOffset,
                                             std::uint16_t depth,
                                             bool exactRelation,
                                             bool repeated) noexcept {
    if (g_branchInspectNodes.size() >= kBranchInspectNodeRetainLimit) {
        g_branchInspectSummary.truncated = true;
        return (std::numeric_limits<std::size_t>::max)();
    }
    const std::size_t nodeIndex = g_branchInspectNodes.size();
    g_branchInspectNodes.push_back(BranchInspectNode{tag,
                                                    metadata.classId,
                                                    metadata.kindClass,
                                                    metadata.payloadClass,
                                                    parentIndex,
                                                    sourceOffset,
                                                    metadata.bytes,
                                                    depth,
                                                    metadata.readable,
                                                    metadata.entityResource,
                                                    exactRelation,
                                                    repeated});
    g_branchInspectSummary.deepestLevel =
        (std::max)(g_branchInspectSummary.deepestLevel, depth);
    if (exactRelation) {
        ++g_branchInspectSummary.exactLinks;
    }
    if (repeated) {
        ++g_branchInspectSummary.repeatedReferences;
    }
    if (depth < kBranchInspectMaximumDepth && walk.expanded.find(tag) == walk.expanded.end()
        && walk.queued.insert(tag).second) {
        walk.queue.push_back(BranchQueueItem{tag, nodeIndex, depth});
    }
    return nodeIndex;
}

/** Expands one entity definition through its exact resource-reference array. */
void branch_expand_entity_definition(BranchWalkRuntime& walk,
                                     const BranchQueueItem& item,
                                     std::span<const std::byte> blob) noexcept {
    tables::Array resources{};
    if (!tables::entity_resources(blob, resources)) {
        return;
    }
    for (std::uint64_t index = 0; index < resources.count; ++index) {
        if (g_branchInspectNodes.size() >= kBranchInspectNodeRetainLimit
            || g_branchInspectSummary.tagReadAttempts >= kBranchInspectReadAttemptLimit) {
            g_branchInspectSummary.truncated = true;
            return;
        }
        std::uint32_t resourceTag = 0;
        if (!tables::entity_resource_at(blob, resources, index, resourceTag)) {
            continue;
        }
        const bool repeated = walk.metadata.find(resourceTag) != walk.metadata.end();
        DeepTagMetadata metadata{};
        if (!branch_read_metadata(walk, resourceTag, metadata)) {
            continue;
        }
        const std::size_t rowOffset =
            resources.dataOffset
            + static_cast<std::size_t>(index) * tables::kEntityResourceEntryStride
            + tables::kEntityResourceTagOffset;
        const std::uint16_t childDepth = static_cast<std::uint16_t>(item.depth + 1U);
        (void)branch_retain_node(walk,
                                 resourceTag,
                                 metadata,
                                 item.nodeIndex,
                                 rowOffset,
                                 childDepth,
                                 true,
                                 repeated);
    }
}

/**
 * Scans one verified readable tag for verified package tags. Entity-definition blobs are special:
 * their resource array is parsed exactly instead of word-scanned, which sharply reduces noise once
 * a branch reaches a child entity.
 */
void branch_expand_node(BranchWalkRuntime& walk, const BranchQueueItem& item) noexcept {
    if (item.depth >= kBranchInspectMaximumDepth || walk.source == nullptr
        || !walk.expanded.insert(item.tag).second) {
        return;
    }
    if (g_branchInspectSummary.tagReadAttempts >= kBranchInspectReadAttemptLimit) {
        g_branchInspectSummary.truncated = true;
        return;
    }

    ++g_branchInspectSummary.tagReadAttempts;
    std::vector<std::byte> blob;
    std::uint32_t classId = 0;
    if (!reader::read_tag(*walk.source, g_work.scratch, item.tag, blob, classId)) {
        ++g_branchInspectSummary.tagReadFailures;
        return;
    }

    if (classId == tables::kEntityDefinitionClass) {
        branch_expand_entity_definition(walk, item, blob);
        return;
    }

    std::unordered_set<std::uint32_t> localCandidates;
    for (std::size_t offset = 0; offset <= blob.size() && blob.size() - offset >= sizeof(std::uint32_t);
         offset += sizeof(std::uint32_t)) {
        if (g_branchInspectNodes.size() >= kBranchInspectNodeRetainLimit
            || g_branchInspectSummary.tagReadAttempts >= kBranchInspectReadAttemptLimit) {
            g_branchInspectSummary.truncated = true;
            break;
        }

        std::uint32_t candidate = 0;
        std::memcpy(&candidate, blob.data() + offset, sizeof candidate);
        if (candidate == item.tag || tables::package_of(candidate) == tables::kAbsentPackageId
            || !localCandidates.insert(candidate).second) {
            continue;
        }
        ++g_branchInspectSummary.candidateWords;

        const bool repeated = walk.metadata.find(candidate) != walk.metadata.end();
        DeepTagMetadata metadata{};
        if (!branch_read_metadata(walk, candidate, metadata)) {
            continue;
        }
        const std::uint16_t childDepth = static_cast<std::uint16_t>(item.depth + 1U);
        (void)branch_retain_node(
            walk, candidate, metadata, item.nodeIndex, offset, childDepth, false, repeated);
    }
}

/** Logs one concise selected-branch summary and a bounded tree sample. */
void report_branch_inspection() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_actor_branch result=%s entity=0x%08X resource=0x%08X class=0x%08X "
        "kind=0x%08X payload=0x%08X bytes=%zu nodes=%zu reads=%zu read_fail=%zu "
        "candidates=%zu exact=%zu repeats=%zu depth=%u truncated=%u",
        branch_inspect_result_label(g_branchInspectSummary.result),
        static_cast<unsigned>(g_branchInspectSummary.entityTag),
        static_cast<unsigned>(g_branchInspectSummary.resourceTag),
        static_cast<unsigned>(g_branchInspectSummary.resourceClass),
        static_cast<unsigned>(g_branchInspectSummary.kindClass),
        static_cast<unsigned>(g_branchInspectSummary.payloadClass),
        g_branchInspectSummary.resourceBytes,
        g_branchInspectSummary.retainedNodes,
        g_branchInspectSummary.tagReadAttempts,
        g_branchInspectSummary.tagReadFailures,
        g_branchInspectSummary.candidateWords,
        g_branchInspectSummary.exactLinks,
        g_branchInspectSummary.repeatedReferences,
        static_cast<unsigned>(g_branchInspectSummary.deepestLevel),
        g_branchInspectSummary.truncated ? 1U : 0U);
    if (written > 0) {
        core::log::write(
            core::log::Channel::state,
            g_branchInspectSummary.result == BranchInspectResult::ok ? core::log::Level::info
                                                                     : core::log::Level::warn,
            {line.data(), static_cast<std::size_t>(written)});
    }

    constexpr std::size_t kLogNodeLimit = 64;
    const std::size_t count = (std::min)(g_branchInspectNodes.size(), kLogNodeLimit);
    for (std::size_t index = 0; index < count; ++index) {
        const BranchInspectNode& node = g_branchInspectNodes[index];
        line = {};
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity_actor_branch_node index=%zu depth=%u parent=%zu offset=0x%zX "
            "tag=0x%08X class=0x%08X kind=0x%08X payload=0x%08X bytes=%zu exact=%u repeated=%u",
            index,
            static_cast<unsigned>(node.depth),
            node.parentIndex,
            node.sourceOffset,
            static_cast<unsigned>(node.tag),
            static_cast<unsigned>(node.classId),
            static_cast<unsigned>(node.kindClass),
            static_cast<unsigned>(node.payloadClass),
            node.bytes,
            node.exactRelation ? 1U : 0U,
            node.repeated ? 1U : 0U);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

struct NestedClassWorkFrequency {
    std::size_t definitions{};
    std::size_t references{};
    std::uint32_t exampleEntityTag{};
    std::uint32_t exampleTag{};
};

struct NestedActorWorkProfile {
    std::uint32_t entityTag{};
    std::vector<std::uint32_t> classes;
    std::size_t references{};
    std::size_t nestedEntityDefinitions{};
    bool complete{true};
};

struct NestedClusterWork {
    std::vector<std::uint32_t> classes;
    std::vector<std::size_t> profileIndices;
};

struct NestedTagClassCache {
    std::unordered_map<std::uint32_t, std::uint32_t> readable;
    std::unordered_set<std::uint32_t> unreadable;
};

/** @return True while both the global and current-actor nested-analysis read budgets remain. */
[[nodiscard]] bool nested_read_budget_available(std::size_t actorReadStart) noexcept {
    return g_nestedAnalysisSummary.tagReadAttempts < kNestedAnalysisReadAttemptLimit
           && g_nestedAnalysisSummary.tagReadAttempts - actorReadStart
                  < kNestedAnalysisPerActorReadAttemptLimit;
}

/** Reads one blob while charging the explicit nested-analysis package-read budget. */
[[nodiscard]] bool nested_read_blob(const reader::Source& source,
                                    std::size_t actorReadStart,
                                    std::uint32_t tag,
                                    std::vector<std::byte>& blob,
                                    std::uint32_t& classId) noexcept {
    blob.clear();
    classId = 0;
    if (!nested_read_budget_available(actorReadStart)) {
        g_nestedAnalysisSummary.truncated = true;
        return false;
    }
    ++g_nestedAnalysisSummary.tagReadAttempts;
    if (!reader::read_tag(source, g_work.scratch, tag, blob, classId)) {
        ++g_nestedAnalysisSummary.tagReadFailures;
        return false;
    }
    return true;
}

/** Resolves only a candidate tag's class, caching both successful and failed package reads. */
[[nodiscard]] bool nested_read_candidate_class(const reader::Source& source,
                                               std::size_t actorReadStart,
                                               NestedTagClassCache& cache,
                                               std::uint32_t tag,
                                               std::uint32_t& classId) noexcept {
    classId = 0;
    const auto known = cache.readable.find(tag);
    if (known != cache.readable.end()) {
        classId = known->second;
        return true;
    }
    if (cache.unreadable.find(tag) != cache.unreadable.end()) {
        return false;
    }
    if (!nested_read_budget_available(actorReadStart)) {
        g_nestedAnalysisSummary.truncated = true;
        return false;
    }

    ++g_nestedAnalysisSummary.tagReadAttempts;
    std::uint32_t readClass = 0;
    if (!reader::read_tag(source, g_work.scratch, tag, g_work.nestedTag, readClass)) {
        ++g_nestedAnalysisSummary.tagReadFailures;
        cache.unreadable.insert(tag);
        return false;
    }
    cache.readable.emplace(tag, readClass);
    classId = readClass;
    return true;
}

/** Logs the cross-actor nested-class analysis and a bounded set of its most shared classes. */
void report_nested_analysis() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_actor_nested result=%s actors=%zu complete=%zu classes=%zu clusters=%zu "
        "reads=%zu read_fail=%zu candidates=%zu truncated=%u",
        nested_analysis_result_label(g_nestedAnalysisSummary.result),
        g_nestedAnalysisSummary.actorCandidates,
        g_nestedAnalysisSummary.completeActors,
        g_nestedAnalysisSummary.uniqueClasses,
        g_nestedAnalysisSummary.clusters,
        g_nestedAnalysisSummary.tagReadAttempts,
        g_nestedAnalysisSummary.tagReadFailures,
        g_nestedAnalysisSummary.candidateWords,
        g_nestedAnalysisSummary.truncated ? 1U : 0U);
    if (written > 0) {
        core::log::write(
            core::log::Channel::state,
            g_nestedAnalysisSummary.result == NestedAnalysisResult::ok ? core::log::Level::info
                                                                       : core::log::Level::warn,
            {line.data(), static_cast<std::size_t>(written)});
    }

    constexpr std::size_t kClassLogLimit = 48;
    const std::size_t classCount =
        (std::min)(g_actorNestedClassFrequencies.size(), kClassLogLimit);
    for (std::size_t index = 0; index < classCount; ++index) {
        const ActorNestedClassFrequency& row = g_actorNestedClassFrequencies[index];
        line = {};
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity_actor_nested_class index=%zu class=0x%08X actors=%zu refs=%zu "
            "example_entity=0x%08X example_tag=0x%08X",
            index,
            static_cast<unsigned>(row.classId),
            row.definitions,
            row.references,
            static_cast<unsigned>(row.exampleEntityTag),
            static_cast<unsigned>(row.exampleTag));
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }

    for (const ActorNestedCluster& cluster : g_actorNestedClusters) {
        line = {};
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity_actor_cluster id=%u actors=%zu classes=%zu representative=0x%08X",
            static_cast<unsigned>(cluster.clusterId),
            cluster.definitions,
            cluster.uniqueClasses,
            static_cast<unsigned>(cluster.representativeEntityTag));
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}


/** Per-click cache and cycle guards for the exact nested-entity graph. */
struct EntityGraphRuntime {
    const reader::Source* source{};
    std::unordered_map<std::uint32_t, DeepTagMetadata> metadata;
    std::unordered_map<std::uint32_t, std::size_t> entityIndices;
    std::unordered_set<std::uint32_t> expandedEntities;
    std::unordered_set<std::uint32_t> scannedCarrierResources;
};

/** Reads metadata for one package tag once within the graph budget. */
[[nodiscard]] bool graph_read_metadata(EntityGraphRuntime& runtime,
                                       std::uint32_t tag,
                                       DeepTagMetadata& output) noexcept {
    output = {};
    const auto known = runtime.metadata.find(tag);
    if (known != runtime.metadata.end()) {
        output = known->second;
        return output.readable;
    }
    if (g_entityGraphSummary.tagReadAttempts >= kEntityGraphReadAttemptLimit) {
        g_entityGraphSummary.truncated = true;
        return false;
    }

    ++g_entityGraphSummary.tagReadAttempts;
    std::vector<std::byte> blob;
    std::uint32_t classId = 0;
    if (runtime.source == nullptr
        || !reader::read_tag(*runtime.source, g_work.scratch, tag, blob, classId)) {
        ++g_entityGraphSummary.tagReadFailures;
        return false;
    }

    output.classId = classId;
    output.bytes = blob.size();
    output.readable = true;
    output.entityResource = classId == tables::kEntityResourceClass;
    if (output.entityResource) {
        (void)tables::resource_pointer_class(
            blob, tables::kEntityResourceKindPointerOffset, output.kindClass);
        (void)tables::resource_pointer_class(
            blob, tables::kEntityResourcePayloadPointerOffset, output.payloadClass);
    }
    runtime.metadata.emplace(tag, output);
    return true;
}

[[nodiscard]] std::size_t graph_expand_entity(EntityGraphRuntime& runtime,
                                              std::uint32_t entityTag,
                                              std::uint16_t depth) noexcept;

/** Adds one direct carrier-resource edge unless the same edge was already retained. */
void graph_retain_edge(std::uint32_t parentEntityTag,
                       std::uint32_t childEntityTag,
                       std::uint32_t resourceTag,
                       const DeepTagMetadata& resourceMetadata,
                       std::size_t sourceOffset,
                       bool repeatedEntity) noexcept {
    for (const EntityGraphEdge& edge : g_entityGraphEdges) {
        if (edge.parentEntityTag == parentEntityTag && edge.childEntityTag == childEntityTag
            && edge.carrierResourceTag == resourceTag && edge.sourceOffset == sourceOffset) {
            return;
        }
    }
    if (g_entityGraphEdges.size() >= kEntityGraphEdgeLimit) {
        g_entityGraphSummary.truncated = true;
        return;
    }
    g_entityGraphEdges.push_back(EntityGraphEdge{parentEntityTag,
                                                childEntityTag,
                                                resourceTag,
                                                resourceMetadata.kindClass,
                                                resourceMetadata.payloadClass,
                                                sourceOffset,
                                                repeatedEntity});
}

/**
 * Searches one exact 0x808084D7 resource only for verified entity-definition tags. Unknown tags
 * are deliberately not recursively expanded here: the graph keeps heuristics solely as the bridge
 * from a known carrier resource to the next known entity definition.
 */
void graph_scan_carrier(EntityGraphRuntime& runtime,
                        std::uint32_t parentEntityTag,
                        std::uint32_t resourceTag,
                        const DeepTagMetadata& resourceMetadata,
                        std::uint16_t childDepth) noexcept {
    if (runtime.source == nullptr || childDepth > kEntityGraphMaximumDepth
        || !runtime.scannedCarrierResources.insert(resourceTag).second) {
        return;
    }
    if (g_entityGraphSummary.tagReadAttempts >= kEntityGraphReadAttemptLimit) {
        g_entityGraphSummary.truncated = true;
        return;
    }

    ++g_entityGraphSummary.carrierScans;
    const std::size_t readStart = g_entityGraphSummary.tagReadAttempts;
    ++g_entityGraphSummary.tagReadAttempts;
    std::vector<std::byte> blob;
    std::uint32_t classId = 0;
    if (!reader::read_tag(*runtime.source, g_work.scratch, resourceTag, blob, classId)) {
        ++g_entityGraphSummary.tagReadFailures;
        return;
    }
    if (classId != tables::kEntityResourceClass) {
        return;
    }

    std::unordered_set<std::uint32_t> localCandidates;
    std::unordered_set<std::uint32_t> localEntities;
    for (std::size_t offset = 0;
         offset <= blob.size() && blob.size() - offset >= sizeof(std::uint32_t);
         offset += sizeof(std::uint32_t)) {
        if (g_entityGraphSummary.tagReadAttempts >= kEntityGraphReadAttemptLimit
            || g_entityGraphSummary.tagReadAttempts - readStart
                   >= kEntityGraphPerCarrierReadAttemptLimit) {
            g_entityGraphSummary.truncated = true;
            break;
        }

        std::uint32_t candidate = 0;
        std::memcpy(&candidate, blob.data() + offset, sizeof candidate);
        if (candidate == resourceTag || tables::package_of(candidate) == tables::kAbsentPackageId
            || !localCandidates.insert(candidate).second) {
            continue;
        }
        ++g_entityGraphSummary.candidateWords;

        DeepTagMetadata metadata{};
        if (!graph_read_metadata(runtime, candidate, metadata)
            || metadata.classId != tables::kEntityDefinitionClass
            || !localEntities.insert(candidate).second) {
            continue;
        }

        ++g_entityGraphSummary.directEntityHits;
        const auto existing = runtime.entityIndices.find(candidate);
        const bool repeatedEntity = existing != runtime.entityIndices.end();
        graph_retain_edge(parentEntityTag,
                          candidate,
                          resourceTag,
                          resourceMetadata,
                          offset,
                          repeatedEntity);
        if (!repeatedEntity) {
            (void)graph_expand_entity(runtime, candidate, childDepth);
        }
    }
}

/** Reads one entity definition exactly, then follows only its exact carrier resources. */
[[nodiscard]] std::size_t graph_expand_entity(EntityGraphRuntime& runtime,
                                              std::uint32_t entityTag,
                                              std::uint16_t depth) noexcept {
    const auto existing = runtime.entityIndices.find(entityTag);
    if (existing != runtime.entityIndices.end()) {
        return existing->second;
    }
    if (depth > kEntityGraphMaximumDepth || g_entityGraphNodes.size() >= kEntityGraphEntityLimit
        || g_entityGraphSummary.tagReadAttempts >= kEntityGraphReadAttemptLimit) {
        g_entityGraphSummary.truncated = true;
        return (std::numeric_limits<std::size_t>::max)();
    }

    ++g_entityGraphSummary.tagReadAttempts;
    std::vector<std::byte> definition;
    std::uint32_t definitionClass = 0;
    if (runtime.source == nullptr
        || !reader::read_tag(*runtime.source,
                             g_work.scratch,
                             entityTag,
                             definition,
                             definitionClass)) {
        ++g_entityGraphSummary.tagReadFailures;
        if (depth == 0) {
            g_entityGraphSummary.result = EntityGraphResult::rootReadFailed;
        }
        return (std::numeric_limits<std::size_t>::max)();
    }
    if (definitionClass != tables::kEntityDefinitionClass) {
        ++g_entityGraphSummary.unexpectedDefinitionClasses;
        if (depth == 0) {
            g_entityGraphSummary.result = EntityGraphResult::unexpectedRootClass;
        }
        return (std::numeric_limits<std::size_t>::max)();
    }

    tables::Array resources{};
    if (!tables::entity_resources(definition, resources)) {
        ++g_entityGraphSummary.malformedDefinitions;
        if (depth == 0) {
            g_entityGraphSummary.result = EntityGraphResult::malformedRootResources;
        }
        return (std::numeric_limits<std::size_t>::max)();
    }

    const std::size_t nodeIndex = g_entityGraphNodes.size();
    runtime.entityIndices.emplace(entityTag, nodeIndex);
    runtime.expandedEntities.insert(entityTag);
    g_entityGraphNodes.push_back(EntityGraphNode{entityTag,
                                                definitionClass,
                                                definition.size(),
                                                static_cast<std::size_t>(resources.count),
                                                0,
                                                0,
                                                depth,
                                                false,
                                                false});
    g_entityGraphSummary.deepestEntityDepth =
        (std::max)(g_entityGraphSummary.deepestEntityDepth, depth);

    struct CarrierRow {
        std::uint32_t tag{};
        DeepTagMetadata metadata{};
    };
    std::vector<CarrierRow> carriers;
    carriers.reserve(static_cast<std::size_t>(resources.count));

    std::size_t readableResources = 0;
    std::size_t carrierResources = 0;
    bool hasModel = false;
    bool hasSkeleton = false;
    for (std::uint64_t index = 0; index < resources.count; ++index) {
        if (g_entityGraphSummary.tagReadAttempts >= kEntityGraphReadAttemptLimit) {
            g_entityGraphSummary.truncated = true;
            break;
        }
        std::uint32_t resourceTag = 0;
        if (!tables::entity_resource_at(definition, resources, index, resourceTag)) {
            continue;
        }
        ++g_entityGraphSummary.exactResourceLinks;

        DeepTagMetadata metadata{};
        if (!graph_read_metadata(runtime, resourceTag, metadata)) {
            continue;
        }
        ++readableResources;
        if (metadata.entityResource) {
            hasModel |= metadata.kindClass == kModelKindClass;
            hasSkeleton |= metadata.kindClass == kSkeletonKindClass;
            if (metadata.kindClass == kNestedEntityCarrierKindClass
                && metadata.payloadClass == kNestedEntityCarrierPayloadClass) {
                ++carrierResources;
                ++g_entityGraphSummary.carrierResources;
                carriers.push_back(CarrierRow{resourceTag, metadata});
            }
        }
    }

    g_entityGraphNodes[nodeIndex].readableResources = readableResources;
    g_entityGraphNodes[nodeIndex].carrierResources = carrierResources;
    g_entityGraphNodes[nodeIndex].hasModel = hasModel;
    g_entityGraphNodes[nodeIndex].hasSkeleton = hasSkeleton;

    if (depth < kEntityGraphMaximumDepth) {
        const std::uint16_t childDepth = static_cast<std::uint16_t>(depth + 1U);
        for (const CarrierRow& carrier : carriers) {
            if (g_entityGraphSummary.truncated
                && g_entityGraphSummary.tagReadAttempts >= kEntityGraphReadAttemptLimit) {
                break;
            }
            graph_scan_carrier(
                runtime, entityTag, carrier.tag, carrier.metadata, childDepth);
        }
    }
    return nodeIndex;
}

/** Logs the compact exact entity graph, not the discarded heuristic candidates. */
void report_entity_graph() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=activity_entity_graph result=%s root=0x%08X entities=%zu edges=%zu exact_resources=%zu "
        "carriers=%zu carrier_scans=%zu entity_hits=%zu malformed=%zu unexpected=%zu reads=%zu "
        "read_fail=%zu candidates=%zu depth=%u truncated=%u",
        entity_graph_result_label(g_entityGraphSummary.result),
        static_cast<unsigned>(g_entityGraphSummary.rootEntityTag),
        g_entityGraphSummary.entities,
        g_entityGraphSummary.edges,
        g_entityGraphSummary.exactResourceLinks,
        g_entityGraphSummary.carrierResources,
        g_entityGraphSummary.carrierScans,
        g_entityGraphSummary.directEntityHits,
        g_entityGraphSummary.malformedDefinitions,
        g_entityGraphSummary.unexpectedDefinitionClasses,
        g_entityGraphSummary.tagReadAttempts,
        g_entityGraphSummary.tagReadFailures,
        g_entityGraphSummary.candidateWords,
        static_cast<unsigned>(g_entityGraphSummary.deepestEntityDepth),
        g_entityGraphSummary.truncated ? 1U : 0U);
    if (written > 0) {
        core::log::write(
            core::log::Channel::state,
            g_entityGraphSummary.result == EntityGraphResult::ok ? core::log::Level::info
                                                                 : core::log::Level::warn,
            {line.data(), static_cast<std::size_t>(written)});
    }

    constexpr std::size_t kNodeLogLimit = 48;
    const std::size_t nodeCount = (std::min)(g_entityGraphNodes.size(), kNodeLogLimit);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        const EntityGraphNode& node = g_entityGraphNodes[index];
        line = {};
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity_entity_graph_node index=%zu depth=%u entity=0x%08X bytes=%zu resources=%zu "
            "readable=%zu carriers=%zu model=%u skeleton=%u",
            index,
            static_cast<unsigned>(node.depth),
            static_cast<unsigned>(node.entityTag),
            node.bytes,
            node.declaredResources,
            node.readableResources,
            node.carrierResources,
            node.hasModel ? 1U : 0U,
            node.hasSkeleton ? 1U : 0U);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }

    constexpr std::size_t kEdgeLogLimit = 64;
    const std::size_t edgeCount = (std::min)(g_entityGraphEdges.size(), kEdgeLogLimit);
    for (std::size_t index = 0; index < edgeCount; ++index) {
        const EntityGraphEdge& edge = g_entityGraphEdges[index];
        line = {};
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=activity_entity_graph_edge index=%zu parent=0x%08X child=0x%08X carrier=0x%08X "
            "kind=0x%08X payload=0x%08X offset=0x%zX repeated=%u",
            index,
            static_cast<unsigned>(edge.parentEntityTag),
            static_cast<unsigned>(edge.childEntityTag),
            static_cast<unsigned>(edge.carrierResourceTag),
            static_cast<unsigned>(edge.carrierKindClass),
            static_cast<unsigned>(edge.carrierPayloadClass),
            edge.sourceOffset,
            edge.repeatedEntity ? 1U : 0U);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

} // namespace

bool scan_destination(std::string_view destination) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    reset_work();
    g_placements.clear();
    g_summary = {};
    g_definitionSummary = {};
    g_definitionResources.clear();
    g_classificationSummary = {};
    g_definitionSignatures.clear();
    g_actorResourceFrequencies.clear();
    g_resourceInspectSummary = {};
    g_nestedTagReferences.clear();
    g_branchInspectSummary = {};
    g_branchInspectNodes.clear();
    g_nestedAnalysisSummary = {};
    g_actorNestedClassFrequencies.clear();
    g_actorNestedProfiles.clear();
    g_actorNestedClusters.clear();
    g_entityGraphSummary = {};
    g_entityGraphNodes.clear();
    g_entityGraphEdges.clear();

    layouts::Definition definition{};
    if (!state::build_data::find_scenario_layout(destination, definition)) {
        fail(ScanResult::destinationMissing, destination);
        report();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    const std::size_t nameLength = std::min(destination.size(), g_summary.destination.size());
    if (nameLength != 0) {
        std::memcpy(g_summary.destination.data(), destination.data(), nameLength);
    }
    g_summary.destinationLength = static_cast<std::uint8_t>(nameLength);
    g_summary.scenarioTag = definition.tag;

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_summary.result = ScanResult::keysUnavailable;
        report();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_summary.result = ScanResult::packageDirectoryMissing;
        report();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};

    if (!reader::read_tag(source, g_work.scratch, definition.tag, g_work.scenario)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_summary.result = ScanResult::scenarioReadFailed;
        report();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    tables::Array bubbles{};
    if (!tables::scenario_bubbles(g_work.scenario, bubbles)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_summary.result = ScanResult::scenarioMalformed;
        report();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_summary.bubbles = static_cast<std::size_t>(bubbles.count);

    bool complete = true;
    for (std::uint64_t bubbleIndex = 0; bubbleIndex < bubbles.count && complete; ++bubbleIndex) {
        tables::Bubble bubble{};
        if (!tables::bubble_at(g_work.scenario, bubbles, bubbleIndex, bubble)) {
            complete = false;
            break;
        }
        for (std::uint64_t stateIndex = 0; stateIndex < bubble.stateCount; ++stateIndex) {
            tables::SliceState state{};
            if (!tables::slice_state_at(g_work.scenario, bubble, stateIndex, state)) {
                complete = false;
                break;
            }
            tables::SliceEntry entry{};
            if (!reader::read_tag(source, g_work.scratch, state.entryTag, g_work.entry)
                || !tables::slice_entry(g_work.entry, entry)) {
                continue;
            }
            const std::uint32_t sliceSetIndex = entry.index * tables::kSliceSetIndexFactor;
            ++g_summary.sliceSets;
            if (!reader::read_tag(source, g_work.scratch, entry.registryTag, g_work.registry)) {
                continue;
            }
            if (!walk_registry(source,
                               definition.tag,
                               static_cast<std::uint16_t>(bubbleIndex),
                               static_cast<std::uint16_t>(stateIndex),
                               bubble.nameHash,
                               sliceSetIndex)) {
                complete = false;
                break;
            }
        }
    }

    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    g_summary.result = complete ? ScanResult::ok : ScanResult::scenarioMalformed;
    report();
    ReleaseSRWLockExclusive(&g_lock);
    return complete;
}

Summary summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const Summary value = g_summary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t placement_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_placements.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool placement_at(std::size_t index, Placement& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_placements.size();
    if (valid) {
        output = g_placements[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

bool inspect_definition(std::uint32_t entityTag) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    reset_definition();
    g_definitionSummary.entityTag = entityTag;

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_definitionSummary.result = InspectResult::keysUnavailable;
        report_definition();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_definitionSummary.result = InspectResult::packageDirectoryMissing;
        report_definition();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};

    std::uint32_t definitionClass = 0;
    if (!reader::read_tag(source,
                          g_work.scratch,
                          entityTag,
                          g_work.definition,
                          definitionClass)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_definitionSummary.result = InspectResult::definitionReadFailed;
        report_definition();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_definitionSummary.definitionClass = definitionClass;
    g_definitionSummary.bytes = g_work.definition.size();
    if (definitionClass != tables::kEntityDefinitionClass) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_definitionSummary.result = InspectResult::unexpectedDefinitionClass;
        report_definition();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    tables::Array resources{};
    if (!tables::entity_resources(g_work.definition, resources)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_definitionSummary.result = InspectResult::malformedResourceArray;
        report_definition();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_definitionSummary.declaredResources = static_cast<std::size_t>(resources.count);
    g_definitionResources.reserve(g_definitionSummary.declaredResources);
    for (std::uint64_t index = 0; index < resources.count; ++index) {
        DefinitionResource row{};
        if (!tables::entity_resource_at(g_work.definition, resources, index, row.tag)) {
            ++g_definitionSummary.resourceReadFailures;
            g_definitionResources.push_back(row);
            continue;
        }

        std::uint32_t tagClass = 0;
        if (!reader::read_tag(source,
                              g_work.scratch,
                              row.tag,
                              g_work.definitionResource,
                              tagClass)) {
            ++g_definitionSummary.resourceReadFailures;
            g_definitionResources.push_back(row);
            continue;
        }
        row.readable = true;
        row.tagClass = tagClass;
        row.bytes = g_work.definitionResource.size();
        row.kindReadable = tables::resource_pointer_class(g_work.definitionResource,
                                                          tables::kEntityResourceKindPointerOffset,
                                                          row.kindClass);
        row.payloadReadable =
            tables::resource_pointer_class(g_work.definitionResource,
                                           tables::kEntityResourcePayloadPointerOffset,
                                           row.payloadClass);
        if (!row.kindReadable || !row.payloadReadable) {
            ++g_definitionSummary.malformedPointers;
        }
        ++g_definitionSummary.readableResources;
        g_definitionResources.push_back(row);
    }

    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    g_definitionSummary.result = InspectResult::ok;
    report_definition();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

DefinitionSummary definition_summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const DefinitionSummary value = g_definitionSummary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t definition_resource_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_definitionResources.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool definition_resource_at(std::size_t index, DefinitionResource& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_definitionResources.size();
    if (valid) {
        output = g_definitionResources[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

bool classify_definitions() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_classificationSummary = {};
    g_definitionSignatures.clear();
    g_actorResourceFrequencies.clear();
    g_nestedAnalysisSummary = {};
    g_actorNestedClassFrequencies.clear();
    g_actorNestedProfiles.clear();
    g_actorNestedClusters.clear();
    g_entityGraphSummary = {};
    g_entityGraphNodes.clear();
    g_entityGraphEdges.clear();
    if (g_placements.empty()) {
        g_classificationSummary.result = ClassificationResult::noPlacements;
        report_classification();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    std::unordered_map<std::uint32_t, std::size_t> placementCounts;
    placementCounts.reserve(g_placements.size());
    for (const Placement& placement : g_placements) {
        ++placementCounts[placement.entityTag];
    }
    g_classificationSummary.definitions = placementCounts.size();
    g_definitionSignatures.reserve(placementCounts.size());

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_classificationSummary.result = ClassificationResult::keysUnavailable;
        report_classification();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_classificationSummary.result = ClassificationResult::packageDirectoryMissing;
        report_classification();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};
    std::unordered_map<std::uint32_t, ResourceKindProbe> resourceCache;
    std::unordered_map<std::uint64_t, ActorResourceFrequency> actorFrequencies;

    for (const auto& [entityTag, placements] : placementCounts) {
        DefinitionSignature signature{};
        signature.entityTag = entityTag;
        signature.placements = placements;

        std::uint32_t definitionClass = 0;
        if (!reader::read_tag(source,
                              g_work.scratch,
                              entityTag,
                              g_work.definition,
                              definitionClass)) {
            ++g_classificationSummary.definitionReadFailures;
            g_definitionSignatures.push_back(signature);
            continue;
        }
        tables::Array resources{};
        if (definitionClass != tables::kEntityDefinitionClass
            || !tables::entity_resources(g_work.definition, resources)) {
            ++g_classificationSummary.malformedDefinitions;
            g_definitionSignatures.push_back(signature);
            continue;
        }

        signature.definitionReadable = true;
        signature.declaredResources = static_cast<std::size_t>(resources.count);
        ++g_classificationSummary.readableDefinitions;
        std::unordered_map<std::uint64_t, std::size_t> localResourceSignatures;
        for (std::uint64_t index = 0; index < resources.count; ++index) {
            std::uint32_t resourceTag = 0;
            if (!tables::entity_resource_at(g_work.definition, resources, index, resourceTag)) {
                ++g_classificationSummary.resourceReadFailures;
                ++signature.unknownKinds;
                continue;
            }
            const ResourceKindProbe probe = probe_resource_kind(source, resourceTag, resourceCache);
            if (!probe.readable) {
                ++signature.unknownKinds;
                continue;
            }
            ++signature.readableResources;
            if (probe.tagClass != tables::kEntityResourceClass || !probe.kindReadable
                || probe.kindClass == 0) {
                ++signature.unknownKinds;
                continue;
            }
            const std::uint32_t payload = probe.payloadReadable ? probe.payloadClass : 0;
            const std::uint64_t resourceSignature =
                (static_cast<std::uint64_t>(probe.kindClass) << 32U) | payload;
            ++localResourceSignatures[resourceSignature];
            note_known_kind(signature, probe.kindClass);
        }

        if (signature.hasModel) {
            ++g_classificationSummary.modelDefinitions;
        }
        if (signature.hasPhysicsModel) {
            ++g_classificationSummary.physicsDefinitions;
        }
        if (signature.hasSkeleton) {
            ++g_classificationSummary.skeletonDefinitions;
        }
        if (signature.hasControlRig) {
            ++g_classificationSummary.controlRigDefinitions;
        }
        signature.actorScore = actor_score(signature);
        if (signature.hasSkeleton) {
            ++g_classificationSummary.actorCandidates;
            for (const auto& [key, references] : localResourceSignatures) {
                ActorResourceFrequency& frequency = actorFrequencies[key];
                frequency.kindClass = static_cast<std::uint32_t>(key >> 32U);
                frequency.payloadClass = static_cast<std::uint32_t>(key);
                ++frequency.definitions;
                frequency.references += references;
                if (frequency.exampleEntityTag == 0) {
                    frequency.exampleEntityTag = entityTag;
                }
            }
        }
        g_definitionSignatures.push_back(signature);
    }

    g_actorResourceFrequencies.reserve(actorFrequencies.size());
    for (const auto& [key, frequency] : actorFrequencies) {
        (void)key;
        g_actorResourceFrequencies.push_back(frequency);
    }
    std::sort(g_actorResourceFrequencies.begin(),
              g_actorResourceFrequencies.end(),
              [](const ActorResourceFrequency& left,
                 const ActorResourceFrequency& right) noexcept {
                  if (left.definitions != right.definitions) {
                      return left.definitions > right.definitions;
                  }
                  if (left.references != right.references) {
                      return left.references > right.references;
                  }
                  if (left.kindClass != right.kindClass) {
                      return left.kindClass < right.kindClass;
                  }
                  return left.payloadClass < right.payloadClass;
              });

    std::sort(g_definitionSignatures.begin(),
              g_definitionSignatures.end(),
              [](const DefinitionSignature& left, const DefinitionSignature& right) noexcept {
                  const unsigned leftScore = signature_score(left);
                  const unsigned rightScore = signature_score(right);
                  if (leftScore != rightScore) {
                      return leftScore > rightScore;
                  }
                  if (left.placements != right.placements) {
                      return left.placements > right.placements;
                  }
                  return left.entityTag < right.entityTag;
              });

    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    g_classificationSummary.result = ClassificationResult::ok;
    report_classification();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

ClassificationSummary classification_summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const ClassificationSummary value = g_classificationSummary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t definition_signature_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_definitionSignatures.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool definition_signature_at(std::size_t index, DefinitionSignature& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_definitionSignatures.size();
    if (valid) {
        output = g_definitionSignatures[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

std::size_t actor_resource_frequency_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_actorResourceFrequencies.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool actor_resource_frequency_at(std::size_t index, ActorResourceFrequency& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_actorResourceFrequencies.size();
    if (valid) {
        output = g_actorResourceFrequencies[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

bool inspect_resource(std::uint32_t resourceTag) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_resourceInspectSummary = {};
    g_nestedTagReferences.clear();
    g_resourceInspectSummary.resourceTag = resourceTag;

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_resourceInspectSummary.result = ResourceInspectResult::keysUnavailable;
        report_resource_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_resourceInspectSummary.result = ResourceInspectResult::packageDirectoryMissing;
        report_resource_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};

    if (!reader::read_tag(source,
                          g_work.scratch,
                          resourceTag,
                          g_work.definitionResource,
                          g_resourceInspectSummary.resourceClass)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_resourceInspectSummary.result = ResourceInspectResult::resourceReadFailed;
        report_resource_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_resourceInspectSummary.bytes = g_work.definitionResource.size();
    if (g_resourceInspectSummary.resourceClass != tables::kEntityResourceClass) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_resourceInspectSummary.result = ResourceInspectResult::unexpectedResourceClass;
        report_resource_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    (void)tables::resource_pointer_class(g_work.definitionResource,
                                         tables::kEntityResourceKindPointerOffset,
                                         g_resourceInspectSummary.kindClass);
    (void)tables::resource_pointer_class(g_work.definitionResource,
                                         tables::kEntityResourcePayloadPointerOffset,
                                         g_resourceInspectSummary.payloadClass);

    std::unordered_set<std::uint32_t> attempted;
    attempted.reserve(kNestedTagReadAttemptLimit);
    g_nestedTagReferences.reserve(kNestedTagRetainLimit);
    for (std::size_t offset = 0; offset <= g_work.definitionResource.size()
                                      && g_work.definitionResource.size() - offset
                                             >= sizeof(std::uint32_t);
         offset += sizeof(std::uint32_t)) {
        std::uint32_t candidate = 0;
        std::memcpy(&candidate, g_work.definitionResource.data() + offset, sizeof candidate);
        if (tables::package_of(candidate) == tables::kAbsentPackageId || candidate == resourceTag) {
            continue;
        }
        ++g_resourceInspectSummary.candidateWords;
        if (!attempted.insert(candidate).second) {
            continue;
        }
        if (g_resourceInspectSummary.readAttempts >= kNestedTagReadAttemptLimit) {
            g_resourceInspectSummary.truncated = true;
            break;
        }
        ++g_resourceInspectSummary.readAttempts;
        std::uint32_t classId = 0;
        if (!reader::read_tag(source, g_work.scratch, candidate, g_work.nestedTag, classId)) {
            ++g_resourceInspectSummary.readFailures;
            continue;
        }
        if (g_nestedTagReferences.size() >= kNestedTagRetainLimit) {
            g_resourceInspectSummary.truncated = true;
            break;
        }
        g_nestedTagReferences.push_back(
            NestedTagReference{offset, candidate, classId, g_work.nestedTag.size()});
    }

    g_resourceInspectSummary.nestedTags = g_nestedTagReferences.size();
    g_resourceInspectSummary.result = ResourceInspectResult::ok;
    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    report_resource_inspection();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

ResourceInspectSummary resource_inspect_summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const ResourceInspectSummary value = g_resourceInspectSummary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t nested_tag_reference_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_nestedTagReferences.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool nested_tag_reference_at(std::size_t index, NestedTagReference& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_nestedTagReferences.size();
    if (valid) {
        output = g_nestedTagReferences[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}



bool inspect_actor_branch(std::uint32_t entityTag, std::uint32_t resourceTag) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_branchInspectSummary = {};
    g_branchInspectNodes.clear();
    g_branchInspectSummary.entityTag = entityTag;
    g_branchInspectSummary.resourceTag = resourceTag;

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_branchInspectSummary.result = BranchInspectResult::keysUnavailable;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_branchInspectSummary.result = BranchInspectResult::packageDirectoryMissing;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};

    ++g_branchInspectSummary.tagReadAttempts;
    std::uint32_t definitionClass = 0;
    if (!reader::read_tag(source,
                          g_work.scratch,
                          entityTag,
                          g_work.definition,
                          definitionClass)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_branchInspectSummary.result = BranchInspectResult::definitionReadFailed;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    if (definitionClass != tables::kEntityDefinitionClass) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_branchInspectSummary.result = BranchInspectResult::unexpectedDefinitionClass;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    tables::Array resources{};
    if (!tables::entity_resources(g_work.definition, resources)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_branchInspectSummary.result = BranchInspectResult::malformedResourceArray;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    bool owned = false;
    for (std::uint64_t index = 0; index < resources.count; ++index) {
        std::uint32_t candidate = 0;
        if (tables::entity_resource_at(g_work.definition, resources, index, candidate)
            && candidate == resourceTag) {
            owned = true;
            break;
        }
    }
    if (!owned) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_branchInspectSummary.result = BranchInspectResult::resourceNotOwned;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    BranchWalkRuntime walk{};
    walk.source = &source;
    walk.metadata.reserve(kBranchInspectNodeRetainLimit);
    walk.attempted.reserve(kBranchInspectReadAttemptLimit);
    walk.expanded.reserve(kBranchInspectNodeRetainLimit);
    walk.queued.reserve(kBranchInspectNodeRetainLimit);
    walk.queue.reserve(kBranchInspectNodeRetainLimit);

    DeepTagMetadata rootMetadata{};
    if (!branch_read_metadata(walk, resourceTag, rootMetadata)) {
        SecureZeroMemory(&keys, sizeof keys);
        reader::close_files(g_work.scratch);
        g_branchInspectSummary.result = BranchInspectResult::resourceReadFailed;
        report_branch_inspection();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_branchInspectSummary.resourceClass = rootMetadata.classId;
    g_branchInspectSummary.kindClass = rootMetadata.kindClass;
    g_branchInspectSummary.payloadClass = rootMetadata.payloadClass;
    g_branchInspectSummary.resourceBytes = rootMetadata.bytes;

    const std::size_t rootIndex = branch_retain_node(
        walk,
        resourceTag,
        rootMetadata,
        (std::numeric_limits<std::size_t>::max)(),
        0,
        0,
        false,
        false);
    if (rootIndex != (std::numeric_limits<std::size_t>::max)()) {
        for (std::size_t queueIndex = 0; queueIndex < walk.queue.size(); ++queueIndex) {
            if (g_branchInspectSummary.truncated) {
                break;
            }
            branch_expand_node(walk, walk.queue[queueIndex]);
        }
    }

    g_branchInspectSummary.retainedNodes = g_branchInspectNodes.size();
    g_branchInspectSummary.result = BranchInspectResult::ok;
    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    report_branch_inspection();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

BranchInspectSummary branch_inspect_summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const BranchInspectSummary value = g_branchInspectSummary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t branch_inspect_node_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_branchInspectNodes.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool branch_inspect_node_at(std::size_t index, BranchInspectNode& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_branchInspectNodes.size();
    if (valid) {
        output = g_branchInspectNodes[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

bool analyze_actor_nested_classes() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_nestedAnalysisSummary = {};
    g_actorNestedClassFrequencies.clear();
    g_actorNestedProfiles.clear();
    g_actorNestedClusters.clear();

    if (g_classificationSummary.result != ClassificationResult::ok) {
        g_nestedAnalysisSummary.result = NestedAnalysisResult::noClassification;
        report_nested_analysis();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    std::vector<std::uint32_t> actorTags;
    for (const DefinitionSignature& signature : g_definitionSignatures) {
        if (signature.definitionReadable && signature.hasSkeleton) {
            actorTags.push_back(signature.entityTag);
        }
    }
    g_nestedAnalysisSummary.actorCandidates = actorTags.size();
    if (actorTags.empty()) {
        g_nestedAnalysisSummary.result = NestedAnalysisResult::noActors;
        report_nested_analysis();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_nestedAnalysisSummary.result = NestedAnalysisResult::keysUnavailable;
        report_nested_analysis();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_nestedAnalysisSummary.result = NestedAnalysisResult::packageDirectoryMissing;
        report_nested_analysis();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};

    NestedTagClassCache classCache{};
    classCache.readable.reserve(2048);
    classCache.unreadable.reserve(2048);
    std::unordered_map<std::uint32_t, NestedClassWorkFrequency> frequencies;
    std::vector<NestedActorWorkProfile> workProfiles;
    workProfiles.reserve(actorTags.size());

    struct ActorClassLocal {
        std::size_t references{};
        std::uint32_t exampleTag{};
    };

    for (const std::uint32_t entityTag : actorTags) {
        const std::size_t actorReadStart = g_nestedAnalysisSummary.tagReadAttempts;
        NestedActorWorkProfile profile{};
        profile.entityTag = entityTag;
        std::unordered_map<std::uint32_t, ActorClassLocal> actorClasses;

        std::uint32_t definitionClass = 0;
        if (!nested_read_blob(source,
                              actorReadStart,
                              entityTag,
                              g_work.definition,
                              definitionClass)) {
            profile.complete = false;
            workProfiles.push_back(std::move(profile));
            if (g_nestedAnalysisSummary.tagReadAttempts >= kNestedAnalysisReadAttemptLimit) {
                break;
            }
            continue;
        }
        tables::Array resources{};
        if (definitionClass != tables::kEntityDefinitionClass
            || !tables::entity_resources(g_work.definition, resources)) {
            profile.complete = false;
            workProfiles.push_back(std::move(profile));
            continue;
        }

        for (std::uint64_t resourceIndex = 0; resourceIndex < resources.count; ++resourceIndex) {
            if (!nested_read_budget_available(actorReadStart)) {
                profile.complete = false;
                g_nestedAnalysisSummary.truncated = true;
                break;
            }
            std::uint32_t resourceTag = 0;
            if (!tables::entity_resource_at(
                    g_work.definition, resources, resourceIndex, resourceTag)) {
                profile.complete = false;
                continue;
            }
            std::uint32_t resourceClass = 0;
            if (!nested_read_blob(source,
                                  actorReadStart,
                                  resourceTag,
                                  g_work.definitionResource,
                                  resourceClass)) {
                if (!nested_read_budget_available(actorReadStart)) {
                    profile.complete = false;
                    break;
                }
                profile.complete = false;
                continue;
            }

            std::unordered_set<std::uint32_t> localCandidates;
            const std::vector<std::byte>& blob = g_work.definitionResource;
            for (std::size_t offset = 0;
                 offset <= blob.size() && blob.size() - offset >= sizeof(std::uint32_t);
                 offset += sizeof(std::uint32_t)) {
                if (!nested_read_budget_available(actorReadStart)) {
                    profile.complete = false;
                    g_nestedAnalysisSummary.truncated = true;
                    break;
                }
                std::uint32_t candidate = 0;
                std::memcpy(&candidate, blob.data() + offset, sizeof candidate);
                if (candidate == resourceTag
                    || tables::package_of(candidate) == tables::kAbsentPackageId
                    || !localCandidates.insert(candidate).second) {
                    continue;
                }
                ++g_nestedAnalysisSummary.candidateWords;

                std::uint32_t candidateClass = 0;
                if (!nested_read_candidate_class(
                        source, actorReadStart, classCache, candidate, candidateClass)) {
                    if (!nested_read_budget_available(actorReadStart)) {
                        profile.complete = false;
                        break;
                    }
                    continue;
                }
                ActorClassLocal& local = actorClasses[candidateClass];
                ++local.references;
                if (local.exampleTag == 0) {
                    local.exampleTag = candidate;
                }
                ++profile.references;
                if (candidateClass == tables::kEntityDefinitionClass) {
                    ++profile.nestedEntityDefinitions;
                }
            }
            if (!profile.complete && !nested_read_budget_available(actorReadStart)) {
                break;
            }
        }

        profile.classes.reserve(actorClasses.size());
        for (const auto& [classId, local] : actorClasses) {
            (void)local;
            profile.classes.push_back(classId);
        }
        std::sort(profile.classes.begin(), profile.classes.end());

        if (profile.complete) {
            ++g_nestedAnalysisSummary.completeActors;
            for (const auto& [classId, local] : actorClasses) {
                NestedClassWorkFrequency& frequency = frequencies[classId];
                ++frequency.definitions;
                frequency.references += local.references;
                if (frequency.exampleEntityTag == 0) {
                    frequency.exampleEntityTag = entityTag;
                    frequency.exampleTag = local.exampleTag;
                }
            }
        }
        workProfiles.push_back(std::move(profile));
        if (g_nestedAnalysisSummary.tagReadAttempts >= kNestedAnalysisReadAttemptLimit) {
            g_nestedAnalysisSummary.truncated = true;
            break;
        }
    }

    g_actorNestedClassFrequencies.reserve(frequencies.size());
    for (const auto& [classId, frequency] : frequencies) {
        g_actorNestedClassFrequencies.push_back(ActorNestedClassFrequency{classId,
                                                                         frequency.definitions,
                                                                         frequency.references,
                                                                         frequency.exampleEntityTag,
                                                                         frequency.exampleTag});
    }
    std::sort(g_actorNestedClassFrequencies.begin(),
              g_actorNestedClassFrequencies.end(),
              [](const ActorNestedClassFrequency& left,
                 const ActorNestedClassFrequency& right) noexcept {
                  if (left.definitions != right.definitions) {
                      return left.definitions > right.definitions;
                  }
                  if (left.references != right.references) {
                      return left.references > right.references;
                  }
                  return left.classId < right.classId;
              });
    g_nestedAnalysisSummary.uniqueClasses = g_actorNestedClassFrequencies.size();

    std::vector<NestedClusterWork> clusters;
    for (std::size_t profileIndex = 0; profileIndex < workProfiles.size(); ++profileIndex) {
        const NestedActorWorkProfile& profile = workProfiles[profileIndex];
        if (!profile.complete) {
            continue;
        }
        auto found = std::find_if(clusters.begin(),
                                  clusters.end(),
                                  [&profile](const NestedClusterWork& cluster) noexcept {
                                      return cluster.classes == profile.classes;
                                  });
        if (found == clusters.end()) {
            NestedClusterWork cluster{};
            cluster.classes = profile.classes;
            cluster.profileIndices.push_back(profileIndex);
            clusters.push_back(std::move(cluster));
        } else {
            found->profileIndices.push_back(profileIndex);
        }
    }
    std::sort(clusters.begin(),
              clusters.end(),
              [&workProfiles](const NestedClusterWork& left,
                              const NestedClusterWork& right) noexcept {
                  if (left.profileIndices.size() != right.profileIndices.size()) {
                      return left.profileIndices.size() > right.profileIndices.size();
                  }
                  if (left.classes.size() != right.classes.size()) {
                      return left.classes.size() > right.classes.size();
                  }
                  const std::uint32_t leftTag =
                      workProfiles[left.profileIndices.front()].entityTag;
                  const std::uint32_t rightTag =
                      workProfiles[right.profileIndices.front()].entityTag;
                  return leftTag < rightTag;
              });

    std::vector<std::uint16_t> clusterIds(workProfiles.size(), 0);
    for (std::size_t clusterIndex = 0; clusterIndex < clusters.size(); ++clusterIndex) {
        const std::uint16_t clusterId = static_cast<std::uint16_t>(clusterIndex + 1U);
        const NestedClusterWork& cluster = clusters[clusterIndex];
        std::uint32_t representative = (std::numeric_limits<std::uint32_t>::max)();
        for (const std::size_t profileIndex : cluster.profileIndices) {
            clusterIds[profileIndex] = clusterId;
            representative =
                (std::min)(representative, workProfiles[profileIndex].entityTag);
        }
        g_actorNestedClusters.push_back(ActorNestedCluster{clusterId,
                                                          cluster.profileIndices.size(),
                                                          cluster.classes.size(),
                                                          representative});
    }

    g_actorNestedProfiles.reserve(workProfiles.size());
    for (std::size_t index = 0; index < workProfiles.size(); ++index) {
        const NestedActorWorkProfile& profile = workProfiles[index];
        g_actorNestedProfiles.push_back(ActorNestedProfile{profile.entityTag,
                                                           clusterIds[index],
                                                           profile.classes.size(),
                                                           profile.references,
                                                           profile.nestedEntityDefinitions,
                                                           profile.complete});
    }
    std::sort(g_actorNestedProfiles.begin(),
              g_actorNestedProfiles.end(),
              [](const ActorNestedProfile& left, const ActorNestedProfile& right) noexcept {
                  if (left.complete != right.complete) {
                      return left.complete > right.complete;
                  }
                  if (left.clusterId != right.clusterId) {
                      if (left.clusterId == 0) {
                          return false;
                      }
                      if (right.clusterId == 0) {
                          return true;
                      }
                      return left.clusterId < right.clusterId;
                  }
                  return left.entityTag < right.entityTag;
              });

    g_nestedAnalysisSummary.clusters = g_actorNestedClusters.size();
    g_nestedAnalysisSummary.result = NestedAnalysisResult::ok;
    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    report_nested_analysis();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

NestedAnalysisSummary nested_analysis_summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const NestedAnalysisSummary value = g_nestedAnalysisSummary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t actor_nested_class_frequency_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_actorNestedClassFrequencies.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool actor_nested_class_frequency_at(std::size_t index,
                                     ActorNestedClassFrequency& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_actorNestedClassFrequencies.size();
    if (valid) {
        output = g_actorNestedClassFrequencies[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

std::size_t actor_nested_profile_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_actorNestedProfiles.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool actor_nested_profile_at(std::size_t index, ActorNestedProfile& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_actorNestedProfiles.size();
    if (valid) {
        output = g_actorNestedProfiles[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

std::size_t actor_nested_cluster_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_actorNestedClusters.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool actor_nested_cluster_at(std::size_t index, ActorNestedCluster& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_actorNestedClusters.size();
    if (valid) {
        output = g_actorNestedClusters[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}


bool build_entity_graph(std::uint32_t entityTag) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_entityGraphSummary = {};
    g_entityGraphNodes.clear();
    g_entityGraphEdges.clear();
    g_entityGraphSummary.rootEntityTag = entityTag;

    reader::BlockKeys keys{};
    if (!collect_keys(keys)) {
        g_entityGraphSummary.result = EntityGraphResult::keysUnavailable;
        report_entity_graph();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    core::path::Buffer directory{};
    if (!package_directory(directory)) {
        SecureZeroMemory(&keys, sizeof keys);
        g_entityGraphSummary.result = EntityGraphResult::packageDirectoryMissing;
        report_entity_graph();
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    const reader::Source source{std::wstring_view(directory.chars.data(), directory.length), &keys};

    EntityGraphRuntime runtime{};
    runtime.source = &source;
    runtime.metadata.reserve(kEntityGraphReadAttemptLimit);
    runtime.entityIndices.reserve(kEntityGraphEntityLimit);
    runtime.expandedEntities.reserve(kEntityGraphEntityLimit);
    runtime.scannedCarrierResources.reserve(kEntityGraphEdgeLimit);
    g_entityGraphNodes.reserve(kEntityGraphEntityLimit);
    g_entityGraphEdges.reserve(kEntityGraphEdgeLimit);

    const std::size_t rootIndex = graph_expand_entity(runtime, entityTag, 0);
    const bool success = rootIndex != (std::numeric_limits<std::size_t>::max)();
    if (success) {
        g_entityGraphSummary.result = EntityGraphResult::ok;
    }
    g_entityGraphSummary.entities = g_entityGraphNodes.size();
    g_entityGraphSummary.edges = g_entityGraphEdges.size();

    SecureZeroMemory(&keys, sizeof keys);
    reader::close_files(g_work.scratch);
    report_entity_graph();
    ReleaseSRWLockExclusive(&g_lock);
    return success;
}

EntityGraphSummary entity_graph_summary() noexcept {
    AcquireSRWLockShared(&g_lock);
    const EntityGraphSummary value = g_entityGraphSummary;
    ReleaseSRWLockShared(&g_lock);
    return value;
}

std::size_t entity_graph_node_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_entityGraphNodes.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool entity_graph_node_at(std::size_t index, EntityGraphNode& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_entityGraphNodes.size();
    if (valid) {
        output = g_entityGraphNodes[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

std::size_t entity_graph_edge_count() noexcept {
    AcquireSRWLockShared(&g_lock);
    const std::size_t count = g_entityGraphEdges.size();
    ReleaseSRWLockShared(&g_lock);
    return count;
}

bool entity_graph_edge_at(std::size_t index, EntityGraphEdge& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_lock);
    const bool valid = index < g_entityGraphEdges.size();
    if (valid) {
        output = g_entityGraphEdges[index];
    }
    ReleaseSRWLockShared(&g_lock);
    return valid;
}

bool first_placement_index(std::uint32_t entityTag, std::size_t& index) noexcept {
    index = 0;
    AcquireSRWLockShared(&g_lock);
    bool found = false;
    for (std::size_t candidate = 0; candidate < g_placements.size(); ++candidate) {
        if (g_placements[candidate].entityTag == entityTag) {
            index = candidate;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

const char* result_label(ScanResult result) noexcept {
    switch (result) {
    case ScanResult::idle:
        return "idle";
    case ScanResult::ok:
        return "ok";
    case ScanResult::destinationMissing:
        return "destination_missing";
    case ScanResult::keysUnavailable:
        return "keys_unavailable";
    case ScanResult::packageDirectoryMissing:
        return "package_directory";
    case ScanResult::scenarioReadFailed:
        return "scenario_read";
    case ScanResult::scenarioMalformed:
        return "scenario_malformed";
    }
    return "unknown";
}

const char* inspect_result_label(InspectResult result) noexcept {
    switch (result) {
    case InspectResult::idle:
        return "idle";
    case InspectResult::ok:
        return "ok";
    case InspectResult::keysUnavailable:
        return "keys_unavailable";
    case InspectResult::packageDirectoryMissing:
        return "package_directory";
    case InspectResult::definitionReadFailed:
        return "definition_read";
    case InspectResult::unexpectedDefinitionClass:
        return "unexpected_definition_class";
    case InspectResult::malformedResourceArray:
        return "malformed_resource_array";
    }
    return "unknown";
}

const char* classification_result_label(ClassificationResult result) noexcept {
    switch (result) {
    case ClassificationResult::idle:
        return "idle";
    case ClassificationResult::ok:
        return "ok";
    case ClassificationResult::noPlacements:
        return "no_placements";
    case ClassificationResult::keysUnavailable:
        return "keys_unavailable";
    case ClassificationResult::packageDirectoryMissing:
        return "package_directory";
    }
    return "unknown";
}

const char* resource_inspect_result_label(ResourceInspectResult result) noexcept {
    switch (result) {
    case ResourceInspectResult::idle:
        return "idle";
    case ResourceInspectResult::ok:
        return "ok";
    case ResourceInspectResult::keysUnavailable:
        return "keys_unavailable";
    case ResourceInspectResult::packageDirectoryMissing:
        return "package_directory";
    case ResourceInspectResult::resourceReadFailed:
        return "resource_read";
    case ResourceInspectResult::unexpectedResourceClass:
        return "unexpected_resource_class";
    }
    return "unknown";
}


const char* branch_inspect_result_label(BranchInspectResult result) noexcept {
    switch (result) {
    case BranchInspectResult::idle:
        return "idle";
    case BranchInspectResult::ok:
        return "ok";
    case BranchInspectResult::keysUnavailable:
        return "keys_unavailable";
    case BranchInspectResult::packageDirectoryMissing:
        return "package_directory";
    case BranchInspectResult::definitionReadFailed:
        return "definition_read";
    case BranchInspectResult::unexpectedDefinitionClass:
        return "unexpected_definition_class";
    case BranchInspectResult::malformedResourceArray:
        return "malformed_resource_array";
    case BranchInspectResult::resourceNotOwned:
        return "resource_not_owned";
    case BranchInspectResult::resourceReadFailed:
        return "resource_read";
    }
    return "unknown";
}

const char* nested_analysis_result_label(NestedAnalysisResult result) noexcept {
    switch (result) {
    case NestedAnalysisResult::idle:
        return "idle";
    case NestedAnalysisResult::ok:
        return "ok";
    case NestedAnalysisResult::noClassification:
        return "no_classification";
    case NestedAnalysisResult::noActors:
        return "no_actors";
    case NestedAnalysisResult::keysUnavailable:
        return "keys_unavailable";
    case NestedAnalysisResult::packageDirectoryMissing:
        return "package_directory";
    }
    return "unknown";
}


const char* entity_graph_result_label(EntityGraphResult result) noexcept {
    switch (result) {
    case EntityGraphResult::idle:
        return "idle";
    case EntityGraphResult::ok:
        return "ok";
    case EntityGraphResult::keysUnavailable:
        return "keys_unavailable";
    case EntityGraphResult::packageDirectoryMissing:
        return "package_directory";
    case EntityGraphResult::rootReadFailed:
        return "root_read";
    case EntityGraphResult::unexpectedRootClass:
        return "unexpected_root_class";
    case EntityGraphResult::malformedRootResources:
        return "malformed_root_resources";
    }
    return "unknown";
}

const char* actor_score_label(std::uint8_t score) noexcept {
    switch (score) {
    case 4:
        return "full actor";
    case 3:
        return "model+skeleton";
    case 2:
        return "skeleton";
    case 1:
        return "model+physics";
    default:
        return "low";
    }
}

} // namespace sunrise::client::content::entities
