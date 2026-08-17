#include <limits>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/bap/account_translation/account_translation_response.h"
#include "../../../../middleware/bap/activity_host/activity_host_response.h"
#include "../../../../middleware/bap/certificate.h"
#include "../../../../middleware/bap/client_config/client_config_response.h"
#include "../../../../middleware/bap/family_subscription.h"
#include "../../../../middleware/bap/family_unsubscription.h"
#include "../../../../middleware/bap/user_message/user_message_response.h"
#include "../../../../middleware/web_service/messages/opcode502.h"
#include "../../../../middleware/web_service/messages/opcode505/opcode505_codec.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../web_service/web_service_runtime.h"
#include "../activity_host_manager/activity_host_manager_route.h"
#include "../activity_message/activity_message_route.h"
#include "../internal.h"
#include "../matchmaking/matchmaking_route.h"
#include "../queuez/queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::body {

/**
 * Processes the body for one authenticated service route.
 * @param route Service route data found earlier.
 * @param queuezState Queuez versions and residents set up by this BAP peer.
 * @param activitySessionId Activity capability allocated through this BAP session.
 * @param matchmakingContext State-owned logical context for this BAP session.
 * @param requestBody Borrowed decrypted request body.
 * @param output Caller-owned response-body storage.
 * @param written Receives encoded body bytes.
 * @param outcome Receives one validated transport action or deferred State transaction.
 * @return True when the chosen body codec succeeds.
 */
