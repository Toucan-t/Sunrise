#include "entities_panel.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

#include <imgui.h>

#include "../../../core/logging/log.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::ui::entities {
namespace {

namespace patterns = client::patterns;
namespace process_memory = client::memory;

/** The same native controlled-object getter signature already used by the Teleport hook. */
constexpr std::string_view kControlledHandleText =
    "40 53 48 83 EC 20 48 8B D9 C7 01 FF FF FF FF 48 8D 4C 24 30 E8 ? ? ? ? 8B 44 24 30 "
    "83 F8 FF 74 18 25 FF 1F 00 00 0F AF 05";
constexpr auto kControlledHandlePattern =
    patterns::signature<patterns::signature_length(kControlledHandleText)>(kControlledHandleText);

/** The getter masks the native object index to 13 bits. */
constexpr std::uint32_t kObjectIndexMask = 0x1FFF;
constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFF;
constexpr std::size_t kObjectSlotCount = 1U << 13U;

/** The imul at getter+0x27 reads the native record stride through this RIP-relative operand. */
constexpr std::size_t kStrideOperandOffset = 42;
constexpr std::size_t kStrideNextInstructionOffset = 46;
/** Enough getter bytes to reach the object-table access while avoiding unrelated later functions. */
constexpr std::size_t kGetterProbeBytes = 224;
/** Fixed diagnostic bounds; every operation is explicit from the UI. */
constexpr std::size_t kCandidateCapacity = 64;
constexpr std::size_t kRecordProbeCapacity = 128;
/** Full bytes retained per native object record. The discovered stride is currently 0x1E0. */
constexpr std::size_t kRecordCaptureCapacity = 512;
/** Fixed UI/log bounds. The full table remains readable even when a summary truncates. */
constexpr std::size_t kChangedRecordCapacity = 1024;
constexpr std::size_t kHexPreviewCapacity = 8;
constexpr std::size_t kWriterCaptureCapacity = 8;

using ControlledHandle = std::uint32_t* (*)(std::uint32_t*);

enum class RipOperation : std::uint8_t {
    mov,
    lea,
    add,
};

enum class BaseMode : std::uint8_t {
    direct,
    dereference1,
    dereference2,
};

struct Candidate final {
    std::size_t sourceOffset{};
    RipOperation operation{};
    BaseMode mode{};
    std::uintptr_t globalAddress{};
    std::uintptr_t baseAddress{};
    std::uintptr_t playerRecord{};
    int fullHandleOffset{-1};
    int lowHandleOffset{-1};
    int index32Offset{-1};
    int index16Offset{-1};
    int score{};
};

struct LocatorState final {
    std::byte* getter{};
    std::uintptr_t strideAddress{};
    std::uint32_t stride{};
    std::uint32_t controlledHandle{kInvalidHandle};
    std::uint32_t controlledIndex{kObjectSlotCount};
    std::array<Candidate, kCandidateCapacity> candidates{};
    std::size_t candidateCount{};
    bool attempted{};
    bool ok{};
};

enum class DiffKind : std::uint8_t {
    none,
    normal,
    action,
};

struct TableCapture final {
    std::uintptr_t baseAddress{};
    std::uint32_t stride{};
    std::uint64_t capturedMs{};
    std::array<std::array<std::byte, kRecordCaptureCapacity>, kObjectSlotCount> records{};
    std::array<bool, kObjectSlotCount> readable{};
    std::size_t readableCount{};
    bool valid{};
};

struct ChangedRecord final {
    std::uint16_t index{};
    std::uint16_t changedBytes{};
    std::uint16_t exclusiveBytes{};
    std::uint16_t firstOffset{};
    std::uint16_t lastOffset{};
};

struct Comparison final {
    std::array<ChangedRecord, kChangedRecordCapacity> rows{};
    std::size_t retained{};
    std::size_t totalChanged{};
    std::size_t totalChangedBytes{};
    std::size_t currentReadable{};
    std::uint64_t comparedMs{};
    bool valid{};
};

struct WriterTrace final {
    std::uintptr_t address{};
    std::uintptr_t writerRip{};
    std::uint16_t recordIndex{};
    std::uint16_t recordOffset{};
    std::uint8_t length{};
    DWORD threadId{};
    std::array<std::byte, kWriterCaptureCapacity> before{};
    std::array<std::byte, kWriterCaptureCapacity> after{};
    std::size_t armedThreads{};
    std::size_t occupiedDebugSlots{};
    std::size_t failedThreads{};
    bool attempted{};
};

LocatorState g_locator{};
TableCapture g_normalBefore{};
TableCapture g_normalAfter{};
TableCapture g_actionBefore{};
TableCapture g_actionAfter{};
Comparison g_normalComparison{};
Comparison g_actionComparison{};
WriterTrace g_writerTrace{};
std::size_t g_selectedCandidate{};
std::size_t g_selectedRecord{kObjectSlotCount};
std::size_t g_selectedRunOffset{};
std::size_t g_selectedRunLength{};
DiffKind g_selectedDiff{DiffKind::none};
std::uint64_t g_actionBaselineMs{};
bool g_actionBaselineReady{};
PVOID g_writerHandler{};
std::atomic_bool g_writerArmed{false};
std::atomic_bool g_writerCaptured{false};

/** @return Milliseconds elapsed without underflow. */
[[nodiscard]] std::uint64_t elapsed(std::uint64_t now, std::uint64_t then) noexcept {
    return now >= then ? now - then : 0;
}

/** @return Human-readable instruction family for one RIP-relative candidate. */
[[nodiscard]] const char* operation_label(RipOperation operation) noexcept {
    switch (operation) {
    case RipOperation::mov:
        return "mov";
    case RipOperation::lea:
        return "lea";
    case RipOperation::add:
        return "add";
    }
    return "?";
}

/** @return How a candidate base was recovered from the RIP-relative target. */
[[nodiscard]] const char* mode_label(BaseMode mode) noexcept {
    switch (mode) {
    case BaseMode::direct:
        return "direct";
    case BaseMode::dereference1:
        return "*global";
    case BaseMode::dereference2:
        return "**global";
    }
    return "?";
}

/** Reads one trivially-copyable scalar from the current process. */
template <typename T> [[nodiscard]] bool read_scalar(std::uintptr_t address, T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<std::byte, sizeof(T)> bytes{};
    if (!process_memory::read_current_process(nullptr, address, bytes)) {
        value = {};
        return false;
    }
    std::memcpy(&value, bytes.data(), sizeof value);
    return true;
}

/** @return True when one complete address range is committed and not guarded/no-access. */
[[nodiscard]] bool readable_range(std::uintptr_t address, std::size_t size) noexcept {
    if (address == 0 || size == 0 || address > std::numeric_limits<std::uintptr_t>::max() - size) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    const void* pointer = reinterpret_cast<const void*>(address);
    if (VirtualQuery(pointer, &info, sizeof info) == 0 || info.State != MEM_COMMIT
        || (info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    const auto regionBase = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (regionBase > std::numeric_limits<std::uintptr_t>::max() - info.RegionSize) {
        return false;
    }
    const auto regionEnd = regionBase + info.RegionSize;
    return address >= regionBase && address + size <= regionEnd;
}

/** Finds one scalar byte pattern in a record prefix. */
template <typename T>
[[nodiscard]] int find_scalar(std::span<const std::byte> bytes, T wanted) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    if (bytes.size() < sizeof(T)) {
        return -1;
    }
    for (std::size_t offset = 0; offset + sizeof(T) <= bytes.size(); ++offset) {
        T value{};
        std::memcpy(&value, bytes.data() + offset, sizeof value);
        if (value == wanted) {
            return static_cast<int>(offset);
        }
    }
    return -1;
}

/** @return True when a seven-byte instruction uses RIP-relative memory. */
[[nodiscard]] bool rip_relative_instruction(std::span<const std::byte> bytes,
                                            std::size_t offset,
                                            RipOperation& operation) noexcept {
    if (offset + 7 > bytes.size()) {
        return false;
    }
    const auto rex = std::to_integer<std::uint8_t>(bytes[offset]);
    const auto opcode = std::to_integer<std::uint8_t>(bytes[offset + 1]);
    const auto modrm = std::to_integer<std::uint8_t>(bytes[offset + 2]);
    if ((rex != 0x48U && rex != 0x4CU) || (modrm & 0xC7U) != 0x05U) {
        return false;
    }
    if (opcode == 0x8BU) {
        operation = RipOperation::mov;
        return true;
    }
    if (opcode == 0x8DU) {
        operation = RipOperation::lea;
        return true;
    }
    if (opcode == 0x03U) {
        operation = RipOperation::add;
        return true;
    }
    return false;
}

/** Scores one possible object-table base against the currently controlled object record. */
[[nodiscard]] bool build_candidate(std::size_t sourceOffset,
                                   RipOperation operation,
                                   BaseMode mode,
                                   std::uintptr_t globalAddress,
                                   std::uintptr_t baseAddress,
                                   Candidate& candidate) noexcept {
    candidate = {};
    if (g_locator.stride == 0 || g_locator.controlledIndex >= kObjectSlotCount || baseAddress == 0) {
        return false;
    }
    const std::uintptr_t product =
        static_cast<std::uintptr_t>(g_locator.controlledIndex) * g_locator.stride;
    if (baseAddress > std::numeric_limits<std::uintptr_t>::max() - product) {
        return false;
    }
    const std::uintptr_t record = baseAddress + product;
    const std::size_t probeBytes =
        (std::min)(static_cast<std::size_t>(g_locator.stride), kRecordProbeCapacity);
    if (probeBytes < sizeof(std::uint16_t) || !readable_range(record, probeBytes)) {
        return false;
    }
    std::array<std::byte, kRecordProbeCapacity> bytes{};
    if (!process_memory::read_current_process(
            nullptr, record, std::span(bytes).first(probeBytes))) {
        return false;
    }

    candidate.sourceOffset = sourceOffset;
    candidate.operation = operation;
    candidate.mode = mode;
    candidate.globalAddress = globalAddress;
    candidate.baseAddress = baseAddress;
    candidate.playerRecord = record;
    candidate.fullHandleOffset = find_scalar<std::uint32_t>(
        std::span(bytes).first(probeBytes), g_locator.controlledHandle);
    candidate.lowHandleOffset = find_scalar<std::uint16_t>(
        std::span(bytes).first(probeBytes), static_cast<std::uint16_t>(g_locator.controlledHandle));
    candidate.index32Offset = find_scalar<std::uint32_t>(
        std::span(bytes).first(probeBytes), g_locator.controlledIndex);
    candidate.index16Offset = find_scalar<std::uint16_t>(
        std::span(bytes).first(probeBytes), static_cast<std::uint16_t>(g_locator.controlledIndex));
    candidate.score = 1;
    if (candidate.fullHandleOffset >= 0) {
        candidate.score += 16;
    }
    if (candidate.lowHandleOffset >= 0) {
        candidate.score += 8;
    }
    if (candidate.index32Offset >= 0) {
        candidate.score += 4;
    }
    if (candidate.index16Offset >= 0) {
        candidate.score += 2;
    }
    return true;
}

/** Adds one base candidate unless the exact mode/base/source row already exists. */
void add_candidate(std::size_t sourceOffset,
                   RipOperation operation,
                   BaseMode mode,
                   std::uintptr_t globalAddress,
                   std::uintptr_t baseAddress) noexcept {
    if (g_locator.candidateCount >= g_locator.candidates.size()) {
        return;
    }
    for (std::size_t index = 0; index < g_locator.candidateCount; ++index) {
        const Candidate& existing = g_locator.candidates[index];
        if (existing.sourceOffset == sourceOffset && existing.mode == mode
            && existing.baseAddress == baseAddress) {
            return;
        }
    }
    Candidate candidate{};
    if (build_candidate(
            sourceOffset, operation, mode, globalAddress, baseAddress, candidate)) {
        g_locator.candidates[g_locator.candidateCount++] = candidate;
    }
}

/** Clears capture metadata without constructing a multi-megabyte temporary on the UI stack. */
void clear_capture(TableCapture& capture) noexcept {
    capture.baseAddress = 0;
    capture.stride = 0;
    capture.capturedMs = 0;
    capture.readable.fill(false);
    capture.readableCount = 0;
    capture.valid = false;
}

/** Clears a retained diff summary. */
void clear_comparison(Comparison& comparison) noexcept {
    comparison.retained = 0;
    comparison.totalChanged = 0;
    comparison.totalChangedBytes = 0;
    comparison.currentReadable = 0;
    comparison.comparedMs = 0;
    comparison.valid = false;
}

/** Resolves the native controlled-object getter, record stride, and likely object-table globals. */
void resolve_object_system() noexcept {
    g_locator = {};
    g_locator.attempted = true;
    g_selectedCandidate = 0;
    clear_capture(g_normalBefore);
    clear_capture(g_normalAfter);
    clear_capture(g_actionBefore);
    clear_capture(g_actionAfter);
    clear_comparison(g_normalComparison);
    clear_comparison(g_actionComparison);
    g_selectedRecord = kObjectSlotCount;
    g_selectedRunOffset = 0;
    g_selectedRunLength = 0;
    g_selectedDiff = DiffKind::none;
    g_actionBaselineMs = 0;
    g_actionBaselineReady = false;

    g_locator.getter = patterns::scan_main_image_unique(kControlledHandlePattern, "object_system_handle");
    if (g_locator.getter == nullptr) {
        return;
    }

    std::uint32_t handle = kInvalidHandle;
    const auto controlled = reinterpret_cast<ControlledHandle>(g_locator.getter);
    if (controlled(&handle) == nullptr || handle == kInvalidHandle) {
        return;
    }
    g_locator.controlledHandle = handle;
    g_locator.controlledIndex = handle & kObjectIndexMask;

    std::byte* const strideAddress = patterns::resolve_relative(
        g_locator.getter + kStrideOperandOffset, g_locator.getter + kStrideNextInstructionOffset);
    g_locator.strideAddress = reinterpret_cast<std::uintptr_t>(strideAddress);
    if (!read_scalar(g_locator.strideAddress, g_locator.stride) || g_locator.stride < 4
        || g_locator.stride > 0x10000U) {
        g_locator.stride = 0;
        return;
    }

    std::array<std::byte, kGetterProbeBytes> bytes{};
    if (!process_memory::read_current_process(nullptr,
                                               reinterpret_cast<std::uintptr_t>(g_locator.getter),
                                               bytes)) {
        return;
    }

    for (std::size_t offset = kStrideNextInstructionOffset; offset + 7 <= bytes.size(); ++offset) {
        RipOperation operation{};
        if (!rip_relative_instruction(bytes, offset, operation)) {
            continue;
        }
        std::byte* const target = patterns::resolve_relative(
            g_locator.getter + offset + 3, g_locator.getter + offset + 7);
        const auto globalAddress = reinterpret_cast<std::uintptr_t>(target);
        add_candidate(offset, operation, BaseMode::direct, globalAddress, globalAddress);

        std::uintptr_t first{};
        if (read_scalar(globalAddress, first) && first != 0) {
            add_candidate(offset, operation, BaseMode::dereference1, globalAddress, first);
            std::uintptr_t second{};
            if (read_scalar(first, second) && second != 0) {
                add_candidate(offset, operation, BaseMode::dereference2, globalAddress, second);
            }
        }
    }

    std::sort(g_locator.candidates.begin(),
              g_locator.candidates.begin() + static_cast<std::ptrdiff_t>(g_locator.candidateCount),
              [](const Candidate& left, const Candidate& right) noexcept {
                  if (left.score != right.score) {
                      return left.score > right.score;
                  }
                  if (left.sourceOffset != right.sourceOffset) {
                      return left.sourceOffset < right.sourceOffset;
                  }
                  return left.baseAddress < right.baseAddress;
              });
    g_locator.ok = g_locator.candidateCount != 0;
}


/** Captures the full discovered record width for every readable native object slot. */
[[nodiscard]] bool capture_table(TableCapture& capture) noexcept {
    clear_capture(capture);
    if (!g_locator.ok || g_selectedCandidate >= g_locator.candidateCount || g_locator.stride == 0
        || g_locator.stride > kRecordCaptureCapacity) {
        return false;
    }

    const Candidate& candidate = g_locator.candidates[g_selectedCandidate];
    capture.baseAddress = candidate.baseAddress;
    capture.stride = g_locator.stride;
    const std::size_t recordBytes = static_cast<std::size_t>(capture.stride);
    for (std::size_t index = 0; index < kObjectSlotCount; ++index) {
        const std::uintptr_t product = static_cast<std::uintptr_t>(index) * capture.stride;
        if (capture.baseAddress > std::numeric_limits<std::uintptr_t>::max() - product) {
            continue;
        }
        const std::uintptr_t address = capture.baseAddress + product;
        if (!readable_range(address, recordBytes)) {
            continue;
        }
        if (!process_memory::read_current_process(
                nullptr, address, std::span(capture.records[index]).first(recordBytes))) {
            continue;
        }
        capture.readable[index] = true;
        ++capture.readableCount;
    }
    capture.capturedMs = GetTickCount64();
    capture.valid = capture.readableCount != 0;
    return capture.valid;
}

/** @return True when two captures describe the same table layout. */
[[nodiscard]] bool compatible(const TableCapture& before, const TableCapture& after) noexcept {
    return before.valid && after.valid && before.baseAddress == after.baseAddress
           && before.stride == after.stride && before.stride != 0
           && before.stride <= kRecordCaptureCapacity;
}

/** @return One record's captured bytes, or an empty span when that record was unreadable. */
[[nodiscard]] std::span<const std::byte>
captured_record(const TableCapture& capture, std::size_t index) noexcept {
    if (!capture.valid || index >= kObjectSlotCount || !capture.readable[index]
        || capture.stride > kRecordCaptureCapacity) {
        return {};
    }
    return std::span(capture.records[index]).first(static_cast<std::size_t>(capture.stride));
}

/** Builds a full-record byte comparison between two table captures. */
void compare_captures(const TableCapture& before,
                      const TableCapture& after,
                      Comparison& comparison) noexcept {
    clear_comparison(comparison);
    if (!compatible(before, after)) {
        return;
    }

    comparison.currentReadable = after.readableCount;
    const std::size_t stride = static_cast<std::size_t>(before.stride);
    for (std::size_t index = 0; index < kObjectSlotCount; ++index) {
        const bool beforeReadable = before.readable[index];
        const bool afterReadable = after.readable[index];
        std::size_t changedBytes = 0;
        std::size_t firstOffset = stride;
        std::size_t lastOffset = 0;

        if (beforeReadable != afterReadable) {
            changedBytes = stride;
            firstOffset = 0;
            lastOffset = stride - 1;
        } else if (beforeReadable) {
            for (std::size_t offset = 0; offset < stride; ++offset) {
                if (before.records[index][offset] == after.records[index][offset]) {
                    continue;
                }
                if (changedBytes == 0) {
                    firstOffset = offset;
                }
                lastOffset = offset;
                ++changedBytes;
            }
        }

        if (changedBytes == 0) {
            continue;
        }
        ++comparison.totalChanged;
        comparison.totalChangedBytes += changedBytes;
        if (comparison.retained >= comparison.rows.size()) {
            continue;
        }
        ChangedRecord& row = comparison.rows[comparison.retained++];
        row.index = static_cast<std::uint16_t>(index);
        row.changedBytes = static_cast<std::uint16_t>(
            (std::min)(changedBytes, static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())));
        row.exclusiveBytes = row.changedBytes;
        row.firstOffset = static_cast<std::uint16_t>(firstOffset);
        row.lastOffset = static_cast<std::uint16_t>(lastOffset);
    }
    comparison.comparedMs = GetTickCount64();
    comparison.valid = true;
}

/** @return True when the normal-control captures changed one exact record byte. */
[[nodiscard]] bool normal_changed_at(std::size_t index, std::size_t offset) noexcept {
    if (!compatible(g_normalBefore, g_normalAfter) || index >= kObjectSlotCount
        || offset >= g_normalBefore.stride) {
        return false;
    }
    if (g_normalBefore.readable[index] != g_normalAfter.readable[index]) {
        return true;
    }
    if (!g_normalBefore.readable[index]) {
        return false;
    }
    return g_normalBefore.records[index][offset] != g_normalAfter.records[index][offset];
}

/** Recomputes local-action-only byte counts after a normal control is available. */
void update_action_exclusive_counts() noexcept {
    if (!g_actionComparison.valid || !compatible(g_actionBefore, g_actionAfter)) {
        return;
    }
    const std::size_t stride = static_cast<std::size_t>(g_actionBefore.stride);
    for (std::size_t rowIndex = 0; rowIndex < g_actionComparison.retained; ++rowIndex) {
        ChangedRecord& row = g_actionComparison.rows[rowIndex];
        const std::size_t index = row.index;
        std::size_t exclusive = 0;
        if (g_actionBefore.readable[index] != g_actionAfter.readable[index]) {
            exclusive = normal_changed_at(index, 0) ? 0 : stride;
        } else if (g_actionBefore.readable[index]) {
            for (std::size_t offset = 0; offset < stride; ++offset) {
                const bool changed =
                    g_actionBefore.records[index][offset] != g_actionAfter.records[index][offset];
                if (changed && !normal_changed_at(index, offset)) {
                    ++exclusive;
                }
            }
        }
        row.exclusiveBytes = static_cast<std::uint16_t>(
            (std::min)(exclusive, static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())));
    }
}

