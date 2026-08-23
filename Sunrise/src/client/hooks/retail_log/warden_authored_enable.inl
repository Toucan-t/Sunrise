// Warden of Nothing authored-path enablement for the pinned Season of Arrivals client.
// Included by retail_log_lifecycle.cpp so no vcxproj entry is required.

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <intrin.h>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../../../core/logging/log.h"
#include "../../../state/activity/forced/activity_forced_destination.h"
#include "../../hooking/detour.h"

namespace sunrise::client::hooks::retail_log {
namespace warden_authored_enable {
namespace {

constexpr std::string_view kWardenOverrideText =
    "Setting activity-name from override 'strike_aries'.";
constexpr std::string_view kLaunchPrefix =
    "world_controller:activity_selection_manager: Launching activity-selection";
constexpr std::string_view kWardenGrognok = "grognok: strike_aries";
constexpr std::string_view kOrbitTransitionText =
    "Starting a new transition of type 'transitioning:orbit'";
constexpr std::string_view kWardenPackage = "strike_aries";
constexpr std::string_view kPosseSessionCreatedPrefix =
    "world_controller:state:activity_session_creation: Activity session 'posse:";
constexpr std::string_view kPosseSessionCreatedSuffix =
    "successfully created, trying to move on.";

// Native helper used by F24CF0 to decide whether both local activity lanes are already committed.
constexpr std::uint32_t kNativeSlotPredicateRva = 0x00C030E0U;
// Return site of F24CF0's first C030E0 call. For Warden, making this one lane non-committed is
// enough for the game's own decision routine to take its normal F207B0 authored-arm path.
constexpr std::uint32_t kArmPredicateLane0ReturnRva = 0x00F24E79U;
// The downstream authored trigger gate checks this byte before dispatching the authored route.
constexpr std::uint32_t kAuthoredSuppressionLatchRva = 0x031DC431U;

// Selection-holder probes are observation-only in this revision. Earlier experiments proved that
// rewriting the early kind-1 seed can select an authored manager before its session target exists,
// while rewriting the later kind-5 record corrupts downstream session state. The functional inputs
// now come from the native service-43 provider policy and the gameplay host-reestablish message.
constexpr std::uint32_t kSlotHolderSetRva = 0x017AD310U;
constexpr std::uint32_t kEarlySeedPublishReturnRva = 0x0175EF59U;
constexpr std::uint32_t kFullRecordPublishReturnRva = 0x0175FC0DU;
constexpr std::uint32_t kLocalManagerInitRva = 0x01772440U;
constexpr std::uint32_t kAuthoredManagerInitRva = 0x01773200U;
constexpr std::uint32_t kReceiverCreateRva = 0x017B8C50U;
constexpr std::uint32_t kComponentDispatchRva = 0x01766A30U;
constexpr std::size_t kManagerIdentityOffset = 0x854;

// 0xA8 holder-record contract recovered from the native selection path.
constexpr std::size_t kHolderRecordSize = 0xA8;
constexpr std::size_t kHolderRecordSourceOffset = 0x08;
constexpr std::size_t kHolderRecordDestinationOffset = 0x0C;
constexpr std::size_t kHolderRecordRouteOffset = 0x12;
constexpr std::size_t kHolderRecordIdOffset = 0x18;
constexpr std::size_t kHolderRecordKindWordOffset = 0xA0;
constexpr std::size_t kHolderRecordGuardOffset = 0xA1;
constexpr std::uint8_t kLocalRoute = 0U;

using NativeSlotPredicate = std::int32_t(__fastcall*)(void*) noexcept;

hooking::detour::Handle g_predicateHook{};
std::atomic<NativeSlotPredicate> g_predicateOriginal{nullptr};
std::atomic_bool g_active{false};
std::atomic_uint32_t g_overrideCount{0};
std::atomic_bool g_firstOverrideLogged{false};
std::atomic_uint64_t g_launchRecordId{0};
std::atomic_uint32_t g_seedCaptureCount{0};
std::atomic_bool g_fullRecordSeen{false};
std::atomic_uint32_t g_fullRecordObservationCount{0};
std::atomic_bool g_posseSessionReady{false};
std::atomic_uint32_t g_lateKindOneCandidateCount{0};
std::atomic_bool g_localIdentityOneSeen{false};
std::atomic_bool g_authoredIdentityOneSeen{false};
SRWLOCK g_gateLock{SRWLOCK_INIT};
std::uintptr_t g_imageBase{};
bool g_latchSaved{};
std::uint8_t g_savedLatch{};
std::atomic_bool g_latchForcedOpen{false};

[[nodiscard]] std::uint32_t return_rva(const void* address) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (g_imageBase == 0 || value < g_imageBase) {
        return 0;
    }
    const auto delta = value - g_imageBase;
    return delta <= 0xFFFFFFFFULL ? static_cast<std::uint32_t>(delta) : 0;
}

[[nodiscard]] bool executable_address(void* address) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    if (address == nullptr || VirtualQuery(address, &memory, sizeof memory) != sizeof memory
        || memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0
        || (memory.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    switch (memory.Protect & 0xFFU) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

template <typename T>
[[nodiscard]] bool read_remote(std::uintptr_t base, std::size_t offset, T& output) noexcept {
    output = {};
    if (base < 0x10000U || offset > (std::numeric_limits<std::uintptr_t>::max)() - base) {
        return false;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(base + offset),
                             &output,
                             sizeof output,
                             &copied)
               != FALSE
           && copied == sizeof output;
}

[[nodiscard]] bool read_byte(std::uintptr_t address, std::uint8_t& value) noexcept {
    return read_remote(address, 0, value);
}

[[nodiscard]] bool write_byte(std::uintptr_t address, std::uint8_t value) noexcept {
    if (address < 0x10000U) {
        return false;
    }
    SIZE_T written = 0;
    if (WriteProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<void*>(address),
                           &value,
                           sizeof value,
                           &written)
            == FALSE
        || written != sizeof value) {
        return false;
    }
    std::uint8_t verified = 0;
    return read_byte(address, verified) && verified == value;
}

/** Opens the downstream authored dispatcher gate and remembers the native byte for restoration. */
[[nodiscard]] bool open_authored_gate() noexcept {
    AcquireSRWLockExclusive(&g_gateLock);
    const auto address = g_imageBase + kAuthoredSuppressionLatchRva;
    std::uint8_t current = 0;
    if (!read_byte(address, current)) {
        ReleaseSRWLockExclusive(&g_gateLock);
        return false;
    }
    if (!g_latchSaved) {
        g_savedLatch = current;
        g_latchSaved = true;
    }
    if (current != 0 && !write_byte(address, 0)) {
        ReleaseSRWLockExclusive(&g_gateLock);
        return false;
    }
    g_latchForcedOpen.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_gateLock);
    return true;
}

/** Restores the exact native latch value captured when Warden was activated. */
void restore_authored_gate() noexcept {
    AcquireSRWLockExclusive(&g_gateLock);
    if (g_latchSaved && g_imageBase != 0) {
        (void)write_byte(g_imageBase + kAuthoredSuppressionLatchRva, g_savedLatch);
    }
    g_latchSaved = false;
    g_savedLatch = 0;
    g_latchForcedOpen.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_gateLock);
}

void log_lifecycle(const char* result) noexcept {
    std::array<char, 288> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=warden_authored stage=lifecycle result=%s overrides=%u seed_id=0x%llX seed_captures=%u full_seen=%u full_observations=%u posse_ready=%u post_posse_kind1=%u local_identity1=%u authored_manager_seen=%u",
        result,
        g_overrideCount.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(g_launchRecordId.load(std::memory_order_relaxed)),
        g_seedCaptureCount.load(std::memory_order_relaxed),
        g_fullRecordSeen.load(std::memory_order_relaxed) ? 1U : 0U,
        g_fullRecordObservationCount.load(std::memory_order_relaxed),
        g_posseSessionReady.load(std::memory_order_relaxed) ? 1U : 0U,
        g_lateKindOneCandidateCount.load(std::memory_order_relaxed),
        g_localIdentityOneSeen.load(std::memory_order_relaxed) ? 1U : 0U,
        g_authoredIdentityOneSeen.load(std::memory_order_relaxed) ? 1U : 0U);
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     {line.data(), length});
}

