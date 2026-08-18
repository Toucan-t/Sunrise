#include "bubble_authority_replacements.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../../../../core/logging/log.h"
#include "../coordinator/network_call_coordinator.h"
#include "../platform.h"
#include "scope/bubble_authority_scope.h"

namespace sunrise::client::hooks::network::bubble_authority {
namespace {

/** Log the first forced build-state arm once; msg-5 argument diagnostics are separately bounded. */
std::atomic_bool g_decoderSeen{false};
std::atomic_bool g_forcedSeen{false};
std::atomic<unsigned> g_msg5Reports{0};

/** A Garden World's measured authored roster group. Used only as a byte-pattern landmark. */
constexpr std::uint32_t kGardenWorldRosterKey = 0xF29221F5U;
/** The first registration-only receive plus enough full receives to compare the native call path. */
constexpr unsigned kMaxMsg5Reports = 12;
/** Direct prefix retained from each native decoder argument. */
constexpr std::size_t kArg0ProbeBytes = 0x1000;
constexpr std::size_t kArg1ProbeBytes = 0x200;
constexpr std::size_t kArg2ProbeBytes = 0x400;
/** One-hop memory retained behind pointer-looking qwords in each argument prefix. */
constexpr std::size_t kPointerTargetBytes = 0x100;
constexpr std::size_t kMaxPointerTargets = 32;
/** Detail bounds keep a normal Director run readable. */
constexpr std::size_t kMaxDirectChangeReports = 6;
constexpr std::size_t kMaxPointerReportsPerRoot = 8;

/** @return True when VirtualQuery's protection permits ordinary reads. */
[[nodiscard]] bool readable_protection(DWORD protect) noexcept {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0) {
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

/**
 * Copies a bounded prefix across committed memory regions. A failed diagnostic read stops the
 * prefix; it never propagates an access violation into Destiny.
 */
__declspec(noinline) std::size_t read_memory_prefix(std::uintptr_t address,
                                                     std::byte* output,
                                                     std::size_t capacity) noexcept {
    if (address == 0 || output == nullptr || capacity == 0) {
        return 0;
    }

    std::size_t copied = 0;
    while (copied < capacity) {
        if (address > (std::numeric_limits<std::uintptr_t>::max)() - copied) {
            break;
        }
        const std::uintptr_t current = address + copied;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof info) == 0
            || info.State != MEM_COMMIT || !readable_protection(info.Protect)) {
            break;
        }
        const std::uintptr_t regionBase = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (regionBase > (std::numeric_limits<std::uintptr_t>::max)() - info.RegionSize) {
            break;
        }
        const std::uintptr_t regionEnd = regionBase + info.RegionSize;
        if (current < regionBase || current >= regionEnd) {
            break;
        }
        const std::size_t available = static_cast<std::size_t>(regionEnd - current);
        const std::size_t chunk = (std::min)(capacity - copied, available);
        if (chunk == 0) {
            break;
        }
        bool ok = true;
        __try {
            std::memcpy(output + copied, reinterpret_cast<const void*>(current), chunk);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) {
            break;
        }
        copied += chunk;
    }
    return copied;
}

template <std::size_t N> struct PrefixSnapshot final {
    std::array<std::byte, N> bytes{};
    std::size_t readable{};
};

template <std::size_t N>
[[nodiscard]] PrefixSnapshot<N> snapshot_prefix(void* address) noexcept {
    PrefixSnapshot<N> output{};
    output.readable = read_memory_prefix(reinterpret_cast<std::uintptr_t>(address),
                                         output.bytes.data(),
                                         output.bytes.size());
    return output;
}

/** Finds one unaligned little-endian 32-bit value in a captured prefix. */
template <std::size_t N>
[[nodiscard]] std::ptrdiff_t find_u32(const PrefixSnapshot<N>& snapshot,
                                      std::uint32_t wanted) noexcept {
    if (snapshot.readable < sizeof wanted) {
        return -1;
    }
    for (std::size_t offset = 0; offset + sizeof wanted <= snapshot.readable; ++offset) {
        std::uint32_t value{};
        std::memcpy(&value, snapshot.bytes.data() + offset, sizeof value);
        if (value == wanted) {
            return static_cast<std::ptrdiff_t>(offset);
        }
    }
    return -1;
}

/** Counts bytes whose captured value changed at offsets readable in both snapshots. */
template <std::size_t N>
[[nodiscard]] std::size_t changed_bytes(const PrefixSnapshot<N>& before,
                                        const PrefixSnapshot<N>& after) noexcept {
    const std::size_t common = (std::min)(before.readable, after.readable);
    std::size_t changed = 0;
    for (std::size_t offset = 0; offset < common; ++offset) {
        changed += before.bytes[offset] != after.bytes[offset] ? 1U : 0U;
    }
    return changed;
}

/** Returns the first changed byte offset, or -1 when the common readable prefix is identical. */
template <std::size_t N>
[[nodiscard]] std::ptrdiff_t first_change(const PrefixSnapshot<N>& before,
                                          const PrefixSnapshot<N>& after) noexcept {
    const std::size_t common = (std::min)(before.readable, after.readable);
    for (std::size_t offset = 0; offset < common; ++offset) {
        if (before.bytes[offset] != after.bytes[offset]) {
            return static_cast<std::ptrdiff_t>(offset);
        }
    }
    return -1;
}

/** Loads one captured qword without alignment assumptions. */
template <std::size_t N>
[[nodiscard]] bool captured_qword(const PrefixSnapshot<N>& snapshot,
                                  std::size_t offset,
                                  std::uint64_t& value) noexcept {
    value = 0;
    if (offset + sizeof value > snapshot.readable) {
        return false;
    }
    std::memcpy(&value, snapshot.bytes.data() + offset, sizeof value);
    return true;
}

/** One readable target reached through an aligned qword in a native argument prefix. */
struct PointerTargetProbe final {
    std::array<std::byte, kPointerTargetBytes> before{};
    std::uintptr_t pointer{};
    std::size_t fieldOffset{};
    std::size_t beforeReadable{};
};

struct PointerProbeSet final {
    std::array<PointerTargetProbe, kMaxPointerTargets> targets{};
    std::size_t count{};
};

/** @return True when this pointer value has already been retained for one root argument. */
[[nodiscard]] bool already_retained(const PointerProbeSet& set, std::uintptr_t pointer) noexcept {
    for (std::size_t index = 0; index < set.count; ++index) {
        if (set.targets[index].pointer == pointer) {
            return true;
        }
    }
    return false;
}

/**
 * Retains the first bounded set of readable one-hop targets referenced by aligned qwords. Direct
 * self-pointers are skipped because the root prefix already covers them.
 */
template <std::size_t N>
[[nodiscard]] PointerProbeSet collect_pointer_targets(const PrefixSnapshot<N>& root,
                                                      std::uintptr_t rootAddress) noexcept {
    PointerProbeSet output{};
    for (std::size_t offset = 0;
         offset + sizeof(std::uintptr_t) <= root.readable && output.count < output.targets.size();
         offset += sizeof(std::uintptr_t)) {
        std::uintptr_t pointer{};
        std::memcpy(&pointer, root.bytes.data() + offset, sizeof pointer);
        if (pointer < 0x10000U || already_retained(output, pointer)) {
            continue;
        }
        if (rootAddress != 0 && pointer >= rootAddress
            && pointer < rootAddress + root.readable) {
            continue;
        }
        PointerTargetProbe candidate{};
        candidate.pointer = pointer;
        candidate.fieldOffset = offset;
        candidate.beforeReadable =
            read_memory_prefix(pointer, candidate.before.data(), candidate.before.size());
        if (candidate.beforeReadable < sizeof(std::uint64_t)) {
            continue;
        }
        output.targets[output.count++] = candidate;
    }
    return output;
}

/** Reports the high-level before/after shape of all three genuine decoder arguments. */
template <std::size_t A, std::size_t B, std::size_t C>
void report_argument_summary(unsigned sequence,
                             void* arg0,
                             void* arg1,
                             void* arg2,
                             bool nativeResult,
                             const PrefixSnapshot<A>& before0,
                             const PrefixSnapshot<A>& after0,
                             const PrefixSnapshot<B>& before1,
                             const PrefixSnapshot<B>& after1,
                             const PrefixSnapshot<C>& before2,
                             const PrefixSnapshot<C>& after2) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=strike stage=msg5_args n=%u native=%u arg0=0x%llX arg1=0x%llX arg2=0x%llX "
        "arg0_read=%zu/%zu arg0_changed=%zu arg0_key=%td/%td "
        "arg1_read=%zu/%zu arg1_changed=%zu arg1_key=%td/%td "
        "arg2_read=%zu/%zu arg2_changed=%zu arg2_key=%td/%td",
        sequence,
        nativeResult ? 1U : 0U,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(arg0)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(arg1)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(arg2)),
        before0.readable,
        after0.readable,
        changed_bytes(before0, after0),
        find_u32(before0, kGardenWorldRosterKey),
        find_u32(after0, kGardenWorldRosterKey),
        before1.readable,
        after1.readable,
        changed_bytes(before1, after1),
        find_u32(before1, kGardenWorldRosterKey),
        find_u32(after1, kGardenWorldRosterKey),
        before2.readable,
        after2.readable,
        changed_bytes(before2, after2),
        find_u32(before2, kGardenWorldRosterKey),
        find_u32(after2, kGardenWorldRosterKey));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Logs a bounded set of aligned qwords whose enclosing native argument changed. */