/** Captures a normal-control baseline without changing any gameplay state. */
void capture_normal_baseline() noexcept {
    clear_capture(g_normalAfter);
    clear_comparison(g_normalComparison);
    (void)capture_table(g_normalBefore);
}

/** Captures a normal-control endpoint and computes background object-record churn. */
void compare_normal_now() noexcept {
    if (!capture_table(g_normalAfter)) {
        clear_comparison(g_normalComparison);
        return;
    }
    compare_captures(g_normalBefore, g_normalAfter, g_normalComparison);
    update_action_exclusive_counts();
}

/** Captures the pre-action table state. The user performs one local gameplay action afterward. */
void capture_action_baseline() noexcept {
    clear_capture(g_actionAfter);
    clear_comparison(g_actionComparison);
    if (!capture_table(g_actionBefore)) {
        g_actionBaselineMs = 0;
        g_actionBaselineReady = false;
        return;
    }
    g_actionBaselineMs = g_actionBefore.capturedMs;
    g_actionBaselineReady = true;
}

/** Captures the post-action table and builds a comparison against the manual action baseline. */
/** Captures the post-action state when the user presses Compare Action Now. */
void compare_action_now() noexcept {
    if (!g_actionBefore.valid || !capture_table(g_actionAfter)) {
        clear_comparison(g_actionComparison);
        return;
    }
    compare_captures(g_actionBefore, g_actionAfter, g_actionComparison);
    update_action_exclusive_counts();
}

