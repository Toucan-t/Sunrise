/**
 * Noclip at the Havok simulation boundary. Havok runs normally, then collision-resolved position
 * is replaced by the position produced from the character body's pre-step velocity.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../hooking/detour.h"
#include "../../input/window_focus.h"
#include "../../movement/movement_settings_store.h"
#include "../../patterns/image_scan.h"
#include "../fly/fly.h"
#include "runtime.h"

namespace sunrise::client::hooks::noclip {
namespace {

constexpr std::string_view kHavokStepText =
    "40 53 48 83 EC 20 83 79 38 01 48 8B D9 77 06 48 8B 01 FF 50 20 F7 43 38 FD FF FF FF "
    "75 09 48 8B 03 48 8B CB FF 50 28";
constexpr auto kHavokStep =
    patterns::signature<patterns::signature_length(kHavokStepText)>(kHavokStepText);

constexpr std::string_view kCharacterMotionVtableText = "48 8D 05 ? ? ? ? C6 43 ? ? 48 89 03 EB";
constexpr auto kCharacterMotionVtable =
    patterns::signature<patterns::signature_length(kCharacterMotionVtableText)>(
        kCharacterMotionVtableText);

constexpr std::size_t kVtableDisplacement = 3;
constexpr std::size_t kVtableInstructionLength = 7;

constexpr std::size_t kSimulationWorld = 0x18;
constexpr std::array<std::size_t, 2> kWorldIslandArrays{0x40, 0x50};
constexpr std::size_t kIslandEntities = 0x60;
constexpr std::size_t kBodyMotion = 0x150;
constexpr std::size_t kBodyPosition = 0x1C0;
constexpr std::size_t kBodyVelocity = 0x230;

constexpr std::size_t kHorizontalX = 0;
constexpr std::size_t kHorizontalY = 1;
constexpr std::size_t kVertical = 2;
constexpr std::size_t kVectorLanes = 4;

constexpr std::uint32_t kArrayCapacityMask = 0x3FFFFFFF;
constexpr std::int32_t kMaximumIslandCount = 4096;
constexpr std::int32_t kMaximumEntityCount = 65536;
constexpr float kMaximumStepSeconds = 0.05F;
constexpr float kMinimumVelocitySquared = 0.000001F;

using HavokStep = std::int32_t(__fastcall*)(std::byte*, float);

struct HavokArray {
    std::byte** entries{};
    std::int32_t size{};
    std::uint32_t capacityAndFlags{};
};

static_assert(sizeof(HavokArray) == 16);

std::atomic_bool g_installed{false};
std::atomic_bool g_toggleDown{false};
std::uintptr_t g_characterMotionVtable{};
hooking::detour::Handle g_stepHandle{};

template <typename T> [[nodiscard]] T& field(std::byte* object, std::size_t offset) noexcept {
    return *reinterpret_cast<T*>(object + offset);
}

template <typename Source, typename Destination>
void copy_lanes(const Source& source, Destination& destination) noexcept {
    constexpr std::size_t lanes =
        (std::min)(std::tuple_size_v<Source>, std::tuple_size_v<Destination>);
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        destination[lane] = source[lane];
    }
}

[[nodiscard]] std::array<float, kVectorLanes>
capped_speed(const std::array<float, kVectorLanes>& velocity, float limit) noexcept {
    const float speedSquared = velocity[kHorizontalX] * velocity[kHorizontalX]
                               + velocity[kHorizontalY] * velocity[kHorizontalY]
                               + velocity[kVertical] * velocity[kVertical];
    if (speedSquared <= limit * limit) {
        return velocity;
    }
    std::array<float, kVectorLanes> capped = velocity;
    const float scale = limit / std::sqrt(speedSquared);
    capped[kHorizontalX] *= scale;
    capped[kHorizontalY] *= scale;
    capped[kVertical] *= scale;
    return capped;
}

[[nodiscard]] bool valid_array(const HavokArray& array, std::int32_t maximum) noexcept {
    const std::uint32_t capacity = array.capacityAndFlags & kArrayCapacityMask;
    return array.size >= 0 && array.size <= maximum
           && static_cast<std::uint32_t>(array.size) <= capacity
           && (array.size == 0 || array.entries != nullptr);
}

[[nodiscard]] std::byte* character_body_in(std::byte* island) noexcept {
    if (island == nullptr) {
        return nullptr;
    }
    const HavokArray& entities = field<HavokArray>(island, kIslandEntities);
    if (!valid_array(entities, kMaximumEntityCount)) {
        return nullptr;
    }
    for (std::int32_t index = 0; index < entities.size; ++index) {
        std::byte* const body = entities.entries[index];
        if (body != nullptr
            && field<std::uintptr_t>(body, kBodyMotion) == g_characterMotionVtable) {
            return body;
        }
    }
    return nullptr;
}

[[nodiscard]] std::byte* character_body(std::byte* simulation) noexcept {
    if (simulation == nullptr) {
        return nullptr;
    }
    std::byte* const world = field<std::byte*>(simulation, kSimulationWorld);
    if (world == nullptr) {
        return nullptr;
    }
    for (const std::size_t offset : kWorldIslandArrays) {
        const HavokArray& islands = field<HavokArray>(world, offset);
        if (!valid_array(islands, kMaximumIslandCount)) {
            continue;
        }
        for (std::int32_t index = 0; index < islands.size; ++index) {
            std::byte* const island = islands.entries[index];
            if (std::byte* const body = character_body_in(island); body != nullptr) {
                return body;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] bool poll_toggle() noexcept {
    const client::movement::Settings settings = client::movement::get();
    if (settings.noclipToggleKey == client::movement::kNoKey) {
        g_toggleDown.store(false, std::memory_order_relaxed);
        return settings.noclipEnabled;
    }
    const bool down =
        client::input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.noclipToggleKey)) & 0x8000) != 0;
    if (core::ui::runtime::snapshot().visible) {
        g_toggleDown.store(down, std::memory_order_relaxed);
        return settings.noclipEnabled;
    }
    if (down && !g_toggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::movement::Settings updated = settings;
        updated.noclipEnabled = !settings.noclipEnabled;
        if (!client::movement::publish(updated)) {
            return settings.noclipEnabled;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.noclipEnabled
                             ? "ev=noclip stage=toggle enabled=1 mode=rigid_body_position"
                             : "ev=noclip stage=toggle enabled=0 mode=rigid_body_position");
        return updated.noclipEnabled;
    }
    if (!down) {
        g_toggleDown.store(false, std::memory_order_release);
    }
    return settings.noclipEnabled;
}

[[nodiscard]] bool enabled() noexcept {
    return client::movement::get().noclipEnabled;
}

std::int32_t __fastcall havok_step(std::byte* simulation, float deltaTime) noexcept {
    std::array<float, kVectorLanes> nativeVelocity{};
    std::array<float, kVectorLanes> nativePosition{};
    const bool enabledBeforeStep = poll_toggle();
    const bool flying = fly::enabled();
    std::byte* const before = (enabledBeforeStep || flying) ? character_body(simulation) : nullptr;
    if (flying) {
        fly::before_step(before);
    }
    const bool hasBody = before != nullptr;
    if (hasBody) {
        nativeVelocity = field<std::array<float, kVectorLanes>>(before, kBodyVelocity);
        nativePosition = field<std::array<float, kVectorLanes>>(before, kBodyPosition);
    }
    const bool rested = enabledBeforeStep && flying && hasBody;
    if (rested) {
        field<std::array<float, kVectorLanes>>(before, kBodyVelocity) = {};
    }

    const HavokStep next = reinterpret_cast<HavokStep>(g_stepHandle.original);
    const std::int32_t result = next != nullptr ? next(simulation, deltaTime) : 0;

    std::byte* const body = (enabledBeforeStep || flying) ? character_body(simulation) : nullptr;
    const bool sameBody = hasBody && body == before;
    const bool noclipping = enabledBeforeStep && enabled();
    const bool verticalToo = noclipping && flying;
    if (flying) {
        fly::after_step(body, verticalToo);
    }
    if (flying && sameBody) {
        const std::array<float, kVectorLanes> moved =
            rested ? nativeVelocity : field<std::array<float, kVectorLanes>>(body, kBodyVelocity);
        field<std::array<float, kVectorLanes>>(body, kBodyVelocity) =
            capped_speed(moved, fly::kPublishedSpeedCap);
    }
    if (!noclipping || !sameBody) {
        return result;
    }

    const float step = std::clamp(deltaTime, 0.0F, kMaximumStepSeconds);
    float velocitySquared = nativeVelocity[kHorizontalX] * nativeVelocity[kHorizontalX]
                            + nativeVelocity[kHorizontalY] * nativeVelocity[kHorizontalY];
    if (verticalToo) {
        velocitySquared += nativeVelocity[kVertical] * nativeVelocity[kVertical];
    }
    if (step <= 0.0F || velocitySquared <= kMinimumVelocitySquared) {
        return result;
    }

    std::array<float, kVectorLanes> position =
        field<std::array<float, kVectorLanes>>(body, kBodyPosition);
    position[kHorizontalX] = nativePosition[kHorizontalX] + nativeVelocity[kHorizontalX] * step;
    position[kHorizontalY] = nativePosition[kHorizontalY] + nativeVelocity[kHorizontalY] * step;
    if (verticalToo) {
        position[kVertical] = nativePosition[kVertical] + nativeVelocity[kVertical] * step;
    }
    field<std::array<float, kVectorLanes>>(body, kBodyPosition) = position;

    if (!rested) {
        std::array<float, kVectorLanes> wakeVelocity =
            field<std::array<float, kVectorLanes>>(body, kBodyVelocity);
        wakeVelocity[kHorizontalX] = nativeVelocity[kHorizontalX];
        wakeVelocity[kHorizontalY] = nativeVelocity[kHorizontalY];
        field<std::array<float, kVectorLanes>>(body, kBodyVelocity) = wakeVelocity;
    }
    return result;
}

void report_install_failure(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=noclip stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const step = patterns::scan_main_image_unique(kHavokStep, "noclip_havok_step");
    if (step == nullptr) {
        report_install_failure("havok_step");
        return false;
    }
    std::byte* const vtableSite =
        patterns::scan_main_image_unique(kCharacterMotionVtable, "noclip_character_motion_vtable");
    if (vtableSite == nullptr) {
        report_install_failure("character_motion_vtable");
        return false;
    }
    void* const vtable = patterns::resolve_relative(vtableSite + kVtableDisplacement,
                                                    vtableSite + kVtableInstructionLength);
    if (vtable == nullptr) {
        report_install_failure("character_motion_vtable_relative");
        return false;
    }
    // Publish the character-motion identity before the detour can run. The replacement may execute
    // as soon as the transaction commits.
    g_characterMotionVtable = reinterpret_cast<std::uintptr_t>(vtable);
    const hooking::detour::Spec spec{step, reinterpret_cast<void*>(&havok_step)};
    if (!hooking::detour::install(spec, g_stepHandle)) {
        g_characterMotionVtable = 0;
        report_install_failure("attach");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=noclip stage=install result=ok mode=rigid_body_position");
    return true;
}

void uninstall() noexcept {
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)hooking::detour::uninstall(g_stepHandle);
    g_stepHandle = {};
    g_characterMotionVtable = 0;
    g_toggleDown.store(false, std::memory_order_release);
}

void read_body_position(void* body, Vector& position) noexcept {
    position = {};
    if (body == nullptr) {
        return;
    }
    const auto& stored =
        field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyPosition);
    copy_lanes(stored, position);
}

void write_body_position(void* body, const Vector& position) noexcept {
    if (body == nullptr) {
        return;
    }
    auto& stored = field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyPosition);
    copy_lanes(position, stored);
}

void read_body_velocity(void* body, Vector& velocity) noexcept {
    velocity = {};
    if (body == nullptr) {
        return;
    }
    const auto& stored =
        field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyVelocity);
    copy_lanes(stored, velocity);
}

void write_body_velocity(void* body, const Vector& velocity) noexcept {
    if (body == nullptr) {
        return;
    }
    auto& stored = field<std::array<float, kVectorLanes>>(static_cast<std::byte*>(body), kBodyVelocity);
    copy_lanes(velocity, stored);
}

} // namespace sunrise::client::hooks::noclip
