#include "matchmaking_response_encoder.h"

#include <array>

#include "../../../protobuf/codec.h"
#include "matchmaking_dynamic_response.h"

namespace sunrise::middleware::bap::matchmaking::response {
namespace {

using protobuf::Writer;

/** Search results use service-43 field 3. */
constexpr std::uint32_t kSearchResultsField = 3;
/** One search-results message contains repeated result field 1. */
constexpr std::uint32_t kSearchResultField = 1;
/** One result wraps its descriptor message in field 1. */
constexpr std::uint32_t kSearchResultDescriptorField = 1;
/** The descriptor message carries the exact opaque 128 bytes in field 1. */
constexpr std::uint32_t kDescriptorBytesField = 1;
/** Matchmaking configuration uses service-43 field 4. */
constexpr std::uint32_t kConfigurationField = 4;
/** Live matchmaking statistics use service-43 field 8. */
constexpr std::uint32_t kLiveStatsField = 8;

/** Local singleton lane-policy service configuration identifier. */
constexpr std::uint32_t kServiceConfiguration = 1;
/** Delay before a searching client also starts advertising, in seconds. */
constexpr std::uint32_t kSearchOnlySeconds = 60;
/** Search desperation threshold, in seconds. */
constexpr std::uint32_t kDesperationSeconds = 60;
/** Default activity bubble capacity measured in the native fallback. */
constexpr std::uint32_t kMaximumPlayers = 9;
/** Default number of players allowed in one posse. */
constexpr std::uint32_t kMaximumPosse = 3;
/** Default number of players supplied by matchmaking. */
constexpr std::uint32_t kMaximumMatchmadePlayers = 6;

/**
 * Encodes one present, zero-length submessage.
 * @param fieldNumber Service-43 field whose presence finishes the request.
 * @param output Caller-owned response storage.
 * @param written Receives 2 encoded bytes or zero on failure.
 * @return True when the empty submessage fits.
 */
[[nodiscard]] bool encode_empty_message(std::uint32_t fieldNumber,
                                        std::span<std::byte> output,
                                        std::size_t& written) noexcept {
    Writer writer(output);
    if (!writer.write_length_delimited(fieldNumber, {})) {
        return false;
    }
    written = writer.size();
    return true;
}

/**
 * Encodes the retail kind-1 single-result shape around one exact 128-byte join descriptor:
 * service-43 field 3 -> repeated field 1 -> field 1 descriptor message -> field 1 bytes.
 */
[[nodiscard]] bool encode_search_result(std::span<const std::byte> descriptor,
                                        std::span<std::byte> output,
                                        std::size_t& written) noexcept {
    written = 0;
    if (descriptor.size() != kJoinDescriptorSize) {
        return false;
    }

    // Build each child in separate fixed storage. Besides keeping failure atomic with respect to
    // caller output, this avoids relying on overlap behavior while wrapping nested protobuf data.
    std::array<std::byte, kMaximumResponseBodySize> descriptorMessage{};
    Writer descriptorWriter(descriptorMessage);
    if (!descriptorWriter.write_length_delimited(kDescriptorBytesField, descriptor)) {
        return false;
    }

    std::array<std::byte, kMaximumResponseBodySize> resultDescriptor{};
    Writer resultDescriptorWriter(resultDescriptor);
    if (!resultDescriptorWriter.write_length_delimited(
            kSearchResultDescriptorField,
            std::span<const std::byte>{descriptorMessage.data(), descriptorWriter.size()})) {
        return false;
    }

    std::array<std::byte, kMaximumResponseBodySize> result{};
    Writer resultWriter(result);
    if (!resultWriter.write_length_delimited(
            kSearchResultField,
            std::span<const std::byte>{resultDescriptor.data(), resultDescriptorWriter.size()})) {
        return false;
    }

    Writer outerWriter(output);
    if (!outerWriter.write_length_delimited(
            kSearchResultsField,
            std::span<const std::byte>{result.data(), resultWriter.size()})) {
        return false;
    }
    written = outerWriter.size();
    return true;
}

/**
 * Encodes the smallest measured service-43 kind-4 configuration that causes Destiny to
 * materialize a real matchmaking lane/provider instead of clearing the lane with an empty field 4.
 *
 * Recovered native schema:
 *   field 4 configuration {
 *     field 1 lane_policy {
 *       field 1 service_config;
 *       field 6 search_only_seconds;
 *       field 7 desperation_seconds;
 *       field 13 provider_policy { repeated field 3 provider_entry; }
 *     }
 *     field 2 bubble_policies {
 *       field 1 default {
 *         field 1 max_players;
 *         field 2 max_posse;
 *         field 3 max_matchmade_players;
 *         field 4 service_config;
 *       }
 *     }
 *   }
 *
 * The single empty provider-entry message is deliberate: it is the minimum recovered shape that
 * keeps the native provider-policy loop present without inventing the still-opaque provider
 * scalars. The policy supplies the client's normal matchmaking lane/provider state; authored
 * manager activation remains a separate gameplay-session transition.
 */
[[nodiscard]] bool encode_configuration(std::span<std::byte> output,
                                        std::size_t& written) noexcept {
    std::array<std::byte, 8> providerPolicy{};
    Writer providerPolicyWriter(providerPolicy);
    if (!providerPolicyWriter.write_length_delimited(3, {})) {
        return false;
    }

    std::array<std::byte, 48> lanePolicy{};
    Writer laneWriter(lanePolicy);
    if (!laneWriter.write_varint(1, kServiceConfiguration)
        || !laneWriter.write_varint(6, kSearchOnlySeconds)
        || !laneWriter.write_varint(7, kDesperationSeconds)
        || !laneWriter.write_length_delimited(
            13, {providerPolicy.data(), providerPolicyWriter.size()})) {
        return false;
    }

    std::array<std::byte, 32> defaultBubble{};
    Writer defaultBubbleWriter(defaultBubble);
    if (!defaultBubbleWriter.write_varint(1, kMaximumPlayers)
        || !defaultBubbleWriter.write_varint(2, kMaximumPosse)
        || !defaultBubbleWriter.write_varint(3, kMaximumMatchmadePlayers)
        || !defaultBubbleWriter.write_varint(4, kServiceConfiguration)) {
        return false;
    }

    std::array<std::byte, 40> bubblePolicies{};
    Writer bubblePoliciesWriter(bubblePolicies);
    if (!bubblePoliciesWriter.write_length_delimited(
            1, {defaultBubble.data(), defaultBubbleWriter.size()})) {
        return false;
    }

    std::array<std::byte, 96> configuration{};
    Writer configurationWriter(configuration);
    if (!configurationWriter.write_length_delimited(
            1, {lanePolicy.data(), laneWriter.size()})
        || !configurationWriter.write_length_delimited(
            2, {bubblePolicies.data(), bubblePoliciesWriter.size()})) {
        return false;
    }

    Writer outputWriter(output);
    if (!outputWriter.write_length_delimited(
            kConfigurationField,
            {configuration.data(), configurationWriter.size()})) {
        return false;
    }
    written = outputWriter.size();
    return true;
}

} // namespace

/** Encodes one service-43 body. The service-42 request kind picks the shape. */
bool encode(const Response& response, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    switch (response.kind) {
    case RequestKind::none:
    case RequestKind::advertisementDelete:
    case RequestKind::rejoinAdvertisementDelete:
        return true;
    case RequestKind::sessionSearch:
        if (response.descriptor.empty()) {
            return encode_empty_message(kSearchResultsField, output, written);
        }
        return encode_search_result(response.descriptor, output, written);
    case RequestKind::advertisementUpdate:
        return encode_advertisement_id(true, response.advertisementId, output, written);
    case RequestKind::configuration:
        return encode_configuration(output, written);
    case RequestKind::rejoinAdvertisementUpdate:
        return encode_advertisement_id(false, response.advertisementId, output, written);
    case RequestKind::locateSession:
        if (response.descriptor.empty()) {
            return true;
        }
        return encode_locate_result(response.advertisementId, response.descriptor, output, written);
    case RequestKind::liveStats:
        return encode_empty_message(kLiveStatsField, output, written);
    }
    return false;
}

} // namespace sunrise::middleware::bap::matchmaking::response
