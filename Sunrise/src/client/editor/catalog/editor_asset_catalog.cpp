#include "editor_asset_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::editor::catalog {
namespace {

namespace package_reader = middleware::content::packages::reader;

constexpr std::size_t kFileTypeCount = 128;
constexpr std::size_t kFileSubtypeCount = 8;
constexpr std::size_t kTypePairCount = kFileTypeCount * kFileSubtypeCount;

std::vector<AssetRecord> g_assets{};
std::vector<PackageRecord> g_packages{};
std::vector<TypeRecord> g_types{};
std::vector<KindRecord> g_kinds{};
std::vector<SchemaClassRecord> g_schemaClasses{};
Stats g_stats{};
RefreshStatus g_status{RefreshStatus::neverRun};
std::uint64_t g_generation{};
std::wstring g_packageDirectoryWide{};
std::string g_packageDirectoryUtf8{};

/** Temporary owner of vectors populated by the package reader visitor. */
struct BuildContext {
    std::vector<AssetRecord>* assets{};
    std::vector<PackageRecord>* packages{};
};

/** Classification products built from a complete asset catalog. */
struct Classification {
    std::vector<TypeRecord> types{};
    std::vector<SchemaClassRecord> schemaClasses{};
    std::vector<KindRecord> kinds{};
    std::size_t schemaEntries{};
    std::size_t emptyEntries{};
    std::size_t textureHeaders{};
    std::size_t textureDataEntries{};
};

/** Lowercases package-name ASCII once so filtering does not repeat it every frame. */
[[nodiscard]] std::string ascii_lower(std::string_view text) {
    std::string result(text);
    for (char& value : result) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return result;
}

/** Converts one Windows path/name to the UTF-8 ImGui side. */
[[nodiscard]] bool wide_to_utf8(std::wstring_view input, std::string& output) noexcept {
    output.clear();
    if (input.empty()) {
        return true;
    }
    const int length = static_cast<int>(input.size());
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(required));
    const int converted = WideCharToMultiByte(CP_UTF8,
                                               WC_ERR_INVALID_CHARS,
                                               input.data(),
                                               length,
                                               output.data(),
                                               required,
                                               nullptr,
                                               nullptr);
    if (converted != required) {
        output.clear();
        return false;
    }
    return true;
}

/** Resolves <game directory>\\packages without creating or modifying anything there. */
[[nodiscard]] bool resolve_package_directory(std::wstring& wide, std::string& utf8) noexcept {
    core::path::Buffer path{};
    const HMODULE gameModule = GetModuleHandleW(nullptr);
    if (gameModule == nullptr || !core::path::module_directory(gameModule, path)
        || !core::path::append(path, L"packages")) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.chars.data());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    wide.assign(path.chars.data(), path.length);
    return wide_to_utf8(wide, utf8);
}

/** @return Stable text used by scan-failure diagnostics. */
[[nodiscard]] std::string_view failure_name(package_reader::CatalogScanFailure failure) noexcept {
    switch (failure) {
        case package_reader::CatalogScanFailure::none:
            return "none";
        case package_reader::CatalogScanFailure::invalidArguments:
            return "invalid_arguments";
        case package_reader::CatalogScanFailure::searchPattern:
            return "search_pattern";
        case package_reader::CatalogScanFailure::enumerateDirectory:
            return "enumerate_directory";
        case package_reader::CatalogScanFailure::locateLatest:
            return "locate_latest";
        case package_reader::CatalogScanFailure::packageFamily:
            return "package_family";
        case package_reader::CatalogScanFailure::buildPath:
            return "build_path";
        case package_reader::CatalogScanFailure::readHeader:
            return "read_header";
        case package_reader::CatalogScanFailure::parseHeader:
            return "parse_header";
        case package_reader::CatalogScanFailure::entryCountOutOfRange:
            return "entry_count_out_of_range";
        case package_reader::CatalogScanFailure::readEntryTable:
            return "read_entry_table";
        case package_reader::CatalogScanFailure::visitorRejected:
            return "visitor_rejected";
        case package_reader::CatalogScanFailure::enumerateDirectoryAdvance:
            return "enumerate_directory_advance";
        case package_reader::CatalogScanFailure::closeEnumeration:
            return "close_enumeration";
    }
    return "unknown";
}

