#include "player_panel.h"

#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {

void draw() noexcept {
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep weapon reserves full. Reloading still uses the game's normal path.");
    ImGui::Spacing();

    if (core::ui::components::toggle::control("Enabled##infinite_ammo",
                                              settings.infiniteAmmoEnabled)) {
        (void)client::player::publish(settings);
    }
}

} // namespace sunrise::client::ui::player
