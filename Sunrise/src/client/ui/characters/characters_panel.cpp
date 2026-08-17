#include "characters_panel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <imgui.h>

#include "../../hooks/network/investment/investment_derived_rebuild.h"
#include "../../../server/bap/runtime.h"
#include "../../../state/runtime/runtime.h"

namespace sunrise::client::ui::characters {
namespace {

constexpr std::array<const char*, 3> kRaceNames{"Human", "Awoken", "Exo"};
constexpr std::array<const char*, 2> kGenderNames{"Male", "Female"};
constexpr std::array<const char*, 3> kClassNames{"Titan", "Hunter", "Warlock"};

std::size_t g_selectedRow{};
std::uint64_t g_editorSoid{};
state::CharacterEdit g_edit{};
int g_createRace{};
int g_createGender{};
int g_createClass{};
bool g_liveCharacter{true};
bool g_confirmDelete{};
bool g_confirmReset{};
std::array<char, 256> g_message{};

/** @return Display name for one bounded enum. */
[[nodiscard]] const char* race_name(state::CharacterRace value) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    return index < kRaceNames.size() ? kRaceNames[index] : "?";
}

[[nodiscard]] const char* gender_name(state::CharacterGender value) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    return index < kGenderNames.size() ? kGenderNames[index] : "?";
}

[[nodiscard]] const char* class_name(state::CharacterClass value) noexcept {
    const std::size_t index = static_cast<std::size_t>(value);
    return index < kClassNames.size() ? kClassNames[index] : "?";
}

/** @return True when one opaque native creator/presentation block contains captured data. */
template <std::size_t Size>
[[nodiscard]] bool has_native_bytes(const std::array<std::byte, Size>& bytes) noexcept {
    return std::any_of(bytes.begin(), bytes.end(), [](std::byte value) {
        return value != std::byte{};
    });
}

/** Copies one runtime character into the editable scalar draft. */
void load_editor(const state::CharacterState& character) noexcept {
    g_editorSoid = character.soid;
    g_edit = {
        character.race,
        character.gender,
        character.characterClass,
        character.level,
        character.accepted,
        character.previewAvailable,
        character.appearanceValue,
        character.lastOrbitedDestination,
        character.contentBypass,
    };
    g_confirmDelete = false;
}

/** Writes a short persisted-character result line below the controls. */
void set_message(const char* action, state::CharacterMutationResult result) noexcept {
    (void)std::snprintf(g_message.data(),
                        g_message.size(),
                        "%s: %s",
                        action,
                        state::character_mutation_name(result));
}

/** Applies the current scalar draft and asks Queuez to rebuild live records when identities match. */
void apply_character_edit(const char* action) noexcept {
    const std::uint64_t editedSoid = g_editorSoid;
    bool classChanged = false;
    const state::CharacterMutationResult result =
        state::update_character(g_selectedRow, g_edit, classChanged);
    if (result != state::CharacterMutationResult::ok) {
        set_message(action, result);
        return;
    }

    g_editorSoid = 0;
    if (classChanged) {
        const bool saved = state::checkpoint_characters();
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "%s: runtime updated; %s; class gear remaps instance identities so "
                            "reselect/transition before expecting the native client to rebuild it",
                            action,
                            saved ? "checkpoint + settings mirror saved"
                                  : "checkpoint incomplete (see log)");
        return;
    }
    const bool queued = server::bap::request_character_refresh(editedSoid);
    const bool savedWithoutPeer = !queued && state::checkpoint_characters();
    (void)std::snprintf(
        g_message.data(),
        g_message.size(),
        "%s: runtime updated; %s",
        action,
        queued ? "targeted Family-4/3/0 refresh queued; checkpoint follows successful publish"
               : (savedWithoutPeer ? "no live peer; checkpoint + settings mirror saved"
                                   : "no live peer; checkpoint incomplete (see log)"));
}

/** Keeps the UI selection and edit draft synchronized with a changing dense character array. */
void sync_editor(const state::AccountState& account) noexcept {
    if (account.characterCount == 0) {
        g_selectedRow = 0;
        g_editorSoid = 0;
        g_confirmDelete = false;
        return;
    }
    g_selectedRow = (std::min)(g_selectedRow, account.characterCount - 1U);
    const state::CharacterState& selected = account.characters[g_selectedRow];
    if (g_editorSoid != selected.soid) {
        load_editor(selected);
    }
}