/** Emits one complete successful catalog-refresh summary. */
void log_success(const package_reader::CatalogScanResult& result,
                 std::size_t packageRows,
                 std::size_t assetRows,
                 std::size_t typePairs,
                 std::size_t schemaClasses,
                 std::size_t schemaEntries,
                 std::size_t emptyEntries,
                 std::uint64_t milliseconds,
                 std::uint64_t generation) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=editor_catalog stage=scan result=ok packages=%zu package_rows=%zu entries=%zu "
        "asset_rows=%zu type_pairs=%zu schema_classes=%zu schema_entries=%zu empty_rows=%zu "
        "texture_headers=%zu texture_data=%zu ms=%llu generation=%llu",
        result.packages,
        packageRows,
        result.entries,
        assetRows,
        typePairs,
        schemaClasses,
        schemaEntries,
        emptyEntries,
        g_stats.textureHeaders,
        g_stats.textureDataEntries,
        static_cast<unsigned long long>(milliseconds),
        static_cast<unsigned long long>(generation));
    if (written > 0) {
        const std::size_t length =
            (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
        core::log::write(
            core::log::Channel::client, core::log::Level::info, {line.data(), length});
    }
}

/** Emits the exact package/table stage at which one catalog scan stopped. */
void log_failure(const package_reader::CatalogScanResult& result,
                 std::uint64_t milliseconds) noexcept {
    const std::string_view stage = failure_name(result.failure);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=editor_catalog stage=scan result=fail failure=%.*s package=0x%04X generation=%u "
        "entry=%u packages=%zu entries=%zu ms=%llu",
        static_cast<int>(stage.size()),
        stage.data(),
        static_cast<unsigned>(result.packageId),
        static_cast<unsigned>(result.patchIndex),
        static_cast<unsigned>(result.entryIndex),
        result.packages,
        result.entries,
        static_cast<unsigned long long>(milliseconds));
    if (written > 0) {
        const std::size_t length =
            (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
        core::log::write(
            core::log::Channel::client, core::log::Level::error, {line.data(), length});
    }
}

/** Adds one package row the first time the sequential scan reaches that package. */
[[nodiscard]] bool ensure_package(BuildContext& build,
                                  const package_reader::CatalogEntry& entry,
                                  std::size_t& packageIndex) noexcept {
    if (!build.packages->empty()) {
        const PackageRecord& current = build.packages->back();
        if (current.packageId == entry.packageId && current.generation == entry.patchIndex) {
            packageIndex = build.packages->size() - 1U;
            return true;
        }
    }

    PackageRecord package{};
    package.packageId = entry.packageId;
    package.generation = entry.patchIndex;
    package.entryCount = entry.packageEntryCount;
    package.origin = PackageOrigin::destinyReadOnly;
    package.family.assign(entry.packageFamily.data(), entry.packageFamily.size());
    if (!wide_to_utf8(package.family, package.displayFamily)) {
        return false;
    }
    package.searchFamily = ascii_lower(package.displayFamily);
    build.packages->push_back(std::move(package));
    packageIndex = build.packages->size() - 1U;
    return true;
}

/** Receives one public entry-table row from the generic package catalog scan. */
[[nodiscard]] bool visit_entry(void* context,
                               const package_reader::CatalogEntry& entry) noexcept {
    auto& build = *static_cast<BuildContext*>(context);
    std::size_t packageIndex = 0;
    if (!ensure_package(build, entry, packageIndex)) {
        return false;
    }

    build.assets->push_back(AssetRecord{entry.tag,
                                        entry.reference,
                                        entry.typeInfo,
                                        entry.fileType,
                                        entry.fileSubtype,
                                        entry.selectorBits,
                                        entry.decodedSize,
                                        entry.entryIndex,
                                        entry.packageId,
                                        entry.patchIndex,
                                        packageIndex,
                                        classify_kind(entry.fileType, entry.fileSubtype)});
    return true;
}