template <std::size_t N>
void report_direct_changes(unsigned sequence,
                           const char* root,
                           const PrefixSnapshot<N>& before,
                           const PrefixSnapshot<N>& after) noexcept {
    const std::size_t common = (std::min)(before.readable, after.readable);
    std::size_t reports = 0;
    for (std::size_t offset = 0;
         offset + sizeof(std::uint64_t) <= common && reports < kMaxDirectChangeReports;
         offset += sizeof(std::uint64_t)) {
        if (std::memcmp(before.bytes.data() + offset,
                        after.bytes.data() + offset,
                        sizeof(std::uint64_t)) == 0) {
            continue;
        }
        std::uint64_t beforeValue{};
        std::uint64_t afterValue{};
        std::memcpy(&beforeValue, before.bytes.data() + offset, sizeof beforeValue);
        std::memcpy(&afterValue, after.bytes.data() + offset, sizeof afterValue);
        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=strike stage=msg5_direct n=%u root=%s offset=0x%zX before=0x%llX after=0x%llX",
            sequence,
            root,
            offset,
            static_cast<unsigned long long>(beforeValue),
            static_cast<unsigned long long>(afterValue));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        ++reports;
    }
}

/** Finds one u32 value in a raw captured target prefix. */
[[nodiscard]] std::ptrdiff_t find_target_u32(const std::byte* bytes,
                                             std::size_t readable,
                                             std::uint32_t wanted) noexcept {
    if (bytes == nullptr || readable < sizeof wanted) {
        return -1;
    }
    for (std::size_t offset = 0; offset + sizeof wanted <= readable; ++offset) {
        std::uint32_t value{};
        std::memcpy(&value, bytes + offset, sizeof value);
        if (value == wanted) {
            return static_cast<std::ptrdiff_t>(offset);
        }
    }
    return -1;
}

