#include "socket_plug_catalog.h"

#include <algorithm>
#include <bitset>

#include "../../table.h"

namespace sunrise::state::build_data::items::socket_plugs {
namespace {

Lock g_lock;
Table<Rule, kRuleCapacity> g_rules;
Table<Pool, kPoolCapacity> g_pools;
Table<Member, kMemberCapacity> g_members;
std::bitset<kMemberDefinitionCapacity> g_membership;

[[nodiscard]] bool rule_less(const Rule& left, const Rule& right) noexcept {
    return left.itemDefinitionIndex < right.itemDefinitionIndex
           || (left.itemDefinitionIndex == right.itemDefinitionIndex && left.lane < right.lane);
}

[[nodiscard]] bool find_pool(std::uint16_t itemDefinitionIndex,
                             std::uint8_t lane,
                             std::span<const Pool>& pools,
                             std::span<const Member>& members,
                             const Pool*& pool) noexcept {
    pool = nullptr;
    if (lane >= kLaneCapacity) {
        return false;
    }
    const auto rules = g_rules.rows();
    pools = g_pools.rows();
    members = g_members.rows();
    const Rule key{itemDefinitionIndex, lane, 0, 0};
    const auto found = std::lower_bound(rules.begin(), rules.end(), key, rule_less);
    if (found == rules.end() || found->itemDefinitionIndex != itemDefinitionIndex
        || found->lane != lane || found->poolIndex >= pools.size()) {
        return false;
    }
    const Pool& candidate = pools[found->poolIndex];
    if (candidate.memberOffset > members.size()
        || candidate.memberCount > members.size() - candidate.memberOffset) {
        return false;
    }
    pool = &candidate;
    return true;
}

} // namespace

void clear() noexcept {
    const Lock::Exclusive guard(g_lock);
    g_rules.clear();
    g_pools.clear();
    g_members.clear();
    g_membership.reset();
}

bool valid(std::span<const Rule> rules,
           std::span<const Pool> pools,
           std::span<const Member> members) noexcept {
    if (rules.empty() || rules.size() > kRuleCapacity || pools.empty()
        || pools.size() > kPoolCapacity || members.size() > kMemberCapacity
        || pools.front().memberOffset != 0 || pools.front().memberCount != 0) {
        return false;
    }
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const Rule& rule = rules[index];
        if (rule.reserved != 0 || rule.lane >= kLaneCapacity || rule.poolIndex >= pools.size()
            || (index != 0 && !rule_less(rules[index - 1], rule))) {
            return false;
        }
    }
    std::size_t expectedOffset = 0;
    for (std::size_t index = 0; index < pools.size(); ++index) {
        const Pool& pool = pools[index];
        if (pool.memberOffset != expectedOffset || (index != 0 && pool.memberCount == 0)
            || pool.memberCount > members.size() - expectedOffset) {
            return false;
        }
        const auto range = members.subspan(expectedOffset, pool.memberCount);
        if (!std::is_sorted(range.begin(), range.end())
            || std::adjacent_find(range.begin(), range.end()) != range.end()) {
            return false;
        }
        expectedOffset += pool.memberCount;
    }
    return expectedOffset == members.size();
}

bool replace(std::span<const Rule> rules,
             std::span<const Pool> pools,
             std::span<const Member> members) noexcept {
    if (!valid(rules, pools, members)) {
        return false;
    }
    std::bitset<kMemberDefinitionCapacity> membership;
    for (const Member member : members) {
        if (member < membership.size()) {
            membership.set(member);
        }
    }
    const Lock::Exclusive guard(g_lock);
    if (!g_rules.replace(rules) || !g_pools.replace(pools) || !g_members.replace(members)) {
        return false;
    }
    g_membership = membership;
    return true;
}

bool allowed(std::uint16_t itemDefinitionIndex,
             std::uint8_t lane,
             std::uint16_t plugDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    std::span<const Pool> pools{};
    std::span<const Member> members{};
    const Pool* pool = nullptr;
    if (!find_pool(itemDefinitionIndex, lane, pools, members, pool) || pool == nullptr) {
        return false;
    }
    const auto range = members.subspan(pool->memberOffset, pool->memberCount);
    return std::binary_search(range.begin(), range.end(), plugDefinitionIndex);
}

bool contains(Member plugDefinitionIndex) noexcept {
    const Lock::Shared guard(g_lock);
    return plugDefinitionIndex < g_membership.size() && g_membership.test(plugDefinitionIndex);
}

bool pool_members(std::uint16_t itemDefinitionIndex,
                  std::uint8_t lane,
                  std::span<Member> output,
                  std::size_t& count) noexcept {
    count = 0;
    const Lock::Shared guard(g_lock);
    std::span<const Pool> pools{};
    std::span<const Member> members{};
    const Pool* pool = nullptr;
    if (!find_pool(itemDefinitionIndex, lane, pools, members, pool) || pool == nullptr
        || output.size() < pool->memberCount) {
        return false;
    }
    const auto range = members.subspan(pool->memberOffset, pool->memberCount);
    std::copy(range.begin(), range.end(), output.begin());
    count = range.size();
    return true;
}

std::size_t rule_count() noexcept {
    const Lock::Shared guard(g_lock);
    return g_rules.count();
}

} // namespace sunrise::state::build_data::items::socket_plugs
