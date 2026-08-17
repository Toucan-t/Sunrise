#include "inventory_panel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string_view>

#include <imgui.h>

#include "../../content/items/packages/build.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/account/inventory/inventory_state.h"
#include "../../../state/account/inventory/item_name_catalog.h"
#include "../../../state/build_data/runtime.h"
#include "../../../state/runtime/runtime.h"

namespace sunrise::client::ui::inventory {
namespace {

constexpr std::array<const char*, 3> kClassNames{"Titan", "Hunter", "Warlock"};
constexpr std::size_t kNoEquipmentEditorSlot = (std::numeric_limits<std::size_t>::max)();
constexpr std::size_t kVisibleItemLimit = 200;

std::size_t g_selectedCharacterRow{};
std::uint64_t g_selectedCharacterSoid{};
std::size_t g_selectedEquipmentSlot{};
std::uint64_t g_equipmentEditorSoid{};
std::size_t g_equipmentEditorSlot{kNoEquipmentEditorSlot};
std::uint32_t g_pendingItemHash{};
bool g_liveEquipment{true};
std::array<char, 128> g_itemSearch{};
std::uint64_t g_socketEditorInstanceSoid{};
std::uint8_t g_socketEditorLane{};
std::uint16_t g_socketPendingPlug{state::build_data::items::details::kUnavailableItemIndex};
std::array<char, 128> g_socketSearch{};
std::uint32_t g_profilePendingHash{};
std::int32_t g_profilePendingQuantity{1};
std::array<char, 128> g_profileSearch{};
std::array<state::build_data::items::socket_plugs::Member,
           state::build_data::items::kDefinitionCapacity>
    g_socketCandidates{};
std::array<state::build_data::items::Definition, state::build_data::items::kDefinitionCapacity>
    g_nativeItemDefinitions{};
std::size_t g_nativeItemCount{};
std::size_t g_nativeItemSourceCount{};
std::array<char, 256> g_message{};

[[nodiscard]] const char* class_name(state::CharacterClass value) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    return index < kClassNames.size() ? kClassNames[index] : "?";
}

/** Resets the item draft without discarding the search text. */
void invalidate_equipment_editor() noexcept {
    g_equipmentEditorSoid = 0;
    g_equipmentEditorSlot = kNoEquipmentEditorSlot;
    g_pendingItemHash = 0;
}

/** Clears the owned-item socket target when the roster/character selection changes. */
void invalidate_socket_editor() noexcept {
    g_socketEditorInstanceSoid = 0;
    g_socketEditorLane = 0;
    g_socketPendingPlug = state::build_data::items::details::kUnavailableItemIndex;
}

/** Selects one owned item for the shared debug socket editor. */
void select_socket_item(std::uint64_t instanceSoid) noexcept {
    if (g_socketEditorInstanceSoid != instanceSoid) {
        g_socketEditorInstanceSoid = instanceSoid;
        g_socketEditorLane = 0;
        g_socketPendingPlug = state::build_data::items::details::kUnavailableItemIndex;
        g_socketSearch.fill('\0');
    }
}

/** Keeps the inventory pages pointed at one valid runtime character as the roster changes. */
void sync_character_selection(const state::AccountState& account) noexcept {
    if (account.characterCount == 0) {
        g_selectedCharacterRow = 0;
        g_selectedCharacterSoid = 0;
        invalidate_equipment_editor();
        invalidate_socket_editor();
        return;
    }
    g_selectedCharacterRow = (std::min)(g_selectedCharacterRow, account.characterCount - 1U);
    const std::uint64_t selectedSoid = account.characters[g_selectedCharacterRow].soid;
    if (g_selectedCharacterSoid != selectedSoid) {
        g_selectedCharacterSoid = selectedSoid;
        invalidate_equipment_editor();
        invalidate_socket_editor();
    }
}

/** Draws a self-contained runtime character selector shared by both inventory pages. */
void draw_character_selector(const state::AccountState& account) noexcept {
    if (account.characterCount == 0) {
        ImGui::TextDisabled("No runtime characters are available.");
        return;
    }
    const state::CharacterState& selected = account.characters[g_selectedCharacterRow];
    std::array<char, 96> preview{};
    (void)std::snprintf(preview.data(),
                        preview.size(),
                        "%zu: %s  0x%016llX",
                        g_selectedCharacterRow + 1U,
                        class_name(selected.characterClass),
                        static_cast<unsigned long long>(selected.soid));
    ImGui::SetNextItemWidth(330.0F);
    if (ImGui::BeginCombo("Character", preview.data())) {
        for (std::size_t index = 0; index < account.characterCount; ++index) {
            const state::CharacterState& character = account.characters[index];
            std::array<char, 96> label{};
            (void)std::snprintf(label.data(),
                                label.size(),
                                "%zu: %s  0x%016llX%s",
                                index + 1U,
                                class_name(character.characterClass),
                                static_cast<unsigned long long>(character.soid),
                                character.selected ? "  [game selected]" : "");
            if (ImGui::Selectable(label.data(), index == g_selectedCharacterRow)) {
                g_selectedCharacterRow = index;
                g_selectedCharacterSoid = character.soid;
                invalidate_equipment_editor();
                invalidate_socket_editor();
            }
        }
        ImGui::EndCombo();
    }
}

/** Loads the raw item-hash draft when either the character or semantic slot changes. */
void sync_equipment_editor(const state::CharacterState& character) noexcept {
    g_selectedEquipmentSlot =
        (std::min)(g_selectedEquipmentSlot, character.equipment.slots.size() - 1U);
    if (g_equipmentEditorSoid == character.soid
        && g_equipmentEditorSlot == g_selectedEquipmentSlot) {
        return;
    }
    g_equipmentEditorSoid = character.soid;
    g_equipmentEditorSlot = g_selectedEquipmentSlot;
    const std::optional<state::account::inventory::Item>& item =
        character.equipment.slots[g_selectedEquipmentSlot];
    g_pendingItemHash = item.has_value() ? item->definitionHash : 0;
}

/** Writes a short equipment mutation result line below the catalogue controls. */
void set_equipment_message(const char* action, state::EquipmentMutationResult result) noexcept {
    (void)std::snprintf(g_message.data(),
                        g_message.size(),
                        "%s: %s",
                        action,
                        state::equipment_mutation_name(result));
}

/** Applies one replacement definition while preserving the occupied item's instance identity. */
void apply_equipment(state::account::inventory::EquipmentSlot slot,
                     std::uint32_t definitionHash,
                     const char* action) noexcept {
    const state::AccountState account = state::account_snapshot();
    const std::size_t slotIndex = static_cast<std::size_t>(slot);
    if (g_selectedCharacterRow >= account.characterCount
        || slotIndex >= state::account::inventory::kEquipmentSlotCount) {
        (void)std::snprintf(g_message.data(), g_message.size(), "%s: character/slot changed", action);
        return;
    }
    const std::optional<state::account::inventory::Item>& reference =
        account.characters[g_selectedCharacterRow].equipment.slots[slotIndex];
    if (!reference.has_value()) {
        set_equipment_message(action, state::EquipmentMutationResult::emptySlot);
        return;
    }
    if (!content::items::packages::ensure_editor_item_details(definitionHash,
                                                              reference->definitionHash)) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "%s: installed item could not be resolved for this equipment slot",
                            action);
        return;
    }
    const std::uint64_t characterSoid = account.characters[g_selectedCharacterRow].soid;
    const std::uint64_t instanceSoid = reference->instanceSoid;
    const state::EquipmentMutationResult result =
        state::update_equipment_definition(g_selectedCharacterRow, slot, definitionHash);
    if (result != state::EquipmentMutationResult::ok) {
        set_equipment_message(action, result);
        return;
    }
    invalidate_equipment_editor();
    const bool queued = server::bap::request_equipment_refresh(characterSoid, instanceSoid);
    const bool savedWithoutPeer = !queued && state::checkpoint_characters();
    (void)std::snprintf(
        g_message.data(),
        g_message.size(),
        "%s: runtime updated; %s",
        action,
        queued ? "item -> character Queuez refresh queued; checkpoint follows successful publish"
               : (savedWithoutPeer ? "no live peer; checkpoint + settings mirror saved"
                                   : "no live peer; checkpoint incomplete (see log)"));
}