/** Formats at most eight bytes from one changed run. */
void format_preview(std::span<const std::byte> bytes,
                    std::size_t offset,
                    std::size_t length,
                    std::array<char, 48>& output) noexcept {
    output = {};
    if (offset >= bytes.size()) {
        std::snprintf(output.data(), output.size(), "-");
        return;
    }
    const std::size_t count =
        (std::min)({length, kHexPreviewCapacity, bytes.size() - offset});
    std::size_t used = 0;
    for (std::size_t index = 0; index < count && used + 3 < output.size(); ++index) {
        const int written = std::snprintf(output.data() + used,
                                          output.size() - used,
                                          "%02X",
                                          std::to_integer<unsigned>(bytes[offset + index]));
        if (written <= 0) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    if (length > count && used + 4 < output.size()) {
        std::snprintf(output.data() + used, output.size() - used, "...");
    }
}

/** Formats a scalar interpretation for naturally-sized changed runs. */
void format_interpretation(std::span<const std::byte> before,
                           std::span<const std::byte> after,
                           std::size_t offset,
                           std::size_t length,
                           std::array<char, 128>& output) noexcept {
    output = {};
    if (offset >= before.size() || offset >= after.size()) {
        std::snprintf(output.data(), output.size(), "-");
        return;
    }

    if (length == 1) {
        std::snprintf(output.data(),
                      output.size(),
                      "u8 %u -> %u",
                      std::to_integer<unsigned>(before[offset]),
                      std::to_integer<unsigned>(after[offset]));
        return;
    }

    if (length == 2 || length == 4 || length == 8) {
        std::uint64_t left = 0;
        std::uint64_t right = 0;
        std::memcpy(&left, before.data() + offset, length);
        std::memcpy(&right, after.data() + offset, length);
        const bool pointerLike = length == 8 && right != 0
                                 && readable_range(static_cast<std::uintptr_t>(right), 1);
        std::snprintf(output.data(),
                      output.size(),
                      "u%zu 0x%llX -> 0x%llX%s",
                      length * 8,
                      static_cast<unsigned long long>(left),
                      static_cast<unsigned long long>(right),
                      pointerLike ? "  (readable ptr)" : "");
        return;
    }
    std::snprintf(output.data(), output.size(), "%zu changed bytes", length);
}

/** @return A pointer to one x64 debug address register. */
[[nodiscard]] DWORD64* debug_register(CONTEXT& context, std::size_t slot) noexcept {
    switch (slot) {
    case 0:
        return &context.Dr0;
    case 1:
        return &context.Dr1;
    case 2:
        return &context.Dr2;
    case 3:
        return &context.Dr3;
    default:
        return nullptr;
    }
}

/** @return True when either local or global enable is set for one debug slot. */
[[nodiscard]] bool debug_slot_enabled(const CONTEXT& context, std::size_t slot) noexcept {
    const DWORD64 mask = static_cast<DWORD64>(0x3ULL << (slot * 2));
    return (context.Dr7 & mask) != 0;
}

/** Clears one x64 hardware-breakpoint slot from a context. */
void clear_debug_slot(CONTEXT& context, std::size_t slot) noexcept {
    DWORD64* const address = debug_register(context, slot);
    if (address != nullptr) {
        *address = 0;
    }
    context.Dr7 &= ~static_cast<DWORD64>(0x3ULL << (slot * 2));
    context.Dr7 &= ~static_cast<DWORD64>(0xFULL << (16 + slot * 4));
}

/** @return Intel DR7 length encoding for a 1/2/4/8-byte hardware data breakpoint. */
[[nodiscard]] DWORD64 debug_length_code(std::size_t length) noexcept {
    switch (length) {
    case 1:
        return 0;
    case 2:
        return 1;
    case 4:
        return 3;
    case 8:
        return 2;
    default:
        return 0;
    }
}

/** Installs one write-only local hardware breakpoint into a free context slot. */
void set_debug_slot(CONTEXT& context,
                    std::size_t slot,
                    std::uintptr_t address,
                    std::size_t length) noexcept {
    DWORD64* const target = debug_register(context, slot);
    if (target == nullptr) {
        return;
    }
    *target = static_cast<DWORD64>(address);
    context.Dr7 &= ~static_cast<DWORD64>(0x3ULL << (slot * 2));
    context.Dr7 &= ~static_cast<DWORD64>(0xFULL << (16 + slot * 4));
    context.Dr7 |= static_cast<DWORD64>(1ULL << (slot * 2)); // local enable
    const DWORD64 rwAndLength = 1ULL | (debug_length_code(length) << 2); // RW=01: writes
    context.Dr7 |= static_cast<DWORD64>(rwAndLength << (16 + slot * 4));
}

/** Removes this probe's watch address from every current process thread. */
void clear_writer_watchpoints() noexcept {
    if (g_writerTrace.address == 0) {
        return;
    }
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    BOOL available = Thread32First(snapshot, &entry);
    const DWORD processId = GetCurrentProcessId();
    const DWORD currentThreadId = GetCurrentThreadId();
    while (available != FALSE) {
        if (entry.th32OwnerProcessID == processId && entry.th32ThreadID != currentThreadId) {
            const HANDLE thread =
                OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                           FALSE,
                           entry.th32ThreadID);
            if (thread != nullptr) {
                const DWORD suspendCount = SuspendThread(thread);
                if (suspendCount != static_cast<DWORD>(-1)) {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(thread, &context) != FALSE) {
                        bool changed = false;
                        for (std::size_t slot = 0; slot < 4; ++slot) {
                            DWORD64* const address = debug_register(context, slot);
                            if (address != nullptr
                                && *address == static_cast<DWORD64>(g_writerTrace.address)
                                && debug_slot_enabled(context, slot)) {
                                clear_debug_slot(context, slot);
                                changed = true;
                            }
                        }
                        if (changed) {
                            (void)SetThreadContext(thread, &context);
                        }
                    }
                    (void)ResumeThread(thread);
                }
                CloseHandle(thread);
            }
        }
        available = Thread32Next(snapshot, &entry);
    }
    CloseHandle(snapshot);
}

