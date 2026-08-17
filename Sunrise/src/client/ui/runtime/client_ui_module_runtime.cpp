#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../characters/characters_panel.h"
#include "../entities/entities_panel.h"
#include "../inventory/inventory_panel.h"
#include "../movement/movement_panel.h"
#include "../player/player_panel.h"
#include "../teleport/teleport_panel.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable IDs prevent Client modules from colliding with Server/Core modules. */
constexpr std::string_view kCharactersStableId = "client.characters";
constexpr std::string_view kInventoryStableId = "client.inventory";
constexpr std::string_view kItemCatalogStableId = "client.item_catalog";
constexpr std::string_view kEntitiesStableId = "client.entities";
constexpr std::string_view kTeleportStableId = "client.teleport";
constexpr std::string_view kMovementStableId = "client.movement";
constexpr std::string_view kPlayerStableId = "client.player";
/** Short menu labels for the client-owned debug pages. */
constexpr std::string_view kCharactersDisplayName = "Characters";
constexpr std::string_view kInventoryDisplayName = "Inventory";
constexpr std::string_view kItemCatalogDisplayName = "Item Catalog";
constexpr std::string_view kEntitiesDisplayName = "Entities";
constexpr std::string_view kTeleportDisplayName = "Teleport";
constexpr std::string_view kMovementDisplayName = "Movement";
constexpr std::string_view kPlayerDisplayName = "Player";

core::ui::modules::registry::PageRegistration g_charactersPage;
core::ui::modules::registry::PageRegistration g_inventoryPage;
core::ui::modules::registry::PageRegistration g_itemCatalogPage;
core::ui::modules::registry::PageRegistration g_entitiesPage;
core::ui::modules::registry::PageRegistration g_teleportPage;
core::ui::modules::registry::PageRegistration g_movementPage;
core::ui::modules::registry::PageRegistration g_playerPage;

} // namespace

/** @return True when every Client page owns its Core UI registry slot. */
bool initialize() noexcept {
    if (!g_charactersPage.acquire(core::ui::modules::Owner::client,
                                  kCharactersStableId,
                                  kCharactersDisplayName,
                                  &characters::draw)) {
        return false;
    }
    if (!g_inventoryPage.acquire(core::ui::modules::Owner::client,
                                 kInventoryStableId,
                                 kInventoryDisplayName,
                                 &inventory::draw_inventory)) {
        g_charactersPage.release();
        return false;
    }
    if (!g_itemCatalogPage.acquire(core::ui::modules::Owner::client,
                                   kItemCatalogStableId,
                                   kItemCatalogDisplayName,
                                   &inventory::draw_item_catalog)) {
        g_inventoryPage.release();
        g_charactersPage.release();
        return false;
    }
    if (!g_entitiesPage.acquire(core::ui::modules::Owner::client,
                                kEntitiesStableId,
                                kEntitiesDisplayName,
                                &entities::draw)) {
        g_itemCatalogPage.release();
        g_inventoryPage.release();
        g_charactersPage.release();
        return false;
    }
    if (!g_teleportPage.acquire(core::ui::modules::Owner::client,
                                kTeleportStableId,
                                kTeleportDisplayName,
                                &teleport::draw)) {
        g_entitiesPage.release();
        g_itemCatalogPage.release();
        g_inventoryPage.release();
        g_charactersPage.release();
        return false;
    }
    if (!g_movementPage.acquire(core::ui::modules::Owner::client,
                                kMovementStableId,
                                kMovementDisplayName,
                                &movement::draw)) {
        g_teleportPage.release();
        g_entitiesPage.release();
        g_itemCatalogPage.release();
        g_inventoryPage.release();
        g_charactersPage.release();
        return false;
    }
    if (!g_playerPage.acquire(core::ui::modules::Owner::client,
                              kPlayerStableId,
                              kPlayerDisplayName,
                              &player::draw)) {
        g_movementPage.release();
        g_teleportPage.release();
        g_entitiesPage.release();
        g_itemCatalogPage.release();
        g_inventoryPage.release();
        g_charactersPage.release();
        return false;
    }
    return true;
}

/** Removes the Client pages from the Core UI registry. */
void shutdown() noexcept {
    g_playerPage.release();
    g_movementPage.release();
    g_teleportPage.release();
    g_entitiesPage.release();
    g_itemCatalogPage.release();
    g_inventoryPage.release();
    g_charactersPage.release();
}

} // namespace sunrise::client::ui::runtime
