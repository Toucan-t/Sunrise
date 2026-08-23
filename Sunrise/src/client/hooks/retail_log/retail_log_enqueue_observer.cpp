#include "retail_log_enqueue_observer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../diagnostics/module_range.h"
#include "../../targets/game.h"

namespace sunrise::client::hooks::retail_log {
namespace {

using Enqueue = void(__fastcall*)(std::int32_t, const char*) noexcept;
using SetCategoryVerbosity = void(__fastcall*)(std::int32_t, std::uint32_t) noexcept;
using RegisterSecurityKey = bool(__fastcall*)(void*, const void*, const void*) noexcept;

constexpr std::size_t kNativeTextSize = 320;
constexpr std::int32_t kUnregisteredSite = -1;
constexpr std::size_t kEventCapacity = kNativeTextSize + 64;
constexpr std::uint64_t kReassertIntervalMs = 2'000;
constexpr std::uint32_t kCategoryCount = 26;
constexpr std::uint32_t kMostVerbose = 0;

/** Warden authored-lifecycle boundaries retained by the current native probes. */
constexpr std::string_view kActivitySelectionLaunchText =
    "world_controller:activity_selection_manager: Launching activity-selection";
constexpr std::string_view kWardenActivityMarker = "(grognok: strike_aries)";
constexpr std::string_view kSetupOrbitEnterText =
    "world_controller:state_manager: Entering state 'setup:orbit'";

/** Native Demonware key-map literals proven by ordinary successful citizen joins. */
constexpr std::string_view kSecurityPutIdText{"Putting bdSecurityID:"};
constexpr std::string_view kSecurityPutKeyText{"Putting bdSecurityKey:"};
constexpr std::size_t kSecurityUnwindDepth = 48;
constexpr std::size_t kSecurityFunctionScanLimit = 64U * 1024U;
constexpr std::uint32_t kSecurityLearnAttemptLimit = 8;
constexpr std::size_t kSecurityDescriptorKeyOffset = 0x5E;

/** Native channel-manager retry path recovered from the successful timeout reset. */
constexpr std::uintptr_t kChannelManagerUpdateRva = 0x0180BDA0U;
constexpr std::uintptr_t kResecureOwnersRva = 0x018016F0U;
constexpr std::uintptr_t kSecureDesiredRva = 0x017E9A80U;
constexpr std::size_t kChannelZeroOffset = 0xA8U;
constexpr std::size_t kChannelSecurityInterfaceOffset = 0x3150U;
constexpr std::size_t kChannelManagerActiveMaskAOffset = 0x1005C0U;
constexpr std::size_t kChannelManagerActiveMaskBOffset = 0x1005C8U;
constexpr std::size_t kSecureDesiredVtableOffset = 0x98U;
constexpr std::size_t kResecureOwnersVtableOffset = 0xC0U;
constexpr std::uint32_t kResecureAttemptLimit = 32U;

/**
 * Exact +0x180BDA0 native manager-update body captured from this pinned client build. The retry
 * hook is refused unless every byte still matches, preventing a fixed RVA from being used on an
 * unexpected executable or on top of another detour.
 */
constexpr std::array<std::uint8_t, 162> kChannelManagerUpdatePreimage{
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
    0x48, 0x83, 0xEC, 0x20, 0x83, 0xCD, 0xFF, 0x48, 0x8B, 0xF9, 0x33, 0xF6, 0x0F, 0x1F, 0x40, 0x00,
    0x8B, 0x9F, 0xB4, 0x05, 0x10, 0x00, 0xB8, 0x43, 0x08, 0x21, 0x84, 0xFF, 0xC3, 0x03, 0xDE, 0xF7,
    0xEB, 0x03, 0xD3, 0xC1, 0xFA, 0x05, 0x8B, 0xC2, 0xC1, 0xE8, 0x1F, 0x03, 0xD0, 0x6B, 0xC2, 0x3E,
    0x2B, 0xD8, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x8B, 0xCB, 0x48, 0xD3, 0xE0, 0x48, 0x85, 0x87, 0xC0,
    0x05, 0x10, 0x00, 0x74, 0x13, 0x83, 0xFD, 0xFF, 0x75, 0x21, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8,
    0x3C, 0x67, 0xFF, 0xFF, 0x8B, 0xEB, 0xEB, 0x13, 0x48, 0x85, 0x87, 0xC8, 0x05, 0x10, 0x00, 0x74,
    0x0A, 0x8B, 0xD3, 0x48, 0x8B, 0xCF, 0xE8, 0x25, 0x67, 0xFF, 0xFF, 0xFF, 0xC6, 0x83, 0xFE, 0x3E,
    0x7C, 0x9E, 0x83, 0xFD, 0xFF, 0x74, 0x06, 0x89, 0xAF, 0xB4, 0x05, 0x10, 0x00, 0x48, 0x8B, 0x5C,
    0x24, 0x30, 0x48, 0x8B, 0x6C, 0x24, 0x38, 0x48, 0x8B, 0x74, 0x24, 0x40, 0x48, 0x83, 0xC4, 0x20,
    0x5F, 0xC3,
};

/**
 * Exact bytes immediately before the native CALL to bdSecurityKeyMap::registerKey in the
 * successful descriptor-registration path captured from this client build. The final five-byte
 * CALL is verified separately so its RIP-relative displacement remains ASLR-independent.
 *
 *   mov    rcx,[rdi]         ; descriptor + 0x00 -> bdSecurityID
 *   lea    r8,[rsp+28h]
 *   movups xmm0,[rdi+5Eh]    ; descriptor + 0x5E -> 16-byte bdSecurityKey
 *   mov    [rsp+20h],rcx
 *   lea    rdx,[rsp+20h]
 *   mov    rcx,rbx           ; owning bdSecurityKeyMap*
 *   movups [rsp+28h],xmm0
 */
constexpr std::array<std::uint8_t, 30> kSecurityCallerPrefix{
    0x48, 0x8B, 0x0F,
    0x4C, 0x8D, 0x44, 0x24, 0x28,
    0x0F, 0x10, 0x47, 0x5E,
    0x48, 0x89, 0x4C, 0x24, 0x20,
    0x48, 0x8D, 0x54, 0x24, 0x20,
    0x48, 0x8B, 0xCB,
    0x0F, 0x11, 0x44, 0x24, 0x28,
};

thread_local bool g_inObserver{};
volatile LONG64 g_nextAssertTick{};
std::atomic_bool g_wardenAuthoredPathActive{false};
std::atomic<std::uintptr_t> g_securityRegisterKey{};
std::atomic<std::uintptr_t> g_securityKeyMap{};
std::atomic_uint32_t g_securityLearnAttempts{};
SRWLOCK g_resecureHookLock{SRWLOCK_INIT};
hooking::detour::Handle g_resecureUpdateHandle{};
using ChannelManagerUpdate = void(__fastcall*)(void*) noexcept;
using ResecureOwners = void(__fastcall*)(void*) noexcept;
std::atomic<ChannelManagerUpdate> g_resecureUpdateOriginal{nullptr};
std::atomic<std::uintptr_t> g_resecureManager{};
std::atomic_bool g_resecurePending{};
std::atomic_uint32_t g_resecureAttempts{};

/** @param protect VirtualQuery protection flags. @return True when a normal read is permitted. */
[[nodiscard]] bool readable_protection(DWORD protect) noexcept {
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protect & 0xFFU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

/** @return True when the protection permits writes to an already-committed object. */
[[nodiscard]] bool writable_protection(DWORD protect) noexcept {
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    switch (protect & 0xFFU) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

/** Advances one x64 unwind context to its caller. */
[[nodiscard]] bool unwind_one(CONTEXT& context) noexcept {
    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
    if (function != nullptr) {
        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        (void)RtlVirtualUnwind(UNW_FLAG_NHANDLER,
                               imageBase,
                               context.Rip,
                               function,
                               &context,
                               &handlerData,
                               &establisherFrame,
                               nullptr);
        return context.Rip != 0;
    }
    __try {
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += sizeof(DWORD64);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        context.Rip = 0;
    }
    return context.Rip != 0;
}

/** Writes one compact diagnostic without re-entering the native retail logger. */
void security_log(core::log::Level level, const char* format, ...) noexcept {
    if (!core::log::accepts(core::log::Channel::client, level)) {
        return;
    }
    std::array<char, 768> line{};
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line.data(), line.size(), format, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                   ? static_cast<std::size_t>(written)
                                   : line.size() - 1U;
    core::log::write(core::log::Channel::client, level, {line.data(), length});
}

/** Looks up the native unwind range containing one instruction pointer. */
[[nodiscard]] bool runtime_function_range(std::uintptr_t pc,
                                          std::uintptr_t& begin,
                                          std::uintptr_t& end) noexcept {
    DWORD64 imageBase = 0;
    const PRUNTIME_FUNCTION function =
        RtlLookupFunctionEntry(static_cast<DWORD64>(pc), &imageBase, nullptr);
    if (function == nullptr) {
        return false;
    }
    begin = static_cast<std::uintptr_t>(imageBase + function->BeginAddress);
    end = static_cast<std::uintptr_t>(imageBase + function->EndAddress);
    return end > begin && end - begin <= kSecurityFunctionScanLimit;
}

/** Tests whether readable memory begins with a requested native format-string literal. */
[[nodiscard]] bool memory_starts_with(std::uintptr_t address, std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) == 0
        || memory.State != MEM_COMMIT || !readable_protection(memory.Protect)) {
        return false;
    }
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t regionEnd = regionBegin + memory.RegionSize;
    if (address < regionBegin || regionEnd < address || text.size() > regionEnd - address) {
        return false;
    }
    bool matches = false;
    __try {
        matches = std::memcmp(reinterpret_cast<const void*>(address), text.data(), text.size()) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        matches = false;
    }
    return matches;
}

/** Finds a RIP-relative LEA in one native function that names a requested literal. */
[[nodiscard]] bool function_references_text(std::uintptr_t begin,
                                            std::uintptr_t end,
                                            std::string_view text) noexcept {
    if (end <= begin || end - begin > kSecurityFunctionScanLimit) {
        return false;
    }
    std::vector<std::byte> bytes(end - begin);
    SIZE_T copied = 0;
    if (ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void*>(begin),
                          bytes.data(),
                          bytes.size(),
                          &copied) == FALSE
        || copied != bytes.size()) {
        return false;
    }
    for (std::size_t index = 0; index + 7U <= bytes.size(); ++index) {
        const auto b0 = std::to_integer<std::uint8_t>(bytes[index]);
        if ((b0 & 0xF8U) != 0x48U
            || std::to_integer<std::uint8_t>(bytes[index + 1]) != 0x8DU
            || (std::to_integer<std::uint8_t>(bytes[index + 2]) & 0xC7U) != 0x05U) {
            continue;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, bytes.data() + index + 3U, sizeof(displacement));
        const std::uintptr_t next = begin + index + 7U;
        const auto target = static_cast<std::uintptr_t>(
            static_cast<std::intptr_t>(next) + static_cast<std::intptr_t>(displacement));
        if (memory_starts_with(target, text)) {
            return true;
        }
    }
    return false;
}

/** @return True when a candidate is a normal writable pointer, without dereferencing it. */
[[nodiscard]] bool writable_pointer(std::uintptr_t pointer) noexcept {
    if (pointer < 0x10000U || (pointer & (alignof(void*) - 1U)) != 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(pointer), &memory, sizeof(memory)) == 0
        || memory.State != MEM_COMMIT || !writable_protection(memory.Protect)) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    const std::uintptr_t end = begin + memory.RegionSize;
    return pointer >= begin && pointer < end;
}