/** Draws the compact roster and lets one row become the editor target. */
void draw_roster(const state::AccountState& account) noexcept {
    if (account.characterCount == 0) {
        ImGui::TextDisabled("No characters. Create one below.");
        return;
    }
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("character_roster", 7, flags)) {
        return;
    }
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 42.0F);
    ImGui::TableSetupColumn("SOID");
    ImGui::TableSetupColumn("Class");
    ImGui::TableSetupColumn("Race");
    ImGui::TableSetupColumn("Gender");
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 48.0F);
    ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthFixed, 58.0F);
    ImGui::TableHeadersRow();
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const state::CharacterState& character = account.characters[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool picked = g_selectedRow == index;
        std::array<char, 16> label{};
        (void)std::snprintf(label.data(), label.size(), "%zu", index + 1U);
        if (ImGui::Selectable(label.data(), picked, ImGuiSelectableFlags_SpanAllColumns)) {
            g_selectedRow = index;
            load_editor(character);
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("0x%016llX", static_cast<unsigned long long>(character.soid));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(class_name(character.characterClass));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(race_name(character.race));
        ImGui::TableSetColumnIndex(4);
        ImGui::TextUnformatted(gender_name(character.gender));
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%u", static_cast<unsigned>(character.level));
        ImGui::TableSetColumnIndex(6);
        ImGui::TextUnformatted(character.selected ? "selected" : "-");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/** Draws creation controls. New rows use Sunrise's bundled three-class factory loadouts. */
void draw_create(const state::AccountState& account) noexcept {
    ImGui::TextUnformatted("Create Character");
    ImGui::Separator();
    ImGui::SetNextItemWidth(150.0F);
    ImGui::Combo("Race##create", &g_createRace, kRaceNames.data(), static_cast<int>(kRaceNames.size()));
    ImGui::SetNextItemWidth(150.0F);
    ImGui::Combo("Gender##create",
                 &g_createGender,
                 kGenderNames.data(),
                 static_cast<int>(kGenderNames.size()));
    ImGui::SetNextItemWidth(150.0F);
    ImGui::Combo("Class##create",
                 &g_createClass,
                 kClassNames.data(),
                 static_cast<int>(kClassNames.size()));

    const bool full = account.characterCount >= state::kCharacterCapacity;
    ImGui::BeginDisabled(full);
    if (ImGui::Button("Create Character")) {
        std::size_t created = state::kCharacterCapacity;
        const state::CharacterMutationResult result = state::create_character(
            static_cast<state::CharacterRace>(g_createRace),
            static_cast<state::CharacterGender>(g_createGender),
            static_cast<state::CharacterClass>(g_createClass),
            created);
        set_message("create", result);
        if (result == state::CharacterMutationResult::ok) {
            g_selectedRow = created;
            g_editorSoid = 0;
            const bool saved = state::checkpoint_characters();
            (void)std::snprintf(g_message.data(),
                                g_message.size(),
                                "create: runtime updated; %s; roster identity changed, so "
                                "reselect/transition to rebuild the native client roster",
                                saved ? "checkpoint + settings mirror saved"
                                      : "checkpoint incomplete (see log)");
        }
    }
    ImGui::EndDisabled();
    if (full) {
        ImGui::SameLine();
        ImGui::TextDisabled("all 3 slots occupied");
    }
}

/** Draws editable scalar metadata for the selected row. */
void draw_editor(const state::AccountState& account) noexcept {
    if (account.characterCount == 0 || g_selectedRow >= account.characterCount) {
        return;
    }
    const state::CharacterState& character = account.characters[g_selectedRow];
    ImGui::Text("Edit Character %zu", g_selectedRow + 1U);
    ImGui::Separator();
    ImGui::Checkbox("Live apply character edits", &g_liveCharacter);
    ImGui::SameLine();
    ImGui::TextDisabled("metadata refreshes through Queuez when possible");

    int race = static_cast<int>(g_edit.race);
    int gender = static_cast<int>(g_edit.gender);
    int characterClass = static_cast<int>(g_edit.characterClass);
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::Combo("Race##edit", &race, kRaceNames.data(), static_cast<int>(kRaceNames.size()))) {
        g_edit.race = static_cast<state::CharacterRace>(race);
        if (g_liveCharacter) {
            apply_character_edit("race");
        }
    }
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::Combo("Gender##edit",
                     &gender,
                     kGenderNames.data(),
                     static_cast<int>(kGenderNames.size()))) {
        g_edit.gender = static_cast<state::CharacterGender>(gender);
        if (g_liveCharacter) {
            apply_character_edit("gender");
        }
    }
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::Combo("Class##edit",
                     &characterClass,
                     kClassNames.data(),
                     static_cast<int>(kClassNames.size()))) {
        g_edit.characterClass = static_cast<state::CharacterClass>(characterClass);
        if (g_liveCharacter) {
            apply_character_edit("class");
        }
    }

    int level = static_cast<int>(g_edit.level);
    ImGui::SetNextItemWidth(160.0F);
    const bool levelChanged = ImGui::InputInt("Level", &level, 1, 10);
    level = std::clamp(level, 0, 255);
    if (levelChanged) {
        g_edit.level = static_cast<std::uint8_t>(level);
    }
    if (g_liveCharacter && ImGui::IsItemDeactivatedAfterEdit()) {
        apply_character_edit("level");
    }

    ImGui::SetNextItemWidth(160.0F);
    (void)ImGui::InputFloat("Appearance value", &g_edit.appearanceValue, 0.05F, 0.25F, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        // This value is persisted, but no traced Family-4/3/0 field currently consumes it.
        if (g_liveCharacter) {
            (void)std::snprintf(g_message.data(),
                                g_message.size(),
                                "appearance: saved with Save Changes; native field mapping is "
                                "still unknown, so no live refresh was sent");
        }
    }
    ImGui::SetNextItemWidth(160.0F);
    (void)ImGui::InputScalar("Last destination",
                             ImGuiDataType_U32,
                             &g_edit.lastOrbitedDestination,
                             nullptr,
                             nullptr,
                             "%08X",
                             ImGuiInputTextFlags_CharsHexadecimal);
    if (g_liveCharacter && ImGui::IsItemDeactivatedAfterEdit()) {
        apply_character_edit("destination");
    }
    if (ImGui::Checkbox("Accepted", &g_edit.accepted) && g_liveCharacter) {
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "accepted: saved with Save Changes; native field mapping is still "
                            "unknown, so no live refresh was sent");
    }
    if (ImGui::Checkbox("Preview available", &g_edit.previewAvailable) && g_liveCharacter) {
        apply_character_edit("preview");
    }
    if (ImGui::Checkbox("Content bypass", &g_edit.contentBypass) && g_liveCharacter) {
        apply_character_edit("content bypass");
    }

    if (g_edit.characterClass != character.characterClass) {
        ImGui::TextWrapped("Changing class swaps helmet, gauntlets, chest, legs, class item, "
                           "subclass, and ability selections from Sunrise's bundled class factory. "
                           "Weapons and general cosmetics are retained.");
    }
    ImGui::Text("Ability selections: move %u  grenade %u  super %u  melee %u  class %u",
                static_cast<unsigned>(character.movementAbilityEntry),
                static_cast<unsigned>(character.grenadeAbilityEntry),
                static_cast<unsigned>(character.superAbilityEntry),
                static_cast<unsigned>(character.meleeAbilityEntry),
                static_cast<unsigned>(character.classAbilityEntry));

    const bool nativePresentation = has_native_bytes(character.presentationHeader);
    const bool nativeCreation = has_native_bytes(character.creationHeader);
    ImGui::Text("Native head appearance: %s", nativePresentation ? "captured" : "legacy / missing");
    ImGui::SameLine();
    ImGui::TextDisabled("creator block: %s", nativeCreation ? "captured" : "missing");
    if (ImGui::Button("Trace next Family-4 lookups")) {
        hooks::network::investment::arm_family4_lookup_trace();
        (void)std::snprintf(g_message.data(),
                            g_message.size(),
                            "lookup trace armed; close debug UI and open inventory");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("diagnoses inventory body/head object reads");
    if (!nativePresentation) {
        ImGui::TextWrapped("This character predates native creator appearance capture. Its helmet "
                           "can render normally while the inventory helmet-hidden view has no "
                           "authored head/face block to reconstruct. Newly created characters are "
                           "the first clean test of that path.");
    }
    if (character.selected) {
        ImGui::TextWrapped("Race/gender are mirrored into the known Family-4, Family-3 and "
                           "Family-0 character records. Test character-select/banner and the "
                           "already-spawned body separately; if only the body stays stale after all "
                           "three partial updates land, another native presentation object remains "
                           "to be identified.");
    }

    if (ImGui::Button("Save Changes")) {
        apply_character_edit("save");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload Values")) {
        load_editor(character);
        g_message = {};
    }

    ImGui::Spacing();
    if (!g_confirmDelete) {
        if (ImGui::Button("Delete Character")) {
            g_confirmDelete = true;
        }
    } else {
        ImGui::TextUnformatted("Delete this character permanently from the local character store?");
        if (character.selected) {
            ImGui::TextWrapped("This specific character is active. Select another character in "
                               "Destiny first; an active character cannot be removed underneath "
                               "the running native player object.");
        }
        ImGui::BeginDisabled(character.selected);
        if (ImGui::Button("Confirm Delete")) {
            const state::CharacterMutationResult result = state::delete_character(g_selectedRow);
            set_message("delete", result);
            if (result == state::CharacterMutationResult::ok) {
                g_editorSoid = 0;
                g_confirmDelete = false;
                const bool saved = state::checkpoint_characters();
                (void)std::snprintf(g_message.data(),
                                    g_message.size(),
                                    "delete: runtime updated; %s; removed item residents change "
                                    "the Family-4 manifest, so reselect/transition to rebuild it",
                                    saved ? "checkpoint + settings mirror saved"
                                          : "checkpoint incomplete (see log)");
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            g_confirmDelete = false;
        }
    }
}

} // namespace