void reset_route_state() noexcept {
    g_launchRecordId.store(0, std::memory_order_release);
    g_seedCaptureCount.store(0, std::memory_order_release);
    g_fullRecordSeen.store(false, std::memory_order_release);
    g_fullRecordObservationCount.store(0, std::memory_order_release);
    g_posseSessionReady.store(false, std::memory_order_release);
    g_lateKindOneCandidateCount.store(0, std::memory_order_release);
    g_localIdentityOneSeen.store(false, std::memory_order_release);
    g_authoredIdentityOneSeen.store(false, std::memory_order_release);
}

void activate() noexcept {
    bool expected = false;
    if (!g_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    g_overrideCount.store(0, std::memory_order_release);
    g_firstOverrideLogged.store(false, std::memory_order_release);
    reset_route_state();
    if (!open_authored_gate()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=warden_authored stage=lifecycle result=activate_gate_failed");
    } else {
        log_lifecycle("active");
    }
}

void deactivate() noexcept {
    if (!g_active.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    restore_authored_gate();
    log_lifecycle("inactive");
    reset_route_state();
}

[[nodiscard]] bool forced_destination_is_warden() noexcept {
    state::activity::forced::ForcedDestination destination{};
    state::activity::forced::snapshot(destination);
    return state::activity::forced::active(destination)
           && destination.packageNameLength == kWardenPackage.size()
           && std::string_view{destination.packageName.data(), destination.packageNameLength}
                  == kWardenPackage;
}

/** Generated ABI-neutral function-entry probes for the route decision and its immediate outcome. */
enum class NativeSiteKind : std::uint8_t {
    slotHolder,
    localManager,
    authoredManager,
    receiver,
    component,
};

struct NativeSiteDefinition final {
    const char* name{};
    std::uint32_t rva{};
    NativeSiteKind kind{NativeSiteKind::slotHolder};
};

constexpr std::array<NativeSiteDefinition, 5> kNativeSites{{
    {"slot_holder_set", kSlotHolderSetRva, NativeSiteKind::slotHolder},
    {"local_manager_init", kLocalManagerInitRva, NativeSiteKind::localManager},
    {"authored_manager_init", kAuthoredManagerInitRva, NativeSiteKind::authoredManager},
    {"receiver_create", kReceiverCreateRva, NativeSiteKind::receiver},
    {"component_dispatch", kComponentDispatchRva, NativeSiteKind::component},
}};

constexpr std::size_t kThunkAllocationSize = 0x400;
constexpr std::size_t kThunkCodeCapacity = 0x1D0;
constexpr std::size_t kTrampolineSlotOffset = 0x1E0;
constexpr DWORD kUnwindInfoOffset = 0x200;
constexpr std::size_t kThunkStackSize = 0xD8;
constexpr std::size_t kFrameXmm0 = 0x20;
constexpr std::size_t kFrameRax = 0x80;
constexpr std::size_t kFrameRcx = 0x88;
constexpr std::size_t kFrameRdx = 0x90;
constexpr std::size_t kFrameR8 = 0x98;
constexpr std::size_t kFrameR9 = 0xA0;
constexpr std::size_t kFrameR10 = 0xA8;
constexpr std::size_t kFrameR11 = 0xB0;

struct NativeSiteRuntime final {
    hooking::detour::Handle hook{};
    std::atomic_uint32_t hits{0};
    std::atomic_bool firstLogged{false};
    void* thunk{};
    std::uintptr_t* trampolineSlot{};
    std::size_t thunkCodeSize{};
    RUNTIME_FUNCTION unwindEntry{};
    bool unwindRegistered{};
};

std::array<NativeSiteRuntime, kNativeSites.size()> g_nativeRuntime{};

template <typename T>
[[nodiscard]] T frame_value(const std::byte* frame, std::size_t offset) noexcept {
    T value{};
    std::memcpy(&value, frame + offset, sizeof value);
    return value;
}

struct HolderRecordProbe final {
    std::uint8_t kind{};
    std::int32_t source{};
    std::int32_t destination{};
    std::uint8_t route{};
    std::uint64_t id{};
    std::uint16_t kindWord{};
    std::uint8_t guard{};
    bool readable{};
};

[[nodiscard]] HolderRecordProbe probe_holder_record(std::uintptr_t address) noexcept {
    HolderRecordProbe output{};
    if (!read_remote(address, 0, output.kind)
        || !read_remote(address, kHolderRecordSourceOffset, output.source)
        || !read_remote(address, kHolderRecordDestinationOffset, output.destination)
        || !read_remote(address, kHolderRecordRouteOffset, output.route)
        || !read_remote(address, kHolderRecordIdOffset, output.id)
        || !read_remote(address, kHolderRecordKindWordOffset, output.kindWord)
        || !read_remote(address, kHolderRecordGuardOffset, output.guard)) {
        return output;
    }
    output.readable = true;
    return output;
}

[[nodiscard]] bool local_seed_shape(const HolderRecordProbe& record) noexcept {
    return record.readable && record.kind == 1U && record.source == 1 && record.destination == -1
           && record.route == kLocalRoute && record.id != 0 && record.id != 1
           && record.kindWord == 0x0104U && record.guard == 1U;
}

[[nodiscard]] bool full_record_shape(const HolderRecordProbe& record,
                                     std::uint64_t expectedId) noexcept {
    return expectedId != 0 && expectedId != 1 && record.readable && record.kind == 5U
           && record.source == 1 && record.destination == -1 && record.route == kLocalRoute
           && record.id == expectedId && record.kindWord == 0x0004U && record.guard == 0U;
}

[[nodiscard]] bool matching_kind_one(const HolderRecordProbe& record,
                                     std::uint64_t expectedId) noexcept {
    return expectedId != 0 && expectedId != 1 && record.readable && record.kind == 1U
           && record.source == 1 && record.destination == -1 && record.route == kLocalRoute
           && record.id == expectedId;
}

void log_seed_capture(std::uint32_t hit,
                      std::uintptr_t source,
                      const HolderRecordProbe& record,
                      const char* result) noexcept {
    std::array<char, 224> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=warden_authored stage=selection_seed result=%s hit=%u caller=+0x%08X source=0x%llX kind=%u source_value=%d destination=%d route=0x%02X id=0x%llX kind_word=0x%04X guard=%u",
        result,
        hit,
        kEarlySeedPublishReturnRva,
        static_cast<unsigned long long>(source),
        record.kind,
        record.source,
        record.destination,
        record.route,
        static_cast<unsigned long long>(record.id),
        record.kindWord,
        record.guard);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         std::string_view(result) == "captured" ? core::log::Level::info
                                                               : core::log::Level::warn,
                         {line.data(), length});
    }
}

