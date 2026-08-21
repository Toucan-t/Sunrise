#include "editor_panel.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "../../editor/catalog/editor_asset_catalog.h"
#include "../../editor/editor_state.h"
#include "../../editor/textures/editor_texture_preview.h"
#include "../../hooks/graphics/renderer/state.h"

namespace sunrise::client::ui::editor {
namespace {

namespace editor_core = sunrise::client::editor;
namespace catalog = sunrise::client::editor::catalog;
namespace texture_preview = sunrise::client::editor::textures;
namespace renderer = sunrise::client::hooks::graphics::renderer;

constexpr std::size_t kSearchCapacity = 128;
constexpr std::size_t kOverviewTypeRows = 20;
constexpr std::size_t kOverviewSchemaRows = 20;
constexpr std::uint64_t kInvalidFilterGeneration = (std::numeric_limits<std::uint64_t>::max)();

std::array<char, kSearchCapacity> g_search{};
std::string g_appliedSearch{};
std::vector<std::size_t> g_filteredAssets{};
std::uint64_t g_filterGeneration{kInvalidFilterGeneration};
bool g_showEmpty{};
bool g_textureOnly{};
bool g_appliedShowEmpty{};
bool g_appliedTextureOnly{};
std::array<char, kSearchCapacity> g_textureSearch{};
std::string g_appliedTextureSearch{};
std::vector<std::size_t> g_filteredTextures{};
std::uint64_t g_textureFilterGeneration{kInvalidFilterGeneration};

/** Lowercases ASCII search text without introducing locale-dependent behavior. */
[[nodiscard]] std::string ascii_lower(std::string_view text) {
    std::string result(text);
    for (char& value : result) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return result;
}

/** @return Search text without a leading 0x for numeric comparisons. */
[[nodiscard]] std::string_view numeric_query(std::string_view query) noexcept {
    if (query.size() > 2 && query[0] == '0' && query[1] == 'x') {
        return query.substr(2);
    }
    return query;
}

/** Tests one fixed-width hexadecimal value against a lowercase query. */
[[nodiscard]] bool hex_matches(std::uint32_t value, std::string_view query) noexcept {
    std::array<char, 9> text{};
    (void)std::snprintf(text.data(), text.size(), "%08x", static_cast<unsigned>(value));
    return std::string_view(text.data()).find(query) != std::string_view::npos;
}

/** Tests one decimal value against a query. */
[[nodiscard]] bool decimal_matches(std::uint32_t value, std::string_view query) noexcept {
    std::array<char, 16> text{};
    (void)std::snprintf(text.data(), text.size(), "%u", static_cast<unsigned>(value));
    return std::string_view(text.data()).find(query) != std::string_view::npos;
}

/** Tests the conventional decimal file-type.subtype form, for example 32.1. */
[[nodiscard]] bool type_pair_matches(const catalog::AssetRecord& asset,
                                     std::string_view query) noexcept {
    std::array<char, 16> text{};
    (void)std::snprintf(text.data(),
                        text.size(),
                        "%u.%u",
                        static_cast<unsigned>(asset.fileType),
                        static_cast<unsigned>(asset.fileSubtype));
    return std::string_view(text.data()).find(query) != std::string_view::npos;
}

/** @return True when one asset matches the current cross-field search text. */
[[nodiscard]] bool asset_matches(const catalog::AssetRecord& asset,
                                 std::string_view query) noexcept {
    if (query.empty()) {
        return true;
    }
    const catalog::PackageRecord* package = catalog::package_for(asset);
    if (package != nullptr && package->searchFamily.find(query) != std::string::npos) {
        return true;
    }
    if (query == "schema" && catalog::is_schema_entry(asset)) {
        return true;
    }
    if (query == "empty" && catalog::is_empty_entry(asset)) {
        return true;
    }
    const std::string_view kindName = catalog::asset_kind_name(asset.kind);
    if (ascii_lower(kindName).find(query) != std::string::npos) {
        return true;
    }

    const std::string_view numberQuery = numeric_query(query);
    return hex_matches(asset.tag, numberQuery) || hex_matches(asset.reference, numberQuery)
           || hex_matches(asset.typeInfo, numberQuery)
           || hex_matches(asset.selectorBits, numberQuery)
           || type_pair_matches(asset, query)
           || decimal_matches(asset.entryIndex, query)
           || decimal_matches(asset.generation, query);
}

/** Rebuilds the filtered row index only when search text or the backing catalog changes. */
void rebuild_filter() {
    const std::string query = ascii_lower(g_search.data());
    const std::uint64_t generation = catalog::generation();
    if (query == g_appliedSearch && generation == g_filterGeneration
        && g_showEmpty == g_appliedShowEmpty && g_textureOnly == g_appliedTextureOnly) {
        return;
    }

    g_appliedSearch = query;
    g_filterGeneration = generation;
    g_appliedShowEmpty = g_showEmpty;
    g_appliedTextureOnly = g_textureOnly;
    g_filteredAssets.clear();
    const auto& assets = catalog::assets();
    g_filteredAssets.reserve(assets.size());
    for (std::size_t index = 0; index < assets.size(); ++index) {
        if ((!g_showEmpty && catalog::is_empty_entry(assets[index]))
            || (g_textureOnly && !catalog::is_texture_header(assets[index]))) {
            continue;
        }
        if (asset_matches(assets[index], query)) {
            g_filteredAssets.push_back(index);
        }
    }
}

/** Formats one byte size for compact table display. */
void format_size(std::uint32_t bytes, std::array<char, 32>& output) noexcept {
    constexpr float kKibibyte = 1024.0F;
    constexpr float kMebibyte = 1024.0F * 1024.0F;
    if (bytes >= static_cast<std::uint32_t>(kMebibyte)) {
        (void)std::snprintf(output.data(), output.size(), "%.2f MiB", bytes / kMebibyte);
    } else if (bytes >= static_cast<std::uint32_t>(kKibibyte)) {
        (void)std::snprintf(output.data(), output.size(), "%.1f KiB", bytes / kKibibyte);
    } else {
        (void)std::snprintf(output.data(), output.size(), "%u B", static_cast<unsigned>(bytes));
    }
}

/** Copies one 32-bit value in the editor's canonical 0xXXXXXXXX format. */
void copy_hex32(std::uint32_t value) noexcept {
    std::array<char, 16> text{};
    (void)std::snprintf(text.data(), text.size(), "0x%08X", static_cast<unsigned>(value));
    ImGui::SetClipboardText(text.data());
}

/** Refreshes the disk catalog and removes a selection that no longer resolves. */
void refresh_catalog() noexcept {
    const bool refreshed = catalog::refresh();
    g_filterGeneration = kInvalidFilterGeneration;
    g_textureFilterGeneration = kInvalidFilterGeneration;
    if (!refreshed) {
        return;
    }
    const editor_core::Selection selection = editor_core::state().selection;
    if (selection.valid && catalog::find_asset(selection.tag) == nullptr) {
        editor_core::clear_selection();
    }
}

/**
 * Fixed catalog-row height shared by ImGui tables and ImGuiListClipper.
 * Keeping both sides on the same explicit value prevents large-table scroll drift.
 */
[[nodiscard]] float catalog_row_height() noexcept {
    return ImGui::GetTextLineHeightWithSpacing();
}

/** Draws catalog state shared by the overview and empty asset/package tabs. */
void draw_catalog_status() noexcept {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Catalog");
    ImGui::SameLine();
    switch (catalog::status()) {
        case catalog::RefreshStatus::neverRun:
            ImGui::TextDisabled("Not scanned");
            break;
        case catalog::RefreshStatus::ready:
            ImGui::TextUnformatted("Ready");
            break;
        case catalog::RefreshStatus::packageDirectoryUnavailable:
            ImGui::TextUnformatted("Package directory unavailable");
            break;
        case catalog::RefreshStatus::scanFailed:
            ImGui::TextUnformatted("Scan failed");
            break;
    }

    const std::string_view directory = catalog::package_directory();
    if (!directory.empty()) {
        ImGui::TextWrapped("Packages: %.*s", static_cast<int>(directory.size()), directory.data());
    }
}

/** Draws the Editor foundation summary and read-only package safety state. */
void draw_overview() noexcept {
    editor_core::State& state = editor_core::state();

    ImGui::TextUnformatted("Editor Mode");
    ImGui::Separator();
    ImGui::TextWrapped("The editor state is persistent outside this page. World/UI picking and "
                       "runtime manipulation are added by later editor patches.");
    ImGui::Spacing();

    if (ImGui::Button(state.editMode ? "Disable Edit Mode" : "Enable Edit Mode")) {
        editor_core::set_edit_mode(!state.editMode);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(state.editMode ? "Enabled (foundation only)" : "Disabled");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Package Safety");
    ImGui::Separator();
    ImGui::TextWrapped("Installed Destiny packages are indexed as read-only inputs. This patch "
                       "contains no package writer and never opens a .pkg for write access.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Source policy: DESTINY / READ ONLY");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Asset Catalog");
    ImGui::Separator();
    draw_catalog_status();
    ImGui::Spacing();
    if (ImGui::Button("Refresh Asset Catalog")) {
        refresh_catalog();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Reads headers + entry tables only");

    if (catalog::status() != catalog::RefreshStatus::ready) {
        return;
    }

    const catalog::Stats stats = catalog::stats();
    ImGui::Spacing();
    ImGui::Text("Package ids scanned: %zu", stats.scannedPackages);
    ImGui::Text("Entries indexed: %zu", stats.scannedEntries);
    ImGui::Text("File type/subtype pairs: %zu", catalog::types().size());
    ImGui::Text("Schema classes (types 8/16): %zu", catalog::schema_classes().size());
    ImGui::Text("Schema entries (types 8/16): %zu", stats.schemaEntries);
    ImGui::Text("Empty 0.0 rows: %zu", stats.emptyEntries);
    ImGui::Text("Texture headers: %zu", stats.textureHeaders);
    ImGui::Text("Texture data rows: %zu", stats.textureDataEntries);
    ImGui::Text("Last scan: %llu ms", static_cast<unsigned long long>(stats.scanMilliseconds));

    ImGui::Spacing();
    ImGui::TextUnformatted("Largest file type/subtype groups");
    if (ImGui::BeginTable("##editor_type_summary",
                          3,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Subtype");
        ImGui::TableSetupColumn("Entries");
        ImGui::TableHeadersRow();
        const auto& types = catalog::types();
        const std::size_t count = (std::min)(types.size(), kOverviewTypeRows);
        for (std::size_t index = 0; index < count; ++index) {
            const catalog::TypeRecord& record = types[index];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", static_cast<unsigned>(record.fileType));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(record.fileSubtype));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", record.entries);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Largest schema classes (type 8 / type 16 only)");
    if (ImGui::BeginTable("##editor_schema_summary",
                          4,
                          ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg
                              | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Schema handle");
        ImGui::TableSetupColumn("Type 8");
        ImGui::TableSetupColumn("Type 16");
        ImGui::TableSetupColumn("Total");
        ImGui::TableHeadersRow();
        const auto& classes = catalog::schema_classes();
        const std::size_t count = (std::min)(classes.size(), kOverviewSchemaRows);
        for (std::size_t index = 0; index < count; ++index) {
            const catalog::SchemaClassRecord& record = classes[index];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%08X", static_cast<unsigned>(record.classId));
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", record.type8Entries);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%zu", record.type16Entries);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", record.entries);
        }
        ImGui::EndTable();
    }
}

/** Draws the selected asset's generic package and decoded entry-table metadata. */
void draw_asset_inspector() noexcept {
    const editor_core::Selection selection = editor_core::state().selection;
    if (!selection.valid) {
        ImGui::TextDisabled("Select an asset to inspect its package metadata.");
        return;
    }

    const catalog::AssetRecord* asset = catalog::find_asset(selection.tag);
    if (asset == nullptr) {
        ImGui::TextDisabled("The selected tag is not present in the current catalog.");
        return;
    }
    const catalog::PackageRecord* package = catalog::package_for(*asset);
    if (package == nullptr) {
        ImGui::TextUnformatted("Selected asset has an invalid package index.");
        return;
    }

    ImGui::Text("Tag: 0x%08X", static_cast<unsigned>(asset->tag));
    ImGui::Text("File type: %u", static_cast<unsigned>(asset->fileType));
    ImGui::Text("File subtype: %u", static_cast<unsigned>(asset->fileSubtype));
    ImGui::Text("Raw type info: 0x%08X", static_cast<unsigned>(asset->typeInfo));
    ImGui::Text("Selector bits: 0x%08X", static_cast<unsigned>(asset->selectorBits));
    ImGui::Text("Reference: 0x%08X", static_cast<unsigned>(asset->reference));
    if (catalog::is_schema_entry(*asset)) {
        ImGui::Text("Schema class: 0x%08X", static_cast<unsigned>(asset->reference));
    } else if (catalog::is_empty_entry(*asset)) {
        ImGui::TextUnformatted("Reference meaning: empty-row sentinel / family metadata");
    } else {
        ImGui::TextUnformatted("Schema class: n/a (reference meaning is family-specific)");
    }
    ImGui::Text("Package: %s", package->displayFamily.c_str());
    ImGui::Text("Package ID: 0x%04X", static_cast<unsigned>(asset->packageId));
    ImGui::Text("Generation: %u", static_cast<unsigned>(asset->generation));
    ImGui::Text("Entry: %u", static_cast<unsigned>(asset->entryIndex));
    ImGui::Text("Declared decoded size: %u bytes", static_cast<unsigned>(asset->declaredSize));
    ImGui::TextUnformatted("Source: DESTINY / READ ONLY");

    ImGui::Spacing();
    if (ImGui::Button("Copy Tag")) {
        copy_hex32(asset->tag);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Reference")) {
        copy_hex32(asset->reference);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Package")) {
        ImGui::SetClipboardText(package->displayFamily.c_str());
    }
}

/** Draws the searchable, clipped read-only entry catalog. */
void draw_assets() noexcept {
    draw_catalog_status();
    if (catalog::status() != catalog::RefreshStatus::ready) {
        ImGui::Spacing();
        if (ImGui::Button("Refresh Asset Catalog")) {
            refresh_catalog();
        }
        return;
    }

    ImGui::Spacing();
    (void)ImGui::InputTextWithHint(
        "##editor_asset_search",
        "Search tag, reference, type.subtype, type info, package, entry or generation",
        g_search.data(),
        g_search.size());
    ImGui::SameLine();
    ImGui::Checkbox("Show empty", &g_showEmpty);
    ImGui::SameLine();
    ImGui::Checkbox("Texture headers only", &g_textureOnly);
    rebuild_filter();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", g_filteredAssets.size(), catalog::assets().size());

    ImGui::Spacing();
    const float tableHeight = (std::max)(200.0F, ImGui::GetContentRegionAvail().y * 0.60F);
    const float rowHeight = catalog_row_height();
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                           | ImGuiTableFlags_Resizable
                                           | ImGuiTableFlags_ScrollY
                                           | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##editor_assets", 8, tableFlags, ImVec2(0.0F, tableHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed, 66.0F);
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, 94.0F);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 58.0F);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 118.0F);
        ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthFixed, 94.0F);
        ImGui::TableSetupColumn("Package");
        ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed, 62.0F);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 78.0F);
        ImGui::TableHeadersRow();

