#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::client::content::entities {

/** Result of the most recent explicit destination scan. */
enum class ScanResult : std::uint8_t {
    idle,
    ok,
    destinationMissing,
    keysUnavailable,
    packageDirectoryMissing,
    scenarioReadFailed,
    scenarioMalformed,
};

/** Result of the most recent explicit entity-definition inspection. */
enum class InspectResult : std::uint8_t {
    idle,
    ok,
    keysUnavailable,
    packageDirectoryMissing,
    definitionReadFailed,
    unexpectedDefinitionClass,
    malformedResourceArray,
};

/** Result of the most recent explicit bulk definition classification. */
enum class ClassificationResult : std::uint8_t {
    idle,
    ok,
    noPlacements,
    keysUnavailable,
    packageDirectoryMissing,
};

/** Result of the most recent explicit entity-resource heuristic inspection. */
enum class ResourceInspectResult : std::uint8_t {
    idle,
    ok,
    keysUnavailable,
    packageDirectoryMissing,
    resourceReadFailed,
    unexpectedResourceClass,
};

/** Result of one bounded selected actor-resource branch inspection. */
enum class BranchInspectResult : std::uint8_t {
    idle,
    ok,
    keysUnavailable,
    packageDirectoryMissing,
    definitionReadFailed,
    unexpectedDefinitionClass,
    malformedResourceArray,
    resourceNotOwned,
    resourceReadFailed,
};

/** Result of one explicit cross-actor nested-class analysis. */
enum class NestedAnalysisResult : std::uint8_t {
    idle,
    ok,
    noClassification,
    noActors,
    keysUnavailable,
    packageDirectoryMissing,
};

/** Result of one exact nested-entity graph extraction rooted at an actor definition. */
enum class EntityGraphResult : std::uint8_t {
    idle,
    ok,
    keysUnavailable,
    packageDirectoryMissing,
    rootReadFailed,
    unexpectedRootClass,
    malformedRootResources,
};

/** Authored entity placement plus the scenario path that reached it. */
struct Placement {
    std::uint32_t scenarioTag{};
    std::uint32_t bubbleNameHash{};
    std::uint32_t sliceSetIndex{};
    std::uint32_t objectTag{};
    std::uint32_t resourceTag{};
    std::uint32_t tableTag{};
    std::uint32_t entityTag{};
    std::uint64_t worldId{};
    std::int32_t objectBubbleIndex{};
    std::uint16_t bubbleOrdinal{};
    std::uint16_t stateOrdinal{};
    std::array<float, 4> rotation{};
    std::array<float, 4> translation{};
    /** Type-specific resource class attached to this map-data entry, when readable. */
    std::uint32_t dataResourceClass{};
    bool dataResourceClassReadable{};
};

/** Counters from the most recent scan. */
struct Summary {
    ScanResult result{ScanResult::idle};
    std::array<char, 40> destination{};
    std::uint8_t destinationLength{};
    std::uint32_t scenarioTag{};
    std::size_t bubbles{};
    std::size_t sliceSets{};
    std::size_t objects{};
    std::size_t resources{};
    std::size_t tables{};
    std::size_t entries{};
    /** Entity rows encountered before exact-placement deduplication. */
    std::size_t rawEntities{};
    /** Unique retained placements after exact-placement deduplication. */
    std::size_t entities{};
    std::size_t duplicateEntities{};
    /** Unique rows whose world id is the authored all-ones sentinel. */
    std::size_t sentinelEntities{};
    std::size_t objectReadFailures{};
    std::size_t resourceReadFailures{};
    std::size_t unexpectedResourceClasses{};
    std::size_t tableReadFailures{};
    std::size_t unexpectedTableClasses{};
    std::size_t malformedTables{};
    std::size_t malformedEntries{};
};

/** One entity-resource row reached from a selected entity definition. */
struct DefinitionResource {
    std::uint32_t tag{};
    std::uint32_t tagClass{};
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    std::size_t bytes{};
    bool readable{};
    bool kindReadable{};
    bool payloadReadable{};
};

/** Counters and identity for the most recent entity-definition inspection. */
struct DefinitionSummary {
    InspectResult result{InspectResult::idle};
    std::uint32_t entityTag{};
    std::uint32_t definitionClass{};
    std::size_t bytes{};
    std::size_t declaredResources{};
    std::size_t readableResources{};
    std::size_t resourceReadFailures{};
    std::size_t malformedPointers{};
};