void log_full_milestone(std::uint32_t hit,
                        std::uint32_t callerRva,
                        std::uintptr_t source,
                        const HolderRecordProbe& record) noexcept {
    std::array<char, 256> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=warden_authored stage=full_milestone result=observed hit=%u caller=+0x%08X source=0x%llX kind=%u source_value=%d destination=%d route=0x%02X id=0x%llX kind_word=0x%04X guard=%u",
        hit,
        callerRva,
        static_cast<unsigned long long>(source),
        record.kind,
        record.source,
        record.destination,
        record.route,
        static_cast<unsigned long long>(record.id),
        record.kindWord,
        record.guard);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
}

void observe_slot_holder(std::uint32_t hit,
                         std::byte* /*frame*/,
                         std::uintptr_t recordAddress,
                         std::uintptr_t returnAddress) noexcept {
    const std::uint32_t caller = return_rva(reinterpret_cast<const void*>(returnAddress));
    const HolderRecordProbe record = probe_holder_record(recordAddress);

    // Observation only. The service-43 provider configuration is now responsible for native lane
    // materialization; this hook must never rewrite holder records or route bytes.
    if (caller == kEarlySeedPublishReturnRva && local_seed_shape(record)) {
        std::uint64_t expected = 0;
        if (g_launchRecordId.compare_exchange_strong(
                expected, record.id, std::memory_order_acq_rel, std::memory_order_acquire)) {
            g_seedCaptureCount.fetch_add(1, std::memory_order_relaxed);
            log_seed_capture(hit, recordAddress, record, "captured_native");
            return;
        }
    }

    const std::uint64_t expectedId = g_launchRecordId.load(std::memory_order_acquire);
    if (caller == kFullRecordPublishReturnRva && full_record_shape(record, expectedId)) {
        const std::uint32_t count =
            g_fullRecordObservationCount.fetch_add(1, std::memory_order_relaxed) + 1U;
        g_fullRecordSeen.store(true, std::memory_order_release);
        if (count <= 4U || (count & (count - 1U)) == 0U) {
            log_full_milestone(hit, caller, recordAddress, record);
        }
        return;
    }

    if (matching_kind_one(record, expectedId)
        && g_posseSessionReady.load(std::memory_order_acquire)) {
        const std::uint32_t candidate =
            g_lateKindOneCandidateCount.fetch_add(1, std::memory_order_relaxed) + 1U;
        if (candidate == 1U) {
            std::array<char, 224> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=warden_authored stage=post_posse_kind1 result=observed hit=%u caller=+0x%08X source=0x%llX route=0x%02X id=0x%llX",
                hit,
                caller,
                static_cast<unsigned long long>(recordAddress),
                record.route,
                static_cast<unsigned long long>(record.id));
            if (written > 0) {
                const auto length = static_cast<std::size_t>(written) < line.size()
                                        ? static_cast<std::size_t>(written)
                                        : line.size() - 1;
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), length});
            }
        }
    }
}