/** Reads exactly one small code window from this process. */
[[nodiscard]] bool read_exact(std::uintptr_t address, void* output, std::size_t size) noexcept {
    if (address == 0 || output == nullptr || size == 0) {
        return false;
    }
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(address),
                             output,
                             size,
                             &copied)
               != FALSE
           && copied == size;
}

struct ResecureTarget final {
    void* securityInterface{};
    std::uintptr_t desiredMethod{};
    std::uintptr_t resecureMethod{};
    std::uint64_t activeMaskA{};
    std::uint64_t activeMaskB{};
};

/** Reads one pointer-sized value without directly dereferencing untrusted native state. */
[[nodiscard]] bool read_pointer(std::uintptr_t address, std::uintptr_t& value) noexcept {
    value = 0;
    return read_exact(address, &value, sizeof(value));
}

/**
 * Resolves channel zero's native security interface and verifies both virtual methods recovered
 * from the captured reset path. Nothing is written during validation.
 */
[[nodiscard]] bool resolve_resecure_target(void* manager, ResecureTarget& output) noexcept {
    output = {};
    const auto managerAddress = reinterpret_cast<std::uintptr_t>(manager);
    diagnostics::ModuleRange game{};
    if (managerAddress < 0x10000U || !writable_pointer(managerAddress)
        || !diagnostics::module_range(GetModuleHandleW(nullptr), game)
        || managerAddress > (std::numeric_limits<std::uintptr_t>::max)()
                                 - kChannelZeroOffset - kChannelSecurityInterfaceOffset) {
        return false;
    }

    const std::uintptr_t channel = managerAddress + kChannelZeroOffset;
    std::uintptr_t security = 0;
    if (!writable_pointer(channel)
        || !read_pointer(channel + kChannelSecurityInterfaceOffset, security)
        || !writable_pointer(security)) {
        return false;
    }

    std::uintptr_t vtable = 0;
    std::uintptr_t desiredMethod = 0;
    std::uintptr_t resecureMethod = 0;
    if (!read_pointer(security, vtable) || !diagnostics::contains(game, vtable)
        || !read_pointer(vtable + kSecureDesiredVtableOffset, desiredMethod)
        || !read_pointer(vtable + kResecureOwnersVtableOffset, resecureMethod)
        || desiredMethod != game.base + kSecureDesiredRva
        || resecureMethod != game.base + kResecureOwnersRva) {
        return false;
    }

    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!runtime_function_range(resecureMethod, begin, end) || begin != resecureMethod) {
        return false;
    }
    if (!runtime_function_range(desiredMethod, begin, end) || begin != desiredMethod) {
        return false;
    }

    (void)read_exact(managerAddress + kChannelManagerActiveMaskAOffset,
                     &output.activeMaskA,
                     sizeof(output.activeMaskA));
    (void)read_exact(managerAddress + kChannelManagerActiveMaskBOffset,
                     &output.activeMaskB,
                     sizeof(output.activeMaskB));
    output.securityInterface = reinterpret_cast<void*>(security);
    output.desiredMethod = desiredMethod;
    output.resecureMethod = resecureMethod;
    return true;
}

