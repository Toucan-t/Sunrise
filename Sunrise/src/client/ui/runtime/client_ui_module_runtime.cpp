#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../editor/editor_panel.h"
#include "../../editor/textures/editor_texture_preview.h"
#include "../movement/movement_panel.h"
#include "../player/player_panel.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable IDs prevent Client modules from colliding with Server modules. */
constexpr std::string_view kMovementStableId = "client.movement";
constexpr std::string_view kPlayerStableId = "client.player";
constexpr std::string_view kEditorStableId = "client.editor";
/** Short menu label for the shared teleport and noclip page. */
constexpr std::string_view kMovementDisplayName = "Movement";
/** Short menu label for the player page. */
constexpr std::string_view kPlayerDisplayName = "Player";
/** Short menu label for the package/content editor foundation. */
constexpr std::string_view kEditorDisplayName = "Editor";

core::ui::modules::registry::PageRegistration g_movementPage;
core::ui::modules::registry::PageRegistration g_playerPage;
core::ui::modules::registry::PageRegistration g_editorPage;

} // namespace

/** @return True when every Client module owns its Core UI registry slot. */
bool initialize() noexcept {
    // Acquisition order is the order the Client pages appear in the menu.
    const bool movementOwned = g_movementPage.acquire(
        core::ui::modules::Owner::client, kMovementStableId, kMovementDisplayName, &movement::draw);
    const bool playerOwned = g_playerPage.acquire(
        core::ui::modules::Owner::client, kPlayerStableId, kPlayerDisplayName, &player::draw);
    const bool editorOwned = g_editorPage.acquire(
        core::ui::modules::Owner::client, kEditorStableId, kEditorDisplayName, &editor::draw);
    return movementOwned && playerOwned && editorOwned;
}

/** Removes the Client modules from the Core UI registry. */
void shutdown() noexcept {
    sunrise::client::editor::textures::release_previews();
    g_editorPage.release();
    g_playerPage.release();
    g_movementPage.release();
}

} // namespace sunrise::client::ui::runtime