void log_manager_site(std::size_t siteIndex,
                      std::uint32_t hit,
                      std::uintptr_t manager,
                      std::uintptr_t rdx,
                      std::uintptr_t r8,
                      std::uintptr_t r9) noexcept {
    std::uint32_t identity = 0;
    if (!read_remote(manager, kManagerIdentityOffset, identity)) {
        return;
    }

    const bool authored = kNativeSites[siteIndex].kind == NativeSiteKind::authoredManager;
    if (authored) {
        // Despite the historic variable name this now means "an authored manager was seen". The
        // newer traces show that manager creation handles are per-run and are not fixed to 1.
        g_authoredIdentityOneSeen.store(true, std::memory_order_release);
    } else if (identity == 1U) {
        g_localIdentityOneSeen.store(true, std::memory_order_release);
    }

    // Manager construction is sparse. Log the first 16 entries of each family plus power-of-two
    // milestones so a fresh authored handle cannot be hidden by the old identity==1 assumption.
    if (hit > 16U && (hit & (hit - 1U)) != 0U) {
        return;
    }

    std::array<char, 288> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=warden_authored stage=manager_init result=%s site=%s hit=%u manager=0x%llX identity=%u rdx=0x%llX r8=0x%llX r9=0x%llX provider_native=1 posse_ready=%u",
        authored ? "authored" : "local",
        kNativeSites[siteIndex].name,
        hit,
        static_cast<unsigned long long>(manager),
        identity,
        static_cast<unsigned long long>(rdx),
        static_cast<unsigned long long>(r8),
        static_cast<unsigned long long>(r9),
        g_posseSessionReady.load(std::memory_order_relaxed) ? 1U : 0U);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
}

void log_downstream_site(std::size_t siteIndex, std::uint32_t hit) noexcept {
    // Shared receiver/component entries exist elsewhere in networking. Only treat them as authored
    // script proof after any authored-manager construction has actually been observed.
    if (!g_authoredIdentityOneSeen.load(std::memory_order_acquire)) {
        return;
    }
    NativeSiteRuntime& runtime = g_nativeRuntime[siteIndex];
    bool expected = false;
    if (!runtime.firstLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const char* stage = kNativeSites[siteIndex].kind == NativeSiteKind::receiver ? "receiver"
                                                                                : "component";
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=warden_authored stage=%s result=hit site=%s hit=%u",
                                      stage,
                                      kNativeSites[siteIndex].name,
                                      hit);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }
}