/** Resource signature for one unique entity definition in the scanned destination. */
struct DefinitionSignature {
    std::uint32_t entityTag{};
    std::size_t placements{};
    std::size_t declaredResources{};
    std::size_t readableResources{};
    std::size_t unknownKinds{};
    bool definitionReadable{};
    bool hasModel{};
    bool hasPhysicsModel{};
    bool hasSkeleton{};
    bool hasControlRig{};
    /** Diagnostic ranking: 4 full actor, 3 model+skeleton, 2 skeleton, 1 model+physics, 0 other. */
    std::uint8_t actorScore{};
};

/** Counters from one explicit bulk definition classification. */
struct ClassificationSummary {
    ClassificationResult result{ClassificationResult::idle};
    std::size_t definitions{};
    std::size_t readableDefinitions{};
    std::size_t modelDefinitions{};
    std::size_t physicsDefinitions{};
    std::size_t skeletonDefinitions{};
    std::size_t controlRigDefinitions{};
    /** Definitions carrying the known Shadowkeep skeleton resource kind. */
    std::size_t actorCandidates{};
    std::size_t definitionReadFailures{};
    std::size_t malformedDefinitions{};
    std::size_t resourceReadFailures{};
};

/** Frequency of one resource-kind/payload signature among skeleton-bearing definitions. */
struct ActorResourceFrequency {
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    /** Skeleton-bearing definitions that carry this exact signature at least once. */
    std::size_t definitions{};
    /** Total references to the signature across those definitions. */
    std::size_t references{};
    /** One entity definition that carries the signature, for quick drill-down. */
    std::uint32_t exampleEntityTag{};
};

/** One definition tag heuristically found in an inspected entity-resource blob. */
struct NestedTagReference {
    std::size_t offset{};
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::size_t bytes{};
};

/** Counters and identity for the most recent heuristic entity-resource inspection. */
struct ResourceInspectSummary {
    ResourceInspectResult result{ResourceInspectResult::idle};
    std::uint32_t resourceTag{};
    std::uint32_t resourceClass{};
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    std::size_t bytes{};
    /** 4-byte-aligned words in tag range examined before the bounded read-attempt cap. */
    std::size_t candidateWords{};
    std::size_t readAttempts{};
    std::size_t nestedTags{};
    std::size_t readFailures{};
    bool truncated{};
};

/** One retained node in a bounded selected-resource branch tree. */
struct BranchInspectNode {
    std::uint32_t tag{};
    std::uint32_t classId{};
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    std::size_t parentIndex{static_cast<std::size_t>(-1)};
    /** Byte offset in the parent blob that exposed this tag; zero for the selected branch root. */
    std::size_t sourceOffset{};
    std::size_t bytes{};
    std::uint16_t depth{};
    bool readable{};
    bool entityResource{};
    /** True when the link came from a parsed entity-definition resource array, not a word scan. */
    bool exactRelation{};
    /** True when this tag was already expanded elsewhere and is retained only as another reference. */
    bool repeated{};
};

/** Counters and identity for the most recent selected actor-resource branch inspection. */
struct BranchInspectSummary {
    BranchInspectResult result{BranchInspectResult::idle};
    std::uint32_t entityTag{};
    std::uint32_t resourceTag{};
    std::uint32_t resourceClass{};
    std::uint32_t kindClass{};
    std::uint32_t payloadClass{};
    std::size_t resourceBytes{};
    std::size_t retainedNodes{};
    std::size_t tagReadAttempts{};
    std::size_t tagReadFailures{};
    std::size_t candidateWords{};
    std::size_t exactLinks{};
    std::size_t repeatedReferences{};
    std::uint16_t deepestLevel{};
    bool truncated{};
};

/** Frequency of one verified nested package-tag class across skeleton-bearing actor definitions. */
struct ActorNestedClassFrequency {
    std::uint32_t classId{};
    /** Complete actor profiles that contained at least one verified tag of this class. */
    std::size_t definitions{};
    /** Total verified references to the class across complete actor profiles. */
    std::size_t references{};
    std::uint32_t exampleEntityTag{};
    std::uint32_t exampleTag{};
};

/** One skeleton-bearing actor's immediate-resource nested-class profile. */
struct ActorNestedProfile {
    std::uint32_t entityTag{};
    /** Exact-class-set cluster identifier; zero when this actor's bounded scan was incomplete. */
    std::uint16_t clusterId{};
    std::size_t uniqueClasses{};
    std::size_t references{};
    std::size_t nestedEntityDefinitions{};
    bool complete{};
};

/** One exact nested-class-set cluster among complete skeleton-bearing actor profiles. */
struct ActorNestedCluster {
    std::uint16_t clusterId{};
    std::size_t definitions{};
    std::size_t uniqueClasses{};
    std::uint32_t representativeEntityTag{};
};

