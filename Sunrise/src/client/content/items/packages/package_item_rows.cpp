#include <array>
#include <cstdio>
#include <span>
#include <vector>

#include "../../../../core/logging/log.h"
#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Reports the optional exact socket-pool extraction without making it a core boot dependency. */
void report_socket_rules(bool success,
                         std::size_t rules,
                         std::size_t pools,
                         std::size_t members,
                         std::size_t skipped) noexcept {
    std::array<char, 192> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=pkg stage=socket_plugs result=%s rules=%zu pools=%zu "
                                      "members=%zu skipped=%zu",
                                      success ? "ok" : "fail",
                                      rules,
                                      pools,
                                      members,
                                      skipped);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         success ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Walks the located item index table, then publishes every domain that depends on it. */
bool build_item_rows(const reader::Source& source,
                     Storage& storage,
                     const tables::Array& table,
                     std::size_t& rowCount,
                     const char*& reason) noexcept {
    const bool needDefinitions = !state::build_data::item_definitions_ready();
    const bool needDetails = !state::build_data::configured_item_details_ready();
    const bool needSocketRules = !state::build_data::socket_plug_rules_ready();
    bool buildSocketRules = needSocketRules;
    if (buildSocketRules) {
        std::uint32_t plugSetTag = 0;
        buildSocketRules =
            tables::slot_tag(std::span<const std::byte>{storage.root},
                             tables::kPlugSetTableSlot,
                             plugSetTag)
            && plugSetTag != 0
            && reader::read_tag(source, storage.scratch, plugSetTag, storage.plugSetTable);
        if (!buildSocketRules) {
            report_socket_rules(false, 0, 0, 0, 0);
        }
    }
    const bool needRows = needDefinitions || needDetails || buildSocketRules;
    bool published = !needRows;
    const std::span<const std::byte> container{storage.child};
    reason = "rows";

    const bool haveAuthored = (needDefinitions || needDetails) && collect_authored_hashes(storage.authoredHashes);
    std::size_t detailCount = 0;
    rowCount = 0;
    storage.specialPlugCategories.fill(0);
    for (std::uint64_t index = 0; needRows && index < table.count && rowCount < storage.rows.size();
         ++index) {
        tables::IndexRow row{};
        if (!tables::index_row(container, table, index, row)) {
            break;
        }
        tables::items::Row item{};
        item.definitionHash = row.definitionHash;
        item.definitionIndex = static_cast<std::uint16_t>(index);
        if (!reader::read_tag(source, storage.scratch, row.targetTag, storage.definition)
            || !tables::items::read_definition(std::span<const std::byte>{storage.definition},
                                               item)) {
            continue;
        }
        storage.rows[rowCount++] = state::build_data::items::Definition{
            item.definitionHash, item.definitionIndex, item.bucketId};
        if (item.definitionIndex < storage.specialPlugCategories.size()) {
            storage.specialPlugCategories[item.definitionIndex] =
                special_plug_category(item.plugCategoryHash);
        }
        if (haveAuthored && authored(storage.authoredHashes, item.definitionHash)) {
            (void)request(item.definitionIndex, storage.requested, detailCount);
            (void)append_initial_plugs(item, storage.requested, detailCount);
        }
    }
    if (needRows) {
        compact_requested(storage.requested, detailCount);
        published = rowCount != 0;
    }
    if (published && needDefinitions) {
        published =
            state::build_data::publish_item_definitions(std::span(storage.rows).first(rowCount));
    }
    if (!published) {
        reason = "publish";
    }

    if (published && needDetails) {
        reason = "details";
        const DetailSource detailSource{
            &source, &storage.scratch, container, table, &storage.definition};
        for (std::size_t slot = 0; published && slot < detailCount; ++slot) {
            published = build_detail(detailSource, storage.requested[slot], storage.details[slot]);
            if (!published) {
                report_detail_failure(slot, storage.requested[slot]);
            }
        }
        published = published
                    && state::build_data::publish_configured_item_details(
                        std::span(storage.details).first(detailCount));
        report_detail_count(detailCount);
    }

    // Socket compatibility is a process-local installed-build relation. Build it even on a cache
    // hit because editor/native socket actions must never guess which plugs one lane accepts.
    if (published && buildSocketRules) {
        SocketPlugBuild socketBuild;
        bool socketRulesPublished = socketBuild.prepare(
            std::span(storage.specialPlugCategories).first(rowCount),
            std::span(storage.rows).first(rowCount));
        if (socketRulesPublished) {
            // First learn shader socket-type ids from any lane whose installed pool carries a
            // bucket-14 seed. Some Shadowkeep items leave the equivalent lane empty/default, so
            // the second pass uses this build-derived semantic type rather than a hard-coded lane.
            for (std::uint64_t index = 0; index < table.count; ++index) {
                tables::IndexRow row{};
                if (!tables::index_row(container, table, index, row)) {
                    socketRulesPublished = false;
                    break;
                }
                tables::items::Row item{};
                item.definitionHash = row.definitionHash;
                item.definitionIndex = static_cast<std::uint16_t>(index);
                if (!reader::read_tag(source, storage.scratch, row.targetTag, storage.definition)
                    || !tables::items::read_definition(
                        std::span<const std::byte>{storage.definition}, item)) {
                    continue;
                }
                socketBuild.observe_shader_socket_types(
                    item,
                    std::span<const std::byte>{storage.definition},
                    std::span<const std::byte>{storage.plugSetTable},
                    rowCount);
            }
        }
        if (socketRulesPublished) {
            for (std::uint64_t index = 0; index < table.count; ++index) {
                tables::IndexRow row{};
                if (!tables::index_row(container, table, index, row)) {
                    socketRulesPublished = false;
                    break;
                }
                tables::items::Row item{};
                item.definitionHash = row.definitionHash;
                item.definitionIndex = static_cast<std::uint16_t>(index);
                if (!reader::read_tag(source, storage.scratch, row.targetTag, storage.definition)
                    || !tables::items::read_definition(
                        std::span<const std::byte>{storage.definition}, item)) {
                    continue;
                }
                (void)socketBuild.append(item,
                                         std::span<const std::byte>{storage.definition},
                                         std::span<const std::byte>{storage.plugSetTable},
                                         rowCount);
            }
        }
        const std::size_t ruleCount = socketBuild.rule_count();
        const std::size_t poolCount = socketBuild.pool_count();
        const std::size_t memberCount = socketBuild.member_count();
        const std::size_t skipped = socketBuild.skipped();
        socketRulesPublished = socketRulesPublished && ruleCount != 0 && socketBuild.publish();
        report_socket_rules(socketRulesPublished, ruleCount, poolCount, memberCount, skipped);
        // Socket metadata is an optional action domain. Core item/character boot remains usable if
        // an installed lane is malformed; native/debug socket actions simply fail closed.
    }

    if (published && !state::build_data::ability_buckets_ready()) {
        reason = "abilities";
        std::size_t abilityCount = 0;
        const bool built = build_character_abilities(source,
                                                     storage.scratch,
                                                     std::span<const std::byte>{storage.root},
                                                     storage.abilityTable,
                                                     storage.definition,
                                                     storage.abilityPool,
                                                     storage.abilityRows,
                                                     abilityCount);
        published = built
                    && state::build_data::publish_ability_buckets(
                        std::span(storage.abilityRows).first(abilityCount));
        if (built) {
            report_ability_count(abilityCount);
        }
    }
    return published && state::build_data::item_definitions_ready()
           && state::build_data::configured_item_details_ready()
           && state::build_data::ability_buckets_ready();
}

} // namespace sunrise::client::content::items::packages
