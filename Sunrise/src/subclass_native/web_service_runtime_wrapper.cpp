#include "../server/web_service/web_service_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "../core/logging/log.h"
#include "../middleware/encoding/bit_reader.h"
#include "../middleware/web_service/web_service_envelope.h"
#include "../state/account/account_state.h"
#include "../state/build_data/runtime.h"
#include "../state/runtime/runtime.h"
#include "subclass_runtime_bridge.h"

namespace sunrise::client::content::items::packages {
[[nodiscard]] bool ensure_subclass_ability_buckets(
    std::uint16_t socketEntryListIndex,
    const state::build_data::abilities::Selection& selection) noexcept;
} // namespace sunrise::client::content::items::packages

namespace sunrise::server::web_service {
// Forward declarations for the macro-renamed overload family inside the included working source.
bool sunrise_base_consume(std::span<const std::byte> request,
                          std::span<std::byte> response,
                          std::size_t& written) noexcept;
bool sunrise_base_consume(std::span<const std::byte> request,
                          std::span<std::byte> response,
                          std::size_t& written,
                          Outcome& outcome) noexcept;
bool sunrise_base_consume(std::span<const std::byte> request,
                          std::span<std::byte> response,
                          std::size_t& written,
                          Outcome& outcome,
                          std::int32_t nextFamily4Version) noexcept;
} // namespace sunrise::server::web_service

// Keep the complete current Web Service implementation, only moving its consume overloads out of
// the public symbol names so opcode 801 can be intercepted without replacing any existing route.
#define consume sunrise_base_consume
#include "../server/web_service/web_service_runtime.cpp"
#undef consume

