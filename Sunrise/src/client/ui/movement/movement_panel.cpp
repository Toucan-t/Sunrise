#include "movement_panel.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../movement/movement_settings_store.h"

namespace sunrise::client::ui::movement {
namespace {

constexpr int kFirstVirtualKey = 1;
constexpr int kLastVirtualKey = 254;
constexpr int kLastMouseKey = 6;
constexpr std::size_t kKeyNameCapacity = 64;

enum class CaptureTarget {
    none,
    noclip,
    fly,
};

CaptureTarget g_capturing{CaptureTarget::none};

void key_name(std::uint32_t virtualKey, std::array<char, kKeyNameCapacity>& output) noexcept {
    if (virtualKey == client::movement::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, kKeyNameCapacity> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                        wide.data(),
                                                        static_cast<int>(wide.size()))
                                      : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

[[nodiscard]] bool capture_key(std::uint32_t& picked) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        picked = client::movement::kNoKey;
        return true;
    }
    for (int key = kFirstVirtualKey; key <= kLastVirtualKey; ++key) {
        if (key <= kLastMouseKey) {
            continue;
        }
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            picked = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool
key_picker(const char* id, CaptureTarget target, std::uint32_t& virtualKey, float width) noexcept {
    ImGui::PushID(id);
    if (g_capturing == target) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturing = CaptureTarget::none;
        }
        ImGui::PopID();
        std::uint32_t picked = client::movement::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturing = CaptureTarget::none;
            return true;
        }
        return false;
    }
    std::array<char, kKeyNameCapacity> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturing = target;
    }
    return false;
}

} // namespace

void draw() noexcept {
    client::movement::Settings settings = client::movement::get();
    bool changed = false;

    const float labelWidth =
        ImGui::CalcTextSize("Toggle key").x + ImGui::GetStyle().ItemSpacing.x * 2;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;

    ImGui::TextUnformatted("Noclip");
    ImGui::Separator();
    ImGui::TextWrapped("Ignore horizontal collision while preserving normal vertical physics. "
                       "With Fly enabled, collision is ignored on all three axes.");
    ImGui::Spacing();
    changed =
        core::ui::components::toggle::control("Enabled##noclip", settings.noclipEnabled) || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine(labelWidth);
    changed =
        key_picker("noclip_key", CaptureTarget::noclip, settings.noclipToggleKey, controlWidth)
        || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Fly");
    ImGui::Separator();
    ImGui::TextWrapped("Move in camera space with the normal movement keys. Jump ascends and "
                       "crouch descends.");
    ImGui::Spacing();
    changed = core::ui::components::toggle::control("Enabled##fly", settings.flyEnabled) || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine(labelWidth);
    changed = key_picker("fly_key", CaptureTarget::fly, settings.flyToggleKey, controlWidth) || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Speed");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    float flySpeed = settings.flySpeed;
    if (ImGui::SliderFloat("##fly_speed",
                           &flySpeed,
                           client::movement::kMinimumFlySpeed,
                           client::movement::kMaximumFlySpeed,
                           "%.0f units/s")) {
        settings.flySpeed = flySpeed;
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Sword Skate Fix");
    ImGui::Separator();
    ImGui::TextWrapped("Allow a glide to start during the sword air-attack throw so the throw's "
                       "momentum can carry into the glide.");
    ImGui::Spacing();
    changed =
        core::ui::components::toggle::control("Enabled##sword_skate", settings.swordSkateEnabled)
        || changed;

    if (changed && !client::movement::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::movement