/** Writes one shared inventory-action result and publishes its selected-character refresh. */
void apply_inventory_move(std::uint64_t instanceSoid, bool unequip) noexcept {
    std::uint64_t characterSoid = 0;
    const state::InventoryMutationResult result =
        unequip ? state::unequip_inventory_item(instanceSoid, characterSoid)
                : state::equip_inventory_item(instanceSoid, characterSoid);
    if (result != state::InventoryMutationResult::ok) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "%s: %s",
                            unequip ? "unequip" : "equip",
                            state::inventory_mutation_name(result));
        return;
    }
    const bool queued = server::bap::request_character_refresh(characterSoid);
    const bool savedWithoutPeer = !queued && state::checkpoint_characters();
    (void)std::snprintf(
        g_message.data(),
        g_message.size(),
        "%s: runtime updated; %s",
        unequip ? "unequip" : "equip",
        queued ? "Family-4 character + roster/banner refresh queued"
               : (savedWithoutPeer ? "no live peer; checkpoint + settings mirror saved"
                                   : "no live peer; checkpoint incomplete (see log)"));
}

/** Creates one new unequipped test instance using the currently selected slot as its contract. */
void add_debug_unequipped(state::account::inventory::EquipmentSlot slot,
                          std::uint32_t definitionHash) noexcept {
    const state::AccountState account = state::account_snapshot();
    const std::size_t slotIndex = static_cast<std::size_t>(slot);
    if (g_selectedCharacterRow >= account.characterCount
        || slotIndex >= state::account::inventory::kEquipmentSlotCount) {
        (void)std::snprintf(g_message.data(), g_message.size(), "debug add: character/slot changed");
        return;
    }
    const state::CharacterState& character = account.characters[g_selectedCharacterRow];
    const std::optional<state::account::inventory::Item>& reference =
        character.equipment.slots[slotIndex];
    if (!reference.has_value()) {
        (void)std::snprintf(g_message.data(), g_message.size(), "debug add: reference slot is empty");
        return;
    }
    if (!content::items::packages::ensure_editor_item_details(definitionHash,
                                                              reference->definitionHash)) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "debug add: installed item could not be resolved for this slot");
        return;
    }

    std::uint64_t createdInstanceSoid = 0;
    const state::InventoryMutationResult result = state::debug_add_unequipped_item(
        g_selectedCharacterRow, slot, definitionHash, createdInstanceSoid);
    if (result != state::InventoryMutationResult::ok) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "debug add: %s",
                            state::inventory_mutation_name(result));
        return;
    }
    const bool queued =
        server::bap::request_inventory_add_refresh(character.soid, createdInstanceSoid);
    const bool savedWithoutPeer = !queued && state::checkpoint_characters();
    (void)std::snprintf(
        g_message.data(),
        g_message.size(),
        "debug add: instance 0x%016llX; %s",
        static_cast<unsigned long long>(createdInstanceSoid),
        queued ? "item resident -> character refresh queued"
               : (savedWithoutPeer ? "no live peer; checkpoint + settings mirror saved"
                                   : "no live peer; checkpoint incomplete (see log)"));
}

/** ASCII-only case fold, sufficient for search while preserving UTF-8 bytes outside A-Z. */
[[nodiscard]] char lower_ascii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

