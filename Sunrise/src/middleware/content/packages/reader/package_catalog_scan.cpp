#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader {
namespace {

/** Entry indices are 13 bits of a tag handle, so a table cannot be larger than this. */
constexpr std::uint32_t kMaximumEntries = layout::kTagEntryMask + 1U;
/** Entry records are read in fixed batches to keep the table off the caller stack. */
constexpr std::size_t kBatchRecords = 256;
/** One bit per package id records which ids have already been cataloged. */
constexpr std::size_t kSeenWords = 1024;
/** 64 ids share one word of the seen set. */
constexpr std::uint16_t kSeenWordShift = 6;
/** The low bits of an id pick its bit inside that word. */
constexpr std::uint16_t kSeenBitMask = 63;
/** typeInfo bits 9..15 hold the seven-bit package file type. */
constexpr std::uint32_t kFileTypeShift = 9;
constexpr std::uint32_t kFileTypeMask = 0x7FU;
/** typeInfo bits 6..8 hold the three-bit package file subtype. */
constexpr std::uint32_t kFileSubtypeShift = 6;
constexpr std::uint32_t kFileSubtypeMask = 0x07U;
/** Bits occupied by the two decoded fields; all other bits stay visible as selectorBits. */
constexpr std::uint32_t kDecodedTypeMask = (kFileTypeMask << kFileTypeShift)
                                            | (kFileSubtypeMask << kFileSubtypeShift);

using SeenSet = std::array<std::uint64_t, kSeenWords>;

/** @param seen Cataloged-id set. @return True when this package id is new. */
[[nodiscard]] bool claim(SeenSet& seen, std::uint16_t packageId) noexcept {
    const std::uint64_t bit = std::uint64_t{1} << (packageId & kSeenBitMask);
    std::uint64_t& word = seen[packageId >> kSeenWordShift];
    if ((word & bit) != 0) {
        return false;
    }
    word |= bit;
    return true;
}

/** Records one failure without discarding the partial counters gathered before it. */
[[nodiscard]] bool fail(CatalogScanResult& result,
                        CatalogScanFailure failure,
                        std::uint16_t packageId = 0,
                        std::uint32_t patchIndex = 0,
                        std::uint32_t entryIndex = 0) noexcept {
    result.failure = failure;
    result.packageId = packageId;
    result.patchIndex = patchIndex;
    result.entryIndex = entryIndex;
    return false;
}

/** Builds the *.pkg search pattern for one installed package directory. */
[[nodiscard]] bool search_pattern(std::wstring_view directory, Path& search) noexcept {
    const bool separated = !directory.empty() && directory.back() == L'\\';
    const int written = std::swprintf(search.chars.data(),
                                      search.chars.size(),
                                      separated ? L"%.*s*.pkg" : L"%.*s\\*.pkg",
                                      static_cast<int>(directory.size()),
                                      directory.data());
    return written > 0;
}

/**
 * Reports every entry in one package id's highest installed patch.
 * Payload blocks are never opened; only the public header and entry table are read.
 */
[[nodiscard]] bool scan_package(std::wstring_view directory,
                                std::uint16_t packageId,
                                CatalogEntryVisitor visitor,
                                void* context,
                                CatalogScanResult& result) noexcept {
    Path stem{};
    std::uint32_t patchIndex = 0;
    if (!find_latest(directory, packageId, stem, patchIndex)) {
        return fail(result, CatalogScanFailure::locateLatest, packageId);
    }

    const std::wstring_view fullStem(stem.chars.data());
    const std::size_t separator = fullStem.find_last_of(L"\\/");
    const std::wstring_view packageFamily =
        separator == std::wstring_view::npos ? fullStem : fullStem.substr(separator + 1U);
    if (packageFamily.empty()) {
        return fail(result, CatalogScanFailure::packageFamily, packageId, patchIndex);
    }

    Path path{};
    if (!build_path(stem, patchIndex, path)) {
        return fail(result, CatalogScanFailure::buildPath, packageId, patchIndex);
    }

    std::array<std::byte, layout::kHeaderSize> headerBytes{};
    if (!read_at(path, 0, headerBytes)) {
        return fail(result, CatalogScanFailure::readHeader, packageId, patchIndex);
    }

    Header header{};
    if (!parse_header(headerBytes, header)) {
        return fail(result, CatalogScanFailure::parseHeader, packageId, patchIndex);
    }
    if (header.entryCount > kMaximumEntries) {
        return fail(result, CatalogScanFailure::entryCountOutOfRange, packageId, patchIndex);
    }

    ++result.packages;

    std::array<layout::EntryRecord, kBatchRecords> batch{};
    for (std::uint32_t index = 0; index < header.entryCount;) {
        const auto remaining = static_cast<std::size_t>(header.entryCount - index);
        const std::size_t count = (std::min)(batch.size(), remaining);
        const std::uint64_t offset =
            header.entryTable + static_cast<std::uint64_t>(index) * sizeof(layout::EntryRecord);
        if (!read_at(path, offset, std::as_writable_bytes(std::span{batch.data(), count}))) {
            return fail(
                result, CatalogScanFailure::readEntryTable, packageId, patchIndex, index);
        }

        for (std::size_t position = 0; position < count; ++position) {
            const std::uint32_t entryIndex = index + static_cast<std::uint32_t>(position);
            const layout::EntryRecord& record = batch[position];
            const layout::EntryPlacement entryPlacement = layout::placement(record);
            const std::uint32_t tag =
                layout::kTagBase + (static_cast<std::uint32_t>(packageId) << layout::kTagEntryBits)
                + entryIndex;

            const std::uint8_t fileType = static_cast<std::uint8_t>(
                (record.typeInfo >> kFileTypeShift) & kFileTypeMask);
            const std::uint8_t fileSubtype = static_cast<std::uint8_t>(
                (record.typeInfo >> kFileSubtypeShift) & kFileSubtypeMask);
            const std::uint32_t selectorBits = record.typeInfo & ~kDecodedTypeMask;

            ++result.entries;
            if (!visitor(context,
                         CatalogEntry{tag,
                                      record.reference,
                                      record.typeInfo,
                                      fileType,
                                      fileSubtype,
                                      selectorBits,
                                      entryPlacement.size,
                                      entryIndex,
                                      header.entryCount,
                                      packageId,
                                      patchIndex,
                                      packageFamily})) {
                return fail(
                    result, CatalogScanFailure::visitorRejected, packageId, patchIndex, entryIndex);
            }
        }
        index += static_cast<std::uint32_t>(count);
    }
    return true;
}

} // namespace

