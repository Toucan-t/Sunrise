#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sunrise::client::editor::catalog {

enum class PackageOrigin : std::uint8_t { destinyReadOnly, sunriseGenerated };

enum class AssetKind : std::uint8_t {
    empty,
    schemaTag,
    schemaGlobal,
    texture2D,
    textureCube,
    texture3D,
    texture2DData,
    textureCubeData,
    texture3DData,
    textureLargeBuffer,
    other,
};

struct PackageRecord {
    std::uint16_t packageId{};
    std::uint32_t generation{};
    std::uint32_t entryCount{};
    PackageOrigin origin{PackageOrigin::destinyReadOnly};
    std::wstring family;
    std::string displayFamily;
    std::string searchFamily;
};

struct AssetRecord {
    std::uint32_t tag{};
    std::uint32_t reference{};
    std::uint32_t typeInfo{};
    std::uint8_t fileType{};
    std::uint8_t fileSubtype{};
    std::uint32_t selectorBits{};
    std::uint32_t declaredSize{};
    std::uint32_t entryIndex{};
    std::uint16_t packageId{};
    std::uint32_t generation{};
    std::size_t packageIndex{};
    AssetKind kind{AssetKind::other};
};

struct TypeRecord { std::uint8_t fileType{}; std::uint8_t fileSubtype{}; std::size_t entries{}; };
struct KindRecord { AssetKind kind{AssetKind::other}; std::size_t entries{}; };
struct SchemaClassRecord {
    std::uint32_t classId{};
    std::size_t type8Entries{};
    std::size_t type16Entries{};
    std::size_t entries{};
};
struct Stats {
    std::size_t scannedPackages{};
    std::size_t scannedEntries{};
    std::size_t schemaEntries{};
    std::size_t emptyEntries{};
    std::size_t textureHeaders{};
    std::size_t textureDataEntries{};
    std::uint64_t scanMilliseconds{};
};
enum class RefreshStatus : std::uint8_t { neverRun, ready, packageDirectoryUnavailable, scanFailed };

[[nodiscard]] constexpr bool is_schema_entry(const AssetRecord& asset) noexcept {
    return asset.fileType == 8U || asset.fileType == 16U;
}
[[nodiscard]] constexpr bool is_empty_entry(const AssetRecord& asset) noexcept {
    return asset.fileType == 0U && asset.fileSubtype == 0U;
}
[[nodiscard]] constexpr bool is_texture_header(const AssetRecord& asset) noexcept {
    return asset.kind == AssetKind::texture2D || asset.kind == AssetKind::textureCube
           || asset.kind == AssetKind::texture3D;
}
[[nodiscard]] constexpr bool is_texture_data(const AssetRecord& asset) noexcept {
    return asset.kind == AssetKind::texture2DData || asset.kind == AssetKind::textureCubeData
           || asset.kind == AssetKind::texture3DData || asset.kind == AssetKind::textureLargeBuffer;
}
[[nodiscard]] AssetKind classify_kind(std::uint8_t fileType, std::uint8_t fileSubtype) noexcept;
[[nodiscard]] std::string_view asset_kind_name(AssetKind kind) noexcept;
[[nodiscard]] bool refresh() noexcept;
[[nodiscard]] const std::vector<AssetRecord>& assets() noexcept;
[[nodiscard]] const std::vector<PackageRecord>& packages() noexcept;
[[nodiscard]] const std::vector<TypeRecord>& types() noexcept;
[[nodiscard]] const std::vector<KindRecord>& kinds() noexcept;
[[nodiscard]] const std::vector<SchemaClassRecord>& schema_classes() noexcept;
[[nodiscard]] Stats stats() noexcept;
[[nodiscard]] RefreshStatus status() noexcept;
[[nodiscard]] std::uint64_t generation() noexcept;
[[nodiscard]] std::string_view package_directory() noexcept;
[[nodiscard]] std::wstring_view package_directory_wide() noexcept;
[[nodiscard]] const AssetRecord* find_asset(std::uint32_t tag) noexcept;
[[nodiscard]] const PackageRecord* package_for(const AssetRecord& asset) noexcept;

} // namespace sunrise::client::editor::catalog