/** @return True when `needle` occurs inside `text`, ignoring ASCII letter case. */
[[nodiscard]] bool contains_case_insensitive(std::string_view text, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > text.size()) {
        return false;
    }
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < needle.size(); ++index) {
            if (lower_ascii(text[start + index]) != lower_ascii(needle[index])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

/** Refreshes the editor copy of the immutable installed-build item table when it appears. */
[[nodiscard]] bool sync_native_item_catalog() noexcept {
    const std::size_t sourceCount = state::build_data::item_definition_count();
    if (sourceCount == 0 || sourceCount > g_nativeItemDefinitions.size()) {
        g_nativeItemCount = 0;
        g_nativeItemSourceCount = sourceCount;
        return false;
    }
    if (g_nativeItemSourceCount == sourceCount && g_nativeItemCount == sourceCount) {
        return true;
    }
    std::size_t copied = 0;
    if (!state::build_data::snapshot_item_definitions(g_nativeItemDefinitions, copied)
        || copied != sourceCount) {
        g_nativeItemCount = 0;
        g_nativeItemSourceCount = sourceCount;
        return false;
    }
    g_nativeItemCount = copied;
    g_nativeItemSourceCount = sourceCount;
    return true;
}

/** Native item search accepts optional name text, hash text, or native definition-index text. */
[[nodiscard]] bool native_option_matches(const state::build_data::items::Definition& definition,
                                         std::string_view name,
                                         std::string_view query) noexcept {
    if (query.empty() || (!name.empty() && contains_case_insensitive(name, query))) {
        return true;
    }
    std::array<char, 16> hashHex{};
    std::array<char, 16> hashDecimal{};
    std::array<char, 16> indexText{};
    (void)std::snprintf(hashHex.data(), hashHex.size(), "0x%08X", definition.definitionHash);
    (void)std::snprintf(hashDecimal.data(), hashDecimal.size(), "%u", definition.definitionHash);
    (void)std::snprintf(indexText.data(), indexText.size(), "%u", definition.definitionIndex);
    return contains_case_insensitive(hashHex.data(), query)
           || contains_case_insensitive(hashDecimal.data(), query)
           || contains_case_insensitive(indexText.data(), query);
}

/** @return Optional display name, falling back to one stable placeholder. */
[[nodiscard]] std::string_view item_name(std::uint32_t definitionHash) noexcept {
    const std::string_view name =
        state::account::inventory::item_names::name_for_hash(definitionHash);
    return name.empty() ? std::string_view{"Unknown item"} : name;
}

/** Resolves one installed definition to the main account-wide profile inventory. */
[[nodiscard]] bool profile_item_definition(
    const state::build_data::items::Definition& definition,
    state::build_data::inventory::buckets::Descriptor& bucket) noexcept {
    return definition.bucketId != state::build_data::items::kUnresolvedBucketId
           && state::build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
           && bucket.arraySelector
                  == state::build_data::inventory::buckets::ArraySelector::profile;
}

/** Seeds one account-wide stack, then republishes the account object to the live peer. */
void add_debug_profile_item(std::uint32_t definitionHash, std::int32_t quantity) noexcept {
    if (!content::items::packages::ensure_socket_plug_details(definitionHash)) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "profile add: installed item detail could not be resolved");
        return;
    }
    std::int64_t totalQuantity = 0;
    std::uint64_t createdInstanceSoid = 0;
    const state::ProfileInventoryMutationResult result =
        state::debug_add_profile_item(
            definitionHash, quantity, totalQuantity, createdInstanceSoid);
    if (result != state::ProfileInventoryMutationResult::ok) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "profile add: %s",
                            state::profile_inventory_mutation_name(result));
        return;
    }
    const bool queued = server::bap::request_profile_item_refresh(
        createdInstanceSoid, createdInstanceSoid != 0);
    const bool savedWithoutPeer = !queued && state::checkpoint_characters();
    (void)std::snprintf(
        g_message.data(),
        g_message.size(),
        "profile add: total=%lld source=0x%llX; %s",
        static_cast<long long>(totalQuantity),
        static_cast<unsigned long long>(createdInstanceSoid),
        queued ? "Family-4 account refresh queued; checkpoint follows successful publish"
               : (savedWithoutPeer ? "no live peer; profile_items + characters saved"
                                   : "no live peer; checkpoint incomplete (see log)"));
}

/** Draws current account-wide stacks plus a native profile-item debug seeder. */
void draw_profile_inventory(const state::AccountState& account) noexcept {
    ImGui::TextUnformatted("Profile Inventory");
    ImGui::TextWrapped("Account-wide stacks live here: shaders, mods, consumables, currencies and "
                       "materials. Debug Add/Increment publishes the account object immediately and "
                       "mirrors profile_items back into settings.json for restart persistence.");
    ImGui::Text("Rows: %zu / %zu", account.profileItemCount, account.profileItems.size());

    constexpr ImGuiTableFlags currentFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##profile_inventory_rows", 5, currentFlags)) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Hash");
        ImGui::TableSetupColumn("Bucket");
        ImGui::TableSetupColumn("Quantity");
        ImGui::TableSetupColumn("Serial");
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < account.profileItemCount; ++index) {
            const state::account::inventory::ProfileItem& item = account.profileItems[index];
            state::build_data::items::Definition definition{};
            const bool native =
                state::build_data::find_item_definition_hash(item.definitionHash, definition);
            const std::string_view name = item_name(item.definitionHash);
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%08X", item.definitionHash);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", native ? static_cast<unsigned>(definition.bucketId) : 0U);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d", item.quantity);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", item.mutationSerial);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(180.0F);
    (void)ImGui::InputScalar("Definition hash##profile_item",
                             ImGuiDataType_U32,
                             &g_profilePendingHash,
                             nullptr,
                             nullptr,
                             "%08X",
                             ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0F);
    (void)ImGui::InputScalar("Quantity##profile_item",
                             ImGuiDataType_S32,
                             &g_profilePendingQuantity,
                             nullptr,
                             nullptr,
                             "%d");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_profilePendingHash == 0 || g_profilePendingQuantity <= 0);
    if (ImGui::Button("Debug Add / Increment Profile Item")) {
        add_debug_profile_item(g_profilePendingHash, g_profilePendingQuantity);
    }
    ImGui::EndDisabled();

    const bool nativeCatalogueReady = sync_native_item_catalog();
    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("Search profile items", g_profileSearch.data(), g_profileSearch.size());
    const std::string_view query{g_profileSearch.data()};
    ImGui::TextDisabled("Tip: search for 'shader', a consumable name, hash, or definition index.");

    constexpr ImGuiTableFlags catalogFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_SizingStretchProp;
    if (nativeCatalogueReady
        && ImGui::BeginTable("##profile_item_catalog", 5, catalogFlags, ImVec2(0.0F, 260.0F))) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Index");
        ImGui::TableSetupColumn("Hash");
        ImGui::TableSetupColumn("Bucket");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        std::size_t visible = 0;
        for (std::size_t index = 0; index < g_nativeItemCount && visible < kVisibleItemLimit;
             ++index) {
            const state::build_data::items::Definition& definition = g_nativeItemDefinitions[index];
            state::build_data::inventory::buckets::Descriptor bucket{};
            if (!profile_item_definition(definition, bucket)) {
                continue;
            }
            const std::string_view name = item_name(definition.definitionHash);
            if (!native_option_matches(definition, name, query)) {
                continue;
            }
            ++visible;
            ImGui::PushID(static_cast<int>(definition.definitionIndex));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(definition.definitionIndex));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%08X", definition.definitionHash);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u (%u slots)",
                        static_cast<unsigned>(definition.bucketId),
                        static_cast<unsigned>(bucket.slotCount));
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Select")) {
                g_profilePendingHash = definition.definitionHash;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    } else if (!nativeCatalogueReady) {
        ImGui::TextDisabled("Installed native item catalogue is not ready yet.");
    }
    ImGui::Separator();
}