/** Reports one-hop target changes or exact Garden World key hits behind a decoder argument. */
void report_pointer_targets(unsigned sequence,
                            const char* root,
                            const PointerProbeSet& beforeSet) noexcept {
    std::size_t reports = 0;
    for (std::size_t index = 0;
         index < beforeSet.count && reports < kMaxPointerReportsPerRoot;
         ++index) {
        const PointerTargetProbe& target = beforeSet.targets[index];
        std::array<std::byte, kPointerTargetBytes> after{};
        const std::size_t afterReadable =
            read_memory_prefix(target.pointer, after.data(), after.size());
        const std::size_t common = (std::min)(target.beforeReadable, afterReadable);
        std::size_t changed = 0;
        std::ptrdiff_t first = -1;
        for (std::size_t offset = 0; offset < common; ++offset) {
            if (target.before[offset] != after[offset]) {
                ++changed;
                if (first < 0) {
                    first = static_cast<std::ptrdiff_t>(offset);
                }
            }
        }
        const std::ptrdiff_t keyBefore =
            find_target_u32(target.before.data(), target.beforeReadable, kGardenWorldRosterKey);
        const std::ptrdiff_t keyAfter =
            find_target_u32(after.data(), afterReadable, kGardenWorldRosterKey);
        if (changed == 0 && target.beforeReadable == afterReadable && keyBefore < 0 && keyAfter < 0) {
            continue;
        }

        std::uint64_t beforeValue{};
        std::uint64_t afterValue{};
        std::size_t valueOffset = 0;
        if (first >= 0) {
            valueOffset = static_cast<std::size_t>(first) & ~std::size_t{7};
            if (valueOffset + sizeof beforeValue <= target.beforeReadable) {
                std::memcpy(&beforeValue, target.before.data() + valueOffset, sizeof beforeValue);
            }
            if (valueOffset + sizeof afterValue <= afterReadable) {
                std::memcpy(&afterValue, after.data() + valueOffset, sizeof afterValue);
            }
        }

        std::array<char, core::log::kLineCapacity> line{};
        const int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=strike stage=msg5_ptr n=%u root=%s field=0x%zX ptr=0x%llX "
            "read=%zu/%zu changed=%zu first=%td key=%td/%td sample_off=0x%zX "
            "before=0x%llX after=0x%llX",
            sequence,
            root,
            target.fieldOffset,
            static_cast<unsigned long long>(target.pointer),
            target.beforeReadable,
            afterReadable,
            changed,
            first,
            keyBefore,
            keyAfter,
            valueOffset,
            static_cast<unsigned long long>(beforeValue),
            static_cast<unsigned long long>(afterValue));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        ++reports;
    }
}