/** Draws character roster, creator identity and persisted character metadata. */
void draw() noexcept {
    state::AccountState account = state::account_snapshot();
    sync_editor(account);
    const state::CharacterStoreStatus store = state::character_store_status();

    ImGui::TextUnformatted("Characters");
    ImGui::Separator();
    ImGui::TextWrapped("Runtime AccountState is authoritative while Sunrise is running. The editor "
                       "mutates that state first and publishes Queuez objects from it; characters.dat "
                       "is the durable checkpoint, while settings.json is only mirrored for startup "
                       "compatibility after a successful live publication.");
    ImGui::Spacing();
    ImGui::Text("Account: 0x%016llX", static_cast<unsigned long long>(account.primarySoid));
    ImGui::Text("Characters: %zu / %zu", account.characterCount, state::kCharacterCapacity);
    if (!store.available) {
        ImGui::TextUnformatted("Checkpoint: unavailable (runtime/live edits still work)");
    } else if (store.rejected) {
        ImGui::TextUnformatted("Checkpoint: characters.dat rejected; runtime booted from settings.json");
    } else if (store.loaded) {
        ImGui::TextUnformatted("Checkpoint: characters.dat active");
    } else {
        ImGui::TextUnformatted("Checkpoint: no characters.dat yet; runtime booted from settings.json");
    }
    if (!store.lastSaveOk) {
        ImGui::TextUnformatted("Last characters.dat checkpoint failed");
    }
    if (!store.lastSettingsMirrorOk) {
        ImGui::TextUnformatted("Last settings.json compatibility mirror failed");
    }

    ImGui::Spacing();
    draw_roster(account);
    ImGui::Spacing();
    draw_create(account);

    account = state::account_snapshot();
    sync_editor(account);
    if (account.characterCount != 0) {
        ImGui::Spacing();
        draw_editor(account);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Identity-preserving edits are runtime-first: Queuez publishes the affected "
                       "Family-4/3/0 objects, then Sunrise checkpoints characters.dat and mirrors "
                       "the compatible character/equipment rows into settings.json. Create/delete/class "
                       "changes alter resident identities and checkpoint immediately because they "
                       "still require a reselect/transition. Equipment, owned inventory and the "
                       "installed item browser now live on the Inventory and Item Catalog pages.");

    if (!g_confirmReset) {
        if (ImGui::Button("Reset Characters to settings.json")) {
            g_confirmReset = true;
        }
    } else {
        ImGui::TextUnformatted("Replace the persistent roster with the settings.json templates?");
        if (ImGui::Button("Confirm Reset")) {
            const state::CharacterMutationResult result = state::reset_characters_to_settings();
            set_message("reset", result);
            if (result == state::CharacterMutationResult::ok) {
                g_selectedRow = 0;
                g_editorSoid = 0;
                g_confirmReset = false;
                const bool saved = state::checkpoint_characters();
                (void)std::snprintf(g_message.data(),
                                    g_message.size(),
                                    "reset: runtime updated; %s; roster identities/loadouts "
                                    "changed, so reselect or transition before expecting the native "
                                    "roster to rebuild",
                                    saved ? "checkpoint + settings mirror saved"
                                          : "checkpoint incomplete (see log)");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel Reset")) {
            g_confirmReset = false;
        }
    }

    if (g_message[0] != '\0') {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", g_message.data());
    }
}

} // namespace sunrise::client::ui::characters