/** Vectored handler for the probe's one-shot hardware write watch. */
LONG CALLBACK writer_exception_handler(PEXCEPTION_POINTERS exception) noexcept {
    if (exception == nullptr || exception->ExceptionRecord == nullptr
        || exception->ContextRecord == nullptr
        || exception->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CONTEXT& context = *exception->ContextRecord;
    DWORD64 ours = 0;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        const DWORD64 bit = static_cast<DWORD64>(1ULL << slot);
        DWORD64* const address = debug_register(context, slot);
        if ((context.Dr6 & bit) != 0 && address != nullptr
            && *address == static_cast<DWORD64>(g_writerTrace.address)) {
            ours |= bit;
            clear_debug_slot(context, slot);
        }
    }
    if (ours == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    context.Dr6 &= ~ours;

    if (g_writerArmed.exchange(false, std::memory_order_acq_rel)) {
        g_writerTrace.writerRip = static_cast<std::uintptr_t>(context.Rip);
        g_writerTrace.threadId = GetCurrentThreadId();
        const auto* source =
            reinterpret_cast<const volatile std::uint8_t*>(g_writerTrace.address);
        for (std::size_t index = 0; index < g_writerTrace.length; ++index) {
            g_writerTrace.after[index] = static_cast<std::byte>(source[index]);
        }
        g_writerCaptured.store(true, std::memory_order_release);
    }

    const DWORD64 other = context.Dr6 & 0xFULL;
    return other == 0 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/** Removes live debug registers/handler while optionally preserving the captured result. */
void stop_writer_watch(bool clearResult) noexcept {
    g_writerArmed.store(false, std::memory_order_release);
    clear_writer_watchpoints();
    if (g_writerHandler != nullptr) {
        (void)RemoveVectoredExceptionHandler(g_writerHandler);
        g_writerHandler = nullptr;
    }
    if (clearResult) {
        g_writerTrace = {};
        g_writerCaptured.store(false, std::memory_order_release);
    }
}

/** Finishes cleanup on the first UI frame after the watched write fired. */
void finalize_writer_watch() noexcept {
    if (!g_writerCaptured.load(std::memory_order_acquire) || g_writerHandler == nullptr) {
        return;
    }
    clear_writer_watchpoints();
    (void)RemoveVectoredExceptionHandler(g_writerHandler);
    g_writerHandler = nullptr;
}

/** Arms the watched address on every current process thread that has a free DR slot. */
[[nodiscard]] bool arm_writer_watch(std::uintptr_t address,
                                    std::size_t length,
                                    std::size_t recordIndex,
                                    std::size_t recordOffset) noexcept {
    stop_writer_watch(true);
    if (address == 0 || recordIndex >= kObjectSlotCount || recordOffset > 0xFFFFU
        || (length != 1 && length != 2 && length != 4 && length != 8)
        || address % length != 0 || !readable_range(address, length)) {
        g_writerTrace.attempted = true;
        return false;
    }

    g_writerTrace.attempted = true;
    g_writerTrace.address = address;
    g_writerTrace.recordIndex = static_cast<std::uint16_t>(recordIndex);
    g_writerTrace.recordOffset = static_cast<std::uint16_t>(recordOffset);
    g_writerTrace.length = static_cast<std::uint8_t>(length);
    if (!process_memory::read_current_process(
            nullptr, address, std::span(g_writerTrace.before).first(length))) {
        return false;
    }

    g_writerHandler = AddVectoredExceptionHandler(1, &writer_exception_handler);
    if (g_writerHandler == nullptr) {
        return false;
    }
    g_writerCaptured.store(false, std::memory_order_release);
    g_writerArmed.store(true, std::memory_order_release);

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        stop_writer_watch(false);
        return false;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    BOOL available = Thread32First(snapshot, &entry);
    const DWORD processId = GetCurrentProcessId();
    const DWORD currentThreadId = GetCurrentThreadId();
    while (available != FALSE && !g_writerCaptured.load(std::memory_order_acquire)) {
        if (entry.th32OwnerProcessID == processId && entry.th32ThreadID != currentThreadId) {
            const HANDLE thread =
                OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                           FALSE,
                           entry.th32ThreadID);
            if (thread == nullptr) {
                ++g_writerTrace.failedThreads;
            } else {
                const DWORD suspendCount = SuspendThread(thread);
                if (suspendCount == static_cast<DWORD>(-1)) {
                    ++g_writerTrace.failedThreads;
                } else {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(thread, &context) == FALSE) {
                        ++g_writerTrace.failedThreads;
                    } else {
                        std::size_t freeSlot = 4;
                        for (std::size_t slot = 0; slot < 4; ++slot) {
                            if (!debug_slot_enabled(context, slot)) {
                                freeSlot = slot;
                                break;
                            }
                        }
                        if (freeSlot == 4) {
                            ++g_writerTrace.occupiedDebugSlots;
                        } else {
                            set_debug_slot(context, freeSlot, address, length);
                            if (SetThreadContext(thread, &context) != FALSE) {
                                ++g_writerTrace.armedThreads;
                            } else {
                                ++g_writerTrace.failedThreads;
                            }
                        }
                    }
                    (void)ResumeThread(thread);
                }
                CloseHandle(thread);
            }
        }
        available = Thread32Next(snapshot, &entry);
    }
    CloseHandle(snapshot);

    if (g_writerTrace.armedThreads == 0
        && !g_writerCaptured.load(std::memory_order_acquire)) {
        stop_writer_watch(false);
        return false;
    }
    return true;
}