/** Invokes only the recovered native owner-resecure virtual method, on its manager update thread. */
[[nodiscard]] bool invoke_pending_resecure(void* manager) noexcept {
    if (!g_resecurePending.load(std::memory_order_acquire)) {
        return false;
    }

    const auto managerAddress = reinterpret_cast<std::uintptr_t>(manager);
    const std::uintptr_t learnedManager = g_resecureManager.load(std::memory_order_acquire);
    if (learnedManager != 0 && learnedManager != managerAddress) {
        return false;
    }

    ResecureTarget target{};
    if (!resolve_resecure_target(manager, target)) {
        return false;
    }
    if (learnedManager == 0) {
        std::uintptr_t expected = 0;
        if (g_resecureManager.compare_exchange_strong(
                expected, managerAddress, std::memory_order_acq_rel, std::memory_order_acquire)) {
            security_log(core::log::Level::info,
                         "ev=native_secure_resecure stage=manager result=learned manager=0x%llX "
                         "channel=0 security=0x%llX",
                         static_cast<unsigned long long>(managerAddress),
                         static_cast<unsigned long long>(
                             reinterpret_cast<std::uintptr_t>(target.securityInterface)));
        } else if (expected != managerAddress) {
            return false;
        }
    }

    const std::uint32_t attempt =
        g_resecureAttempts.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    if (attempt > kResecureAttemptLimit) {
        if (g_resecurePending.exchange(false, std::memory_order_acq_rel)) {
            security_log(core::log::Level::warn,
                         "ev=native_secure_resecure stage=invoke result=abandoned "
                         "reason=target_unresolved attempts=%u",
                         attempt - 1U);
        }
        return false;
    }

    bool invoked = false;
    __try {
        reinterpret_cast<ResecureOwners>(target.resecureMethod)(target.securityInterface);
        invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        invoked = false;
    }

    g_resecurePending.store(false, std::memory_order_release);
    diagnostics::ModuleRange game{};
    (void)diagnostics::module_range(GetModuleHandleW(nullptr), game);
    security_log(invoked ? core::log::Level::info : core::log::Level::warn,
                 "ev=native_secure_resecure stage=invoke result=%s manager=0x%llX channel=0 "
                 "security=0x%llX method=+0x%llX active_a=0x%llX active_b=0x%llX attempt=%u",
                 invoked ? "ok" : "fault",
                 static_cast<unsigned long long>(managerAddress),
                 static_cast<unsigned long long>(
                     reinterpret_cast<std::uintptr_t>(target.securityInterface)),
                 static_cast<unsigned long long>(
                     game.base != 0 && target.resecureMethod >= game.base
                         ? target.resecureMethod - game.base
                         : 0U),
                 static_cast<unsigned long long>(target.activeMaskA),
                 static_cast<unsigned long long>(target.activeMaskB),
                 attempt);
    return invoked;
}

