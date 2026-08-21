#include <Windows.h>

#include <array>

#include "../../../../core/filesystem/path.h"
#include "../../../../state/content/content_catalog.h"
#include "../../packages/package_keys.h"
#include "build.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

/** Installed packages sit beside the main executable. */
constexpr std::wstring_view kPackageDirectory = L"packages";
/** Compact name id of the container that names every investment definition table. */
constexpr std::uint32_t kInvestmentGlobalsNameHash = 0x6F7125CBU;

} // namespace

/** Reports whether the pass can read anything yet. */
bool readable() noexcept {
    reader::BlockKeys keys{};
    const bool available = collect_keys(keys);
    SecureZeroMemory(&keys, sizeof keys);
    return available;
}

/** Copies the block key material this pass borrows. */
bool collect_keys(reader::BlockKeys& keys) noexcept {
    return sunrise::client::content::packages::collect_block_keys(keys);
}

/** Collects every catalogue tag carrying the container name. */
bool investment_globals_tags(std::array<std::uint32_t, kContainerCandidates>& candidates,
                             std::size_t& count) noexcept {
    candidates = {};
    count = 0;
    std::array<state::content::Definition, kContainerCandidates> matches{};
    std::size_t matched = 0;
    if (!state::content::lookup_hash(kInvestmentGlobalsNameHash, matches, matched)) {
        return false;
    }
    for (std::size_t index = 0; index < matched; ++index) {
        if (matches[index].tag != 0) {
            candidates[count++] = matches[index].tag;
        }
    }
    return count != 0;
}

/** @param directory Receives the installed packages directory. @return True when it exists. */
bool package_directory(core::path::Buffer& directory) noexcept {
    if (!core::path::module_directory(GetModuleHandleW(nullptr), directory)
        || !core::path::append(directory, kPackageDirectory)) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(directory.chars.data());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

} // namespace sunrise::client::content::items::packages