/** Counters from the most recent explicit nested-class analysis. */
struct NestedAnalysisSummary {
    NestedAnalysisResult result{NestedAnalysisResult::idle};
    std::size_t actorCandidates{};
    std::size_t completeActors{};
    std::size_t uniqueClasses{};
    std::size_t clusters{};
    std::size_t tagReadAttempts{};
    std::size_t tagReadFailures{};
    std::size_t candidateWords{};
    bool truncated{};
};

/** One unique entity definition retained in the 0x808084D7 nested-entity graph. */
struct EntityGraphNode {
    std::uint32_t entityTag{};
    std::uint32_t definitionClass{};
    std::size_t bytes{};
    std::size_t declaredResources{};
    std::size_t readableResources{};
    /** Exact immediate resources whose kind is the graph-carrier signature 0x808084D7. */
    std::size_t carrierResources{};
    std::uint16_t depth{};
    bool hasModel{};
    bool hasSkeleton{};
};

/** One direct nested-entity edge discovered inside an exact 0x808084D7 entity resource. */
struct EntityGraphEdge {
    std::uint32_t parentEntityTag{};
    std::uint32_t childEntityTag{};
    std::uint32_t carrierResourceTag{};
    std::uint32_t carrierKindClass{};
    std::uint32_t carrierPayloadClass{};
    /** Byte offset in the carrier resource whose verified tag was the child entity definition. */
    std::size_t sourceOffset{};
    bool repeatedEntity{};
};

/** Counters from one bounded exact nested-entity graph extraction. */
struct EntityGraphSummary {
    EntityGraphResult result{EntityGraphResult::idle};
    std::uint32_t rootEntityTag{};
    std::size_t entities{};
    std::size_t edges{};
    std::size_t exactResourceLinks{};
    std::size_t carrierResources{};
    std::size_t carrierScans{};
    std::size_t directEntityHits{};
    std::size_t malformedDefinitions{};
    std::size_t unexpectedDefinitionClasses{};
    std::size_t tagReadAttempts{};
    std::size_t tagReadFailures{};
    std::size_t candidateWords{};
    std::uint16_t deepestEntityDepth{};
    bool truncated{};
};

/** Reads one published destination directly from the installed packages. */
[[nodiscard]] bool scan_destination(std::string_view destination) noexcept;

/** @return Counters for the most recent scan. */
[[nodiscard]] Summary summary() noexcept;

/** @return Number of retained authored placements from the most recent scan. */
[[nodiscard]] std::size_t placement_count() noexcept;

/** Copies one placement by index. */
[[nodiscard]] bool placement_at(std::size_t index, Placement& output) noexcept;

/** Reads one selected entity definition and the resource tags it owns. */
[[nodiscard]] bool inspect_definition(std::uint32_t entityTag) noexcept;

/** @return Summary for the most recent definition inspection. */
[[nodiscard]] DefinitionSummary definition_summary() noexcept;

/** @return Number of retained resource rows from the most recent definition inspection. */
[[nodiscard]] std::size_t definition_resource_count() noexcept;

/** Copies one retained definition-resource row. */
[[nodiscard]] bool definition_resource_at(std::size_t index, DefinitionResource& output) noexcept;

/** Reads every unique entity definition in the current destination and classifies known kinds. */
[[nodiscard]] bool classify_definitions() noexcept;

/** @return Counters for the most recent bulk classification. */
[[nodiscard]] ClassificationSummary classification_summary() noexcept;

/** @return Number of retained definition-signature rows. */
[[nodiscard]] std::size_t definition_signature_count() noexcept;

/** Copies one retained definition-signature row. */
[[nodiscard]] bool definition_signature_at(std::size_t index, DefinitionSignature& output) noexcept;

/** @return Number of resource-kind/payload signatures carried by skeleton-bearing definitions. */
[[nodiscard]] std::size_t actor_resource_frequency_count() noexcept;

/** Copies one actor resource frequency row. */
[[nodiscard]] bool actor_resource_frequency_at(std::size_t index,
                                               ActorResourceFrequency& output) noexcept;

/**
 * Re-reads one entity-resource tag and performs a bounded heuristic scan for nested package tags.
 * Kept as a low-level diagnostic primitive; the UI now uses the targeted actor branch inspector instead.
 */
[[nodiscard]] bool inspect_resource(std::uint32_t resourceTag) noexcept;

/** @return Summary for the most recent selected resource inspection. */
[[nodiscard]] ResourceInspectSummary resource_inspect_summary() noexcept;

/** @return Number of retained heuristic nested-tag rows. */
[[nodiscard]] std::size_t nested_tag_reference_count() noexcept;

/** Copies one retained heuristic nested-tag row. */
[[nodiscard]] bool nested_tag_reference_at(std::size_t index,
                                           NestedTagReference& output) noexcept;