/** One native reader operation captured only while the genuine msg-5 decoder is on this thread. */
enum class Msg5ReadKind : std::uint8_t {
    bits,
    wide,
    boolean,
    status,
};

struct Msg5ReadRecord final {
    std::uintptr_t stream{};
    std::uintptr_t auxiliary{};
    std::uint64_t value{};
    std::uint32_t width{};
    unsigned depth{};
    Msg5ReadKind kind{Msg5ReadKind::bits};
    bool decoderStream{};
};

/** Registration-only msg 5 is small; this also leaves generous room for early full-body reads. */
constexpr std::size_t kMaxMsg5ReadRecords = 512;
/** The first three receives get the full operation sequence; later receives keep one summary line. */
constexpr unsigned kDetailedMsg5ReadReceives = 3;

struct Msg5ReadTrace final {
    std::array<Msg5ReadRecord, kMaxMsg5ReadRecords> records{};
    void* decoderStream{};
    unsigned sequence{};
    std::size_t count{};
    std::size_t dropped{};
    bool active{};
};

thread_local Msg5ReadTrace g_msg5ReadTrace{};
thread_local unsigned g_msg5ReaderDepth{};
constexpr std::size_t kNoMsg5ReadRecord = static_cast<std::size_t>(-1);

/** Begins one thread-local reader trace around a genuine msg-5 decoder call. */
void begin_msg5_read_trace(unsigned sequence, void* decoderStream) noexcept {
    g_msg5ReadTrace.decoderStream = decoderStream;
    g_msg5ReadTrace.sequence = sequence;
    g_msg5ReadTrace.count = 0;
    g_msg5ReadTrace.dropped = 0;
    g_msg5ReaderDepth = 0;
    g_msg5ReadTrace.active = true;
}

/** Stops collection without clearing the retained records needed by the post-call reporter. */
void end_msg5_read_trace() noexcept {
    g_msg5ReadTrace.active = false;
}

/**
 * Retains one reader entry before the native helper runs so nested helpers preserve call order.
 * @return Record index to complete on return, or the sentinel when this thread is not tracing.
 */
[[nodiscard]] std::size_t begin_msg5_read(Msg5ReadKind kind,
                                          void* stream,
                                          std::uint32_t width,
                                          std::uintptr_t auxiliary = 0) noexcept {
    if (!g_msg5ReadTrace.active) {
        return kNoMsg5ReadRecord;
    }
    const unsigned depth = g_msg5ReaderDepth++;
    if (g_msg5ReadTrace.count >= g_msg5ReadTrace.records.size()) {
        ++g_msg5ReadTrace.dropped;
        return kNoMsg5ReadRecord;
    }
    const std::size_t index = g_msg5ReadTrace.count++;
    Msg5ReadRecord& record = g_msg5ReadTrace.records[index];
    record = {};
    record.kind = kind;
    record.stream = reinterpret_cast<std::uintptr_t>(stream);
    record.auxiliary = auxiliary;
    record.width = width;
    record.depth = depth;
    record.decoderStream = stream == g_msg5ReadTrace.decoderStream;
    return index;
}

/** Completes one retained reader call and balances its thread-local nesting depth. */
void finish_msg5_read(std::size_t index, std::uint64_t value) noexcept {
    if (!g_msg5ReadTrace.active) {
        return;
    }
    if (index != kNoMsg5ReadRecord && index < g_msg5ReadTrace.count) {
        g_msg5ReadTrace.records[index].value = value;
    }
    if (g_msg5ReaderDepth != 0) {
        --g_msg5ReaderDepth;
    }
}