extern "C" void __fastcall sunrise_warden_authored_capture(std::uint32_t siteIndex,
                                                            std::byte* frame,
                                                            std::uintptr_t /*originalRsp*/,
                                                            std::uintptr_t returnAddress) noexcept {
    if (siteIndex >= kNativeSites.size() || frame == nullptr) {
        return;
    }

    const NativeSiteKind kind = kNativeSites[siteIndex].kind;
    if (!g_active.load(std::memory_order_acquire)) {
        // Retail launch telemetry normally arms Warden first. The forced destination is an
        // independent fallback so the native provider route selection does not depend on log level.
        if (kind != NativeSiteKind::slotHolder || !forced_destination_is_warden()) {
            return;
        }
        activate();
    }
    if (!g_active.load(std::memory_order_acquire)) {
        return;
    }

    NativeSiteRuntime& runtime = g_nativeRuntime[siteIndex];
    const std::uint32_t hit = runtime.hits.fetch_add(1, std::memory_order_relaxed) + 1U;
    const std::uintptr_t rcx = frame_value<std::uintptr_t>(frame, kFrameRcx);
    const std::uintptr_t rdx = frame_value<std::uintptr_t>(frame, kFrameRdx);
    const std::uintptr_t r8 = frame_value<std::uintptr_t>(frame, kFrameR8);
    const std::uintptr_t r9 = frame_value<std::uintptr_t>(frame, kFrameR9);

    switch (kind) {
    case NativeSiteKind::slotHolder:
        observe_slot_holder(hit, frame, rdx, returnAddress);
        break;
    case NativeSiteKind::localManager:
    case NativeSiteKind::authoredManager:
        log_manager_site(siteIndex, hit, rcx, rdx, r8, r9);
        break;
    case NativeSiteKind::receiver:
    case NativeSiteKind::component:
        log_downstream_site(siteIndex, hit);
        break;
    }
}

class CodeWriter final {
public:
    explicit CodeWriter(std::span<unsigned char> output) noexcept : output_(output) {}

    [[nodiscard]] bool byte(unsigned char value) noexcept {
        if (used_ >= output_.size()) {
            return false;
        }
        output_[used_++] = value;
        return true;
    }

    template <typename T> [[nodiscard]] bool scalar(T value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (used_ + sizeof value > output_.size()) {
            return false;
        }
        std::memcpy(output_.data() + used_, &value, sizeof value);
        used_ += sizeof value;
        return true;
    }

