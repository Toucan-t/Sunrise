#include <Windows.h>

#include <array>

#include "../../patterns/game.h"
#include "network.h"
#include "network/network_content_derivation.h"
#include "relative.h"
#include "resolution/internal.h"

namespace sunrise::client::targets::game::network {
namespace {

Targets g_targets;

/** @param id Network signature identity. @return Group-local match index. */
[[nodiscard]] constexpr std::size_t index(patterns::game::Id id) noexcept {
    return static_cast<std::size_t>(id);
}

/** The token load's rip-relative displacement begins inside its lea. */
constexpr std::size_t kContentIdTokenDisplacementOffset = 14;
/** The token load's lea ends after its signed rel32. */
constexpr std::size_t kContentIdTokenInstructionEndOffset = 18;

/** Confirmed Season of Arrivals native bitstream helper RVAs used by the msg-5 diagnostic. */
constexpr std::size_t kMsg5ReadBitsRva = 0x003513B0;
constexpr std::size_t kMsg5ReadWideRva = 0x00351070;
constexpr std::size_t kMsg5ReadBoolRva = 0x00350EF0;
constexpr std::size_t kMsg5StreamStatusRva = 0x0034E9B0;

} // namespace

/**
 * Derives a network target table without publishing it.
 * @param image Game executable ranges.
 * @param matches Unique network matches.
 * @param output Receives the derived table.
 * @return True when every address validates.
 */
bool derive(std::span<const patterns::ImageRange> image,
            std::span<const patterns::Match> matches,
            Targets& output) noexcept {
    output = {};
    if (matches.size() != resolution::kNetworkMatchCount) {
        return false;
    }

    Targets resolved;
    resolved.transportKind = matches[index(patterns::game::Id::transportKind)].address;
    resolved.httpExecuteRequest = matches[index(patterns::game::Id::httpExecuteRequest)].address;
    resolved.signOnReadinessFailure =
        matches[index(patterns::game::Id::signOnReadinessFailure)].address;
    resolved.signOnReadinessReady =
        matches[index(patterns::game::Id::signOnReadinessReady)].address;
    resolved.contentConfigFetch = matches[index(patterns::game::Id::contentConfigFetch)].address;
    resolved.contentConfigTick = matches[index(patterns::game::Id::contentConfigTick)].address;
    std::byte* const contentManifestGate =
        matches[index(patterns::game::Id::contentManifestGate)].address;
    resolved.bubbleAuthorityDecoder =
        matches[index(patterns::game::Id::bubbleAuthorityDecoder)].address;
    resolved.contentUntrackedGetter =
        matches[index(patterns::game::Id::contentUntrackedGetter)].address;

    // These four helpers are pinned-executable diagnostics, not general signatures. An RVA outside
    // executable memory becomes null so the optional diagnostic group fails closed without
    // preventing the ordinary networking target table from publishing.
    auto* const moduleBase = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
    if (moduleBase == nullptr) {
        return false;
    }
    std::byte* const readBits = moduleBase + kMsg5ReadBitsRva;
    std::byte* const readWide = moduleBase + kMsg5ReadWideRva;
    std::byte* const readBool = moduleBase + kMsg5ReadBoolRva;
    std::byte* const streamStatus = moduleBase + kMsg5StreamStatusRva;
    resolved.msg5ReadBits = relative::contains(image, readBits) ? readBits : nullptr;
    resolved.msg5ReadWide = relative::contains(image, readWide) ? readWide : nullptr;
    resolved.msg5ReadBool = relative::contains(image, readBool) ? readBool : nullptr;
    resolved.msg5StreamStatus =
        relative::contains(image, streamStatus) ? streamStatus : nullptr;

    std::byte* const contentIdTokenLoad =
        matches[index(patterns::game::Id::contentIdTokenLoad)].address;
    if (!relative::resolve(contentIdTokenLoad,
                           kContentIdTokenDisplacementOffset,
                           kContentIdTokenInstructionEndOffset,
                           resolved.contentIdToken)
        || !content_derivation::derive(
            image, resolved.contentConfigFetch, contentManifestGate, resolved)) {
        return false;
    }
    output = resolved;
    return true;
}

/** @param targets Fully validated network table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
}

/** Resolves the early network targets and publishes only a complete group. */
bool resolve(std::span<const patterns::ImageRange> image) noexcept {
    g_targets = {};
    const auto definitions = patterns::game::definitions().first(resolution::kNetworkMatchCount);
    std::array<patterns::Match, resolution::kNetworkMatchCount> matches{};
    if (!patterns::resolve_all(image, definitions, matches)) {
        return false;
    }
    for (const patterns::Match match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }

    Targets resolved;
    if (!derive(image, matches, resolved)) {
        return false;
    }
    publish(resolved);
    return true;
}

/** Clears every published early network target. */
void clear() noexcept {
    g_targets = {};
}

/** @return Current immutable early network target group. */
const Targets& get() noexcept {
    return g_targets;
}

} // namespace sunrise::client::targets::game::network