/** @return Stable short name used by the compact per-read log line. */
[[nodiscard]] const char* msg5_read_name(Msg5ReadKind kind) noexcept {
    switch (kind) {
    case Msg5ReadKind::bits:
        return "bits";
    case Msg5ReadKind::wide:
        return "wide";
    case Msg5ReadKind::boolean:
        return "bool";
    case Msg5ReadKind::status:
        return "status";
    }
    return "?";
}

/** Reports the retained native reader sequence after leaving the hot decoder call. */
void report_msg5_read_trace(bool nativeResult,
                            bool finalStatusKnown,
                            std::uint64_t finalStatus) noexcept {
    std::size_t bits = 0;
    std::size_t wide = 0;
    std::size_t boolean = 0;
    std::size_t status = 0;
    std::size_t width7 = 0;
    std::size_t width8 = 0;
    std::size_t foreignStream = 0;
    for (std::size_t index = 0; index < g_msg5ReadTrace.count; ++index) {
        const Msg5ReadRecord& record = g_msg5ReadTrace.records[index];
        switch (record.kind) {
        case Msg5ReadKind::bits:
            ++bits;
            break;
        case Msg5ReadKind::wide:
            ++wide;
            break;
        case Msg5ReadKind::boolean:
            ++boolean;
            break;
        case Msg5ReadKind::status:
            ++status;
            break;
        }
        if (record.kind == Msg5ReadKind::bits || record.kind == Msg5ReadKind::wide) {
            width7 += record.width == 7U ? 1U : 0U;
            width8 += record.width == 8U ? 1U : 0U;
        }
        foreignStream += record.decoderStream ? 0U : 1U;
    }

    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=strike stage=msg5_trace n=%u native=%u stream=0x%llX reads=%zu dropped=%zu "
        "bits=%zu wide=%zu bool=%zu status=%zu width7=%zu width8=%zu foreign_stream=%zu "
        "final_status_known=%u final_status=0x%llX",
        g_msg5ReadTrace.sequence,
        nativeResult ? 1U : 0U,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(g_msg5ReadTrace.decoderStream)),
        g_msg5ReadTrace.count,
        g_msg5ReadTrace.dropped,
        bits,
        wide,
        boolean,
        status,
        width7,
        width8,
        foreignStream,
        finalStatusKnown ? 1U : 0U,
        static_cast<unsigned long long>(finalStatus));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }

    if (g_msg5ReadTrace.sequence > kDetailedMsg5ReadReceives) {
        return;
    }
    for (std::size_t index = 0; index < g_msg5ReadTrace.count; ++index) {
        const Msg5ReadRecord& record = g_msg5ReadTrace.records[index];
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=strike stage=msg5_read n=%u i=%zu op=%s depth=%u width=%u value=0x%llX "
            "stream=0x%llX decoder_stream=%u aux=0x%llX",
            g_msg5ReadTrace.sequence,
            index,
            msg5_read_name(record.kind),
            record.depth,
            record.width,
            static_cast<unsigned long long>(record.value),
            static_cast<unsigned long long>(record.stream),
            record.decoderStream ? 1U : 0U,
            static_cast<unsigned long long>(record.auxiliary));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
}

/**
 * Logs the first roster decode and the first forced authority read.
 * @param stage Which event this is.
 */
void report_once(const char* stage) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=bubbleauth stage=%s result=ok", stage);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

using Decoder = bool(__fastcall*)(void*, void*, void*);
using ContentUntracked = bool(__fastcall*)();
using ReadBits = std::uint64_t(__fastcall*)(void*, std::uint32_t);
using ReadWide = std::uint64_t(__fastcall*)(void*, void*, std::uint32_t);
using ReadBool = std::uint64_t(__fastcall*)(void*);
using StreamStatus = std::uint64_t(__fastcall*)(void*);

/** Observes the confirmed native read_bits helper only while msg 5 owns this thread. */
__declspec(noinline) std::uint64_t __fastcall read_bits_body(void* stream,
                                                              std::uint32_t width) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::msg5ReadBits, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ReadBits>(lease.original);
    std::uint64_t result{};
    const std::size_t trace =
        lease.accepting ? begin_msg5_read(Msg5ReadKind::bits, stream, width) : kNoMsg5ReadRecord;
    __try {
        if (call != nullptr) {
            result = call(stream, width);
        }
    } __finally {
        if (lease.accepting) {
            finish_msg5_read(trace, result);
        }
        coordinator::g_callEgress();
    }
    return result;
}

