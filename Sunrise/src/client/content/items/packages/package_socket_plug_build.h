#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "../../../../middleware/content/packages/tables/items.h"
#include "../../../../state/build_data/items/item_catalog.h"
#include "../../../../state/build_data/items/socket_plugs/definition.h"

namespace sunrise::client::content::items::packages {

namespace tables = middleware::content::packages::tables;
namespace socket_plugs = state::build_data::items::socket_plugs;

class SocketPlugBuild final {
public:
    SocketPlugBuild() = default;
    ~SocketPlugBuild() = default;
    SocketPlugBuild(const SocketPlugBuild&) = delete;
    SocketPlugBuild& operator=(const SocketPlugBuild&) = delete;

    [[nodiscard]] bool
    prepare(std::span<const std::uint8_t> specialCategories,
            std::span<const state::build_data::items::Definition> itemDefinitions) noexcept;
    /** Learns native socket-type ids whose declared pool contains at least one shader seed. */
    void observe_shader_socket_types(const tables::items::Row& item,
                                     std::span<const std::byte> itemDefinition,
                                     std::span<const std::byte> plugSetTable,
                                     std::size_t itemDefinitionCount) noexcept;
    [[nodiscard]] bool append(const tables::items::Row& item,
                              std::span<const std::byte> itemDefinition,
                              std::span<const std::byte> plugSetTable,
                              std::size_t itemDefinitionCount) noexcept;
    [[nodiscard]] bool publish() noexcept;
    [[nodiscard]] std::size_t skipped() const noexcept;
    [[nodiscard]] std::size_t rule_count() const noexcept;
    [[nodiscard]] std::size_t pool_count() const noexcept;
    [[nodiscard]] std::size_t member_count() const noexcept;
    [[nodiscard]] bool add(std::uint32_t itemDefinitionIndex,
                           std::size_t itemDefinitionCount) noexcept;

private:
    struct PoolLookup {
        std::uint64_t fingerprint{};
        std::uint32_t poolIndex{UINT32_MAX};
    };
    static constexpr std::size_t kCategoryCount = 3;
    static constexpr std::size_t kLookupCapacity = 1U << 19U;
    std::unique_ptr<socket_plugs::Rule[]> rules_{};
    std::unique_ptr<socket_plugs::Pool[]> pools_{};
    std::unique_ptr<socket_plugs::Member[]> members_{};
    std::unique_ptr<socket_plugs::Member[]> candidates_{};
    std::unique_ptr<socket_plugs::Member[]> categoryMembers_{};
    /** Installed profile shader definitions (native bucket 14) used as a Shadowkeep fallback. */
    std::unique_ptr<socket_plugs::Member[]> shaderMembers_{};
    std::unique_ptr<PoolLookup[]> lookup_{};
    std::array<std::size_t, kCategoryCount> categoryCounts_{};
    /** Native socket-type ids proven by the installed build to be shader lanes. */
    std::bitset<1U << 16U> shaderSocketTypes_{};
    std::array<socket_plugs::Member, 3> trackerMembers_{};
    std::size_t trackerCount_{};
    std::size_t shaderCount_{};
    std::size_t ruleCount_{};
    std::size_t poolCount_{};
    std::size_t memberCount_{};
    std::size_t candidateCount_{};
    std::size_t skipped_{};
    [[nodiscard]] bool intern(std::uint32_t& poolIndex, bool forceShaderFamily) noexcept;
    void release() noexcept;
};

[[nodiscard]] std::uint8_t special_plug_category(std::uint32_t categoryHash) noexcept;

} // namespace sunrise::client::content::items::packages