/** Locates one owned instance on the selected character and reports whether it is equipped. */
[[nodiscard]] const state::account::inventory::Item*
find_owned_item(const state::CharacterState& character,
                std::uint64_t instanceSoid,
                bool& equipped) noexcept {
    equipped = false;
    if (instanceSoid == 0) {
        return nullptr;
    }
    for (const auto& item : character.equipment.slots) {
        if (item.has_value() && item->instanceSoid == instanceSoid) {
            equipped = true;
            return &*item;
        }
    }
    for (std::size_t index = 0; index < character.inventory.count; ++index) {
        if (character.inventory.values[index].instanceSoid == instanceSoid) {
            return &character.inventory.values[index];
        }
    }
    return nullptr;
}

/** Resolves the effective current native plug index for one authored/native-default socket lane. */
[[nodiscard]] std::uint16_t current_plug_index(
    const state::account::inventory::Item& item,
    const state::build_data::items::details::Definition& detail,
    std::uint8_t lane) noexcept {
    using namespace state::account::inventory;
    namespace detail_domain = state::build_data::items::details;
    if (lane >= detail.ordinarySocketCount || lane >= kPlugCapacity) {
        return detail_domain::kUnavailableItemIndex;
    }
    if (item.sockets.policy == SocketPolicy::nativeDefaults) {
        return detail.initialPlugIndices[lane];
    }
    if (item.sockets.policy != SocketPolicy::authored || lane >= item.sockets.plugCount
        || !item.sockets.plugs[lane].has_value()) {
        return detail_domain::kUnavailableItemIndex;
    }
    state::build_data::items::Definition plug{};
    return state::build_data::find_item_definition_hash(*item.sockets.plugs[lane], plug)
               ? plug.definitionIndex
               : detail_domain::kUnavailableItemIndex;
}

/** Gives the editor a best-effort semantic hint without using it as compatibility authority. */
[[nodiscard]] const char* socket_hint(
    std::span<const state::build_data::items::socket_plugs::Member> candidates) noexcept {
    std::size_t shaders = 0;
    std::size_t mods = 0;
    std::size_t ornaments = 0;
    for (const auto candidate : candidates) {
        state::build_data::items::Definition definition{};
        if (!state::build_data::find_item_definition_index(candidate, definition)) {
            continue;
        }
        shaders += static_cast<std::size_t>(definition.bucketId == 14U);
        mods += static_cast<std::size_t>(definition.bucketId == 13U);
        const std::string_view name =
            state::account::inventory::item_names::name_for_hash(definition.definitionHash);
        ornaments += static_cast<std::size_t>(contains_case_insensitive(name, "ornament"));
    }
    if (ornaments != 0) {
        return "Ornament / cosmetic";
    }
    if (!candidates.empty() && shaders == candidates.size()) {
        return "Shader";
    }
    if (!candidates.empty() && mods == candidates.size()) {
        return "Mod";
    }
    return "Ordinary socket";
}

/** Applies one debug socket choice through the same State transaction used by opcodes 903/1901. */
void apply_socket_choice(const state::account::inventory::Item& target,
                         std::uint8_t lane,
                         std::uint16_t plugDefinitionIndex) noexcept {
    state::build_data::items::Definition plug{};
    if (!state::build_data::find_item_definition_index(plugDefinitionIndex, plug)
        || !content::items::packages::ensure_socket_plug_details(plug.definitionHash)) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "socket: selected plug details could not be resolved");
        return;
    }
    std::uint64_t characterSoid = 0;
    bool equipped = false;
    const state::SocketMutationResult result = state::apply_socket_plug(
        target.instanceSoid, lane, plugDefinitionIndex, characterSoid, equipped);
    if (result != state::SocketMutationResult::ok) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "socket: %s",
                            state::socket_mutation_name(result));
        return;
    }
    const bool queued =
        server::bap::request_socket_refresh(characterSoid, target.instanceSoid, equipped);
    const bool savedWithoutPeer = !queued && state::checkpoint_characters();
    (void)std::snprintf(
        g_message.data(),
        g_message.size(),
        "socket: runtime updated; %s",
        queued ? (equipped ? "item -> character + appearance mirrors queued"
                           : "item-instance refresh queued")
               : (savedWithoutPeer ? "no live peer; checkpoint + settings mirror saved"
                                   : "no live peer; checkpoint incomplete (see log)"));
}