/** Behavior-preserving manager-update detour that services one pending migrated resecure first. */
__declspec(noinline) void __fastcall channel_manager_update_body(void* manager) noexcept {
    if (g_resecurePending.load(std::memory_order_acquire)) {
        (void)invoke_pending_resecure(manager);
    } else if (g_resecureManager.load(std::memory_order_acquire) == 0) {
        ResecureTarget target{};
        if (resolve_resecure_target(manager, target)) {
            std::uintptr_t expected = 0;
            const auto managerAddress = reinterpret_cast<std::uintptr_t>(manager);
            if (g_resecureManager.compare_exchange_strong(
                    expected, managerAddress, std::memory_order_acq_rel, std::memory_order_acquire)) {
                security_log(core::log::Level::debug,
                             "ev=native_secure_resecure stage=manager result=learned manager=0x%llX "
                             "channel=0 security=0x%llX",
                             static_cast<unsigned long long>(managerAddress),
                             static_cast<unsigned long long>(
                                 reinterpret_cast<std::uintptr_t>(target.securityInterface)));
            }
        }
    }

    const ChannelManagerUpdate original =
        g_resecureUpdateOriginal.load(std::memory_order_acquire);
    if (original != nullptr) {
        original(manager);
    }
}

/** Installs the single pinned manager-update hook only when the full captured preimage matches. */
[[nodiscard]] bool install_native_resecure_hook() noexcept {
    if (g_resecureUpdateOriginal.load(std::memory_order_acquire) != nullptr) {
        return true;
    }

    AcquireSRWLockExclusive(&g_resecureHookLock);
    if (g_resecureUpdateOriginal.load(std::memory_order_relaxed) != nullptr) {
        ReleaseSRWLockExclusive(&g_resecureHookLock);
        return true;
    }

    diagnostics::ModuleRange game{};
    bool resolved = diagnostics::module_range(GetModuleHandleW(nullptr), game);
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    const std::uintptr_t target = resolved ? game.base + kChannelManagerUpdateRva : 0U;
    std::array<std::uint8_t, kChannelManagerUpdatePreimage.size()> current{};
    resolved = resolved && diagnostics::contains(game, target)
               && runtime_function_range(target, begin, end) && begin == target
               && end - begin == kChannelManagerUpdatePreimage.size()
               && read_exact(target, current.data(), current.size())
               && current == kChannelManagerUpdatePreimage;

    bool installed = false;
    if (resolved) {
        const hooking::detour::Spec spec{reinterpret_cast<void*>(target),
                                         reinterpret_cast<void*>(&channel_manager_update_body)};
        installed = hooking::detour::install(spec, g_resecureUpdateHandle);
        if (installed) {
            g_resecureUpdateOriginal.store(
                reinterpret_cast<ChannelManagerUpdate>(g_resecureUpdateHandle.original),
                std::memory_order_release);
        }
    }
    ReleaseSRWLockExclusive(&g_resecureHookLock);

    security_log(installed ? core::log::Level::info : core::log::Level::warn,
                 "ev=native_secure_resecure stage=install result=%s update=+0x%llX "
                 "preimage=%u mode=native_owner_reset",
                 installed ? "ok" : resolved ? "detour_fail" : "preimage_mismatch",
                 static_cast<unsigned long long>(kChannelManagerUpdateRva),
                 resolved ? 1U : 0U);
    return installed;
}

