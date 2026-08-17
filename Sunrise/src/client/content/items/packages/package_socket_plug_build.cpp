#include "package_socket_plug_build.h"

#include <algorithm>
#include <limits>
#include <new>

#include "../../../../state/build_data/runtime.h"

namespace sunrise::client::content::items::packages {
namespace {

constexpr std::array<std::uint32_t, 3> kExpandableCategories{
    0xB134761EU,
    0x87727F34U,
    0x6C863692U,
};
constexpr std::array<std::uint32_t, 3> kTrackerPlugHashes{
    2'285'418'970U,
    2'302'094'943U,
    38'912'240U,
};
constexpr std::uint16_t kTrackerSocketType = 518;
/** Shadowkeep profile-inventory bucket containing shader action sources. */
constexpr std::uint8_t kShaderBucketId = 14;
constexpr std::uint64_t kHashOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

struct VisitorContext {
    SocketPlugBuild* build{};
    std::size_t itemDefinitionCount{};
};

[[nodiscard]] bool visit_member(void* opaque, std::uint32_t itemDefinitionIndex) noexcept {
    auto& context = *static_cast<VisitorContext*>(opaque);
    return context.build != nullptr
           && context.build->add(itemDefinitionIndex, context.itemDefinitionCount);
}

} // namespace

std::uint8_t special_plug_category(std::uint32_t categoryHash) noexcept {
    for (std::size_t index = 0; index < kExpandableCategories.size(); ++index) {
        if (kExpandableCategories[index] == categoryHash) {
            return static_cast<std::uint8_t>(index + 1);
        }
    }
    return 0;
}

bool SocketPlugBuild::prepare(
    std::span<const std::uint8_t> specialCategories,
    std::span<const state::build_data::items::Definition> itemDefinitions) noexcept {
    release();
    if (specialCategories.size() < itemDefinitions.size()
        || itemDefinitions.size() > state::build_data::items::kDefinitionCapacity) {
        return false;
    }
    rules_.reset(new (std::nothrow) socket_plugs::Rule[socket_plugs::kRuleCapacity]);
    pools_.reset(new (std::nothrow) socket_plugs::Pool[socket_plugs::kPoolCapacity]);
    members_.reset(new (std::nothrow) socket_plugs::Member[socket_plugs::kMemberCapacity]);
    candidates_.reset(new (std::nothrow)
                          socket_plugs::Member[state::build_data::items::kDefinitionCapacity]);
    categoryMembers_.reset(
        new (std::nothrow)
            socket_plugs::Member[kCategoryCount * state::build_data::items::kDefinitionCapacity]);
    shaderMembers_.reset(
        new (std::nothrow) socket_plugs::Member[state::build_data::items::kDefinitionCapacity]);
    lookup_.reset(new (std::nothrow) PoolLookup[kLookupCapacity]);
    if (!rules_ || !pools_ || !members_ || !candidates_ || !categoryMembers_ || !shaderMembers_
        || !lookup_) {
        release();
        return false;
    }
    pools_[socket_plugs::kEmptyPoolIndex] = {};
    poolCount_ = 1;
    for (std::size_t item = 0; item < itemDefinitions.size(); ++item) {
        const auto nativeIndex = itemDefinitions[item].definitionIndex;
        if (nativeIndex >= specialCategories.size()) {
            release();
            return false;
        }
        const std::uint8_t category = specialCategories[nativeIndex];
        if (category != 0 && category <= kCategoryCount) {
            const std::size_t family = category - 1;
            categoryMembers_[family * state::build_data::items::kDefinitionCapacity
                             + categoryCounts_[family]++] = nativeIndex;
        }
        if (itemDefinitions[item].bucketId == kShaderBucketId) {
            if (shaderCount_ >= state::build_data::items::kDefinitionCapacity) {
                release();
                return false;
            }
            shaderMembers_[shaderCount_++] = nativeIndex;
        }
        for (const std::uint32_t trackerHash : kTrackerPlugHashes) {
            if (itemDefinitions[item].definitionHash != trackerHash) {
                continue;
            }
            if (trackerCount_ >= trackerMembers_.size()) {
                release();
                return false;
            }
            trackerMembers_[trackerCount_++] = nativeIndex;
        }
    }
    for (std::size_t i = 0; i < kLookupCapacity; ++i) {
        lookup_[i].poolIndex = UINT32_MAX;
    }
    return true;
}

bool SocketPlugBuild::add(std::uint32_t itemDefinitionIndex,
                          std::size_t itemDefinitionCount) noexcept {
    if (!candidates_ || itemDefinitionIndex >= itemDefinitionCount
        || itemDefinitionIndex >= state::build_data::items::kDefinitionCapacity
        || candidateCount_ >= state::build_data::items::kDefinitionCapacity) {
        return false;
    }
    candidates_[candidateCount_++] = static_cast<std::uint16_t>(itemDefinitionIndex);
    return true;
}

bool SocketPlugBuild::intern(std::uint32_t& poolIndex, bool forceShaderFamily) noexcept {
    poolIndex = socket_plugs::kEmptyPoolIndex;
    if (!candidates_ || !categoryMembers_ || !lookup_) {
        return false;
    }
    std::sort(candidates_.get(), candidates_.get() + candidateCount_);
    candidateCount_ = static_cast<std::size_t>(
        std::unique(candidates_.get(), candidates_.get() + candidateCount_) - candidates_.get());
    const bool expandShaders =
        shaderCount_ != 0
        && (forceShaderFamily
            || std::any_of(candidates_.get(),
                           candidates_.get() + candidateCount_,
                           [this](socket_plugs::Member candidate) noexcept {
                               return std::binary_search(shaderMembers_.get(),
                                                         shaderMembers_.get() + shaderCount_,
                                                         candidate);
                           }));
    std::array<bool, kCategoryCount> expand{};
    for (std::size_t family = 0; family < kCategoryCount; ++family) {
        const auto* familyMembers =
            categoryMembers_.get() + family * state::build_data::items::kDefinitionCapacity;
        for (std::size_t seed = 0; seed < candidateCount_ && !expand[family]; ++seed) {
            expand[family] = std::binary_search(
                familyMembers, familyMembers + categoryCounts_[family], candidates_[seed]);
        }
    }
    for (std::size_t family = 0; family < kCategoryCount; ++family) {
        if (!expand[family]) {
            continue;
        }
        if (categoryCounts_[family]
            > state::build_data::items::kDefinitionCapacity - candidateCount_) {
            return false;
        }
        const auto* first =
            categoryMembers_.get() + family * state::build_data::items::kDefinitionCapacity;
        std::copy_n(first, categoryCounts_[family], candidates_.get() + candidateCount_);
        candidateCount_ += categoryCounts_[family];
    }
    // Some Shadowkeep shader definitions do not expose the category hash consistently. A lane
    // that already names any shader seed is nevertheless the native shader action lane, and all
    // profile-bucket-14 shaders are valid members of that global family.
    if (expandShaders) {
        if (shaderCount_ > state::build_data::items::kDefinitionCapacity - candidateCount_) {
            return false;
        }
        std::copy_n(shaderMembers_.get(), shaderCount_, candidates_.get() + candidateCount_);
        candidateCount_ += shaderCount_;
    }
    std::sort(candidates_.get(), candidates_.get() + candidateCount_);
    candidateCount_ = static_cast<std::size_t>(
        std::unique(candidates_.get(), candidates_.get() + candidateCount_) - candidates_.get());
    if (candidateCount_ == 0) {
        return true;
    }
    std::uint64_t fingerprint = kHashOffsetBasis;
    for (std::size_t member = 0; member < candidateCount_; ++member) {
        std::uint16_t value = candidates_[member];
        for (std::size_t byte = 0; byte < sizeof value; ++byte) {
            fingerprint ^= static_cast<std::uint8_t>(value);
            fingerprint *= kHashPrime;
            value >>= 8U;
        }
    }
    fingerprint ^= candidateCount_;
    fingerprint *= kHashPrime;
    static_assert((kLookupCapacity & (kLookupCapacity - 1)) == 0);
    const std::size_t start = static_cast<std::size_t>(fingerprint) & (kLookupCapacity - 1);
    for (std::size_t probe = 0; probe < kLookupCapacity; ++probe) {
        PoolLookup& slot = lookup_[(start + probe) & (kLookupCapacity - 1)];
        if (slot.poolIndex == UINT32_MAX) {
            if (poolCount_ >= socket_plugs::kPoolCapacity
                || candidateCount_ > socket_plugs::kMemberCapacity - memberCount_) {
                return false;
            }
            poolIndex = static_cast<std::uint32_t>(poolCount_);
            pools_[poolCount_++] = {static_cast<std::uint32_t>(memberCount_),
                                    static_cast<std::uint32_t>(candidateCount_)};
            std::copy_n(candidates_.get(), candidateCount_, members_.get() + memberCount_);
            memberCount_ += candidateCount_;
            slot = {fingerprint, poolIndex};
            return true;
        }
        if (slot.fingerprint != fingerprint || slot.poolIndex >= poolCount_) {
            continue;
        }
        const socket_plugs::Pool& pool = pools_[slot.poolIndex];
        if (pool.memberCount == candidateCount_
            && std::equal(candidates_.get(),
                          candidates_.get() + candidateCount_,
                          members_.get() + pool.memberOffset)) {
            poolIndex = slot.poolIndex;
            return true;
        }
    }
    return false;
}

/** Learns shader socket types before the exact pool pass so empty/default lanes still expand. */
void SocketPlugBuild::observe_shader_socket_types(
    const tables::items::Row& item,
    std::span<const std::byte> itemDefinition,
    std::span<const std::byte> plugSetTable,
    std::size_t itemDefinitionCount) noexcept {
    if (!candidates_ || !shaderMembers_ || shaderCount_ == 0
        || item.definitionIndex >= itemDefinitionCount
        || item.socketCount > socket_plugs::kLaneCapacity) {
        return;
    }
    for (std::uint8_t lane = 0; lane < item.socketCount; ++lane) {
        candidateCount_ = 0;
        VisitorContext visitor{this, itemDefinitionCount};
        bool valid = tables::items::visit_allowed_plugs(
            itemDefinition, plugSetTable, lane, visit_member, &visitor);
        if (valid && item.initialPlugs[lane] != tables::items::kUnavailablePlug) {
            valid = add(item.initialPlugs[lane], itemDefinitionCount);
        }
        if (!valid || item.socketTypes[lane] == tables::items::kUnavailableSocketType) {
            continue;
        }
        const bool containsShader =
            std::any_of(candidates_.get(),
                        candidates_.get() + candidateCount_,
                        [this](socket_plugs::Member candidate) noexcept {
                            return std::binary_search(shaderMembers_.get(),
                                                      shaderMembers_.get() + shaderCount_,
                                                      candidate);
                        });
        if (containsShader) {
            shaderSocketTypes_.set(item.socketTypes[lane]);
        }
    }
    candidateCount_ = 0;
}

bool SocketPlugBuild::append(const tables::items::Row& item,
                             std::span<const std::byte> itemDefinition,
                             std::span<const std::byte> plugSetTable,
                             std::size_t itemDefinitionCount) noexcept {
    if (!rules_ || item.definitionIndex >= itemDefinitionCount
        || item.socketCount > socket_plugs::kLaneCapacity) {
        return false;
    }
    bool complete = true;
    for (std::uint8_t lane = 0; lane < item.socketCount; ++lane) {
        candidateCount_ = 0;
        VisitorContext visitor{this, itemDefinitionCount};
        bool laneValid = tables::items::visit_allowed_plugs(
            itemDefinition, plugSetTable, lane, visit_member, &visitor);
        if (laneValid && item.initialPlugs[lane] != tables::items::kUnavailablePlug) {
            laneValid = add(item.initialPlugs[lane], itemDefinitionCount);
        }
        if (laneValid && item.socketTypes[lane] == kTrackerSocketType) {
            for (std::size_t tracker = 0; tracker < trackerCount_ && laneValid; ++tracker) {
                laneValid = add(trackerMembers_[tracker], itemDefinitionCount);
            }
        }
        const bool learnedShaderLane =
            item.socketTypes[lane] != tables::items::kUnavailableSocketType
            && shaderSocketTypes_.test(item.socketTypes[lane]);
        std::uint32_t poolIndex = socket_plugs::kEmptyPoolIndex;
        laneValid = laneValid && intern(poolIndex, learnedShaderLane);
        if (!laneValid || ruleCount_ >= socket_plugs::kRuleCapacity) {
            ++skipped_;
            complete = false;
            continue;
        }
        rules_[ruleCount_++] = {item.definitionIndex, lane, 0, poolIndex};
    }
    return complete;
}

bool SocketPlugBuild::publish() noexcept {
    const bool published =
        rules_ && pools_ && members_
        && state::build_data::publish_socket_plug_rules(std::span(rules_.get(), ruleCount_),
                                                        std::span(pools_.get(), poolCount_),
                                                        std::span(members_.get(), memberCount_));
    release();
    return published;
}

std::size_t SocketPlugBuild::skipped() const noexcept { return skipped_; }
std::size_t SocketPlugBuild::rule_count() const noexcept { return ruleCount_; }
std::size_t SocketPlugBuild::pool_count() const noexcept { return poolCount_; }
std::size_t SocketPlugBuild::member_count() const noexcept { return memberCount_; }

void SocketPlugBuild::release() noexcept {
    rules_.reset();
    pools_.reset();
    members_.reset();
    candidates_.reset();
    categoryMembers_.reset();
    shaderMembers_.reset();
    lookup_.reset();
    categoryCounts_ = {};
    shaderSocketTypes_.reset();
    trackerMembers_ = {};
    trackerCount_ = 0;
    shaderCount_ = 0;
    ruleCount_ = 0;
    poolCount_ = 0;
    memberCount_ = 0;
    candidateCount_ = 0;
    skipped_ = 0;
}

} // namespace sunrise::client::content::items::packages