/** Draws the shared exact-pool socket/perk/shader/ornament editor for one owned item. */
void draw_socket_editor(const state::CharacterState& character) noexcept {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Socket / Appearance Editor");
    ImGui::TextWrapped("Select Sockets on any owned item. Choices come from that exact installed "
                       "item/socket plug pool; this covers ordinary perks/mods and the cosmetic "
                       "shader/ornament lanes exposed by the Shadowkeep build.");

    bool equipped = false;
    const state::account::inventory::Item* target =
        find_owned_item(character, g_socketEditorInstanceSoid, equipped);
    if (target == nullptr) {
        ImGui::TextDisabled("Choose an item with its Sockets button above.");
        return;
    }

    state::build_data::items::Definition base{};
    state::build_data::items::details::Definition detail{};
    if (!state::build_data::find_item_definition_hash(target->definitionHash, base)
        || !state::build_data::find_configured_item_detail(base.definitionIndex, detail)
        || detail.ordinarySocketState
               != state::build_data::items::details::OrdinarySocketState::present
        || detail.ordinarySocketCount == 0) {
        ImGui::TextDisabled("This owned item has no resolved ordinary socket block.");
        return;
    }

    g_socketEditorLane = static_cast<std::uint8_t>(
        (std::min)(static_cast<std::size_t>(g_socketEditorLane),
                   static_cast<std::size_t>(detail.ordinarySocketCount - 1U)));
    std::array<char, 160> targetLabel{};
    const std::string_view targetName = item_name(target->definitionHash);
    (void)std::snprintf(targetLabel.data(),
                        targetLabel.size(),
                        "%.*s  0x%016llX%s",
                        static_cast<int>(targetName.size()),
                        targetName.data(),
                        static_cast<unsigned long long>(target->instanceSoid),
                        equipped ? "  [equipped]" : "");
    ImGui::TextUnformatted(targetLabel.data());

    const std::uint16_t effective = current_plug_index(*target, detail, g_socketEditorLane);
    if (g_socketPendingPlug == state::build_data::items::details::kUnavailableItemIndex) {
        g_socketPendingPlug = effective;
    }

    std::array<char, 80> lanePreview{};
    (void)std::snprintf(lanePreview.data(),
                        lanePreview.size(),
                        "Lane %u  type %u",
                        static_cast<unsigned>(g_socketEditorLane),
                        static_cast<unsigned>(detail.socketTypes[g_socketEditorLane]));
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::BeginCombo("Socket lane", lanePreview.data())) {
        for (std::uint8_t lane = 0; lane < detail.ordinarySocketCount; ++lane) {
            std::array<char, 80> label{};
            (void)std::snprintf(label.data(),
                                label.size(),
                                "Lane %u  type %u",
                                static_cast<unsigned>(lane),
                                static_cast<unsigned>(detail.socketTypes[lane]));
            if (ImGui::Selectable(label.data(), lane == g_socketEditorLane)) {
                g_socketEditorLane = lane;
                g_socketPendingPlug = current_plug_index(*target, detail, lane);
                g_socketSearch.fill('\0');
            }
        }
        ImGui::EndCombo();
    }

    std::size_t candidateCount = 0;
    const bool candidatesReady = state::build_data::socket_plug_candidates(
        base.definitionIndex, g_socketEditorLane, g_socketCandidates, candidateCount);
    const auto candidates = std::span(g_socketCandidates).first(candidateCount);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", candidatesReady ? socket_hint(candidates) : "pool unavailable");

    if (effective != state::build_data::items::details::kUnavailableItemIndex) {
        state::build_data::items::Definition current{};
        if (state::build_data::find_item_definition_index(effective, current)) {
            const std::string_view currentName = item_name(current.definitionHash);
            ImGui::Text("Current: %.*s  [index %u / 0x%08X]",
                        static_cast<int>(currentName.size()),
                        currentName.data(),
                        static_cast<unsigned>(effective),
                        current.definitionHash);
        }
    } else {
        ImGui::TextDisabled("Current: empty / unresolved");
    }

    if (!candidatesReady) {
        ImGui::TextDisabled("Installed socket compatibility rules are not ready yet.");
        return;
    }
    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("Search compatible plugs", g_socketSearch.data(), g_socketSearch.size());
    const std::string_view query{g_socketSearch.data()};

    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    std::size_t shown = 0;
    std::size_t matches = 0;
    const bool childVisible =
        ImGui::BeginChild("socket_candidate_list", ImVec2(0.0F, 230.0F), true);
    if (childVisible && ImGui::BeginTable("socket_candidate_table", 4, flags)) {
        ImGui::TableSetupColumn("Compatible plug");
        ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 105.0F);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 58.0F);
        ImGui::TableSetupColumn("Bucket", ImGuiTableColumnFlags_WidthFixed, 52.0F);
        ImGui::TableHeadersRow();
        for (const auto candidateIndex : candidates) {
            state::build_data::items::Definition candidate{};
            if (!state::build_data::find_item_definition_index(candidateIndex, candidate)) {
                continue;
            }
            const std::string_view name =
                state::account::inventory::item_names::name_for_hash(candidate.definitionHash);
            if (!native_option_matches(candidate, name, query)) {
                continue;
            }
            ++matches;
            if (shown >= kVisibleItemLimit) {
                continue;
            }
            ++shown;
            ImGui::PushID(static_cast<int>(candidateIndex));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const char* label = name.empty() ? "<unnamed compatible plug>" : name.data();
            if (ImGui::Selectable(label,
                                  g_socketPendingPlug == candidateIndex,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                g_socketPendingPlug = candidateIndex;
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%08X", candidate.definitionHash);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", static_cast<unsigned>(candidateIndex));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", static_cast<unsigned>(candidate.bucketId));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    if (matches > shown) {
        ImGui::TextDisabled("Showing %zu of %zu compatible matches; narrow the search.", shown, matches);
    } else if (matches == 0) {
        ImGui::TextDisabled("No compatible plugs match this search.");
    }

    const bool canApply = g_socketPendingPlug
                              != state::build_data::items::details::kUnavailableItemIndex
                          && g_socketPendingPlug != effective;
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("Apply Compatible Plug")) {
        apply_socket_choice(*target, g_socketEditorLane, g_socketPendingPlug);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Only package-declared compatible plugs can be applied.");
}