/** Builds file-type statistics and schema-class statistics without misclassifying references. */
[[nodiscard]] Classification classify(const std::vector<AssetRecord>& assets) {
    std::array<std::size_t, kTypePairCount> typeCounts{};
    std::unordered_map<std::uint32_t, SchemaClassRecord> schemaCounts{};
    std::array<std::size_t, 11> kindCounts{};
    schemaCounts.reserve(1024);

    Classification result{};
    for (const AssetRecord& asset : assets) {
        const std::size_t typeIndex = static_cast<std::size_t>(asset.fileType)
                                      * kFileSubtypeCount
                                      + static_cast<std::size_t>(asset.fileSubtype);
        ++typeCounts[typeIndex];
        ++kindCounts[static_cast<std::size_t>(asset.kind)];
        if (is_texture_header(asset)) { ++result.textureHeaders; }
        if (is_texture_data(asset)) { ++result.textureDataEntries; }

        if (is_empty_entry(asset)) {
            ++result.emptyEntries;
        }
        if (!is_schema_entry(asset)) {
            continue;
        }

        ++result.schemaEntries;
        SchemaClassRecord& record = schemaCounts[asset.reference];
        record.classId = asset.reference;
        if (asset.fileType == 8U) {
            ++record.type8Entries;
        } else {
            ++record.type16Entries;
        }
        ++record.entries;
    }

    result.types.reserve(kTypePairCount);
    for (std::size_t fileType = 0; fileType < kFileTypeCount; ++fileType) {
        for (std::size_t fileSubtype = 0; fileSubtype < kFileSubtypeCount; ++fileSubtype) {
            const std::size_t count = typeCounts[fileType * kFileSubtypeCount + fileSubtype];
            if (count == 0) {
                continue;
            }
            result.types.push_back(TypeRecord{static_cast<std::uint8_t>(fileType),
                                              static_cast<std::uint8_t>(fileSubtype),
                                              count});
        }
    }
    std::sort(result.types.begin(), result.types.end(), [](const TypeRecord& left,
                                                           const TypeRecord& right) {
        if (left.entries != right.entries) {
            return left.entries > right.entries;
        }
        if (left.fileType != right.fileType) {
            return left.fileType < right.fileType;
        }
        return left.fileSubtype < right.fileSubtype;
    });

    for (std::size_t index = 0; index < kindCounts.size(); ++index) {
        if (kindCounts[index] != 0) {
            result.kinds.push_back(KindRecord{static_cast<AssetKind>(index), kindCounts[index]});
        }
    }
    std::sort(result.kinds.begin(), result.kinds.end(), [](const KindRecord& left, const KindRecord& right) {
        if (left.entries != right.entries) return left.entries > right.entries;
        return static_cast<unsigned>(left.kind) < static_cast<unsigned>(right.kind);
    });

    result.schemaClasses.reserve(schemaCounts.size());
    for (const auto& [classId, record] : schemaCounts) {
        (void)classId;
        result.schemaClasses.push_back(record);
    }
    std::sort(result.schemaClasses.begin(),
              result.schemaClasses.end(),
              [](const SchemaClassRecord& left, const SchemaClassRecord& right) {
                  if (left.entries != right.entries) {
                      return left.entries > right.entries;
                  }
                  return left.classId < right.classId;
              });
    return result;
}

} // namespace

AssetKind classify_kind(std::uint8_t fileType, std::uint8_t fileSubtype) noexcept {
    if (fileType == 0U && fileSubtype == 0U) return AssetKind::empty;
    if (fileType == 8U) return AssetKind::schemaTag;
    if (fileType == 16U) return AssetKind::schemaGlobal;
    if (fileType == 32U) {
        if (fileSubtype == 1U) return AssetKind::texture2D;
        if (fileSubtype == 2U) return AssetKind::textureCube;
        if (fileSubtype == 3U) return AssetKind::texture3D;
    }
    if (fileType == 40U) {
        if (fileSubtype == 1U) return AssetKind::texture2DData;
        if (fileSubtype == 2U) return AssetKind::textureCubeData;
        if (fileSubtype == 3U) return AssetKind::texture3DData;
    }
    if (fileType == 48U && fileSubtype == 1U) return AssetKind::textureLargeBuffer;
    return AssetKind::other;
}

std::string_view asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
        case AssetKind::empty: return "Empty";
        case AssetKind::schemaTag: return "Tag";
        case AssetKind::schemaGlobal: return "TagGlobal";
        case AssetKind::texture2D: return "Texture2D";
        case AssetKind::textureCube: return "TextureCube";
        case AssetKind::texture3D: return "Texture3D";
        case AssetKind::texture2DData: return "Texture2D Data";
        case AssetKind::textureCubeData: return "TextureCube Data";
        case AssetKind::texture3DData: return "Texture3D Data";
        case AssetKind::textureLargeBuffer: return "Texture Large Buffer";
        case AssetKind::other: return "Other";
    }
    return "Other";
}

