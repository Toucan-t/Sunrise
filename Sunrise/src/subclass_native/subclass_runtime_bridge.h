#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "../state/account/account_state.h"
#include "../state/build_data/socket_entry_lists/definition.h"

namespace sunrise::subclass_native {

inline constexpr std::uint8_t kNoDestinationBucket = 0xFF;
inline constexpr std::size_t kBucketMapCapacity = 32;

struct BucketMapRow final {
    std::uint16_t socketEntryListIndex{0xFFFF};
    std::uint8_t entryCount{};
    std::array<std::uint8_t, state::build_data::socket_entry_lists::kEntryCapacity> buckets{};
};

inline SRWLOCK g_bucketMapLock{SRWLOCK_INIT};
inline std::array<BucketMapRow, kBucketMapCapacity> g_bucketMaps{};
inline std::size_t g_bucketMapCount{};

inline void publish_entry_buckets(
    std::uint16_t socketEntryListIndex,
    const std::array<std::uint8_t, state::build_data::socket_entry_lists::kEntryCapacity>& buckets,
    std::size_t entryCount) noexcept {
    if (socketEntryListIndex == 0xFFFF || entryCount == 0 || entryCount > buckets.size()) {
        return;
    }
    AcquireSRWLockExclusive(&g_bucketMapLock);
    std::size_t row = g_bucketMapCount;
    for (std::size_t index = 0; index < g_bucketMapCount; ++index) {
        if (g_bucketMaps[index].socketEntryListIndex == socketEntryListIndex) {
            row = index;
            break;
        }
    }
    if (row == g_bucketMapCount) {
        if (g_bucketMapCount >= g_bucketMaps.size()) {
            ReleaseSRWLockExclusive(&g_bucketMapLock);
            return;
        }
        ++g_bucketMapCount;
    }
    g_bucketMaps[row].socketEntryListIndex = socketEntryListIndex;
    g_bucketMaps[row].entryCount = static_cast<std::uint8_t>(entryCount);
    g_bucketMaps[row].buckets = buckets;
    ReleaseSRWLockExclusive(&g_bucketMapLock);
}

[[nodiscard]] inline bool find_entry_bucket(std::uint16_t socketEntryListIndex,
                                            std::uint8_t entry,
                                            std::uint8_t& bucket) noexcept {
    bucket = kNoDestinationBucket;
    AcquireSRWLockShared(&g_bucketMapLock);
    for (std::size_t index = 0; index < g_bucketMapCount; ++index) {
        const BucketMapRow& row = g_bucketMaps[index];
        if (row.socketEntryListIndex == socketEntryListIndex && entry < row.entryCount) {
            bucket = row.buckets[entry];
            ReleaseSRWLockShared(&g_bucketMapLock);
            return bucket != kNoDestinationBucket;
        }
    }
    ReleaseSRWLockShared(&g_bucketMapLock);
    return false;
}

struct PendingSelection final {
    bool active{};
    std::uint64_t accountSoid{};
    std::uint64_t characterSoid{};
    std::uint64_t subclassInstanceSoid{};
    std::size_t characterIndex{};
    std::uint16_t socketEntryListIndex{};
    std::uint8_t requestedEntry{};
    state::CharacterState beforeCharacter{};
    state::CharacterState afterCharacter{};
};

inline thread_local PendingSelection g_pendingSelection{};

[[nodiscard]] inline PendingSelection& pending_selection() noexcept {
    return g_pendingSelection;
}

inline void clear_pending_selection() noexcept {
    g_pendingSelection = {};
}

} // namespace sunrise::subclass_native
