#include <Windows.h>

#include <algorithm>

#include "../../../core/logging/log.h"
#include "../../../middleware/secure_channel/runtime.h"
#include "../../../state/runtime/runtime.h"
#include "../internal.h"
#include "activity_transaction/activity_transaction_notifications.h"
#include "bap_connection_publication.h"
#include "internal.h"
#include "queuez/queuez_outcome_staging.h"
#include "transactions/service_outcome_commit.h"

namespace sunrise::server::bap::encrypted {
namespace {

/**
 * Wipes the part of one scratch buffer that may hold written bytes.
 * @param buffer Lock-owned scratch storage.
 * @param size Largest prefix that may hold transformed bytes.
 */
void clear_prefix(std::span<std::byte> buffer, std::size_t size) noexcept {
    SecureZeroMemory(buffer.data(), (std::min)(buffer.size(), size));
}

} // namespace

/**
 * Authenticates and answers one supported encrypted post-bootstrap request.
 * @param session Connection-owned authentication and nonce state.
 * @param scratch Lock-owned transform buffers kept off the Client thread stack.
 * @param outer Validated encrypted outer frame.
 * @param response Caller-owned complete-frame storage.
 * @param written Receives encoded response bytes.
 * @return True when routing succeeds and any response fits, commits State, and publishes its nonce.
 */
bool consume(Session& session,
             Scratch& scratch,
             const middleware::bap::OuterFrame& outer,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    written = 0;
    if (!session.authenticated) {
        // Staying silent here looks the same as a decode fault, and both look like a dead link.
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap stage=encrypted result=drop reason=unauthenticated");
        return false;
    }

    std::size_t plaintextSize = 0;
    const auto& bapState = state::bap();
    if (!middleware::secure_channel::open_frame(bapState.sessionKey,
                                                session.receiveNonce,
                                                outer.payload,
                                                scratch.plaintext,
                                                plaintextSize)) {
        const std::size_t possiblePlaintextSize =
            outer.payload.size() >= middleware::secure_channel::kFrameTagSize
                ? outer.payload.size() - middleware::secure_channel::kFrameTagSize
                : 0;
        clear_prefix(scratch.plaintext, possiblePlaintextSize);
        // The service is unreadable while the frame is sealed, so this line names no service.
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=none stage=decrypt result=fail");
        return false;
    }
    // Authentication consumes the receive nonce even when the inner service is unsupported.
    middleware::secure_channel::advance_nonce(session.receiveNonce);

    middleware::bap::RequestFrame frame;
    ServiceRoute route;
    std::size_t responseBodySize = 0;
    std::size_t framedSize = 0;
    ServiceOutcome outcome{};
    transactions::Publication publication{};
    queuez::SessionState nextQueuez = session.queuez;
    bool publishesQueuez = false;
    bool handled =
        middleware::bap::parse_request_payload(std::span(scratch.plaintext).first(plaintextSize),
                                               middleware::bap::FrameType::encrypted,
                                               frame)
        && routing::resolve(frame.messageId, route);
    if (!handled) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=bap svc=none stage=parse result=fail");
    }
    const bool processesBody = handled && route.responseMode != ResponseMode::none;
    const bool sendsReply = handled && route.responseMode == ResponseMode::reply;
    // Pure one-way services consume only the authenticated receive nonce.
    if (processesBody
        && !body::process(route,
                          session.queuez,
                          session.activitySessionId,
                          session.matchmakingContext,
                          frame.body,
                          scratch.responseBody,
                          responseBodySize,
                          outcome)) {
        diagnostics::report_failure(frame.messageId, "body");
        // A reply-mode service answers with an empty body instead of not at all. The Client
        // matches only the head of its pending ring, so one unanswered request jams that ring for
        // good and every later reply is rejected, which is worse than a thin reply.
        clear_prefix(scratch.responseBody, responseBodySize);
        responseBodySize = 0;
        SecureZeroMemory(&outcome, sizeof outcome);
        handled = sendsReply;
    }
    if (handled && sendsReply) {
        handled = reply::encode(scratch,
                                route,
                                frame.taskId,
                                bapState.sessionKey,
                                session.sendNonce,
                                std::span(scratch.responseBody).first(responseBodySize),
                                framedSize);
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "encode");
        }
    }
    // Stage every requested frame and check for caller room before committing State or the nonce.
    auto nextSendNonce = session.sendNonce;
    if (handled && sendsReply) {
        middleware::secure_channel::advance_nonce(nextSendNonce);
    }
    queuez::StagedPublication queuezPublication{};
    if (handled) {
        handled = queuez::stage_service_outcome(scratch,
                                                session.queuez,
                                                outcome,
                                                bapState.sessionKey,
                                                nextSendNonce,
                                                scratch.framed,
                                                framedSize,
                                                queuezPublication);
        if (handled && queuezPublication.hasState) {
            nextQueuez = queuezPublication.after;
            publishesQueuez = true;
        }
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "stage");
        }
    }
    if (handled && outcome.hasActivityTransaction) {
        const auto& activityPlan = outcome.activityPlan;
        handled = route.responseMode == ResponseMode::uncorrelatedPush
                  && activity_transaction::stage_notifications(session,
                                                               scratch,
                                                               activityPlan,
                                                               bapState.sessionKey,
                                                               nextSendNonce,
                                                               scratch.framed,
                                                               framedSize);
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "notify");
        }
    }
    // Committing the transaction clears the mutation the member key lives in, so the connection
    // fields are captured before the commit and published after it.
    const ConnectionFields connection = connection_fields(outcome);
    if (handled && processesBody) {
        // State changes become visible only after every requested frame and caller byte fit.
        handled = framedSize <= response.size() && transactions::commit(outcome, publication);
        if (!handled) {
            diagnostics::report_failure(frame.messageId, "commit");
        }
        if (handled) {
            std::copy_n(scratch.framed.begin(), framedSize, response.begin());
            written = framedSize;
            // The caller copy finishes before connection fields are published.
            session.sendNonce = nextSendNonce;
            if (publishesQueuez) {
                const std::uint64_t priorRoot = session.queuez.family4RootSoid;
                const queuez::Family3Phase priorFamily3Phase = session.queuez.family3Phase;
                session.queuez = nextQueuez;
                if (session.queuez.family4RootSoid != priorRoot
                    || session.queuez.family3Phase != priorFamily3Phase) {
                    // A successful root/selection transition supersedes the targeted editor chain
                    // with a fresh native publication. It is therefore also a valid checkpoint
                    // boundary for runtime State that was waiting on that older chain.
                    if (session.editorPersistencePending) {
                        const bool saved = state::checkpoint_characters();
                        core::log::write(core::log::Channel::server,
                                         saved ? core::log::Level::info : core::log::Level::warn,
                                         saved
                                             ? "ev=queuez stage=editor_checkpoint result=ok reason=transition"
                                             : "ev=queuez stage=editor_checkpoint result=fail reason=transition_disk_or_settings");
                    }
                    session.editorPersistencePending = false;
                    session.editorRefreshFailed = false;
                    session.editorFamily3Version = 0;
                    session.profileRefreshInstanceSoid = 0;
                    session.profileRefreshAddResident = false;
                    session.profileRefreshPending = false;
                    session.accountRefreshPending = false;
                    session.inventoryAddInstanceSoid = 0;
                    session.inventoryAddCharacterSoid = 0;
                    session.inventoryAddRefreshPending = false;
                    session.equipmentRefreshInstanceSoid = 0;
                    session.equipmentRefreshCharacterSoid = 0;
                    session.equipmentRefreshPending = false;
                    session.socketRefreshInstanceSoid = 0;
                    session.socketRefreshCharacterSoid = 0;
                    session.socketRefreshIncludeCharacter = false;
                    session.socketRefreshPending = false;
                    session.createdCharacterRefreshSoid = 0;
                    session.createdCharacterRefreshPending = false;
                    session.createdCharacterSelectSoid = 0;
                    session.createdCharacterSelectPending = false;
                    session.characterRefreshPending = false;
                    session.characterRefreshSoid = 0;
                    session.rosterRefreshCharacterSoid = 0;
                    session.rosterRefreshPending = false;
                    session.rosterRefreshFromCreate = false;
                    session.bannerRefreshCharacterSoid = 0;
                    session.bannerRefreshPending = false;
                }
            }
            if (queuezPublication.armsEquipmentMirrors
                && queuezPublication.equipmentMirrorCharacterSoid != 0) {
                const std::uint64_t characterSoid =
                    queuezPublication.equipmentMirrorCharacterSoid;
                session.rosterRefreshCharacterSoid = characterSoid;
                session.rosterRefreshPending = true;
                session.rosterRefreshFromCreate = false;
                const std::uint64_t selected =
                    state::account::selected_character_soid(state::account_snapshot());
                session.bannerRefreshCharacterSoid =
                    session.queuez.family0Active && selected == characterSoid
                            && session.queuez.family0Character == characterSoid
                        ? characterSoid
                        : 0;
                session.bannerRefreshPending = session.bannerRefreshCharacterSoid != 0;
                session.editorPersistencePending = true;
                session.editorRefreshFailed = false;
            }
            if (outcome.hasCharacterDeletion) {
                // Family 4 has already stopped referring to the deleted member/items. Publish the
                // compacted complete roster next; consume_frame can append this deferred Family-3
                // notification in the same socket write immediately after the delete response.
                session.rosterRefreshCharacterSoid = 0;
                session.rosterRefreshPending = true;
                session.rosterRefreshFromCreate = false;
                session.bannerRefreshCharacterSoid = 0;
                session.bannerRefreshPending = false;
                session.editorPersistencePending = true;
                session.editorRefreshFailed = false;
                core::log::write(core::log::Channel::server,
                                 core::log::Level::info,
                                 "ev=queuez stage=character_delete3 result=armed");
            }
            if (outcome.hasSubscription
                && outcome.subscription.familyType == queuez::kRosterFamilyType) {
                // Explicit Family-3 subscriptions publish (or reset toward) the version-zero
                // snapshot contract. The next editor mirror therefore starts at version one.
                session.editorFamily3Version = 0;
            }
            if (outcome.hasCreatedCharacter && outcome.createdCharacterSoid != 0) {
                // A cold create can establish the peer's complete version-zero roster/account
                // ladder in the same response. Do not then append the same item's resident SOIDs
                // again as a version-one addition.
                if (queuezPublication.createdCharacterBootstrap) {
                    session.createdCharacterSelectSoid = 0;
                    session.createdCharacterSelectPending = false;
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::info,
                                     "ev=queuez stage=create_publish result=bootstrapped_selected");
                } else if (session.queuez.family4Active && session.queuez.family4RootSoid != 0
                           && session.queuez.family3Phase == queuez::Family3Phase::normal) {
                    session.createdCharacterRefreshSoid = outcome.createdCharacterSoid;
                    session.createdCharacterRefreshPending = true;
                    session.createdCharacterSelectSoid = outcome.createdCharacterSoid;
                    session.createdCharacterSelectPending = false;
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::info,
                                     "ev=queuez stage=create_publish result=armed");
                } else {
                    core::log::write(core::log::Channel::server,
                                     core::log::Level::info,
                                     "ev=queuez stage=create_publish result=deferred_to_snapshot");
                }
            }
            arm_repushes(session, queuezPublication);
            publish_connection_fields(session, publication, connection);
        }
    }
    clear_prefix(scratch.plaintext, plaintextSize);
    clear_prefix(scratch.responseBody, responseBodySize);
    clear_prefix(scratch.framed, framedSize);
    SecureZeroMemory(&outcome, sizeof outcome);
    SecureZeroMemory(&publication, sizeof publication);
    SecureZeroMemory(&queuezPublication, sizeof queuezPublication);
    if (handled) {
        core::log::write(core::log::Channel::server, core::log::Level::info, route.successEvent);
    }
    return handled;
}

} // namespace sunrise::server::bap::encrypted