/** Reports every entry of every highest-generation installed package without decoding payloads. */
bool scan_entries(std::wstring_view directory,
                  CatalogEntryVisitor visitor,
                  void* context,
                  CatalogScanResult& result) noexcept {
    result = {};
    if (directory.empty() || visitor == nullptr) {
        return fail(result, CatalogScanFailure::invalidArguments);
    }

    Path search{};
    if (!search_pattern(directory, search)) {
        return fail(result, CatalogScanFailure::searchPattern);
    }

    WIN32_FIND_DATAW found{};
    const HANDLE enumeration = FindFirstFileW(search.chars.data(), &found);
    if (enumeration == INVALID_HANDLE_VALUE) {
        return fail(result, CatalogScanFailure::enumerateDirectory);
    }

    // Every patch of one package repeats its id. find_latest chooses the generation to catalog.
    SeenSet seen{};
    bool complete = true;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        std::uint16_t packageId = 0;
        std::uint32_t encounteredPatch = 0;
        if (!parse_leaf(found.cFileName, packageId, encounteredPatch) || !claim(seen, packageId)) {
            continue;
        }
        if (!scan_package(directory, packageId, visitor, context, result)) {
            complete = false;
            break;
        }
    } while (FindNextFileW(enumeration, &found) != 0);

    if (complete && GetLastError() != ERROR_NO_MORE_FILES) {
        complete = false;
        (void)fail(result, CatalogScanFailure::enumerateDirectoryAdvance);
    }

    const bool closed = FindClose(enumeration) != FALSE;
    if (!closed && complete) {
        complete = false;
        (void)fail(result, CatalogScanFailure::closeEnumeration);
    }
    return complete;
}

} // namespace sunrise::middleware::content::packages::reader
