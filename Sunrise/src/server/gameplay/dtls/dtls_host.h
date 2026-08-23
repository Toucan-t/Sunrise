#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/gameplay/definition.h"

namespace sunrise::server::gameplay::dtls {

/**
 * Answers one association handshake datagram.
 * The peer opens every connection with this handshake, so nothing above it runs first.
 * @param from Source endpoint in host order.
 * @param datagram Whole received payload.
 * @param now Monotonic tick count in milliseconds.
 * @return True when the datagram belonged to this layer and needs no further routing.
 */
[[nodiscard]] bool route(const state::gameplay::Endpoint& from,
                         std::span<const std::byte> datagram,
                         std::uint64_t now) noexcept;

/**
 * Seals one transport payload as a record and sends it.
 * @param to Peer endpoint in host order.
 * @param payload Plaintext to seal.
 * @return True when an established association carried it.
 */
[[nodiscard]] bool send_payload(const state::gameplay::Endpoint& to,
                                std::span<const std::byte> payload) noexcept;

/**
 * Registers the key paired with a migrating security id without changing outbound routing.
 * @param endpoint Peer endpoint in host order.
 * @param securityId Opaque little-endian security id from the host-reestablish descriptor.
 * @param securityKey Sixteen-byte key paired with that id.
 */
void register_security_key(const state::gameplay::Endpoint& endpoint,
                           std::uint64_t securityId,
                           std::span<const std::byte> securityKey) noexcept;

/**
 * Selects the security association a migrating managed session will commit for one endpoint.
 * Sending falls back to the current association until this id has completed its handshake.
 * @param endpoint Peer endpoint in host order.
 * @param securityId Opaque little-endian security id carried by the host-reestablish descriptor.
 */
void prefer_security_id(const state::gameplay::Endpoint& endpoint,
                        std::uint64_t securityId) noexcept;

/** Drops every association and clears its key material. */
void reset() noexcept;

} // namespace sunrise::server::gameplay::dtls