/** Draws all semantic equipment slots without exposing catalogue controls. */
void draw_equipment_table(const state::CharacterState& character) noexcept {
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("inventory_equipment", 9, flags)) {
        return;
    }
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Item");
    ImGui::TableSetupColumn("Definition", ImGuiTableColumnFlags_WidthFixed, 104.0F);
    ImGui::TableSetupColumn("Instance");
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 48.0F);
    ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 42.0F);
    ImGui::TableSetupColumn("Serial", ImGuiTableColumnFlags_WidthFixed, 52.0F);
    ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 64.0F);
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 138.0F);
    ImGui::TableHeadersRow();
    for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
        const auto semantic = static_cast<state::account::inventory::EquipmentSlot>(slot);
        const std::optional<state::account::inventory::Item>& item = character.equipment.slots[slot];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const std::string_view slotName = state::account::inventory::slot_name(semantic);
        ImGui::TextUnformatted(slotName.data(), slotName.data() + slotName.size());
        ImGui::TableSetColumnIndex(1);
        if (item.has_value()) {
            const std::string_view name = item_name(item->definitionHash);
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
        } else {
            ImGui::TextDisabled("empty");
        }
        ImGui::TableSetColumnIndex(2);
        item.has_value() ? ImGui::Text("0x%08X", item->definitionHash) : ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(3);
        item.has_value()
            ? ImGui::Text("0x%016llX", static_cast<unsigned long long>(item->instanceSoid))
            : ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(4);
        item.has_value() ? ImGui::Text("%d", item->level) : ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(5);
        item.has_value() ? ImGui::Text("%d", item->quantity) : ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(6);
        item.has_value() ? ImGui::Text("%d", item->mutationSerial) : ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(7);
        item.has_value() ? ImGui::Text("0x%X", item->flags) : ImGui::TextDisabled("-");
        ImGui::TableSetColumnIndex(8);
        if (item.has_value()) {
            ImGui::PushID(static_cast<int>(slot));
            if (ImGui::SmallButton("Unequip")) {
                apply_inventory_move(item->instanceSoid, true);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Sockets")) {
                select_socket_item(item->instanceSoid);
            }
            ImGui::PopID();
        } else {
            ImGui::TextDisabled("-");
        }
    }
    ImGui::EndTable();
}

/** Draws the persisted dense unequipped prefix for the selected character. */
void draw_unequipped_table(const state::CharacterState& character) noexcept {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    const bool childVisible = ImGui::BeginChild("unequipped_inventory", ImVec2(0.0F, 300.0F), true);
    if (childVisible && ImGui::BeginTable("unequipped_inventory_table", 9, flags)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0F);
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("Definition", ImGuiTableColumnFlags_WidthFixed, 104.0F);
        ImGui::TableSetupColumn("Instance");
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 48.0F);
        ImGui::TableSetupColumn("Qty", ImGuiTableColumnFlags_WidthFixed, 42.0F);
        ImGui::TableSetupColumn("Serial", ImGuiTableColumnFlags_WidthFixed, 52.0F);
        ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 64.0F);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 128.0F);
        ImGui::TableHeadersRow();
        for (std::size_t index = 0; index < character.inventory.count; ++index) {
            const state::account::inventory::Item& item = character.inventory.values[index];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu", index + 1U);
            ImGui::TableSetColumnIndex(1);
            const std::string_view name = item_name(item.definitionHash);
            ImGui::TextUnformatted(name.data(), name.data() + name.size());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("0x%08X", item.definitionHash);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("0x%016llX", static_cast<unsigned long long>(item.instanceSoid));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", item.level);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", item.quantity);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%d", item.mutationSerial);
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("0x%X", item.flags);
            ImGui::TableSetColumnIndex(8);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::SmallButton("Equip")) {
                apply_inventory_move(item.instanceSoid, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Sockets")) {
                select_socket_item(item.instanceSoid);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

/** Draws semantic slot selection used to constrain the native catalogue to a proven bucket. */
void draw_catalog_slot_selector(const state::CharacterState& character) noexcept {
    sync_equipment_editor(character);
    const auto selectedSlot =
        static_cast<state::account::inventory::EquipmentSlot>(g_selectedEquipmentSlot);
    const std::string_view selectedSlotName = state::account::inventory::slot_name(selectedSlot);
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::BeginCombo("Equipment slot", selectedSlotName.data())) {
        for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
            const auto semantic = static_cast<state::account::inventory::EquipmentSlot>(slot);
            const std::string_view slotName = state::account::inventory::slot_name(semantic);
            const bool occupied = character.equipment.slots[slot].has_value();
            std::array<char, 64> label{};
            (void)std::snprintf(label.data(),
                                label.size(),
                                "%.*s%s",
                                static_cast<int>(slotName.size()),
                                slotName.data(),
                                occupied ? "" : "  [empty]");
            if (ImGui::Selectable(label.data(), slot == g_selectedEquipmentSlot)) {
                g_selectedEquipmentSlot = slot;
                invalidate_equipment_editor();
            }
        }
        ImGui::EndCombo();
    }
    sync_equipment_editor(character);
}

} // namespace