namespace sunrise::server::web_service {
namespace {

constexpr std::uint16_t kSubclassSelectionOpcode = 801;
constexpr std::size_t kSubclassSelectionPayloadSize = 10;
constexpr std::uint64_t kSocketEntryBias = 0x80ULL;
constexpr std::size_t kNativeBundleSize = 4;
constexpr std::uint8_t kGrenadeBucket = 0;
constexpr std::uint8_t kSuperBucket = 1;
constexpr std::uint8_t kMeleeBucket = 2;
constexpr std::uint8_t kMovementBucket = 3;

[[nodiscard]] std::uint8_t class_bucket(state::CharacterClass characterClass) noexcept {
    switch (characterClass) {
    case state::CharacterClass::hunter:
        return 9;
    case state::CharacterClass::warlock:
        return 11;
    case state::CharacterClass::titan:
    default:
        return 6;
    }
}

[[nodiscard]] bool parse_subclass_selection(const middleware::web_service::Message& message,
                                            std::uint64_t& subclassInstanceSoid,
                                            std::uint8_t& requestedEntry) noexcept {
    subclassInstanceSoid = 0;
    requestedEntry = 0;
    if (message.opcode != kSubclassSelectionOpcode
        || message.payload.size() != kSubclassSelectionPayloadSize) {
        return false;
    }
    middleware::encoding::bits::Reader reader(message.payload);
    std::uint64_t encodedEntry = 0;
    std::uint64_t trailer = 0;
    std::uint64_t padding = 0;
    if (!reader.read(64, subclassInstanceSoid) || !reader.read(8, encodedEntry)
        || !reader.read(2, trailer) || !reader.read(6, padding) || reader.remaining_bits() != 0
        || subclassInstanceSoid == 0 || encodedEntry < kSocketEntryBias
        || encodedEntry - kSocketEntryBias
               >= state::build_data::socket_entry_lists::kEntryCapacity
        || trailer != 0 || padding != 0) {
        subclassInstanceSoid = 0;
        return false;
    }
    requestedEntry = static_cast<std::uint8_t>(encodedEntry - kSocketEntryBias);
    return true;
}

[[nodiscard]] bool same_ability_selection(const state::CharacterState& left,
                                          const state::CharacterState& right) noexcept {
    return left.movementAbilityEntry == right.movementAbilityEntry
           && left.grenadeAbilityEntry == right.grenadeAbilityEntry
           && left.superAbilityEntry == right.superAbilityEntry
           && left.meleeAbilityEntry == right.meleeAbilityEntry
           && left.classAbilityEntry == right.classAbilityEntry;
}

[[nodiscard]] bool entry_bucket(std::uint16_t socketEntryListIndex,
                                std::uint8_t entry,
                                const state::CharacterState& character,
                                std::uint8_t& bucket) noexcept {
    if (subclass_native::find_entry_bucket(socketEntryListIndex, entry, bucket)) {
        return true;
    }
    // Warm-cache fallback: alternatives in one semantic ability group share the same authored kind.
    // Match that kind against the already-published bucket row when the package walk that records
    // the exact selector destination did not run in this process yet.
    state::build_data::socket_entry_lists::EntryTable entries{};
    state::build_data::abilities::Definition abilities{};
    const state::build_data::abilities::Selection selection{
        character.movementAbilityEntry, character.grenadeAbilityEntry, character.superAbilityEntry,
        character.meleeAbilityEntry, character.classAbilityEntry};
    if (!state::build_data::find_socket_entry_table(socketEntryListIndex, entries)
        || !state::build_data::find_ability_buckets(socketEntryListIndex, selection, abilities)) {
        return false;
    }
    const std::uint8_t kind = entries.entries[entry].kind;
    std::uint8_t found = subclass_native::kNoDestinationBucket;
    for (std::size_t index = 0; index < abilities.buckets.size(); ++index) {
        if (abilities.buckets[index].kind != kind) {
            continue;
        }
        if (found != subclass_native::kNoDestinationBucket) {
            return false;
        }
        found = static_cast<std::uint8_t>(index);
    }
    bucket = found;
    return found != subclass_native::kNoDestinationBucket;
}

[[nodiscard]] bool route_entry(std::uint16_t socketEntryListIndex,
                               std::uint8_t entry,
                               state::CharacterState& character) noexcept {
    std::uint8_t bucket = subclass_native::kNoDestinationBucket;
    if (!entry_bucket(socketEntryListIndex, entry, character, bucket)) {
        return false;
    }
    if (bucket == kMovementBucket) {
        character.movementAbilityEntry = entry;
        return true;
    }
    if (bucket == kGrenadeBucket) {
        character.grenadeAbilityEntry = entry;
        return true;
    }
    if (bucket == kSuperBucket) {
        character.superAbilityEntry = entry;
        return true;
    }
    if (bucket == kMeleeBucket) {
        character.meleeAbilityEntry = entry;
        return true;
    }
    if (bucket == class_bucket(character.characterClass)) {
        character.classAbilityEntry = entry;
        return true;
    }
    return false;
}

void reset_bucket_if_routed(std::uint8_t bucket, state::CharacterState& character) noexcept {
    if (bucket == kMovementBucket) {
        character.movementAbilityEntry = state::kDefaultMovementAbilityEntry;
    } else if (bucket == kGrenadeBucket) {
        character.grenadeAbilityEntry = state::kDefaultGrenadeAbilityEntry;
    } else if (bucket == kSuperBucket) {
        character.superAbilityEntry = state::kDefaultSuperAbilityEntry;
    } else if (bucket == kMeleeBucket) {
        character.meleeAbilityEntry = state::kDefaultMeleeAbilityEntry;
    } else if (bucket == class_bucket(character.characterClass)) {
        character.classAbilityEntry = state::kDefaultClassAbilityEntry;
    }
}

[[nodiscard]] bool prepare_subclass_selection(std::uint64_t subclassInstanceSoid,
                                              std::uint8_t requestedEntry,
                                              subclass_native::PendingSelection& pending) noexcept {
    pending = {};
    const state::AccountState snapshot = state::account_snapshot();
    if (!state::account::valid(snapshot)) {
        return false;
    }
    std::size_t characterIndex = snapshot.characterCount;
    for (std::size_t index = 0; index < snapshot.characterCount; ++index) {
        if (snapshot.characters[index].selected) {
            characterIndex = index;
            break;
        }
    }
    if (characterIndex >= snapshot.characterCount) {
        return false;
    }

    constexpr std::size_t kSubclassSlot =
        static_cast<std::size_t>(state::account::inventory::EquipmentSlot::subclass);
    const state::CharacterState& before = snapshot.characters[characterIndex];
    const auto& subclass = before.equipment.slots[kSubclassSlot];
    state::build_data::items::Definition itemDefinition{};
    state::build_data::items::details::Definition detail{};
    state::build_data::socket_entry_lists::EntryTable entries{};
    if (!subclass.has_value() || subclass->instanceSoid != subclassInstanceSoid
        || !state::build_data::find_item_definition_hash(subclass->definitionHash, itemDefinition)
        || !state::build_data::find_configured_item_detail(itemDefinition.definitionIndex, detail)
        || !state::build_data::find_socket_entry_table(detail.socketEntryListIndex, entries)
        || requestedEntry >= state::build_data::socket_entry_lists::kEntryCapacity) {
        return false;
    }

    const auto& requested = entries.entries[requestedEntry];
    if (requested.group == state::build_data::socket_entry_lists::kNoEntryGroup) {
        return false;
    }

    std::array<std::uint8_t, state::build_data::socket_entry_lists::kEntryCapacity> groupEntries{};
    std::size_t groupCount = 0;
    std::size_t requestedPosition = groupEntries.size();
    for (std::size_t index = 0; index < groupEntries.size(); ++index) {
        if (entries.entries[index].group != requested.group) {
            continue;
        }
        if (index == requestedEntry) {
            requestedPosition = groupCount;
        }
        groupEntries[groupCount++] = static_cast<std::uint8_t>(index);
    }
    if (requestedPosition >= groupCount) {
        return false;
    }

    state::CharacterState after = before;
    if (groupCount <= kNativeBundleSize) {
        if (!route_entry(detail.socketEntryListIndex, requestedEntry, after)) {
            return false;
        }
    } else {
        // Wide legacy groups are several four-node bundles sharing one group id. Clear every
        // semantic field that group can own before applying the selected bundle so a prior
        // Attunement cannot leave a stale super/melee/class pick behind.
        for (std::size_t position = 0; position < groupCount; ++position) {
            std::uint8_t bucket = subclass_native::kNoDestinationBucket;
            if (entry_bucket(
                    detail.socketEntryListIndex, groupEntries[position], after, bucket)) {
                reset_bucket_if_routed(bucket, after);
            }
        }
        const std::size_t blockStart = (requestedPosition / kNativeBundleSize) * kNativeBundleSize;
        bool routed = false;
        for (std::size_t position = blockStart;
             position < groupCount && position < blockStart + kNativeBundleSize;
             ++position) {
            routed = route_entry(detail.socketEntryListIndex, groupEntries[position], after) || routed;
        }
        if (!routed) {
            return false;
        }
    }

    if (same_ability_selection(before, after)) {
        return false;
    }
    state::AccountState candidate = snapshot;
    candidate.characters[characterIndex] = after;
    if (!state::account::valid(candidate)) {
        return false;
    }

    pending.active = true;
    pending.accountSoid = snapshot.primarySoid;
    pending.characterSoid = before.soid;
    pending.subclassInstanceSoid = subclassInstanceSoid;
    pending.characterIndex = characterIndex;
    pending.socketEntryListIndex = detail.socketEntryListIndex;
    pending.requestedEntry = requestedEntry;
    pending.beforeCharacter = before;
    pending.afterCharacter = after;
    return true;
}

[[nodiscard]] bool consume_subclass_selection(const middleware::web_service::Message& message,
                                              std::int32_t nextFamily4Version,
                                              std::span<std::byte> response,
                                              std::size_t& written,
                                              Outcome& outcome) noexcept {
    subclass_native::clear_pending_selection();
    outcome = {};
    middleware::web_service::StatusResponse status{};
    std::uint64_t subclassInstanceSoid = 0;
    std::uint8_t requestedEntry = 0;
    const bool parsed = parse_subclass_selection(message, subclassInstanceSoid, requestedEntry);
    const bool versionReady =
        nextFamily4Version > (std::numeric_limits<std::int32_t>::min)();
    subclass_native::PendingSelection pending{};
    const bool prepared = parsed && versionReady
                          && prepare_subclass_selection(subclassInstanceSoid, requestedEntry, pending);
    bool abilityRowReady = false;
    if (prepared) {
        const state::CharacterState& character = pending.afterCharacter;
        const state::build_data::abilities::Selection selection{
            character.movementAbilityEntry,
            character.grenadeAbilityEntry,
            character.superAbilityEntry,
            character.meleeAbilityEntry,
            character.classAbilityEntry};
        abilityRowReady = client::content::items::packages::ensure_subclass_ability_buckets(
            pending.socketEntryListIndex, selection);
    }
    const bool accepted = prepared && abilityRowReady;
    if (accepted) {
        status.value = nextFamily4Version;
    } else {
        status.code = 1;
    }

    const bool encoded = middleware::web_service::encode_response(
        message, middleware::web_service::ResponseShape::statusPair, status, response, written);
    if (encoded && accepted) {
        subclass_native::pending_selection() = pending;
    }

    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=subclass stage=ws result=%s opcode=801 instance=0x%llX entry=%u character=0x%llX "
        "family_version=%d reason=%s",
        encoded && accepted ? "ok" : "fail",
        static_cast<unsigned long long>(subclassInstanceSoid),
        static_cast<unsigned>(requestedEntry),
        static_cast<unsigned long long>(pending.characterSoid),
        nextFamily4Version,
        !parsed ? "payload" : (!versionReady ? "family4_not_ready"
                              : (!prepared ? "selection"
                                           : (!abilityRowReady ? "ability_row" : "prepared"))));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         encoded && accepted ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    return encoded;
}

} // namespace

bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome,
             std::int32_t nextFamily4Version) noexcept {
    middleware::web_service::Message message{};
    if (middleware::web_service::parse_request(request, message)
        && message.opcode == kSubclassSelectionOpcode) {
        written = 0;
        report_opcode(message.opcode);
        return consume_subclass_selection(
            message, nextFamily4Version, response, written, outcome);
    }
    subclass_native::clear_pending_selection();
    return sunrise_base_consume(request, response, written, outcome, nextFamily4Version);
}

bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome) noexcept {
    return consume(request,
                   response,
                   written,
                   outcome,
                   (std::numeric_limits<std::int32_t>::min)());
}

bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    Outcome outcome{};
    return consume(request, response, written, outcome);
}

} // namespace sunrise::server::web_service