        const auto& assets = catalog::assets();
        const editor_core::Selection selection = editor_core::state().selection;
        ImGuiListClipper clipper{};
        clipper.Begin(static_cast<int>(g_filteredAssets.size()), rowHeight);
        while (clipper.Step()) {
            for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
                const std::size_t filteredIndex = static_cast<std::size_t>(visible);
                const std::size_t sourceIndex = g_filteredAssets[filteredIndex];
                const catalog::AssetRecord& asset = assets[sourceIndex];
                const catalog::PackageRecord* package = catalog::package_for(asset);
                if (package == nullptr) {
                    continue;
                }

                std::array<char, 16> tagText{};
                std::array<char, 16> typeText{};
                std::array<char, 16> referenceText{};
                std::array<char, 32> sizeText{};
                (void)std::snprintf(tagText.data(),
                                    tagText.size(),
                                    "%08X",
                                    static_cast<unsigned>(asset.tag));
                (void)std::snprintf(typeText.data(),
                                    typeText.size(),
                                    "%u.%u",
                                    static_cast<unsigned>(asset.fileType),
                                    static_cast<unsigned>(asset.fileSubtype));
                (void)std::snprintf(referenceText.data(),
                                    referenceText.size(),
                                    "%08X",
                                    static_cast<unsigned>(asset.reference));
                format_size(asset.declaredSize, sizeText);

                ImGui::PushID(static_cast<int>(asset.tag));
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%zu", sourceIndex);
                ImGui::TableSetColumnIndex(1);
                const bool selected = selection.valid && selection.tag == asset.tag;
                if (ImGui::Selectable(tagText.data(),
                                      selected,
                                      ImGuiSelectableFlags_SpanAllColumns,
                                      ImVec2(0.0F, rowHeight))) {
                    editor_core::select_asset(asset.tag);
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(typeText.data());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(catalog::asset_kind_name(asset.kind).data());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(referenceText.data());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(package->displayFamily.c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%u", static_cast<unsigned>(asset.entryIndex));
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted(sizeText.data());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled(
        "Row is the absolute sorted catalog index; gaps while search is active are expected.");
    ImGui::TextDisabled(
        "Reference is only a schema class for file types 8 and 16; other families reuse it.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Selection");
    ImGui::Separator();
    draw_asset_inspector();
}

void rebuild_texture_filter() {
    const std::string query = ascii_lower(g_textureSearch.data());
    const std::uint64_t generation = catalog::generation();
    if (query == g_appliedTextureSearch && generation == g_textureFilterGeneration) return;
    g_appliedTextureSearch = query;
    g_textureFilterGeneration = generation;
    g_filteredTextures.clear();
    const auto& assets = catalog::assets();
    for (std::size_t index = 0; index < assets.size(); ++index) {
        if (catalog::is_texture_header(assets[index]) && asset_matches(assets[index], query)) {
            g_filteredTextures.push_back(index);
        }
    }
}

void draw_textures() noexcept {
    draw_catalog_status();
    if (catalog::status() != catalog::RefreshStatus::ready) {
        ImGui::Spacing();
        if (ImGui::Button("Refresh Asset Catalog")) refresh_catalog();
        return;
    }
    texture_preview::begin_frame(catalog::generation());
    (void)ImGui::InputTextWithHint("##editor_texture_search",
                                   "Search texture tag, kind, package or entry",
                                   g_textureSearch.data(), g_textureSearch.size());
    rebuild_texture_filter();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu textures", g_filteredTextures.size());
    ImGui::TextDisabled("Previews are lazy: at most two new package-backed textures decode per frame.");

    constexpr float thumb = 96.0F;
    const float rowHeight = thumb + 10.0F;
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY
                                      | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##editor_textures", 6, flags, ImVec2(0.0F, 0.0F))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 108.0F);
    ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, 94.0F);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 100.0F);
    ImGui::TableSetupColumn("Metadata", ImGuiTableColumnFlags_WidthFixed, 210.0F);
    ImGui::TableSetupColumn("Package");
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 190.0F);
    ImGui::TableHeadersRow();