/** Draws equipped and unequipped owned items for one runtime character. */
void draw_inventory() noexcept {
    state::AccountState account = state::account_snapshot();
    sync_character_selection(account);

    ImGui::TextUnformatted("Inventory");
    ImGui::Separator();
    ImGui::TextWrapped("Runtime AccountState now distinguishes equipped items from a dense owned "
                       "unequipped inventory. characters.dat v4 persists both, and full Family-4 "
                       "snapshots publish every owned item instance resident.");
    ImGui::Spacing();
    draw_profile_inventory(account);
    ImGui::Spacing();
    draw_character_selector(account);
    if (account.characterCount == 0 || g_selectedCharacterRow >= account.characterCount) {
        return;
    }

    const state::CharacterState& character = account.characters[g_selectedCharacterRow];
    std::size_t equippedCount = 0;
    for (const auto& item : character.equipment.slots) {
        equippedCount += static_cast<std::size_t>(item.has_value());
    }
    ImGui::Text("Equipped: %zu / %zu", equippedCount, character.equipment.slots.size());
    ImGui::SameLine();
    ImGui::Text("Unequipped: %zu / %zu", character.inventory.count, character.inventory.values.size());
    ImGui::SameLine();
    ImGui::Text("Next serial: %u", character.nextInventorySerial);

    ImGui::Spacing();
    ImGui::TextUnformatted("Equipped");
    draw_equipment_table(character);

    ImGui::Spacing();
    ImGui::TextUnformatted("Unequipped owned items");
    if (character.inventory.count == 0) {
        ImGui::TextDisabled("No unequipped items are persisted on this character yet.");
    } else {
        draw_unequipped_table(character);
    }
    ImGui::TextWrapped("Equip/Unequip uses the same State transition as native Web Service "
                       "opcodes 403/404. These buttons are useful for separating inventory-state "
                       "problems from request-decoding problems while testing.");
    draw_socket_editor(character);
}