    [[nodiscard]] bool bytes(std::initializer_list<unsigned char> values) noexcept {
        for (const unsigned char value : values) {
            if (!byte(value)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t used() const noexcept { return used_; }

private:
    std::span<unsigned char> output_;
    std::size_t used_{};
};

[[nodiscard]] bool emit_xmm_store(CodeWriter& writer, unsigned reg, unsigned disp) noexcept {
    const unsigned char modrm = static_cast<unsigned char>(0x44U | ((reg & 7U) << 3U));
    return writer.bytes({0x0F, 0x11, modrm, 0x24, static_cast<unsigned char>(disp)});
}

[[nodiscard]] bool emit_xmm_load(CodeWriter& writer, unsigned reg, unsigned disp) noexcept {
    const unsigned char modrm = static_cast<unsigned char>(0x44U | ((reg & 7U) << 3U));
    return writer.bytes({0x0F, 0x10, modrm, 0x24, static_cast<unsigned char>(disp)});
}

[[nodiscard]] bool emit_gpr_store(CodeWriter& writer,
                                  unsigned char rex,
                                  unsigned char modrm,
                                  std::uint32_t disp) noexcept {
    return writer.bytes({rex, 0x89, modrm, 0x24}) && writer.scalar(disp);
}

[[nodiscard]] bool emit_gpr_load(CodeWriter& writer,
                                 unsigned char rex,
                                 unsigned char modrm,
                                 std::uint32_t disp) noexcept {
    return writer.bytes({rex, 0x8B, modrm, 0x24}) && writer.scalar(disp);
}

[[nodiscard]] bool build_native_thunk(std::uint32_t siteIndex,
                                      NativeSiteRuntime& runtime) noexcept {
    auto* const allocation = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, kThunkAllocationSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (allocation == nullptr) {
        return false;
    }
    std::memset(allocation, 0, kThunkAllocationSize);

    std::span<unsigned char> code(allocation, kThunkCodeCapacity);
    CodeWriter writer(code);
    bool ok = true;
    // sub rsp, 0xD8 -- aligns the callback stack and preserves 32 bytes of shadow space.
    ok = ok && writer.bytes({0x48, 0x81, 0xEC})
         && writer.scalar(static_cast<std::uint32_t>(kThunkStackSize));
    for (unsigned reg = 0; reg < 6; ++reg) {
        ok = ok && emit_xmm_store(writer, reg, static_cast<unsigned>(kFrameXmm0 + reg * 0x10U));
    }
    ok = ok && emit_gpr_store(writer, 0x48, 0x84, static_cast<std::uint32_t>(kFrameRax));
    ok = ok && emit_gpr_store(writer, 0x48, 0x8C, static_cast<std::uint32_t>(kFrameRcx));
    ok = ok && emit_gpr_store(writer, 0x48, 0x94, static_cast<std::uint32_t>(kFrameRdx));
    ok = ok && emit_gpr_store(writer, 0x4C, 0x84, static_cast<std::uint32_t>(kFrameR8));
    ok = ok && emit_gpr_store(writer, 0x4C, 0x8C, static_cast<std::uint32_t>(kFrameR9));
    ok = ok && emit_gpr_store(writer, 0x4C, 0x94, static_cast<std::uint32_t>(kFrameR10));
    ok = ok && emit_gpr_store(writer, 0x4C, 0x9C, static_cast<std::uint32_t>(kFrameR11));

    // callback(siteIndex, frame=rsp, originalRsp=rsp+0xD8, returnAddress=*originalRsp)
    ok = ok && writer.byte(0xB9) && writer.scalar(siteIndex);
    ok = ok && writer.bytes({0x48, 0x8D, 0x14, 0x24});
    ok = ok && writer.bytes({0x4C, 0x8D, 0x84, 0x24})
         && writer.scalar(static_cast<std::uint32_t>(kThunkStackSize));
    ok = ok && writer.bytes({0x4D, 0x8B, 0x08});
    ok = ok && writer.bytes({0x48, 0xB8})
         && writer.scalar(reinterpret_cast<std::uintptr_t>(&sunrise_warden_authored_capture));
    ok = ok && writer.bytes({0xFF, 0xD0});

    for (unsigned reg = 0; reg < 6; ++reg) {
        ok = ok && emit_xmm_load(writer, reg, static_cast<unsigned>(kFrameXmm0 + reg * 0x10U));
    }
    ok = ok && emit_gpr_load(writer, 0x48, 0x84, static_cast<std::uint32_t>(kFrameRax));
    ok = ok && emit_gpr_load(writer, 0x48, 0x8C, static_cast<std::uint32_t>(kFrameRcx));
    ok = ok && emit_gpr_load(writer, 0x48, 0x94, static_cast<std::uint32_t>(kFrameRdx));
    ok = ok && emit_gpr_load(writer, 0x4C, 0x84, static_cast<std::uint32_t>(kFrameR8));
    ok = ok && emit_gpr_load(writer, 0x4C, 0x8C, static_cast<std::uint32_t>(kFrameR9));
    ok = ok && emit_gpr_load(writer, 0x4C, 0x94, static_cast<std::uint32_t>(kFrameR10));
    ok = ok && emit_gpr_load(writer, 0x4C, 0x9C, static_cast<std::uint32_t>(kFrameR11));
    ok = ok && writer.bytes({0x48, 0x81, 0xC4})
         && writer.scalar(static_cast<std::uint32_t>(kThunkStackSize));

    // jmp qword ptr [rip+disp32] -> embedded trampoline slot.
    const std::size_t jmpOffset = writer.used();
    ok = ok && writer.bytes({0xFF, 0x25});
    const std::int64_t next = static_cast<std::int64_t>(jmpOffset + 6U);
    const std::int64_t target = static_cast<std::int64_t>(kTrampolineSlotOffset);
    const std::int64_t displacement = target - next;
    if (displacement < (std::numeric_limits<std::int32_t>::min)()
        || displacement > (std::numeric_limits<std::int32_t>::max)()) {
        ok = false;
    }
    ok = ok && writer.scalar(static_cast<std::int32_t>(displacement));

    if (!ok || writer.used() >= kTrampolineSlotOffset) {
        (void)VirtualFree(allocation, 0, MEM_RELEASE);
        return false;
    }

    runtime.thunk = allocation;
    runtime.thunkCodeSize = writer.used();
    runtime.trampolineSlot = reinterpret_cast<std::uintptr_t*>(allocation + kTrampolineSlotOffset);
    *runtime.trampolineSlot = 0;

    // UNWIND_INFO for the only unwind-relevant prolog operation: sub rsp, 0xD8.
    auto* const unwind = allocation + kUnwindInfoOffset;
    unwind[0] = 0x01;
    unwind[1] = 0x07;
    unwind[2] = 0x02;
    unwind[3] = 0x00;
    unwind[4] = 0x07;
    unwind[5] = 0x01;
    unwind[6] = static_cast<unsigned char>((kThunkStackSize / 8U) & 0xFFU);
    unwind[7] = static_cast<unsigned char>(((kThunkStackSize / 8U) >> 8U) & 0xFFU);

    runtime.unwindEntry.BeginAddress = 0;
    runtime.unwindEntry.EndAddress = static_cast<DWORD>(runtime.thunkCodeSize);
    runtime.unwindEntry.UnwindData = kUnwindInfoOffset;
    if (RtlAddFunctionTable(&runtime.unwindEntry,
                            1,
                            reinterpret_cast<DWORD64>(runtime.thunk)) == FALSE) {
        (void)VirtualFree(allocation, 0, MEM_RELEASE);
        runtime.thunk = nullptr;
        runtime.trampolineSlot = nullptr;
        runtime.thunkCodeSize = 0;
        runtime.unwindEntry = {};
        return false;
    }
    runtime.unwindRegistered = true;
    (void)FlushInstructionCache(GetCurrentProcess(), runtime.thunk, runtime.thunkCodeSize);
    return true;
}

void release_native_thunk(NativeSiteRuntime& runtime) noexcept {
    if (runtime.unwindRegistered) {
        (void)RtlDeleteFunctionTable(&runtime.unwindEntry);
        runtime.unwindRegistered = false;
        runtime.unwindEntry = {};
    }
    if (runtime.thunk != nullptr) {
        (void)VirtualFree(runtime.thunk, 0, MEM_RELEASE);
    }
    runtime.thunk = nullptr;
    runtime.trampolineSlot = nullptr;
    runtime.thunkCodeSize = 0;
}

void log_native_attach(std::size_t index, const char* result) noexcept {
    std::array<char, 160> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=warden_authored stage=native_site site=%s target=+0x%08X result=%s",
                                      kNativeSites[index].name,
                                      kNativeSites[index].rva,
                                      result);
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         std::string_view(result) == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), length});
    }
}

