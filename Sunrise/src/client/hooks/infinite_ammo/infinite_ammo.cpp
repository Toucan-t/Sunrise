/**
 * Infinite ammo. The native setters are called with a full value instead of writing encoded weapon
 * state directly. The magazine setter itself is left alone so reload behaviour remains native.
 */

#include "infinite_ammo.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::hooks::infinite_ammo {
namespace {

constexpr std::string_view kReservesCallText = "2B F5 48 8B CF 8B D6 E8 ? ? ? ?";
constexpr std::string_view kMagazineCallText = "8D 14 33 48 8B CF E8 ? ? ? ?";
constexpr auto kReservesCall =
    patterns::signature<patterns::signature_length(kReservesCallText)>(kReservesCallText);
constexpr auto kMagazineCall =
    patterns::signature<patterns::signature_length(kMagazineCallText)>(kMagazineCallText);

constexpr std::size_t kReservesCallOffset = 7;
constexpr std::size_t kMagazineCallOffset = 6;
constexpr std::size_t kNearCallOperand = 1;
constexpr std::size_t kNearCallLength = 5;

constexpr std::string_view kSwordText =
    "40 56 48 83 EC 50 0F 29 74 24 40 48 8B F1 0F 29 7C 24 30 48 83 C1 60";
constexpr auto kSword = patterns::signature<patterns::signature_length(kSwordText)>(kSwordText);

constexpr std::int32_t kRequestedCount = 500;
constexpr float kRequestedSupply = 9999.0F;

using Setter = std::int64_t(__fastcall*)(void*, std::int32_t);
using SwordSetter = void(__fastcall*)(void*, float);

constexpr std::size_t kHandleCount = 3;
constexpr std::size_t kReservesSlot = 0;
constexpr std::size_t kMagazineSlot = 1;
constexpr std::size_t kSwordSlot = 2;

std::array<hooking::detour::Handle, kHandleCount> g_handles{};

[[nodiscard]] bool enabled() noexcept {
    return client::player::get().infiniteAmmoEnabled;
}

std::int64_t __fastcall set_reserves(void* weapon, std::int32_t amount) noexcept {
    const Setter next = reinterpret_cast<Setter>(g_handles[kReservesSlot].original);
    if (next == nullptr) {
        return 0;
    }
    return next(weapon, enabled() ? kRequestedCount : amount);
}

std::int64_t __fastcall set_magazine(void* weapon, std::int32_t amount) noexcept {
    const Setter next = reinterpret_cast<Setter>(g_handles[kMagazineSlot].original);
    if (next == nullptr) {
        return 0;
    }
    const std::int64_t result = next(weapon, amount);
    const Setter reserves = reinterpret_cast<Setter>(g_handles[kReservesSlot].original);
    if (enabled() && reserves != nullptr && weapon != nullptr) {
        (void)reserves(weapon, kRequestedCount);
    }
    return result;
}

void __fastcall set_sword_supply(void* weapon, float supply) noexcept {
    const SwordSetter next = reinterpret_cast<SwordSetter>(g_handles[kSwordSlot].original);
    if (next != nullptr) {
        next(weapon, enabled() ? kRequestedSupply : supply);
    }
}

[[nodiscard]] std::byte* setter_from(std::byte* site, std::size_t offset) noexcept {
    if (site == nullptr) {
        return nullptr;
    }
    std::byte* const call = site + offset;
    return static_cast<std::byte*>(
        patterns::resolve_relative(call + kNearCallOperand, call + kNearCallLength));
}

[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=infinite_ammo stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

bool install() noexcept {
    if (g_handles[kReservesSlot].original != nullptr) {
        return true;
    }
    std::byte* const reserves =
        setter_from(patterns::scan_main_image_unique(kReservesCall, "infinite_ammo_reserves"),
                    kReservesCallOffset);
    if (reserves == nullptr) {
        return fail("reserves");
    }
    std::byte* const magazine =
        setter_from(patterns::scan_main_image_unique(kMagazineCall, "infinite_ammo_magazine"),
                    kMagazineCallOffset);
    if (magazine == nullptr) {
        return fail("magazine");
    }
    std::byte* const sword = patterns::scan_main_image_unique(kSword, "infinite_ammo_sword");
    if (sword == nullptr) {
        return fail("sword");
    }
    const std::array<hooking::detour::Spec, kHandleCount> specs{
        hooking::detour::Spec{reserves, reinterpret_cast<void*>(&set_reserves)},
        hooking::detour::Spec{magazine, reinterpret_cast<void*>(&set_magazine)},
        hooking::detour::Spec{sword, reinterpret_cast<void*>(&set_sword_supply)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        return fail("attach");
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=infinite_ammo stage=install result=ok");
    return true;
}

void uninstall() noexcept {
    if (g_handles[kReservesSlot].original == nullptr) {
        return;
    }
    (void)hooking::detour::uninstall(g_handles);
    g_handles = {};
}

} // namespace sunrise::client::hooks::infinite_ammo