/** Atomically replaces the editor catalog after one complete read-only package-table pass. */
bool refresh() noexcept {
    const std::uint64_t started = GetTickCount64();

    std::wstring packageDirectoryWide{};
    std::string packageDirectoryUtf8{};
    if (!resolve_package_directory(packageDirectoryWide, packageDirectoryUtf8)) {
        g_packageDirectoryWide.clear();
        g_packageDirectoryUtf8.clear();
        g_status = RefreshStatus::packageDirectoryUnavailable;
        core::log::write(core::log::Channel::client,
                         core::log::Level::error,
                         "ev=editor_catalog stage=resolve_packages result=fail");
        return false;
    }

    // Publish the resolved input path even if the scan fails, so the Editor can diagnose it.
    g_packageDirectoryWide = packageDirectoryWide;
    g_packageDirectoryUtf8 = packageDirectoryUtf8;

    // A prior package lookup may have cached the then-highest generation. An explicit Editor
    // refresh must re-enumerate the directory so newly published package generations appear.
    package_reader::release_caches();
    package_reader::invalidate_locations();

    std::vector<AssetRecord> newAssets{};
    std::vector<PackageRecord> newPackages{};
    BuildContext build{&newAssets, &newPackages};
    package_reader::CatalogScanResult result{};
    const bool scanned =
        package_reader::scan_entries(packageDirectoryWide, &visit_entry, &build, result);
    package_reader::release_caches();

    if (!scanned) {
        const std::uint64_t elapsed = GetTickCount64() - started;
        g_status = RefreshStatus::scanFailed;
        log_failure(result, elapsed);
        return false;
    }

    // The visitor is one-to-one with scan entries, and every non-empty package contributes one
    // package row. Refuse to publish a catalog if those invariants ever stop matching.
    if (result.entries != newAssets.size() || result.packages != newPackages.size()) {
        const std::uint64_t elapsed = GetTickCount64() - started;
        g_status = RefreshStatus::scanFailed;
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=editor_catalog stage=publish result=fail failure=count_mismatch packages=%zu "
            "package_rows=%zu entries=%zu asset_rows=%zu ms=%llu",
            result.packages,
            newPackages.size(),
            result.entries,
            newAssets.size(),
            static_cast<unsigned long long>(elapsed));
        if (written > 0) {
            const std::size_t length =
                (std::min)(static_cast<std::size_t>(written), line.size() - 1U);
            core::log::write(
                core::log::Channel::client, core::log::Level::error, {line.data(), length});
        }
        return false;
    }

    std::sort(
        newAssets.begin(), newAssets.end(), [](const AssetRecord& left, const AssetRecord& right) {
            return left.tag < right.tag;
        });
    Classification classification = classify(newAssets);
    const std::uint64_t scanMilliseconds = GetTickCount64() - started;

    g_assets = std::move(newAssets);
    g_packages = std::move(newPackages);
    g_types = std::move(classification.types);
    g_schemaClasses = std::move(classification.schemaClasses);
    g_kinds = std::move(classification.kinds);
    g_stats = Stats{result.packages,
                    result.entries,
                    classification.schemaEntries,
                    classification.emptyEntries,
                    classification.textureHeaders,
                    classification.textureDataEntries,
                    scanMilliseconds};
    ++g_generation;
    g_status = RefreshStatus::ready;

    log_success(result,
                g_packages.size(),
                g_assets.size(),
                g_types.size(),
                g_schemaClasses.size(),
                g_stats.schemaEntries,
                g_stats.emptyEntries,
                scanMilliseconds,
                g_generation);
    return true;
}

const std::vector<AssetRecord>& assets() noexcept {
    return g_assets;
}

const std::vector<PackageRecord>& packages() noexcept {
    return g_packages;
}

const std::vector<TypeRecord>& types() noexcept {
    return g_types;
}

const std::vector<KindRecord>& kinds() noexcept {
    return g_kinds;
}

const std::vector<SchemaClassRecord>& schema_classes() noexcept {
    return g_schemaClasses;
}

Stats stats() noexcept {
    return g_stats;
}

RefreshStatus status() noexcept {
    return g_status;
}

std::uint64_t generation() noexcept {
    return g_generation;
}

std::string_view package_directory() noexcept {
    return g_packageDirectoryUtf8;
}

std::wstring_view package_directory_wide() noexcept {
    return g_packageDirectoryWide;
}

const AssetRecord* find_asset(std::uint32_t tag) noexcept {
    const auto found = std::lower_bound(
        g_assets.begin(), g_assets.end(), tag, [](const AssetRecord& asset, std::uint32_t wanted) {
            return asset.tag < wanted;
        });
    if (found == g_assets.end() || found->tag != tag) {
        return nullptr;
    }
    return &*found;
}

const PackageRecord* package_for(const AssetRecord& asset) noexcept {
    if (asset.packageIndex >= g_packages.size()) {
        return nullptr;
    }
    return &g_packages[asset.packageIndex];
}

} // namespace sunrise::client::editor::catalog