[[nodiscard]] bool install_native_sites(std::size_t& attached) noexcept {
    attached = 0;
    for (std::size_t index = 0; index < kNativeSites.size(); ++index) {
        NativeSiteRuntime& runtime = g_nativeRuntime[index];
        if (runtime.hook.attached) {
            ++attached;
            continue;
        }
        release_native_thunk(runtime);
        auto* const target = reinterpret_cast<void*>(g_imageBase + kNativeSites[index].rva);
        if (!executable_address(target) || !build_native_thunk(static_cast<std::uint32_t>(index), runtime)) {
            log_native_attach(index, "prepare_failed");
            release_native_thunk(runtime);
            continue;
        }
        const hooking::detour::Spec spec{target, runtime.thunk};
        if (!hooking::detour::install(spec, runtime.hook)) {
            log_native_attach(index, "detour_failed");
            release_native_thunk(runtime);
            continue;
        }
        (void)InterlockedExchange64(
            reinterpret_cast<volatile LONG64*>(runtime.trampolineSlot),
            static_cast<LONG64>(reinterpret_cast<std::uintptr_t>(runtime.hook.original)));
        (void)FlushInstructionCache(GetCurrentProcess(), runtime.thunk, runtime.thunkCodeSize);
        runtime.hits.store(0, std::memory_order_release);
        runtime.firstLogged.store(false, std::memory_order_release);
        ++attached;
        log_native_attach(index, "ok");
    }
    // The slot-holder probe is observation-only. Both manager probes are required so the run
    // can prove whether the native provider/session activation path changes manager construction.
    return g_nativeRuntime[0].hook.attached && g_nativeRuntime[1].hook.attached
           && g_nativeRuntime[2].hook.attached;
}

[[nodiscard]] bool uninstall_native_sites() noexcept {
    bool success = true;
    for (std::size_t reverse = kNativeSites.size(); reverse != 0; --reverse) {
        NativeSiteRuntime& runtime = g_nativeRuntime[reverse - 1U];
        if (!runtime.hook.attached) {
            release_native_thunk(runtime);
            continue;
        }
        const std::array<hooking::detour::ProtectedCodeEntry, 1> protectedEntries{{
            hooking::detour::ProtectedCodeEntry{runtime.thunk}}};
        const hooking::detour::UninstallResult removed =
            hooking::detour::uninstall(runtime.hook, protectedEntries);
        if (removed != hooking::detour::UninstallResult::removed) {
            success = false;
            continue;
        }
        release_native_thunk(runtime);
    }
    return success;
}

/**
 * Calls the real native predicate first, then changes only Warden's first F24CF0 lane from the
 * already-committed result (1) to the alternate result (0). All other callers/results are native.
 */
std::int32_t __fastcall predicate_override(void* slot) noexcept {
    const NativeSlotPredicate original = g_predicateOriginal.load(std::memory_order_acquire);
    const std::int32_t native = original != nullptr ? original(slot) : 0;
    if (native != 1 || return_rva(_ReturnAddress()) != kArmPredicateLane0ReturnRva) {
        return native;
    }
    if (!g_active.load(std::memory_order_acquire)) {
        if (!forced_destination_is_warden()) {
            return native;
        }
        activate();
    }

    // The trigger gate is normally a sticky byte. Re-open it if another native path changed it
    // during the Warden transition, but do not touch it for any non-Warden predicate call.
    if (!g_latchForcedOpen.load(std::memory_order_acquire)) {
        (void)open_authored_gate();
    }

    const std::uint32_t count = g_overrideCount.fetch_add(1, std::memory_order_relaxed) + 1U;
    bool expected = false;
    if (g_firstOverrideLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::array<char, 176> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=warden_authored stage=arm_gate result=enabled lane=0 native=%d slot=0x%llX count=%u",
            native,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(slot)),
            count);
        if (written > 0) {
            const auto length = static_cast<std::size_t>(written) < line.size()
                                    ? static_cast<std::size_t>(written)
                                    : line.size() - 1;
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), length});
        }
    }
    return 0;
}

} // namespace

