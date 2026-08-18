#include "activity_patch_epoch_route.h"

#include <array>
#include <cstdio>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/bap/activity_message/activity_patch_epoch_parser.h"

namespace sunrise::server::bap::encrypted::activity_message::patch_epoch {

namespace message = middleware::bap::activity_message::patch_epoch;

/** Parses type 52 and keeps its epoch for the next roster update. */
bool prepare(std::uint64_t sessionId,
             const middleware::bap::activity_message::Request& request,
             ActivityPlan& plan) noexcept {
    message::PatchEpoch epoch{};
    if (!message::parse_patch_epoch(request.payload, epoch)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=strike stage=patch_epoch result=parse_fail");
        return false;
    }

    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=strike stage=patch_epoch result=ok session=0x%llX first=0x%llX second=0x%llX",
        static_cast<unsigned long long>(sessionId),
        static_cast<unsigned long long>(epoch.first),
        static_cast<unsigned long long>(epoch.second));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }

    plan.patchEpoch = epoch;
    plan.sessionId = sessionId;
    plan.delivery = Delivery::none;
    plan.mutationDomain = MutationDomain::patchEpoch;
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_message::patch_epoch