/** Chooses the widest naturally-aligned watchpoint contained by a selected changed run. */
[[nodiscard]] std::size_t writer_watch_length(std::uintptr_t address,
                                              std::size_t runLength) noexcept {
    constexpr std::array<std::size_t, 4> widths{8, 4, 2, 1};
    for (const std::size_t width : widths) {
        if (runLength >= width && address % width == 0) {
            return width;
        }
    }
    return 1;
}

/** Arms a one-shot hardware writer watch for the selected native record bytes. */
[[nodiscard]] bool arm_selected_writer_watch() noexcept {
    if (!g_locator.ok || g_selectedCandidate >= g_locator.candidateCount
        || g_selectedRecord >= kObjectSlotCount || g_selectedRunLength == 0
        || g_selectedRunOffset >= g_locator.stride) {
        return false;
    }

    const Candidate& candidate = g_locator.candidates[g_selectedCandidate];
    const std::uintptr_t recordOffset =
        static_cast<std::uintptr_t>(g_selectedRecord) * g_locator.stride;
    if (candidate.baseAddress > std::numeric_limits<std::uintptr_t>::max() - recordOffset) {
        return false;
    }
    const std::uintptr_t recordAddress = candidate.baseAddress + recordOffset;
    if (recordAddress > std::numeric_limits<std::uintptr_t>::max() - g_selectedRunOffset) {
        return false;
    }
    const std::uintptr_t watchAddress = recordAddress + g_selectedRunOffset;
    const std::size_t watchLength = writer_watch_length(watchAddress, g_selectedRunLength);
    return arm_writer_watch(
        watchAddress, watchLength, g_selectedRecord, g_selectedRunOffset);
}