    const auto& assets = catalog::assets();
    const editor_core::Selection selection = editor_core::state().selection;
    ImGuiListClipper clipper{};
    clipper.Begin(static_cast<int>(g_filteredTextures.size()), rowHeight);
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const std::size_t sourceIndex = g_filteredTextures[static_cast<std::size_t>(visible)];
            const catalog::AssetRecord& asset = assets[sourceIndex];
            const catalog::PackageRecord* package = catalog::package_for(asset);
            if (package == nullptr) continue;
            const texture_preview::Preview& preview = texture_preview::request(
                renderer::g_resources.device, asset.tag, asset.reference);
            ImGui::PushID(static_cast<int>(asset.tag));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableSetColumnIndex(0);
            if (preview.status == texture_preview::PreviewStatus::ready && preview.view != nullptr) {
                ImGui::Image(reinterpret_cast<ImTextureID>(preview.view), ImVec2(thumb, thumb));
            } else {
                ImGui::Dummy(ImVec2(thumb, thumb));
            }
            ImGui::TableSetColumnIndex(1);
            std::array<char, 16> tagText{};
            (void)std::snprintf(tagText.data(), tagText.size(), "%08X", static_cast<unsigned>(asset.tag));
            if (ImGui::Selectable(tagText.data(), selection.valid && selection.tag == asset.tag,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                editor_core::select_asset(asset.tag);
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(catalog::asset_kind_name(asset.kind).data());
            ImGui::TableSetColumnIndex(3);
            if (preview.status == texture_preview::PreviewStatus::ready) {
                ImGui::Text("%ux%u  depth %u  array %u",
                            static_cast<unsigned>(preview.metadata.width),
                            static_cast<unsigned>(preview.metadata.height),
                            static_cast<unsigned>(preview.metadata.depth),
                            static_cast<unsigned>(preview.metadata.arraySize));
                const std::string_view format = texture_preview::format_name(preview.metadata.dxgiFormat);
                ImGui::Text("%.*s (%u)", static_cast<int>(format.size()), format.data(),
                            static_cast<unsigned>(preview.metadata.dxgiFormat));
                ImGui::Text("data 0x%08X", static_cast<unsigned>(preview.metadata.dataTag));
            } else {
                ImGui::TextDisabled("Header metadata loads with preview");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(package->displayFamily.c_str());
            ImGui::TableSetColumnIndex(5);
            const std::string_view status = texture_preview::status_name(preview.status);
            ImGui::TextWrapped("%.*s", static_cast<int>(status.size()), status.data());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

/** Draws one row per highest-generation package represented by the entry catalog. */
void draw_packages() noexcept {
    draw_catalog_status();
    if (catalog::status() != catalog::RefreshStatus::ready) {
        ImGui::Spacing();
        if (ImGui::Button("Refresh Asset Catalog")) {
            refresh_catalog();
        }
        return;
    }

    ImGui::Spacing();
    const float rowHeight = catalog_row_height();
    constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                           | ImGuiTableFlags_Resizable
                                           | ImGuiTableFlags_ScrollY
                                           | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##editor_packages", 6, tableFlags, ImVec2(0.0F, 0.0F))) {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed, 58.0F);
    ImGui::TableSetupColumn("Package");
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 62.0F);
    ImGui::TableSetupColumn("Gen", ImGuiTableColumnFlags_WidthFixed, 48.0F);
    ImGui::TableSetupColumn("Entries", ImGuiTableColumnFlags_WidthFixed, 70.0F);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 145.0F);
    ImGui::TableHeadersRow();

    const auto& packages = catalog::packages();
    ImGuiListClipper clipper{};
    clipper.Begin(static_cast<int>(packages.size()), rowHeight);
    while (clipper.Step()) {
        for (int visible = clipper.DisplayStart; visible < clipper.DisplayEnd; ++visible) {
            const std::size_t row = static_cast<std::size_t>(visible);
            const catalog::PackageRecord& package = packages[row];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", row);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(package.displayFamily.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%04X", static_cast<unsigned>(package.packageId));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", static_cast<unsigned>(package.generation));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%u", static_cast<unsigned>(package.entryCount));
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(package.origin == catalog::PackageOrigin::destinyReadOnly
                                       ? "DESTINY / READ ONLY"
                                       : "SUNRISE GENERATED");
        }
    }
    ImGui::EndTable();
}

} // namespace

/** Draws the Editor foundation workspace: state, asset catalog, and package catalog. */
void draw() noexcept {
    if (ImGui::BeginTabBar("##sunrise_editor_tabs")) {
        if (ImGui::BeginTabItem("Overview")) {
            draw_overview();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Assets")) {
            draw_assets();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            draw_textures();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Packages")) {
            draw_packages();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace sunrise::client::ui::editor