/** Detaches the manager-update hook before Sunrise code can unload. */
void uninstall_native_resecure_hook() noexcept {
    AcquireSRWLockExclusive(&g_resecureHookLock);
    bool removed = true;
    if (g_resecureUpdateHandle.attached) {
        const std::array<hooking::detour::ProtectedCodeEntry, 1> protectedEntries{
            hooking::detour::ProtectedCodeEntry{
                reinterpret_cast<void*>(&channel_manager_update_body)}};
        removed = hooking::detour::uninstall(g_resecureUpdateHandle, protectedEntries)
                  == hooking::detour::UninstallResult::removed;
    }
    if (removed) {
        g_resecureUpdateOriginal.store(nullptr, std::memory_order_release);
        g_resecureManager.store(0, std::memory_order_release);
        g_resecurePending.store(false, std::memory_order_release);
        g_resecureAttempts.store(0, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_resecureHookLock);

    security_log(removed ? core::log::Level::debug : core::log::Level::warn,
                 "ev=native_secure_resecure stage=uninstall result=%s",
                 removed ? "ok" : "fail");
}

/**
 * Validates the exact captured caller and extracts its live bdSecurityKeyMap* from RBX. Rtl unwind
 * restores nonvolatile RBX to the value held by the caller at the native registerKey CALL site.
 */
[[nodiscard]] bool validate_security_caller(const CONTEXT& caller,
                                            std::uintptr_t registerKey,
                                            const diagnostics::ModuleRange& game,
                                            std::uintptr_t& keyMap) noexcept {
    const auto returnAddress = static_cast<std::uintptr_t>(caller.Rip);
    constexpr std::size_t kCallSize = 5;
    constexpr std::size_t kWindowSize = kSecurityCallerPrefix.size() + kCallSize;
    if (!diagnostics::contains(game, returnAddress) || returnAddress < game.base + kWindowSize) {
        return false;
    }

    std::array<std::uint8_t, kWindowSize> code{};
    if (!read_exact(returnAddress - kWindowSize, code.data(), code.size())
        || std::memcmp(code.data(), kSecurityCallerPrefix.data(), kSecurityCallerPrefix.size()) != 0
        || code[kSecurityCallerPrefix.size()] != 0xE8U) {
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement,
                code.data() + kSecurityCallerPrefix.size() + 1U,
                sizeof(displacement));
    const auto target = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(returnAddress) + static_cast<std::intptr_t>(displacement));
    if (target != registerKey) {
        return false;
    }

    const auto map = static_cast<std::uintptr_t>(caller.Rbx);
    if (!writable_pointer(map) || map > (std::numeric_limits<std::uintptr_t>::max)() - 0x28U
        || !writable_pointer(map + 0x08U) || !writable_pointer(map + 0x28U)) {
        return false;
    }
    keyMap = map;
    return true;
}