/** Draws the native installed item catalogue and the existing safe replacement action. */
void draw_item_catalog() noexcept {
    state::AccountState account = state::account_snapshot();
    sync_character_selection(account);

    ImGui::TextUnformatted("Item Catalog");
    ImGui::Separator();
    ImGui::TextWrapped("Browse the installed Destiny package item table and replace an already "
                       "occupied equipment slot without changing its instance SOID. The currently "
                       "equipped item supplies the proven native bucket/equipment-slot contract.");
    ImGui::Spacing();
    draw_character_selector(account);
    if (account.characterCount == 0 || g_selectedCharacterRow >= account.characterCount) {
        return;
    }

    const state::CharacterState& character = account.characters[g_selectedCharacterRow];
    draw_catalog_slot_selector(character);
    const auto selectedSlot =
        static_cast<state::account::inventory::EquipmentSlot>(g_selectedEquipmentSlot);
    const std::optional<state::account::inventory::Item>& selectedItem =
        character.equipment.slots[g_selectedEquipmentSlot];
    const std::string_view selectedSlotName = state::account::inventory::slot_name(selectedSlot);
    ImGui::Text("Selected slot: %.*s", static_cast<int>(selectedSlotName.size()), selectedSlotName.data());
    if (selectedItem.has_value()) {
        const std::string_view currentName = item_name(selectedItem->definitionHash);
        ImGui::Text("Current: %.*s  (0x%08X)",
                    static_cast<int>(currentName.size()),
                    currentName.data(),
                    selectedItem->definitionHash);
    } else {
        ImGui::TextDisabled("Current: empty; catalogue replacement requires an occupied reference slot.");
    }

    ImGui::Checkbox("Live apply item selection", &g_liveEquipment);
    ImGui::SetNextItemWidth(180.0F);
    (void)ImGui::InputScalar("Definition hash##catalog_item",
                             ImGuiDataType_U32,
                             &g_pendingItemHash,
                             nullptr,
                             nullptr,
                             "%08X",
                             ImGuiInputTextFlags_CharsHexadecimal);
    if (g_liveEquipment && selectedItem.has_value() && ImGui::IsItemDeactivatedAfterEdit()) {
        apply_equipment(selectedSlot, g_pendingItemHash, "item hash");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedItem.has_value());
    if (ImGui::Button("Apply Item")) {
        apply_equipment(selectedSlot, g_pendingItemHash, "item");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedItem.has_value() || g_pendingItemHash == 0);
    if (ImGui::Button("Debug Add Unequipped")) {
        add_debug_unequipped(selectedSlot, g_pendingItemHash);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Debug Add Unequipped creates a new instance in the selected slot's native "
                       "bucket without replacing the equipped item. It is intended for inventory "
                       "switching tests until Collections acquisition is implemented.");

    const bool nativeCatalogueReady = sync_native_item_catalog();
    const state::account::inventory::item_names::Status names =
        state::account::inventory::item_names::status();

    state::build_data::items::Definition referenceDefinition{};
    const bool referenceDefinitionReady =
        selectedItem.has_value()
        && state::build_data::find_item_definition_hash(selectedItem->definitionHash,
                                                        referenceDefinition)
        && referenceDefinition.bucketId != state::build_data::items::kUnresolvedBucketId;
    state::build_data::inventory::buckets::Descriptor referenceBucket{};
    const bool referenceBucketReady =
        referenceDefinitionReady
        && state::build_data::find_inventory_bucket_descriptor(referenceDefinition.bucketId,
                                                               referenceBucket);

    ImGui::Spacing();
    ImGui::TextUnformatted("Installed item catalogue");
    ImGui::TextWrapped("The installed package item table is authoritative. items.js is optional "
                       "and only enriches rows with display names.");
    ImGui::Text("Native rows: %zu%s",
                state::build_data::item_definition_count(),
                nativeCatalogueReady ? "" : " (not ready)");
    if (names.loaded) {
        ImGui::Text("Optional names: %zu hashes (%zu old chooser rows ignored)",
                    names.nameCount,
                    names.optionCount);
    } else {
        ImGui::TextDisabled("Optional items.js names unavailable; hashes/indices remain browseable.");
    }
    if (referenceDefinitionReady) {
        ImGui::Text("Selected native bucket: %u  definition index: %u",
                    static_cast<unsigned>(referenceDefinition.bucketId),
                    static_cast<unsigned>(referenceDefinition.definitionIndex));
        if (referenceBucketReady) {
            ImGui::Text("Bucket routing: array %u  rows %u..%u (%u slots)",
                        static_cast<unsigned>(referenceBucket.arraySelector),
                        static_cast<unsigned>(referenceBucket.firstSlot),
                        static_cast<unsigned>(referenceBucket.firstSlot + referenceBucket.slotCount),
                        static_cast<unsigned>(referenceBucket.slotCount));
        }
    } else if (selectedItem.has_value()) {
        ImGui::TextDisabled("Current equipped definition is missing from the native item table.");
    }

    state::build_data::items::Definition pendingDefinition{};
    const bool pendingNative =
        state::build_data::find_item_definition_hash(g_pendingItemHash, pendingDefinition);
    state::build_data::items::details::Definition pendingDetail{};
    bool pendingDetailReady =
        pendingNative
        && state::build_data::find_configured_item_detail(pendingDefinition.definitionIndex,
                                                          pendingDetail);
    if (pendingNative) {
        ImGui::Text("Candidate native: index %u  bucket %u  detail %s",
                    static_cast<unsigned>(pendingDefinition.definitionIndex),
                    static_cast<unsigned>(pendingDefinition.bucketId),
                    pendingDetailReady ? "loaded" : "not loaded");
        if (pendingDetailReady) {
            ImGui::Text("Instance: %s  max stack: %d  native equip slot: %d  sockets: %u",
                        pendingDetail.instancedDefinitionState
                                == state::build_data::items::details::InstancedDefinitionState::instanced
                            ? "instanced"
                            : "stackable",
                        pendingDetail.maxStackSize,
                        pendingDetail.equipmentSlot.has_value()
                            ? static_cast<int>(*pendingDetail.equipmentSlot)
                            : -1,
                        static_cast<unsigned>(pendingDetail.ordinarySocketCount));
            ImGui::Text("Socket list: %u  stats: %u  sandbox perks: %u",
                        static_cast<unsigned>(pendingDetail.socketEntryListIndex),
                        static_cast<unsigned>(pendingDetail.statCount),
                        static_cast<unsigned>(pendingDetail.sandboxPerkCount));
        }
    } else if (g_pendingItemHash != 0) {
        ImGui::TextDisabled("Candidate hash is not a unique installed native item definition.");
    }

    const bool canResolve = selectedItem.has_value() && referenceDefinitionReady && pendingNative
                            && pendingDefinition.bucketId == referenceDefinition.bucketId;
    ImGui::BeginDisabled(!canResolve || pendingDetailReady);
    if (ImGui::Button("Resolve Native Details")) {
        const bool resolved = content::items::packages::ensure_editor_item_details(
            g_pendingItemHash, selectedItem->definitionHash);
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "native detail resolve: %s",
                            resolved ? "ok" : "failed (see log)");
        if (resolved) {
            pendingDetailReady = state::build_data::find_configured_item_detail(
                pendingDefinition.definitionIndex, pendingDetail);
        }
    }
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(300.0F);
    ImGui::InputText("Search installed items", g_itemSearch.data(), g_itemSearch.size());
    const std::string_view query{g_itemSearch.data()};

    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    std::size_t shown = 0;
    std::size_t matches = 0;
    if (nativeCatalogueReady && referenceDefinitionReady) {
        const bool childVisible =
            ImGui::BeginChild("native_item_catalogue", ImVec2(0.0F, 330.0F), true);
        if (childVisible && ImGui::BeginTable("native_item_catalogue_table", 5, flags)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 105.0F);
            ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 58.0F);
            ImGui::TableSetupColumn("Bucket", ImGuiTableColumnFlags_WidthFixed, 52.0F);
            ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthFixed, 54.0F);
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < g_nativeItemCount; ++index) {
                const state::build_data::items::Definition& definition = g_nativeItemDefinitions[index];
                if (definition.bucketId != referenceDefinition.bucketId) {
                    continue;
                }
                const std::string_view name =
                    state::account::inventory::item_names::name_for_hash(definition.definitionHash);
                if (!native_option_matches(definition, name, query)) {
                    continue;
                }
                ++matches;
                if (shown >= kVisibleItemLimit) {
                    continue;
                }
                ++shown;
                state::build_data::items::details::Definition detail{};
                const bool detailReady = state::build_data::find_configured_item_detail(
                    definition.definitionIndex, detail);
                ImGui::PushID(static_cast<int>(definition.definitionIndex));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const char* label = name.empty() ? "<unnamed installed item>" : name.data();
                if (ImGui::Selectable(label,
                                      g_pendingItemHash == definition.definitionHash,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    g_pendingItemHash = definition.definitionHash;
                    if (g_liveEquipment && selectedItem.has_value()) {
                        apply_equipment(selectedSlot, definition.definitionHash, "native item");
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("0x%08X", definition.definitionHash);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%u", static_cast<unsigned>(definition.definitionIndex));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", static_cast<unsigned>(definition.bucketId));
                ImGui::TableSetColumnIndex(4);
                detailReady ? ImGui::TextUnformatted("loaded") : ImGui::TextDisabled("lazy");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }
    if (!nativeCatalogueReady) {
        ImGui::TextDisabled("Native installed item definitions are not ready yet.");
    } else if (!referenceDefinitionReady) {
        ImGui::TextDisabled("Select an occupied slot with a resolved native bucket to browse replacements.");
    } else if (matches == 0) {
        ImGui::TextDisabled("No installed native items match this bucket/search.");
    } else if (matches > shown) {
        ImGui::TextDisabled("Showing %zu of %zu native matches; narrow the search to see the rest.",
                            shown,
                            matches);
    }

    if (g_message[0] != '\0') {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", g_message.data());
    }
}

} // namespace sunrise::client::ui::inventory