/** Observes the confirmed native read_wide helper without dereferencing its caller-owned output. */
__declspec(noinline) std::uint64_t __fastcall read_wide_body(void* stream,
                                                              void* destination,
                                                              std::uint32_t width) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::msg5ReadWide, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ReadWide>(lease.original);
    std::uint64_t result{};
    const std::size_t trace =
        lease.accepting
            ? begin_msg5_read(Msg5ReadKind::wide,
                              stream,
                              width,
                              reinterpret_cast<std::uintptr_t>(destination))
            : kNoMsg5ReadRecord;
    __try {
        if (call != nullptr) {
            result = call(stream, destination, width);
        }
    } __finally {
        if (lease.accepting) {
            finish_msg5_read(trace, result);
        }
        coordinator::g_callEgress();
    }
    return result;
}

/** Observes the confirmed native read_bool helper and preserves its raw return register value. */
__declspec(noinline) std::uint64_t __fastcall read_bool_body(void* stream) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::msg5ReadBool, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ReadBool>(lease.original);
    std::uint64_t result{};
    const std::size_t trace =
        lease.accepting ? begin_msg5_read(Msg5ReadKind::boolean, stream, 1U) : kNoMsg5ReadRecord;
    __try {
        if (call != nullptr) {
            result = call(stream);
        }
    } __finally {
        if (lease.accepting) {
            finish_msg5_read(trace, result & 0xFFU);
        }
        coordinator::g_callEgress();
    }
    return result;
}

/** Observes native status checks that happen naturally inside the msg-5 decoder. */
__declspec(noinline) std::uint64_t __fastcall stream_status_body(void* stream) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(lease, HookSlot::msg5StreamStatus, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<StreamStatus>(lease.original);
    std::uint64_t result{};
    const std::size_t trace =
        lease.accepting ? begin_msg5_read(Msg5ReadKind::status, stream, 0U) : kNoMsg5ReadRecord;
    __try {
        if (call != nullptr) {
            result = call(stream);
        }
    } __finally {
        if (lease.accepting) {
            finish_msg5_read(trace, result);
        }
        coordinator::g_callEgress();
    }
    return result;
}

/**
 * Runs one confirmed read-only status query after the native decoder, through its trampoline.
 * The updated Homecoming notes identify 0x34E9B0 as stream_status(stream), which checks the
 * overflow/sticky-error state. SEH keeps an unexpected diagnostic ABI from escaping into Destiny.
 */
[[nodiscard]] bool query_final_stream_status(void* stream, std::uint64_t& status) noexcept {
    status = 0;
    const auto call = original<StreamStatus>(HookSlot::msg5StreamStatus);
    if (stream == nullptr || call == nullptr) {
        return false;
    }
    bool ok = true;
    __try {
        status = call(stream);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = 0;
        ok = false;
    }
    return ok;
}

/**
 * Runs the genuine msg-5 decoder in the existing authority scope. This diagnostic deliberately
 * treats the three native parameters as opaque arguments rather than assuming arg0+0x40 is a
 * roster-entry array. It snapshots only bounded readable memory before/after the native call.
 */