/**
 * Learns the exact native registerKey function and its owning map from Destiny's own successful
 * descriptor-registration call. No container internals are written or inferred: the function is
 * identified by its two native log literals, and the caller is accepted only when its instruction
 * bytes prove descriptor+0 is the ID, descriptor+0x5E is the 16-byte key, RCX is RBX, and its CALL
 * resolves back to that exact registerKey body.
 */
void learn_native_security_registration() noexcept {
    if (g_securityRegisterKey.load(std::memory_order_acquire) != 0
        && g_securityKeyMap.load(std::memory_order_acquire) != 0) {
        return;
    }
    const std::uint32_t attempt =
        g_securityLearnAttempts.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    if (attempt > kSecurityLearnAttemptLimit) {
        return;
    }

    diagnostics::ModuleRange game{};
    if (!diagnostics::module_range(GetModuleHandleW(nullptr), game)) {
        if (attempt == kSecurityLearnAttemptLimit) {
            security_log(core::log::Level::warn,
                         "ev=native_security_map stage=learn result=unresolved reason=module "
                         "attempts=%u",
                         attempt);
        }
        return;
    }

    CONTEXT context{};
    RtlCaptureContext(&context);
    for (std::size_t depth = 0; depth < kSecurityUnwindDepth && context.Rip != 0; ++depth) {
        const auto pc = static_cast<std::uintptr_t>(context.Rip);
        if (diagnostics::contains(game, pc)) {
            std::uintptr_t begin = 0;
            std::uintptr_t end = 0;
            if (runtime_function_range(pc, begin, end)
                && function_references_text(begin, end, kSecurityPutIdText)
                && function_references_text(begin, end, kSecurityPutKeyText)) {
                CONTEXT caller = context;
                std::uintptr_t keyMap = 0;
                if (unwind_one(caller) && validate_security_caller(caller, begin, game, keyMap)) {
                    g_securityKeyMap.store(keyMap, std::memory_order_release);
                    g_securityRegisterKey.store(begin, std::memory_order_release);
                    const auto callerReturn = static_cast<std::uintptr_t>(caller.Rip);
                    security_log(core::log::Level::info,
                                 "ev=native_security_map stage=learn result=ok attempt=%u "
                                 "register_key=+0x%llX caller_ret=+0x%llX map=0x%llX "
                                 "descriptor=0x%llX key_offset=%zu",
                                 attempt,
                                 static_cast<unsigned long long>(begin - game.base),
                                 static_cast<unsigned long long>(callerReturn - game.base),
                                 static_cast<unsigned long long>(keyMap),
                                 static_cast<unsigned long long>(caller.Rdi),
                                 kSecurityDescriptorKeyOffset);
                    (void)install_native_resecure_hook();
                    return;
                }
                security_log(core::log::Level::debug,
                             "ev=native_security_map stage=learn result=skip reason=caller_signature "
                             "attempt=%u register_key=+0x%llX",
                             attempt,
                             static_cast<unsigned long long>(begin - game.base));
                return;
            }
        }
        if (!unwind_one(context)) {
            break;
        }
    }

    if (attempt == kSecurityLearnAttemptLimit) {
        security_log(core::log::Level::warn,
                     "ev=native_security_map stage=learn result=unresolved reason=fingerprint "
                     "attempts=%u",
                     attempt);
    }
}