[[nodiscard]] bool active() noexcept {
    return g_active.load(std::memory_order_acquire);
}

bool install() noexcept {
    if (g_predicateHook.attached && g_nativeRuntime[0].hook.attached
        && g_nativeRuntime[1].hook.attached && g_nativeRuntime[2].hook.attached) {
        return true;
    }
    g_imageBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_imageBase == 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=warden_authored stage=install result=fail reason=image");
        return false;
    }

    std::size_t attached = 0;
    if (!install_native_sites(attached)) {
        (void)uninstall_native_sites();
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=warden_authored stage=install result=fail reason=native_activation_sites");
        return false;
    }

    auto* const target = reinterpret_cast<void*>(g_imageBase + kNativeSlotPredicateRva);
    if (!executable_address(target)) {
        (void)uninstall_native_sites();
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=warden_authored stage=install result=fail reason=predicate_target");
        return false;
    }
    if (!g_predicateHook.attached) {
        const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&predicate_override)};
        if (!hooking::detour::install(spec, g_predicateHook)) {
            (void)uninstall_native_sites();
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "ev=warden_authored stage=install result=fail reason=predicate_detour");
            return false;
        }
        g_predicateOriginal.store(reinterpret_cast<NativeSlotPredicate>(g_predicateHook.original),
                                  std::memory_order_release);
    }

    std::array<char, 192> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=warden_authored stage=install result=%s mode=native_provider_plus_kind22 sites=%zu/%zu",
        attached == kNativeSites.size() ? "ok" : "partial",
        attached,
        kNativeSites.size());
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         attached == kNativeSites.size() ? core::log::Level::info
                                                         : core::log::Level::warn,
                         {line.data(), length});
    }
    return true;
}

bool uninstall() noexcept {
    g_active.store(false, std::memory_order_release);
    bool predicateRemoved = true;
    if (g_predicateHook.attached) {
        const std::array<hooking::detour::ProtectedCodeEntry, 1> protectedEntries{{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&predicate_override)}}};
        predicateRemoved = hooking::detour::uninstall(g_predicateHook, protectedEntries)
                           == hooking::detour::UninstallResult::removed;
    }
    const bool sitesRemoved = uninstall_native_sites();
    if (predicateRemoved && sitesRemoved) {
        g_predicateOriginal.store(nullptr, std::memory_order_release);
        restore_authored_gate();
        g_imageBase = 0;
        g_overrideCount.store(0, std::memory_order_release);
        g_firstOverrideLogged.store(false, std::memory_order_release);
        reset_route_state();
    }
    return predicateRemoved && sitesRemoved;
}

void observe(std::string_view text) noexcept {
    const bool wardenLaunch = text.find(kWardenOverrideText) != std::string_view::npos
                              || (text.starts_with(kLaunchPrefix)
                                  && text.find(kWardenGrognok) != std::string_view::npos);
    if (wardenLaunch) {
        activate();
        return;
    }

    if (!g_active.load(std::memory_order_acquire)) {
        return;
    }

    if (text.find(kPosseSessionCreatedPrefix) != std::string_view::npos
        && text.find(kPosseSessionCreatedSuffix) != std::string_view::npos) {
        bool expected = false;
        if (g_posseSessionReady.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
            std::array<char, 208> line{};
            const int written = std::snprintf(
                line.data(),
                line.size(),
                "ev=warden_authored stage=session result=posse_ready seed_id=0x%llX full_seen=%u full_observations=%u late_candidates=%u",
                static_cast<unsigned long long>(g_launchRecordId.load(std::memory_order_relaxed)),
                g_fullRecordSeen.load(std::memory_order_relaxed) ? 1U : 0U,
                g_fullRecordObservationCount.load(std::memory_order_relaxed),
                g_lateKindOneCandidateCount.load(std::memory_order_relaxed));
            if (written > 0) {
                const auto length = static_cast<std::size_t>(written) < line.size()
                                        ? static_cast<std::size_t>(written)
                                        : line.size() - 1;
                core::log::write(core::log::Channel::client,
                                 core::log::Level::info,
                                 {line.data(), length});
            }
        }
        return;
    }

    // Returning to Orbit is the normal teardown. A different activity launch is also a hard scope
    // boundary so the override can never leak into another destination after an aborted Warden run.
    if (text.find(kOrbitTransitionText) != std::string_view::npos
        || (text.starts_with(kLaunchPrefix)
            && text.find(kWardenGrognok) == std::string_view::npos)) {
        deactivate();
    }
}

} // namespace warden_authored_enable

bool install_warden_authored_path() noexcept {
    return warden_authored_enable::install();
}

bool uninstall_warden_authored_path() noexcept {
    return warden_authored_enable::uninstall();
}

void observe_warden_authored_path(std::string_view text) noexcept {
    warden_authored_enable::observe(text);
}

bool warden_authored_path_active() noexcept {
    return warden_authored_enable::active();
}

} // namespace sunrise::client::hooks::retail_log