/** Writes the current object-record watcher state to Sunrise's normal client log. */
void write_snapshot_to_log() noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=native_entity_probe stage=locator ok=%u getter=0x%llX "
                                "handle=0x%X index=%u stride_addr=0x%llX stride=%u candidates=%zu "
                                "selected=%zu",
                                g_locator.ok ? 1U : 0U,
                                static_cast<unsigned long long>(
                                    reinterpret_cast<std::uintptr_t>(g_locator.getter)),
                                g_locator.controlledHandle,
                                g_locator.controlledIndex,
                                static_cast<unsigned long long>(g_locator.strideAddress),
                                g_locator.stride,
                                g_locator.candidateCount,
                                g_selectedCandidate);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
    }

    if (g_locator.ok && g_selectedCandidate < g_locator.candidateCount) {
        const Candidate& selected = g_locator.candidates[g_selectedCandidate];
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=native_entity_probe stage=table base=0x%llX record=0x%llX "
            "normal_changed=%zu normal_bytes=%zu action_changed=%zu action_bytes=%zu "
            "action_baseline=%u",
            static_cast<unsigned long long>(selected.baseAddress),
            static_cast<unsigned long long>(selected.playerRecord),
            g_normalComparison.totalChanged,
            g_normalComparison.totalChangedBytes,
            g_actionComparison.totalChanged,
            g_actionComparison.totalChangedBytes,
            g_actionBaselineReady ? 1U : 0U);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
        }
    }

    const std::size_t count =
        (std::min)(g_actionComparison.retained, static_cast<std::size_t>(128));
    for (std::size_t index = 0; index < count; ++index) {
        const ChangedRecord& row = g_actionComparison.rows[index];
        written = std::snprintf(line.data(),
                                line.size(),
                                "ev=native_entity_probe stage=action_change index=%u bytes=%u "
                                "exclusive=%u first=0x%X last=0x%X",
                                static_cast<unsigned>(row.index),
                                static_cast<unsigned>(row.changedBytes),
                                static_cast<unsigned>(row.exclusiveBytes),
                                static_cast<unsigned>(row.firstOffset),
                                static_cast<unsigned>(row.lastOffset));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::debug,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
        }
    }

    const bool captured = g_writerCaptured.load(std::memory_order_acquire);
    if (g_writerTrace.attempted) {
        const auto moduleBase =
            reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t moduleOffset =
            captured && moduleBase != 0 && g_writerTrace.writerRip >= moduleBase
                ? g_writerTrace.writerRip - moduleBase
                : 0;
        written = std::snprintf(
            line.data(),
            line.size(),
            "ev=native_entity_probe stage=writer captured=%u armed=%u record=%u "
            "offset=0x%X address=0x%llX width=%u rip=0x%llX module_off=0x%llX thread=%lu "
            "threads=%zu occupied=%zu failed=%zu",
            captured ? 1U : 0U,
            g_writerArmed.load(std::memory_order_acquire) ? 1U : 0U,
            static_cast<unsigned>(g_writerTrace.recordIndex),
            static_cast<unsigned>(g_writerTrace.recordOffset),
            static_cast<unsigned long long>(g_writerTrace.address),
            static_cast<unsigned>(g_writerTrace.length),
            static_cast<unsigned long long>(g_writerTrace.writerRip),
            static_cast<unsigned long long>(moduleOffset),
            static_cast<unsigned long>(g_writerTrace.threadId),
            g_writerTrace.armedThreads,
            g_writerTrace.occupiedDebugSlots,
            g_writerTrace.failedThreads);
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {line.data(),
                              (std::min)(static_cast<std::size_t>(written), line.size() - 1)});
        }
    }
}