bool process(const ServiceRoute& route,
             const queuez::SessionState& queuezState,
             std::uint64_t activitySessionId,
             state::matchmaking::ContextHandle matchmakingContext,
             std::span<const std::byte> requestBody,
             std::span<std::byte> output,
             std::size_t& written,
             ServiceOutcome& outcome) noexcept {
    outcome = {};
    switch (route.bodyCodec) {
    case BodyCodec::empty:
        written = 0;
        return true;
    case BodyCodec::accountTranslationResponse: {
        const state::AccountState account = state::account_snapshot();
        return middleware::bap::account_translation::encode_response(
            requestBody, account.primarySoid, output, written);
    }
    case BodyCodec::activityHostManagerResponse:
        return activity_host_manager::encode_response(requestBody,
                                                      output,
                                                      written,
                                                      outcome.activitySessionAllocation,
                                                      outcome.hasActivitySessionAllocation);
    case BodyCodec::activityMessageRequest:
        written = 0;
        return activity_message::process(
            activitySessionId, requestBody, outcome.activityPlan, outcome.hasActivityTransaction);
    case BodyCodec::activityHostResponse: {
        const state::SignOnState& signOn = state::sign_on();
        return middleware::bap::activity_host::encode_response(
            requestBody, signOn.relayAddress, signOn.relayPort, output, written);
    }
    case BodyCodec::clientConfigResponse:
        return middleware::bap::client_config::encode_minimal_response(output, written);
    case BodyCodec::familySubscription:
        written = 0;
        outcome.hasSubscription =
            middleware::bap::family_subscription::parse(requestBody, outcome.subscription);
        return outcome.hasSubscription;
    case BodyCodec::familyUnsubscription: {
        written = 0;
        outcome.hasUnsubscription =
            middleware::bap::family_unsubscription::parse(requestBody, outcome.unsubscription);
        return outcome.hasUnsubscription;
    }
    case BodyCodec::matchmakingResponse:
        return matchmaking::encode_response(matchmakingContext,
                                            requestBody,
                                            output,
                                            written,
                                            outcome.matchmakingMutation,
                                            outcome.hasMatchmakingMutation);
    case BodyCodec::steamCertificate:
        return middleware::bap::certificate::encode_response(requestBody, output, written);
    case BodyCodec::userMessageResponse:
        return middleware::bap::user_message::encode_minimal_response(output, written);
    case BodyCodec::webService: {
        middleware::web_service::Message message;
        const bool messageParsed = middleware::web_service::parse_request(requestBody, message);
        if (messageParsed
            && message.opcode == middleware::web_service::messages::opcode505::kOpcode) {
            if (!middleware::web_service::messages::opcode505::parse_request(message)
                || !queuez::stage_change_character(queuezState, outcome.changeCharacter)
                || !middleware::web_service::messages::opcode505::encode_response(
                    message, outcome.changeCharacter.after.family4Version, output, written)) {
                core::log::write(core::log::Channel::server,
                                 core::log::Level::warn,
                                 "ev=ws505 stage=change result=fail");
                // The plain status pair still goes out. The Client's Change Character waits on the
                // echoed transaction id, so a missing reply hangs it for the rest of the run.
                outcome.changeCharacter = {};
                return middleware::web_service::encode_response(
                    message,
                    middleware::web_service::ResponseShape::statusPair,
                    middleware::web_service::StatusResponse{},
                    output,
                    written);
            }
            outcome.hasChangeCharacter = true;
            return true;
        }
        const bool characterDelete =
            messageParsed && message.opcode == middleware::web_service::messages::opcode502::kOpcode;
        const std::uint64_t runtimeSelected =
            state::account::selected_character_soid(state::account_snapshot());
        // Opcode 502 belongs to character select. Opcode 505 clears the peer's account selection
        // before runtime State is necessarily updated, so a non-normal Family-3 phase is sufficient
        // proof that the stale selected marker no longer represents an active gameplay character.
        const bool characterDeleteReady =
            characterDelete && queuezState.family4Active && queuezState.family4RootSoid != 0
            && (runtimeSelected == 0 || queuezState.family3Phase != queuez::Family3Phase::normal);
        const std::int32_t nextFamily4Version =
            queuezState.family4Active
                    && (queuezState.family3Phase == queuez::Family3Phase::normal
                        || characterDeleteReady)
                    && queuezState.family4Version < (std::numeric_limits<std::int32_t>::max)()
                ? queuezState.family4Version + 1
                : (std::numeric_limits<std::int32_t>::min)();
        web_service::Outcome webOutcome;
        if (!sunrise::server::web_service::consume(
                requestBody, output, written, webOutcome, nextFamily4Version)) {
            return false;
        }
        outcome.hasSubscription = webOutcome.hasSubscription;
        outcome.subscription = webOutcome.subscription;
        outcome.hasCreatedCharacter = webOutcome.hasCreatedCharacter;
        outcome.createdCharacterSoid = webOutcome.createdCharacterSoid;
        outcome.hasCharacterDeletion = webOutcome.hasCharacterDeletion;
        outcome.characterDeletion = webOutcome.characterDeletion;
        outcome.hasEquipmentMutation = webOutcome.hasEquipmentMutation;
        outcome.equipmentMutation = webOutcome.equipmentMutation;
        outcome.hasItemAcquisition = webOutcome.hasItemAcquisition;
        outcome.itemAcquisition = webOutcome.itemAcquisition;
        outcome.hasProfileItemAcquisition = webOutcome.hasProfileItemAcquisition;
        outcome.profileItemAcquisition = webOutcome.profileItemAcquisition;
        outcome.hasItemDismantle = webOutcome.hasItemDismantle;
        outcome.itemDismantle = webOutcome.itemDismantle;
        outcome.hasProfileItemDismantle = webOutcome.hasProfileItemDismantle;
        outcome.profileItemDismantle = webOutcome.profileItemDismantle;
        outcome.hasSocketMutation = webOutcome.hasSocketMutation;
        outcome.socketMutation = webOutcome.socketMutation;
        outcome.hasItemStateMutation = webOutcome.hasItemStateMutation;
        outcome.itemStateMutation = webOutcome.itemStateMutation;
        // Normal opcode-504 picks stage their Family-4 move immediately. Opcode 501 now selects
        // the newly created Guardian in runtime State too, but its new item-instance residents must
        // be published before the character object can safely become resident. That create path is
        // sequenced by the deferred Queuez chain instead of racing this immediate move.
        if (!webOutcome.hasCreatedCharacter && webOutcome.hasSelectedCharacter
            && queuez::stage_select_character(
                queuezState, webOutcome.selectedCharacterSoid, outcome.selectCharacter)) {
            outcome.hasSelectCharacter = true;
        } else {
            outcome.selectCharacter = {};
        }
        return true;
    }
    }
    written = 0;
    return false;
}

} // namespace sunrise::server::bap::encrypted::body
