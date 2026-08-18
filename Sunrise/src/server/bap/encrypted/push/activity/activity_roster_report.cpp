#include <array>
#include <atomic>
#include <cstdio>
#include <string_view>

#include "../../../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

/** Log names for each outcome, in the enum's own order. */
constexpr std::array<const char*, 5> kOutcomeNames = {
    "ok", "no_epoch", "no_layout", "no_groups", "encode"};

/** A Garden World's normal Director destination. */
constexpr std::string_view kGardenWorldDestination = "strike_bond";
/** Emit the authored 21-slot map once per process; ordinary roster retries stay compact. */
std::atomic_bool g_gardenWorldRosterDumped{false};

/** Emits the exact server-side group/slot skeleton used to build msg 5, without changing it. */
void report_garden_world_roster(const message::Snapshot& snapshot) noexcept {
    if (g_gardenWorldRosterDumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    const message::Roster& roster = snapshot.roster;
    std::array<char, core::log::kLineCapacity> line{};
    for (std::size_t groupIndex = 0; groupIndex < roster.groupCount; ++groupIndex) {
        const message::Group& group = roster.groups[groupIndex];
        int written = std::snprintf(
            line.data(),
            line.size(),
            "ev=strike stage=roster_group group=%zu key=0x%08X slots=%zu player_group=%u "
            "player_key=0x%llX key_all_participation=%u",
            groupIndex,
            group.key,
            group.slotTypes.size(),
            group.key == roster.playerKeyGroup ? 1U : 0U,
            static_cast<unsigned long long>(snapshot.playerKey),
            snapshot.keyOnEveryParticipationSlot ? 1U : 0U);
        if (written > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }

        bool firstParticipation = true;
        for (std::size_t slot = 0; slot < group.slotTypes.size(); ++slot) {
            const std::uint8_t type = group.slotTypes[slot];
            const std::uint8_t flags =
                slot < group.slotFlags.size() ? group.slotFlags[slot] : 0U;
            const bool participation = type == 13U;
            const bool carriesPlayer =
                group.key == roster.playerKeyGroup && participation
                && (snapshot.keyOnEveryParticipationSlot || firstParticipation);
            written = std::snprintf(
                line.data(),
                line.size(),
                "ev=strike stage=roster_slot group=%zu index=%zu type=%u flags=0x%02X "
                "sense=%u auth=%u player_key=%u",
                groupIndex,
                slot,
                static_cast<unsigned>(type),
                static_cast<unsigned>(flags),
                (flags & message::kSlotSenseFlag) != 0 ? 1U : 0U,
                (flags & message::kSlotAuthFlag) != 0 ? 1U : 0U,
                carriesPlayer ? 1U : 0U);
            if (written > 0) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
            if (participation) {
                firstParticipation = false;
            }
        }
    }
}

} // namespace

/** Reports one roster push, and only when its outcome is new. */
void report_roster_push(Session& session,
                        const message::Snapshot& snapshot,
                        std::string_view destination,
                        std::size_t bytes,
                        std::int32_t grant,
                        RosterOutcome outcome) noexcept {
    const message::Roster& roster = snapshot.roster;
    const auto reason = static_cast<std::uint8_t>(outcome) + 1U;
    if (outcome != RosterOutcome::published && session.activityRosterReason == reason) {
        return;
    }
    session.activityRosterReason = static_cast<std::uint8_t>(reason);

    if (outcome == RosterOutcome::published && destination == kGardenWorldDestination) {
        report_garden_world_roster(snapshot);
    }

    std::size_t slots = 0;
    for (std::size_t index = 0; index < roster.groupCount; ++index) {
        slots += roster.groups[index].slotTypes.size();
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=roster result=%s dest=%.*s groups=%zu "
                      "objects=%zu bytes=%zu state=%u phase1_only=%u keygroup=0x%X grant=%d "
                      "region=%u slice=%u spawn=0x%X join=0x%llX player=0x%llX",
                      kOutcomeNames[static_cast<std::size_t>(outcome)],
                      static_cast<int>(destination.size()),
                      destination.data(),
                      roster.groupCount,
                      slots,
                      bytes,
                      session.activityRosterState,
                      snapshot.phaseOneOnly ? 1U : 0U,
                      roster.playerKeyGroup,
                      grant,
                      snapshot.region,
                      snapshot.spawnSliceSet,
                      snapshot.spawnSetHash,
                      static_cast<unsigned long long>(session.activityCharacterSoid),
                      static_cast<unsigned long long>(snapshot.playerKey));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         outcome == RosterOutcome::published ? core::log::Level::debug
                                                             : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::server::bap::encrypted::push::activity