__declspec(noinline) bool __fastcall decoder_body(void* arg0,
                                                  void* arg1,
                                                  void* arg2) noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::bubbleAuthorityDecoder, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<Decoder>(lease.original);
    const bool scoped = lease.accepting && call != nullptr;
    bool result{};

    const unsigned claimed = g_msg5Reports.fetch_add(1, std::memory_order_relaxed);
    const bool observe = claimed < kMaxMsg5Reports;
    const unsigned reportSequence = claimed + 1U;

    PrefixSnapshot<kArg0ProbeBytes> before0{};
    PrefixSnapshot<kArg1ProbeBytes> before1{};
    PrefixSnapshot<kArg2ProbeBytes> before2{};
    PointerProbeSet pointers0{};
    PointerProbeSet pointers1{};
    PointerProbeSet pointers2{};
    if (observe) {
        before0 = snapshot_prefix<kArg0ProbeBytes>(arg0);
        before1 = snapshot_prefix<kArg1ProbeBytes>(arg1);
        before2 = snapshot_prefix<kArg2ProbeBytes>(arg2);
        pointers0 = collect_pointer_targets(before0, reinterpret_cast<std::uintptr_t>(arg0));
        pointers1 = collect_pointer_targets(before1, reinterpret_cast<std::uintptr_t>(arg1));
        pointers2 = collect_pointer_targets(before2, reinterpret_cast<std::uintptr_t>(arg2));
        // The original decoder ABI names arg1 as the bitstream. Reader hooks still retain any
        // foreign stream pointer so this assumption is observable rather than silently trusted.
        begin_msg5_read_trace(reportSequence, arg1);
    }

    std::uint64_t finalStatus{};
    bool finalStatusKnown{};
    if (scoped) {
        scope::enter();
        if (!g_decoderSeen.exchange(true, std::memory_order_relaxed)) {
            report_once("decode");
        }
    }
    __try {
        if (call != nullptr) {
            result = call(arg0, arg1, arg2);
        }
        if (observe && call != nullptr) {
            finalStatusKnown = query_final_stream_status(arg1, finalStatus);
        }
    } __finally {
        if (observe) {
            end_msg5_read_trace();
        }
        if (scoped) {
            scope::leave();
        }
        coordinator::g_callEgress();
    }

    if (observe) {
        report_msg5_read_trace(result, finalStatusKnown, finalStatus);
        const PrefixSnapshot<kArg0ProbeBytes> after0 = snapshot_prefix<kArg0ProbeBytes>(arg0);
        const PrefixSnapshot<kArg1ProbeBytes> after1 = snapshot_prefix<kArg1ProbeBytes>(arg1);
        const PrefixSnapshot<kArg2ProbeBytes> after2 = snapshot_prefix<kArg2ProbeBytes>(arg2);
        report_argument_summary(reportSequence,
                                arg0,
                                arg1,
                                arg2,
                                result,
                                before0,
                                after0,
                                before1,
                                after1,
                                before2,
                                after2);
        report_direct_changes(reportSequence, "arg0", before0, after0);
        report_direct_changes(reportSequence, "arg1", before1, after1);
        report_direct_changes(reportSequence, "arg2", before2, after2);
        report_pointer_targets(reportSequence, "arg0", pointers0);
        report_pointer_targets(reportSequence, "arg1", pointers1);
        report_pointer_targets(reportSequence, "arg2", pointers2);
    }
    return result;
}

/**
 * Keeps the native build-state read, and forces it true only on the scoped decoder thread.
 * This existing compatibility bypass is intentionally unchanged by the diagnostic patch.
 */
__declspec(noinline) bool __fastcall content_untracked_body() noexcept {
    coordinator::CallLease lease{};
    coordinator::g_callIngress(
        lease, HookSlot::contentUntrackedGetter, coordinator::ConsumerKind::none);
    const auto call = reinterpret_cast<ContentUntracked>(lease.original);
    bool result{};
    __try {
        if (call != nullptr) {
            result = call();
        }
        const bool forced = !result && scope::active();
        result = result || forced;
        if (forced && !g_forcedSeen.exchange(true, std::memory_order_relaxed)) {
            report_once("force");
        }
    } __finally {
        coordinator::g_callEgress();
    }
    return result;
}

} // namespace

/** @return The internal-linkage decoder body, kept safe while the detour is removed. */
void* decoder_entry_point() noexcept {
    return reinterpret_cast<void*>(&decoder_body);
}

/** @return The internal-linkage getter body, kept safe while the detour is removed. */
void* content_untracked_entry_point() noexcept {
    return reinterpret_cast<void*>(&content_untracked_body);
}

/** @return The internal-linkage read_bits observer kept safe while the detour is removed. */
void* read_bits_entry_point() noexcept {
    return reinterpret_cast<void*>(&read_bits_body);
}

/** @return The internal-linkage read_wide observer kept safe while the detour is removed. */
void* read_wide_entry_point() noexcept {
    return reinterpret_cast<void*>(&read_wide_body);
}

/** @return The internal-linkage read_bool observer kept safe while the detour is removed. */
void* read_bool_entry_point() noexcept {
    return reinterpret_cast<void*>(&read_bool_body);
}

/** @return The internal-linkage stream_status observer kept safe while the detour is removed. */
void* stream_status_entry_point() noexcept {
    return reinterpret_cast<void*>(&stream_status_body);
}

} // namespace sunrise::client::hooks::network::bubble_authority