/** Draws candidate object-table bases recovered from the native controlled-object getter. */
void draw_locator() noexcept {
    if (!ImGui::CollapsingHeader("Native Object System Locator", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::TextWrapped(
        "Uses the local-player controlled-object getter as a native object-system anchor. The "
        "getter masks a 13-bit object index, multiplies it by a runtime record stride, then reaches "
        "RIP-relative globals. This pass is read-only; it ranks table-base candidates by whether "
        "the controlled object's computed record is readable and contains its handle/index.");

    if (g_writerArmed.load(std::memory_order_acquire)) {
        ImGui::TextDisabled("Resolve / Refresh is disabled while a writer watch is armed.");
    } else if (ImGui::Button("Resolve / Refresh Object System")) {
        resolve_object_system();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write Probe Snapshot to Log")) {
        write_snapshot_to_log();
    }

    if (!g_locator.attempted) {
        ImGui::TextDisabled("Press Resolve / Refresh Object System while in a destination.");
        return;
    }
    if (g_locator.getter == nullptr) {
        ImGui::Text("Result: failed - controlled-object getter signature was not unique.");
        return;
    }
    if (g_locator.controlledHandle == kInvalidHandle) {
        ImGui::Text("Result: waiting - the native getter reports no controlled object.");
        return;
    }
    if (g_locator.stride == 0) {
        ImGui::Text("Result: failed - the getter's record stride was not readable/sane.");
        return;
    }

    ImGui::Text("Getter: 0x%llX",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_locator.getter)));
    ImGui::Text("Controlled handle: 0x%X   index: %u / 8191",
                g_locator.controlledHandle,
                g_locator.controlledIndex);
    ImGui::Text("Record stride: %u   stride global: 0x%llX",
                g_locator.stride,
                static_cast<unsigned long long>(g_locator.strideAddress));
    ImGui::Text("Readable candidate bases: %zu", g_locator.candidateCount);

    if (g_locator.candidateCount == 0) {
        ImGui::TextDisabled(
            "No inline table base validated yet. The getter/stride anchor still resolved; the "
            "next step would be decoding its post-imul instructions more specifically.");
        return;
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("object_table_candidates", 11, flags, ImVec2(0.0F, 260.0F))) {
        return;
    }
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Src");
    ImGui::TableSetupColumn("Op");
    ImGui::TableSetupColumn("Mode");
    ImGui::TableSetupColumn("Global");
    ImGui::TableSetupColumn("Base");
    ImGui::TableSetupColumn("Player rec");
    ImGui::TableSetupColumn("Score");
    ImGui::TableSetupColumn("H32");
    ImGui::TableSetupColumn("H16");
    ImGui::TableSetupColumn("I32/I16");
    ImGui::TableHeadersRow();

    for (std::size_t index = 0; index < g_locator.candidateCount; ++index) {
        const Candidate& row = g_locator.candidates[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char label[16]{};
        std::snprintf(label, sizeof label, "%zu", index);
        if (ImGui::Selectable(label,
                              index == g_selectedCandidate,
                              ImGuiSelectableFlags_SpanAllColumns)) {
            stop_writer_watch(true);
            g_selectedCandidate = index;
            clear_capture(g_normalBefore);
            clear_capture(g_normalAfter);
            clear_capture(g_actionBefore);
            clear_capture(g_actionAfter);
            clear_comparison(g_normalComparison);
            clear_comparison(g_actionComparison);
            g_selectedRecord = kObjectSlotCount;
            g_selectedRunOffset = 0;
            g_selectedRunLength = 0;
            g_selectedDiff = DiffKind::none;
            g_actionBaselineMs = 0;
            g_actionBaselineReady = false;
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("0x%zX", row.sourceOffset);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(operation_label(row.operation));
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(mode_label(row.mode));
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("0x%llX", static_cast<unsigned long long>(row.globalAddress));
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("0x%llX", static_cast<unsigned long long>(row.baseAddress));
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("0x%llX", static_cast<unsigned long long>(row.playerRecord));
        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%d", row.score);
        ImGui::TableSetColumnIndex(8);
        row.fullHandleOffset >= 0 ? ImGui::Text("0x%X", row.fullHandleOffset)
                                  : ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(9);
        row.lowHandleOffset >= 0 ? ImGui::Text("0x%X", row.lowHandleOffset)
                                 : ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(10);
        if (row.index32Offset >= 0 || row.index16Offset >= 0) {
            ImGui::Text("%s0x%X / %s0x%X",
                        row.index32Offset >= 0 ? "" : "-",
                        row.index32Offset >= 0 ? row.index32Offset : 0,
                        row.index16Offset >= 0 ? "" : "-",
                        row.index16Offset >= 0 ? row.index16Offset : 0);
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/** Draws one record-level comparison and lets the user select a row for byte inspection. */
void draw_comparison_table(const char* tableId,
                           const Comparison& comparison,
                           DiffKind kind,
                           bool showExclusive) noexcept {
    if (!comparison.valid) {
        ImGui::TextDisabled("No comparison captured yet.");
        return;
    }

    ImGui::Text("Changed records: %zu / 8192   changed bytes: %zu   retained: %zu   readable now: %zu",
                comparison.totalChanged,
                comparison.totalChangedBytes,
                comparison.retained,
                comparison.currentReadable);
    if (comparison.totalChanged > comparison.retained) {
        ImGui::TextDisabled("Row list truncated at %zu records.", comparison.rows.size());
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    const int columns = showExclusive ? 7 : 6;
    if (!ImGui::BeginTable(tableId, columns, flags, ImVec2(0.0F, 220.0F))) {
        return;
    }
    ImGui::TableSetupColumn("Index");
    ImGui::TableSetupColumn("Player?");
    ImGui::TableSetupColumn("Bytes");
    if (showExclusive) {
        ImGui::TableSetupColumn("Action-only");
    }
    ImGui::TableSetupColumn("First");
    ImGui::TableSetupColumn("Last");
    ImGui::TableSetupColumn("Select");
    ImGui::TableHeadersRow();

    for (std::size_t rowIndex = 0; rowIndex < comparison.retained; ++rowIndex) {
        const ChangedRecord& row = comparison.rows[rowIndex];
        ImGui::PushID(static_cast<int>(rowIndex));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%u", static_cast<unsigned>(row.index));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(row.index == g_locator.controlledIndex ? "yes" : "-");
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", static_cast<unsigned>(row.changedBytes));
        int column = 3;
        if (showExclusive) {
            ImGui::TableSetColumnIndex(column++);
            ImGui::Text("%u", static_cast<unsigned>(row.exclusiveBytes));
        }
        ImGui::TableSetColumnIndex(column++);
        ImGui::Text("0x%X", static_cast<unsigned>(row.firstOffset));
        ImGui::TableSetColumnIndex(column++);
        ImGui::Text("0x%X", static_cast<unsigned>(row.lastOffset));
        ImGui::TableSetColumnIndex(column);
        const bool selected = g_selectedDiff == kind && g_selectedRecord == row.index;
        if (ImGui::Selectable(selected ? "selected" : "inspect")) {
            g_selectedDiff = kind;
            g_selectedRecord = row.index;
            g_selectedRunOffset = 0;
            g_selectedRunLength = 0;
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

/** Selects the retained before/after pair currently driving the byte inspector. */
[[nodiscard]] bool selected_pair(const TableCapture*& before,
                                 const TableCapture*& after) noexcept {
    before = nullptr;
    after = nullptr;
    if (g_selectedDiff == DiffKind::normal) {
        before = &g_normalBefore;
        after = &g_normalAfter;
    } else if (g_selectedDiff == DiffKind::action) {
        before = &g_actionBefore;
        after = &g_actionAfter;
    }
    return before != nullptr && after != nullptr && compatible(*before, *after)
           && g_selectedRecord < kObjectSlotCount;
}

/** Draws exact changed byte runs for the selected native object record. */
void draw_selected_record_diff() noexcept {
    if (!ImGui::CollapsingHeader("Selected Record Byte Diff", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    const TableCapture* beforeCapture = nullptr;
    const TableCapture* afterCapture = nullptr;
    if (!selected_pair(beforeCapture, afterCapture)) {
        ImGui::TextDisabled("Select a changed record from the normal or local-action table.");
        return;
    }

    const std::span<const std::byte> before =
        captured_record(*beforeCapture, g_selectedRecord);
    const std::span<const std::byte> after =
        captured_record(*afterCapture, g_selectedRecord);
    ImGui::Text("Record %zu   source: %s   stride: %u",
                g_selectedRecord,
                g_selectedDiff == DiffKind::action ? "local action" : "normal control",
                beforeCapture->stride);
    if (before.empty() || after.empty()) {
        ImGui::TextDisabled(
            "This record changed readability state, so a byte-for-byte run is not available.");
        return;
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                      | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("selected_record_runs", 6, flags, ImVec2(0.0F, 260.0F))) {
        return;
    }
    ImGui::TableSetupColumn("Offset");
    ImGui::TableSetupColumn("Len");
    ImGui::TableSetupColumn("Before");
    ImGui::TableSetupColumn("After");
    ImGui::TableSetupColumn("Control");
    ImGui::TableSetupColumn("Interpretation");
    ImGui::TableHeadersRow();

    std::size_t offset = 0;
    while (offset < before.size()) {
        while (offset < before.size() && before[offset] == after[offset]) {
            ++offset;
        }
        if (offset >= before.size()) {
            break;
        }
        const std::size_t runStart = offset;
        while (offset < before.size() && before[offset] != after[offset]) {
            ++offset;
        }
        const std::size_t runLength = offset - runStart;

        std::size_t normalBytes = 0;
        if (g_selectedDiff == DiffKind::action) {
            for (std::size_t runOffset = 0; runOffset < runLength; ++runOffset) {
                if (normal_changed_at(g_selectedRecord, runStart + runOffset)) {
                    ++normalBytes;
                }
            }
        }

        std::array<char, 48> beforeText{};
        std::array<char, 48> afterText{};
        std::array<char, 128> interpretation{};
        format_preview(before, runStart, runLength, beforeText);
        format_preview(after, runStart, runLength, afterText);
        format_interpretation(before, after, runStart, runLength, interpretation);

        ImGui::PushID(static_cast<int>(runStart));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char label[24]{};
        std::snprintf(label, sizeof label, "0x%zX", runStart);
        const bool selected = g_selectedRecord < kObjectSlotCount
                              && g_selectedRunOffset == runStart
                              && g_selectedRunLength == runLength;
        if (ImGui::Selectable(label, selected)) {
            g_selectedRunOffset = runStart;
            g_selectedRunLength = runLength;
        }
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%zu", runLength);
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(beforeText.data());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(afterText.data());
        ImGui::TableSetColumnIndex(4);
        if (g_selectedDiff != DiffKind::action) {
            ImGui::TextUnformatted("-");
        } else if (!g_normalComparison.valid) {
            ImGui::TextUnformatted("no control");
        } else if (normalBytes == 0) {
            ImGui::TextUnformatted("action-only");
        } else if (normalBytes == runLength) {
            ImGui::TextUnformatted("background");
        } else {
            ImGui::Text("%zu/%zu bg", normalBytes, runLength);
        }
        ImGui::TableSetColumnIndex(5);
        ImGui::TextUnformatted(interpretation.data());
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (g_selectedRunLength == 0 || !g_locator.ok
        || g_selectedCandidate >= g_locator.candidateCount) {
        ImGui::TextDisabled(
            "Select one changed byte run above to prepare a native writer watchpoint.");
        return;
    }

    const Candidate& candidate = g_locator.candidates[g_selectedCandidate];
    const std::uintptr_t recordOffset =
        static_cast<std::uintptr_t>(g_selectedRecord) * g_locator.stride;
    const std::uintptr_t recordAddress = candidate.baseAddress + recordOffset;
    const std::uintptr_t watchAddress = recordAddress + g_selectedRunOffset;
    const std::size_t watchLength = writer_watch_length(watchAddress, g_selectedRunLength);
    ImGui::Separator();
    ImGui::Text("Writer target: record %zu + 0x%zX = 0x%llX   hardware width: %zu",
                g_selectedRecord,
                g_selectedRunOffset,
                static_cast<unsigned long long>(watchAddress),
                watchLength);
    ImGui::TextDisabled(
        "The writer trace uses a temporary x64 hardware write breakpoint in every current process "
        "thread with a free debug-register slot. It does not patch the object record. Arm it, close "
        "the overlay, and repeat the same local action.");

    if (ImGui::Button("Arm Writer Watch")) {
        (void)arm_selected_writer_watch();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop Writer Watch")) {
        stop_writer_watch(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Writer Result")) {
        stop_writer_watch(true);
    }

    if (!g_writerTrace.attempted) {
        ImGui::TextDisabled("Writer trace: not armed.");
        return;
    }

    const bool captured = g_writerCaptured.load(std::memory_order_acquire);
    const bool armed = g_writerArmed.load(std::memory_order_acquire);
    ImGui::Text("Writer trace: %s   threads armed: %zu   full-DR threads: %zu   failures: %zu",
                captured ? "CAPTURED" : (armed ? "armed" : "idle / missed"),
                g_writerTrace.armedThreads,
                g_writerTrace.occupiedDebugSlots,
                g_writerTrace.failedThreads);
    if (!captured) {
        return;
    }

    const auto moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::uintptr_t moduleOffset =
        moduleBase != 0 && g_writerTrace.writerRip >= moduleBase
            ? g_writerTrace.writerRip - moduleBase
            : 0;
    std::array<char, 48> beforeText{};
    std::array<char, 48> afterText{};
    format_preview(std::span(g_writerTrace.before),
                   0,
                   g_writerTrace.length,
                   beforeText);
    format_preview(std::span(g_writerTrace.after),
                   0,
                   g_writerTrace.length,
                   afterText);
    ImGui::Text("Post-write RIP: 0x%llX   Destiny2.exe + 0x%llX   thread: %lu",
                static_cast<unsigned long long>(g_writerTrace.writerRip),
                static_cast<unsigned long long>(moduleOffset),
                static_cast<unsigned long>(g_writerTrace.threadId));
    ImGui::Text("Watched bytes: %s -> %s", beforeText.data(), afterText.data());
    ImGui::TextDisabled(
        "x64 data breakpoints trap after the write executes, so this RIP is the instruction "
        "immediately following the writer. The writer itself is directly before this address.");
}

/** Draws normal-churn subtraction, local action capture, and the writer watch. */
void draw_record_watcher() noexcept {
    if (!ImGui::CollapsingHeader("Object Record Watcher", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (!g_locator.ok || g_selectedCandidate >= g_locator.candidateCount) {
        ImGui::TextDisabled("Resolve and select a native table candidate first.");
        return;
    }
    if (g_locator.stride > kRecordCaptureCapacity) {
        ImGui::Text("Record stride %u exceeds this watcher's %zu-byte capture bound.",
                    g_locator.stride,
                    kRecordCaptureCapacity);
        return;
    }

    const Candidate& candidate = g_locator.candidates[g_selectedCandidate];
    ImGui::Text("Selected base: 0x%llX   stride: %u   player record: 0x%llX",
                static_cast<unsigned long long>(candidate.baseAddress),
                g_locator.stride,
                static_cast<unsigned long long>(candidate.playerRecord));
    ImGui::TextWrapped(
        "The normal control tells us which record bytes churn on their own. The local-action "
        "capture is entirely client-side: take a baseline, perform one repeatable action that may "
        "create a transient entity (for example a grenade/projectile/ability), then compare. "
        "Action-only byte runs are the best writer-watch targets.");

    if (ImGui::CollapsingHeader("Normal Control", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Capture Normal Baseline")) {
            capture_normal_baseline();
        }
        ImGui::SameLine();
        if (ImGui::Button("Compare Normal Now")) {
            compare_normal_now();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Normal")) {
            clear_capture(g_normalBefore);
            clear_capture(g_normalAfter);
            clear_comparison(g_normalComparison);
            update_action_exclusive_counts();
        }

        const std::uint64_t now = GetTickCount64();
        if (g_normalBefore.valid) {
            ImGui::Text("Normal baseline: %zu readable records   age: %.2f s",
                        g_normalBefore.readableCount,
                        static_cast<double>(elapsed(now, g_normalBefore.capturedMs)) / 1000.0);
        } else {
            ImGui::TextDisabled("Normal baseline: not captured.");
        }
        draw_comparison_table(
            "normal_record_changes", g_normalComparison, DiffKind::normal, false);
    }

    if (ImGui::CollapsingHeader("Local Action Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Capture Action Baseline")) {
            capture_action_baseline();
        }
        ImGui::SameLine();
        if (ImGui::Button("Compare Action Now")) {
            compare_action_now();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear Action")) {
            clear_capture(g_actionBefore);
            clear_capture(g_actionAfter);
            clear_comparison(g_actionComparison);
            g_actionBaselineMs = 0;
            g_actionBaselineReady = false;
        }

        const std::uint64_t now = GetTickCount64();
        if (g_actionBefore.valid) {
            ImGui::Text("Action baseline: %zu readable records   age: %.2f s",
                        g_actionBefore.readableCount,
                        static_cast<double>(elapsed(now, g_actionBefore.capturedMs)) / 1000.0);
            ImGui::TextDisabled(
                "Close the overlay, perform exactly one repeatable local action, reopen it, then "
                "press Compare Action Now. Prefer a grenade/projectile/ability over a region move.");
        } else {
            ImGui::TextDisabled("Action baseline: not captured.");
        }
        if (g_actionAfter.valid) {
            ImGui::Text("Action comparison captured.");
        }
        draw_comparison_table(
            "action_record_changes", g_actionComparison, DiffKind::action, true);
    }

    draw_selected_record_diff();
}

} // namespace

/** Draws the entirely client-side native entity-system probe. */
void draw() noexcept {
    finalize_writer_watch();

    ImGui::TextUnformatted("Native Entity System Probe");
    ImGui::Separator();
    ImGui::TextWrapped(
        "This probe no longer uses Sunrise's server, roster, sensor-auth, or entity-slot state. "
        "It treats the controlled-object getter and the 8192 x 0x1E0 native record store as anchors "
        "into Destiny's local entity/object system, then uses repeatable local gameplay actions to "
        "find records that initialize/change and the native code that writes them.");
    ImGui::TextDisabled(
        "Table captures are read-only. The optional writer trace temporarily installs x64 hardware "
        "write breakpoints on existing process threads; it does not modify entity records or call "
        "a native create function.");

    draw_locator();
    draw_record_watcher();

    if (ImGui::CollapsingHeader("Recommended Capture", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped(
            "1) Resolve the object system and select candidate #0 after each launch. "
            "2) Capture Normal Baseline, stand still for roughly 5-10 seconds, then Compare Normal "
            "Now. 3) Capture Action Baseline, close the overlay, perform one repeatable local action "
            "(grenade/projectile/ability), reopen it, then Compare Action Now. "
            "4) Select an action record with nonzero Action-only bytes, then select one action-only "
            "byte run. 5) Press Arm Writer Watch, close the overlay, and repeat the same action. "
            "A captured Destiny2.exe+offset is our direct lead into the native entity system; the "
            "reported RIP is immediately after the instruction that performed the write.");
    }
}

} // namespace sunrise::client::ui::entities