/** Tracks only the lifecycle predicate required by the current Warden authored probes. */
void observe_warden_authored_lifecycle(std::string_view text) noexcept {
    if (text.find(kActivitySelectionLaunchText) != std::string_view::npos
        && text.find(kWardenActivityMarker) != std::string_view::npos) {
        g_resecurePending.store(false, std::memory_order_release);
        g_resecureAttempts.store(0, std::memory_order_release);
        g_wardenAuthoredPathActive.store(true, std::memory_order_release);
        return;
    }
    if (text.find(kSetupOrbitEnterText) != std::string_view::npos) {
        g_wardenAuthoredPathActive.store(false, std::memory_order_release);
        g_resecurePending.store(false, std::memory_order_release);
        g_resecureAttempts.store(0, std::memory_order_release);
    }
}

/** Copies one native retail line into printable fixed storage. */
[[nodiscard]] std::size_t sanitize(const char* text,
                                   std::array<char, kNativeTextSize>& output) noexcept {
    std::size_t length = 0;
    __try {
        for (; length < kNativeTextSize - 1U && text[length] != '\0'; ++length) {
            const char value = text[length];
            output[length] = value >= ' ' && value != '\x7F' ? value : ' ';
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    while (length != 0 && output[length - 1U] == ' ') {
        --length;
    }
    return length;
}

/** Writes one captured native retail line and handles the retained lifecycle/security learners. */
void capture_line(std::int32_t siteId, const char* text) noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::info)) {
        return;
    }
    std::array<char, kNativeTextSize> sanitized{};
    const std::size_t textLength = sanitize(text, sanitized);
    const std::string_view sanitizedText{sanitized.data(), textLength};
    observe_warden_authored_lifecycle(sanitizedText);

    std::array<char, kEventCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=retail site=%d text=%.*s",
                                      siteId,
                                      static_cast<int>(textLength),
                                      sanitized.data());
    if (written > 0) {
        const std::size_t length = static_cast<std::size_t>(written) < line.size()
                                       ? static_cast<std::size_t>(written)
                                       : line.size() - 1U;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), length});
    }

    if (sanitizedText.find(kSecurityPutIdText) != std::string_view::npos) {
        learn_native_security_registration();
    }
}

/** Mirrors the single funnel every retail log line passes through. */
__declspec(noinline) void __fastcall enqueue_body(std::int32_t siteId, const char* text) noexcept {
    const bool outer = !g_inObserver;
    g_inObserver = true;
    const auto call = reinterpret_cast<Enqueue>(g_handle.original);
    if (call != nullptr) {
        call(siteId, text);
    }
    if (outer) {
        if (siteId != kUnregisteredSite && text != nullptr) {
            capture_line(siteId, text);
        }
        assert_verbosity();
        g_inObserver = false;
    }
}

} // namespace