/**
 * Recursively inspects one selected immediate entity-resource branch. Exact entity-definition
 * resource arrays encountered below the root are parsed structurally; other unknown blobs use
 * verified readable package-tag discovery. The explicit action is hard-capped for responsiveness.
 */
[[nodiscard]] bool inspect_actor_branch(std::uint32_t entityTag,
                                        std::uint32_t resourceTag) noexcept;

/** @return Summary for the most recent selected actor-resource branch inspection. */
[[nodiscard]] BranchInspectSummary branch_inspect_summary() noexcept;

/** @return Number of retained nodes in the most recent selected branch tree. */
[[nodiscard]] std::size_t branch_inspect_node_count() noexcept;

/** Copies one retained selected-branch tree node. */
[[nodiscard]] bool branch_inspect_node_at(std::size_t index, BranchInspectNode& output) noexcept;

/**
 * Scans the immediate resources of every currently classified skeleton-bearing actor for verified
 * nested package-tag classes, then builds exact class-set clusters. This is an explicit bounded
 * diagnostic action and requires a successful definition classification first.
 */
[[nodiscard]] bool analyze_actor_nested_classes() noexcept;

/** @return Summary for the most recent cross-actor nested-class analysis. */
[[nodiscard]] NestedAnalysisSummary nested_analysis_summary() noexcept;

/** @return Number of retained nested-class frequency rows. */
[[nodiscard]] std::size_t actor_nested_class_frequency_count() noexcept;

/** Copies one nested-class frequency row. */
[[nodiscard]] bool actor_nested_class_frequency_at(std::size_t index,
                                                   ActorNestedClassFrequency& output) noexcept;

/** @return Number of retained actor nested-class profiles. */
[[nodiscard]] std::size_t actor_nested_profile_count() noexcept;

/** Copies one actor nested-class profile. */
[[nodiscard]] bool actor_nested_profile_at(std::size_t index, ActorNestedProfile& output) noexcept;

/** @return Number of exact nested-class-set clusters. */
[[nodiscard]] std::size_t actor_nested_cluster_count() noexcept;

/** Copies one exact nested-class-set cluster row. */
[[nodiscard]] bool actor_nested_cluster_at(std::size_t index, ActorNestedCluster& output) noexcept;

/**
 * Builds a bounded entity-to-entity graph rooted at one actor definition. Only exact entity-resource
 * arrays are followed structurally. Unknown data is scanned only inside entity resources whose
 * exact kind/payload signature is 0x808084D7 -> 0x808084E9, and only verified entity-definition
 * tags are retained from those scans.
 */
[[nodiscard]] bool build_entity_graph(std::uint32_t entityTag) noexcept;

/** @return Summary for the most recent exact nested-entity graph extraction. */
[[nodiscard]] EntityGraphSummary entity_graph_summary() noexcept;

/** @return Number of retained unique entity-definition nodes. */
[[nodiscard]] std::size_t entity_graph_node_count() noexcept;

/** Copies one retained entity-definition node. */
[[nodiscard]] bool entity_graph_node_at(std::size_t index, EntityGraphNode& output) noexcept;

/** @return Number of retained direct carrier-resource edges. */
[[nodiscard]] std::size_t entity_graph_edge_count() noexcept;

/** Copies one retained nested-entity edge. */
[[nodiscard]] bool entity_graph_edge_at(std::size_t index, EntityGraphEdge& output) noexcept;

/** Finds the first retained authored placement that uses one entity definition. */
[[nodiscard]] bool first_placement_index(std::uint32_t entityTag, std::size_t& index) noexcept;

/** Human-readable short result label for the debug UI. */
[[nodiscard]] const char* result_label(ScanResult result) noexcept;

/** Human-readable short result label for entity-definition inspection. */
[[nodiscard]] const char* inspect_result_label(InspectResult result) noexcept;

/** Human-readable short result label for bulk definition classification. */
[[nodiscard]] const char* classification_result_label(ClassificationResult result) noexcept;

/** Human-readable short result label for selected resource inspection. */
[[nodiscard]] const char* resource_inspect_result_label(ResourceInspectResult result) noexcept;

/** Human-readable short result label for selected actor-resource branch inspection. */
[[nodiscard]] const char* branch_inspect_result_label(BranchInspectResult result) noexcept;

/** Human-readable short result label for cross-actor nested-class analysis. */
[[nodiscard]] const char* nested_analysis_result_label(NestedAnalysisResult result) noexcept;

/** Human-readable short result label for nested-entity graph extraction. */
[[nodiscard]] const char* entity_graph_result_label(EntityGraphResult result) noexcept;

/** Human-readable actor-priority label for one definition signature. */
[[nodiscard]] const char* actor_score_label(std::uint8_t score) noexcept;

} // namespace sunrise::client::content::entities
