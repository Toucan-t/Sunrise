#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../hooking/detour.h"
#include "internal.h"

namespace sunrise::client::hooks::bootflow {
namespace {

/**
 * `World_CheckActivityBubbles`* @ `0x7FF74208DB80`. Matched from its prologue through the activity
 * object load and the handle shift, which is unique in the image.
 */
constexpr std::string_view kCheckSignatureText =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 48 8B 79 10 41 8B C0 "
    "41 8B D8 C1 F8 0D";
constexpr auto kCheckSignature =
    signature<signature_length(kCheckSignatureText)>(kCheckSignatureText);

constexpr std::int32_t kRemoteSlot = 1;
constexpr std::uint8_t kNotArmed = 0;
constexpr unsigned kMaxReports = 8;
constexpr std::size_t kLineCapacity = 256;

using CheckBubbles =
    std::uint8_t(__fastcall*)(void*, void*, std::int32_t, void*, std::int64_t, std::int32_t);

hooking::detour::Handle g_handle{};
std::atomic<CheckBubbles> g_original{nullptr};
std::atomic<unsigned> g_reported{0};

/**
 * Reports only fields whose meaning is independent of the now-unverified +0x40/0x28 roster-row
 * interpretation. The compatibility force itself is unchanged.
 */
void report(void* container,
            void* prefix,
            std::int32_t activity,
            std::int64_t roleIsLocal,
            std::int32_t slot) noexcept {
    if (g_reported.fetch_add(1, std::memory_order_relaxed) >= kMaxReports) {
        return;
    }
    std::array<char, kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=strike stage=participation activity=%d role_local=%lld native_slot=%d forced_slot=%d "
        "container=0x%llX prefix=0x%llX roster_geometry=unverified",
        static_cast<int>(activity),
        static_cast<long long>(roleIsLocal),
        static_cast<int>(slot),
        static_cast<int>(kRemoteSlot),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(container)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(prefix)));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }

    if (slot != kRemoteSlot) {
        std::array<char, 96> forced{};
        const int forcedWritten = std::snprintf(
            forced.data(),
            forced.size(),
            "ev=bootflow stage=owner_slot result=forced was=%d now=%d",
            static_cast<int>(slot),
            static_cast<int>(kRemoteSlot));
        if (forcedWritten > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {forced.data(), static_cast<std::size_t>(forcedWritten)});
        }
    }
}

/**
 * Passes a non-zero owner activity slot into the native check. The forcing behavior is unchanged;
 * this diagnostic only records what the native caller supplied immediately beforehand.
 */
__declspec(noinline) std::uint8_t __fastcall check(void* container,
                                                   void* reporter,
                                                   std::int32_t activity,
                                                   void* prefix,
                                                   std::int64_t roleIsLocal,
                                                   std::int32_t slot) noexcept {
    const CheckBubbles original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return kNotArmed;
    }
    if (!core::settings::get().client.pinReplicatedRecord) {
        return original(container, reporter, activity, prefix, roleIsLocal, slot);
    }

    report(container, prefix, activity, roleIsLocal, slot);
    return original(container, reporter, activity, prefix, roleIsLocal, kRemoteSlot);
}

} // namespace

bool install_owner_activity_slot() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kCheckSignature, "check_activity_bubbles");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=owner_slot result=fail reason=target");
        return false;
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&check)};
    if (!hooking::detour::install(spec, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=bootflow stage=owner_slot result=fail reason=attach");
        return false;
    }
    g_original.store(reinterpret_cast<CheckBubbles>(g_handle.original), std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=bootflow stage=owner_slot result=ok");
    return true;
}

void uninstall_owner_activity_slot() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(0, std::memory_order_release);
}

} // namespace sunrise::client::hooks::bootflow