/** Invokes Destiny's exact learned bdSecurityKeyMap::registerKey path for native kind-22. */
NativeSecurityRegistrationResult register_migrated_security_key(
    const std::array<std::byte, 8>& securityId,
    const std::array<std::byte, 16>& securityKey) noexcept {
    NativeSecurityRegistrationResult result{};
    const std::uintptr_t function = g_securityRegisterKey.load(std::memory_order_acquire);
    const std::uintptr_t map = g_securityKeyMap.load(std::memory_order_acquire);

    diagnostics::ModuleRange game{};
    std::uintptr_t functionBegin = 0;
    std::uintptr_t functionEnd = 0;
    const bool functionValid = function != 0
                               && diagnostics::module_range(GetModuleHandleW(nullptr), game)
                               && diagnostics::contains(game, function)
                               && runtime_function_range(function, functionBegin, functionEnd)
                               && functionBegin == function;
    const bool mapValid = map != 0 && writable_pointer(map)
                          && map <= (std::numeric_limits<std::uintptr_t>::max)() - 0x28U
                          && writable_pointer(map + 0x08U) && writable_pointer(map + 0x28U);
    result.ready = functionValid && mapValid;
    if (!result.ready) {
        security_log(core::log::Level::warn,
                     "ev=native_security_map stage=migration_register result=unavailable "
                     "function=%s map=%s attempts=%u",
                     functionValid ? "ready" : "missing",
                     mapValid ? "ready" : "missing",
                     g_securityLearnAttempts.load(std::memory_order_acquire));
        return result;
    }

    struct alignas(8) NativeSecurityId {
        std::array<std::byte, 8> bytes{};
    } id{securityId};
    struct alignas(16) NativeSecurityKey {
        std::array<std::byte, 16> bytes{};
    } key{securityKey};

    const auto call = reinterpret_cast<RegisterSecurityKey>(function);
    __try {
        result.inserted = call(reinterpret_cast<void*>(map), &id, &key);
        result.invoked = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result.invoked = false;
        result.inserted = false;
    }

    security_log(result.invoked && result.inserted ? core::log::Level::info
                                                    : core::log::Level::warn,
                 "ev=native_security_map stage=migration_register result=%s inserted=%u "
                 "register_key=+0x%llX map=0x%llX",
                 result.invoked ? "invoked" : "fault",
                 result.inserted ? 1U : 0U,
                 static_cast<unsigned long long>(function - game.base),
                 static_cast<unsigned long long>(map));
    if (result.invoked && result.inserted
        && g_wardenAuthoredPathActive.load(std::memory_order_acquire)) {
        const bool retryReady = install_native_resecure_hook();
        g_resecureAttempts.store(0, std::memory_order_release);
        g_resecurePending.store(retryReady, std::memory_order_release);
        security_log(retryReady ? core::log::Level::info : core::log::Level::warn,
                     "ev=native_secure_resecure stage=arm result=%s security_registered=1 "
                     "channel=0 mode=native_owner_reset",
                     retryReady ? "ok" : "unavailable");
    }
    return result;
}

/** @return True while the observed Warden authored activity lifecycle is active. */
bool warden_authored_path_active() noexcept {
    return g_wardenAuthoredPathActive.load(std::memory_order_acquire);
}

/** The retired Warden branch experiment no longer modifies code; retained for lifecycle ABI. */
void restore_warden_private_fast_path() noexcept {
    g_wardenAuthoredPathActive.store(false, std::memory_order_release);
    g_resecurePending.store(false, std::memory_order_release);
    g_resecureAttempts.store(0, std::memory_order_release);
    uninstall_native_resecure_hook();
}

/** Retired descriptor diagnostic compatibility shim. */
void register_gameplay_join_descriptor(const std::byte*,
                                       std::size_t,
                                       std::int32_t,
                                       std::uint64_t) noexcept {}

/** Retired schema-reference diagnostic compatibility shim. */
void register_schema_marker(std::uint32_t) noexcept {}

/** @return The enqueue observer body itself. */
void* enqueue_entry_point() noexcept {
    return reinterpret_cast<void*>(&enqueue_body);
}

/** Opens every native retail-log category at the configured debug level. */
void assert_verbosity() noexcept {
    if (!core::log::accepts(core::log::Channel::client, core::log::Level::debug)) {
        return;
    }
    const auto now = static_cast<LONG64>(GetTickCount64());
    const LONG64 due = g_nextAssertTick;
    if (now < due) {
        return;
    }
    if (InterlockedCompareExchange64(
            &g_nextAssertTick, now + static_cast<LONG64>(kReassertIntervalMs), due)
        != due) {
        return;
    }
    const auto setter = reinterpret_cast<SetCategoryVerbosity>(
        targets::game::retail_log::get().setCategoryVerbosity);
    if (setter == nullptr) {
        return;
    }
    for (std::uint32_t category = 0; category < kCategoryCount; ++category) {
        setter(static_cast<std::int32_t>(category), kMostVerbose);
    }
}

} // namespace sunrise::client::hooks::retail_log
