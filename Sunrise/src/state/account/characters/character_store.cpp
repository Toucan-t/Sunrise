#include "character_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/settings/settings.h"
#include "../../../../resources/resource.h"
#include "../inventory/inventory_state.h"

namespace sunrise::state::account::characters {
namespace {

/** The character store lives beside settings.json in Sunrise's owned artifact directory. */
constexpr std::wstring_view kStoreSuffix = L"\\characters.dat";
/** Compatibility mirror updated only after runtime/live publication succeeds. */
constexpr std::wstring_view kSettingsSuffix = L"\\settings.json";
/** Numbered raw captures are intentionally never replaced; each creator Finish gets its own file. */
constexpr std::uint32_t kMaximumCreationCaptures = 9999;
/** A complete binary-store replacement is staged beside the final file. */
constexpr std::wstring_view kStageSuffix = L".new";
/** Runtime settings mirrors use a distinct stage so they cannot collide with schema upgrades. */
constexpr std::wstring_view kSettingsStageSuffix = L".runtime.new";
/** Core refuses settings files above this size, so the mirror never creates one either. */
constexpr std::size_t kSettingsCapacity = 64 * 1024;
/** JSON recursion is bounded well above the current layout while protecting the scanner. */
constexpr unsigned kJsonDepthLimit = 32;
/** Stable format marker. */
constexpr std::array<char, 8> kMagic{'S', 'U', 'N', 'C', 'H', 'A', 'R', '1'};
constexpr std::uint32_t kVersion = 4;
/** Version 3 added the remaining native creator blocks. */
constexpr std::uint32_t kCreatorVersion = 3;
/** Version 2 added the per-character native presentation header. */
constexpr std::uint32_t kPresentationVersion = 2;
/** Version 1 predates all per-character native creator blocks. */
constexpr std::uint32_t kLegacyVersion = 1;
/** FNV-1a constants used only as an accidental-corruption check over disk rows. */
constexpr std::uint64_t kHashOffset = 14695981039346656037ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

#pragma pack(push, 1)
struct DiskHeader final {
    std::array<char, 8> magic{};
    std::uint32_t version{};
    std::uint32_t headerSize{};
    std::uint32_t characterSize{};
    std::uint32_t characterCount{};
    std::uint64_t checksum{};
};

struct DiskItem final {
    std::uint8_t present{};
    std::uint8_t socketPolicy{};
    std::uint8_t plugCount{};
    std::uint8_t reservedA{};
    std::uint16_t plugPresentMask{};
    std::uint16_t reservedB{};
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t level{};
    std::int32_t quantity{};
    std::array<std::uint32_t, account::inventory::kPlugCapacity> plugs{};
};

/** Version-4 item row adds native inventory generations and accumulated item state. */
struct DiskItemV4 final {
    std::uint8_t present{};
    std::uint8_t socketPolicy{};
    std::uint8_t plugCount{};
    std::uint8_t reservedA{};
    std::uint16_t plugPresentMask{};
    std::uint16_t reservedB{};
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t level{};
    std::int32_t quantity{};
    std::int32_t mutationSerial{};
    std::uint32_t flags{};
    std::array<std::uint32_t, account::inventory::kPlugCapacity> plugs{};
};

struct DiskCharacterV1 final {
    std::uint8_t race{};
    std::uint8_t gender{};
    std::uint8_t characterClass{};
    std::uint8_t level{};
    std::uint8_t accepted{};
    std::uint8_t previewAvailable{};
    std::uint8_t contentBypass{};
    std::uint8_t reservedA{};
    std::uint64_t accountOffset{};
    float appearanceValue{};
    std::uint32_t lastOrbitedDestination{};
    std::uint8_t movementAbilityEntry{};
    std::uint8_t grenadeAbilityEntry{};
    std::uint8_t superAbilityEntry{};
    std::uint8_t meleeAbilityEntry{};
    std::uint8_t classAbilityEntry{};
    std::array<std::uint8_t, 3> reservedB{};
    std::array<DiskItem, account::inventory::kEquipmentSlotCount> equipment{};
};

struct DiskCharacterV2 final {
    std::uint8_t race{};
    std::uint8_t gender{};
    std::uint8_t characterClass{};
    std::uint8_t level{};
    std::uint8_t accepted{};
    std::uint8_t previewAvailable{};
    std::uint8_t contentBypass{};
    std::uint8_t reservedA{};
    std::uint64_t accountOffset{};
    float appearanceValue{};
    std::uint32_t lastOrbitedDestination{};
    std::uint8_t movementAbilityEntry{};
    std::uint8_t grenadeAbilityEntry{};
    std::uint8_t superAbilityEntry{};
    std::uint8_t meleeAbilityEntry{};
    std::uint8_t classAbilityEntry{};
    std::array<std::uint8_t, 3> reservedB{};
    std::array<std::byte, kCharacterPresentationHeaderSize> presentationHeader{};
    std::array<DiskItem, account::inventory::kEquipmentSlotCount> equipment{};
};

struct DiskCharacterV3 final {
    std::uint8_t race{};
    std::uint8_t gender{};
    std::uint8_t characterClass{};
    std::uint8_t level{};
    std::uint8_t accepted{};
    std::uint8_t previewAvailable{};
    std::uint8_t contentBypass{};
    std::uint8_t reservedA{};
    std::uint64_t accountOffset{};
    float appearanceValue{};
    std::uint32_t lastOrbitedDestination{};
    std::uint8_t movementAbilityEntry{};
    std::uint8_t grenadeAbilityEntry{};
    std::uint8_t superAbilityEntry{};
    std::uint8_t meleeAbilityEntry{};
    std::uint8_t classAbilityEntry{};
    std::array<std::uint8_t, 3> reservedB{};
    std::array<std::byte, kCharacterPresentationHeaderSize> presentationHeader{};
    std::array<std::byte, kCharacterCreationHeaderSize> creationHeader{};
    std::array<std::byte, kCharacterCreationTailSize> creationTail{};
    std::array<DiskItem, account::inventory::kEquipmentSlotCount> equipment{};
};

/** Version-4 row persists unequipped inventory and the native mutation generation ladder. */
struct DiskCharacter final {
    std::uint8_t race{};
    std::uint8_t gender{};
    std::uint8_t characterClass{};
    std::uint8_t level{};
    std::uint8_t accepted{};
    std::uint8_t previewAvailable{};
    std::uint8_t contentBypass{};
    std::uint8_t reservedA{};
    std::uint64_t accountOffset{};
    float appearanceValue{};
    std::uint32_t lastOrbitedDestination{};
    std::uint8_t movementAbilityEntry{};
    std::uint8_t grenadeAbilityEntry{};
    std::uint8_t superAbilityEntry{};
    std::uint8_t meleeAbilityEntry{};
    std::uint8_t classAbilityEntry{};
    std::array<std::uint8_t, 3> reservedB{};
    std::array<std::byte, kCharacterPresentationHeaderSize> presentationHeader{};
    std::array<std::byte, kCharacterCreationHeaderSize> creationHeader{};
    std::array<std::byte, kCharacterCreationTailSize> creationTail{};
    std::uint32_t nextInventorySerial{};
    std::uint32_t inventoryCount{};
    std::array<DiskItemV4, account::inventory::kEquipmentSlotCount> equipment{};
    std::array<DiskItemV4, account::inventory::kCharacterItemCapacity> inventory{};
};
#pragma pack(pop)

static_assert(sizeof(DiskHeader) == 32);
static_assert(sizeof(DiskItem) == 76);
static_assert(sizeof(DiskItemV4) == 84);
static_assert(sizeof(DiskCharacterV1)
              == 32 + account::inventory::kEquipmentSlotCount * sizeof(DiskItem));
static_assert(sizeof(DiskCharacterV2)
              == sizeof(DiskCharacterV1) + kCharacterPresentationHeaderSize);
static_assert(sizeof(DiskCharacterV3)
              == sizeof(DiskCharacterV2) + kCharacterCreationHeaderSize + kCharacterCreationTailSize);
static_assert(sizeof(DiskCharacter)
              == 136 + account::inventory::kEquipmentSlotCount * sizeof(DiskItemV4)
                     + account::inventory::kCharacterItemCapacity * sizeof(DiskItemV4));

core::path::Buffer g_storePath{};
core::path::Buffer g_settingsPath{};
core::path::Buffer g_creationCaptureDirectory{};
AccountState g_configuredAccount{};
AccountState g_factoryAccount{};
StoreStatus g_status{};

/**
 * Reads the immutable three-class factory account bundled into the Sunrise DLL. This is distinct
 * from the user's settings.json: it is used only as a stable source of class starter equipment and
 * subclass/ability defaults for runtime character creation and build-data prefetch.
 */
[[nodiscard]] bool load_factory_account(void* module, AccountState& output) noexcept {
    output = {};
    if (module == nullptr) {
        return false;
    }
    const HMODULE loadedModule = static_cast<HMODULE>(module);
    const HRSRC resource =
        FindResourceW(loadedModule, MAKEINTRESOURCEW(IDR_DEFAULT_SETTINGS), RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(loadedModule, resource);
    const HGLOBAL loaded = LoadResource(loadedModule, resource);
    const auto* bytes = loaded != nullptr ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (size == 0 || bytes == nullptr) {
        return false;
    }
    core::settings::Settings parsed{};
    if (!core::settings::parse(std::string_view(bytes, size), parsed)
        || !sunrise::state::account::valid(parsed.initialAccount)
        || parsed.initialAccount.characterCount != kCharacterCapacity) {
        return false;
    }
    output = parsed.initialAccount;
    for (std::size_t index = 0; index < output.characterCount; ++index) {
        output.characters[index].selected = false;
    }
    return true;
}

/** @return FNV-1a over a fixed byte span. */
[[nodiscard]] std::uint64_t checksum(std::span<const std::byte> bytes) noexcept {
    std::uint64_t value = kHashOffset;
    for (const std::byte byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= kHashPrime;
    }
    return value;
}

/** Emits one compact character-store state line. */
void report(const char* stage, const char* result, std::size_t count = 0) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=characters stage=%s result=%s count=%zu",
                                      stage,
                                      result,
                                      count);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Encodes one authored item with explicit optional-plug presence bits. */
void encode_item(const std::optional<account::inventory::Item>& source, DiskItemV4& output) noexcept {
    output = {};
    if (!source.has_value()) {
        return;
    }
    output.present = 1;
    output.socketPolicy = static_cast<std::uint8_t>(source->sockets.policy);
    output.plugCount = static_cast<std::uint8_t>(source->sockets.plugCount);
    output.instanceSoid = source->instanceSoid;
    output.definitionHash = source->definitionHash;
    output.level = source->level;
    output.quantity = source->quantity;
    output.mutationSerial = source->mutationSerial;
    output.flags = source->flags;
    for (std::size_t lane = 0; lane < source->sockets.plugCount; ++lane) {
        if (!source->sockets.plugs[lane].has_value()) {
            continue;
        }
        output.plugPresentMask = static_cast<std::uint16_t>(
            output.plugPresentMask | static_cast<std::uint16_t>(1U << lane));
        output.plugs[lane] = *source->sockets.plugs[lane];
    }
}

/** Encodes one runtime character, excluding SOID and selected state. */
void encode_character(const CharacterState& source,
                      std::uint64_t primarySoid,
                      DiskCharacter& output) noexcept {
    output = {};
    output.race = static_cast<std::uint8_t>(source.race);
    output.gender = static_cast<std::uint8_t>(source.gender);
    output.characterClass = static_cast<std::uint8_t>(source.characterClass);
    output.level = source.level;
    output.accepted = source.accepted ? 1U : 0U;
    output.previewAvailable = source.previewAvailable ? 1U : 0U;
    output.contentBypass = source.contentBypass ? 1U : 0U;
    output.accountOffset = source.soid > primarySoid ? source.soid - primarySoid : 0;
    output.appearanceValue = source.appearanceValue;
    output.presentationHeader = source.presentationHeader;
    output.creationHeader = source.creationHeader;
    output.creationTail = source.creationTail;
    output.lastOrbitedDestination = source.lastOrbitedDestination;
    output.movementAbilityEntry = source.movementAbilityEntry;
    output.grenadeAbilityEntry = source.grenadeAbilityEntry;
    output.superAbilityEntry = source.superAbilityEntry;
    output.meleeAbilityEntry = source.meleeAbilityEntry;
    output.classAbilityEntry = source.classAbilityEntry;
    output.nextInventorySerial = source.nextInventorySerial;
    output.inventoryCount = static_cast<std::uint32_t>(source.inventory.count);
    for (std::size_t slot = 0; slot < source.equipment.slots.size(); ++slot) {
        encode_item(source.equipment.slots[slot], output.equipment[slot]);
    }
    for (std::size_t index = 0; index < source.inventory.count; ++index) {
        encode_item(std::optional<account::inventory::Item>{source.inventory.values[index]},
                    output.inventory[index]);
    }
}

/** Decodes one persisted item and re-applies State's normal inventory validation. */
template <typename DiskItemRow>
[[nodiscard]] bool decode_item(const DiskItemRow& source,
                               std::optional<account::inventory::Item>& output) noexcept {
    output.reset();
    if (source.present == 0) {
        const bool baseEmpty = source.socketPolicy == 0 && source.plugCount == 0
                               && source.plugPresentMask == 0 && source.instanceSoid == 0
                               && source.definitionHash == 0 && source.level == 0
                               && source.quantity == 0;
        if (!baseEmpty) {
            return false;
        }
        if constexpr (requires { source.mutationSerial; source.flags; }) {
            return source.mutationSerial == 0 && source.flags == 0;
        } else {
            return true;
        }
    }
    if (source.present != 1 || source.plugCount > account::inventory::kPlugCapacity
        || source.socketPolicy
               > static_cast<std::uint8_t>(account::inventory::SocketPolicy::authored)) {
        return false;
    }
    const std::uint16_t allowedMask = source.plugCount == 0
                                          ? 0
                                          : static_cast<std::uint16_t>((1U << source.plugCount) - 1U);
    if ((source.plugPresentMask & static_cast<std::uint16_t>(~allowedMask)) != 0) {
        return false;
    }

    account::inventory::Item item{};
    item.instanceSoid = source.instanceSoid;
    item.definitionHash = source.definitionHash;
    item.level = source.level;
    item.quantity = source.quantity;
    if constexpr (requires { source.mutationSerial; source.flags; }) {
        item.mutationSerial = source.mutationSerial;
        item.flags = source.flags;
    }
    item.sockets.policy = static_cast<account::inventory::SocketPolicy>(source.socketPolicy);
    item.sockets.plugCount = source.plugCount;
    for (std::size_t lane = 0; lane < item.sockets.plugCount; ++lane) {
        if ((source.plugPresentMask & static_cast<std::uint16_t>(1U << lane)) != 0) {
            item.sockets.plugs[lane] = source.plugs[lane];
        }
    }
    if (!account::inventory::valid(item)) {
        return false;
    }
    output = item;
    return true;
}

/** Decodes one persisted character and assigns its current account-relative SOID. */
template <typename DiskRow>
[[nodiscard]] bool decode_character_common(const DiskRow& source,
                                           std::uint64_t primarySoid,
                                           std::size_t index,
                                           CharacterState& output) noexcept {
    if (source.race > static_cast<std::uint8_t>(CharacterRace::exo)
        || source.gender > static_cast<std::uint8_t>(CharacterGender::female)
        || source.characterClass > static_cast<std::uint8_t>(CharacterClass::warlock)
        || source.accepted > 1 || source.previewAvailable > 1 || source.contentBypass > 1
        || source.accountOffset == 0
        || primarySoid > (std::numeric_limits<std::uint64_t>::max)() - source.accountOffset
        || source.movementAbilityEntry > kMaximumMovementAbilityEntry
        || source.grenadeAbilityEntry > kMaximumMovementAbilityEntry
        || source.superAbilityEntry > kMaximumMovementAbilityEntry
        || source.meleeAbilityEntry > kMaximumMovementAbilityEntry
        || source.classAbilityEntry > kMaximumMovementAbilityEntry) {
        return false;
    }
    CharacterState character{};
    (void)index;
    character.soid = primarySoid + source.accountOffset;
    character.selected = false;
    character.race = static_cast<CharacterRace>(source.race);
    character.gender = static_cast<CharacterGender>(source.gender);
    character.characterClass = static_cast<CharacterClass>(source.characterClass);
    character.level = source.level;
    character.accepted = source.accepted != 0;
    character.previewAvailable = source.previewAvailable != 0;
    character.appearanceValue = source.appearanceValue;
    if constexpr (requires { source.presentationHeader; }) {
        character.presentationHeader = source.presentationHeader;
    }
    if constexpr (requires { source.creationHeader; }) {
        character.creationHeader = source.creationHeader;
        character.creationTail = source.creationTail;
    }
    character.lastOrbitedDestination = source.lastOrbitedDestination;
    character.contentBypass = source.contentBypass != 0;
    character.movementAbilityEntry = source.movementAbilityEntry;
    character.grenadeAbilityEntry = source.grenadeAbilityEntry;
    character.superAbilityEntry = source.superAbilityEntry;
    character.meleeAbilityEntry = source.meleeAbilityEntry;
    character.classAbilityEntry = source.classAbilityEntry;
    for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
        if (!decode_item(source.equipment[slot], character.equipment.slots[slot])) {
            return false;
        }
    }
    if constexpr (requires { source.nextInventorySerial; source.inventoryCount; source.inventory; }) {
        if (source.inventoryCount > character.inventory.values.size()) {
            return false;
        }
        character.nextInventorySerial = source.nextInventorySerial;
        character.inventory.count = source.inventoryCount;
        for (std::size_t itemIndex = 0; itemIndex < source.inventory.size(); ++itemIndex) {
            std::optional<account::inventory::Item> decoded{};
            if (!decode_item(source.inventory[itemIndex], decoded)) {
                return false;
            }
            if (itemIndex < character.inventory.count) {
                if (!decoded.has_value()) {
                    return false;
                }
                character.inventory.values[itemIndex] = *decoded;
            } else if (decoded.has_value()) {
                return false;
            }
        }
    }
    output = character;
    return true;
}

/**
 * Seeds a canonical per-character mutation ladder for legacy/settings-authored equipment.
 * Version-4 rows keep an already-valid ladder; earlier stores deterministically receive one.
 */
void normalize_inventory_generations(AccountState& account) noexcept {
    for (std::size_t characterIndex = 0; characterIndex < account.characterCount; ++characterIndex) {
        CharacterState& character = account.characters[characterIndex];
        std::size_t itemCount = character.inventory.count;
        for (const std::optional<account::inventory::Item>& item : character.equipment.slots) {
            itemCount += static_cast<std::size_t>(item.has_value());
        }
        if (itemCount == 0) {
            character.nextInventorySerial = 0;
            continue;
        }

        bool currentValid = character.nextInventorySerial >= itemCount
                            && character.nextInventorySerial
                                   <= static_cast<std::uint32_t>(
                                       (std::numeric_limits<std::int32_t>::max)());
        if (currentValid) {
            for (const std::optional<account::inventory::Item>& item : character.equipment.slots) {
                if (item.has_value()
                    && (item->mutationSerial < 0
                        || static_cast<std::uint32_t>(item->mutationSerial)
                               >= character.nextInventorySerial)) {
                    currentValid = false;
                    break;
                }
            }
        }
        if (currentValid) {
            for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
                const account::inventory::Item& item = character.inventory.values[itemIndex];
                if (item.mutationSerial < 0
                    || static_cast<std::uint32_t>(item.mutationSerial)
                           >= character.nextInventorySerial) {
                    currentValid = false;
                    break;
                }
            }
        }
        if (currentValid) {
            continue;
        }

        std::int32_t serial = 0;
        for (std::optional<account::inventory::Item>& item : character.equipment.slots) {
            if (item.has_value()) {
                item->mutationSerial = serial++;
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            character.inventory.values[itemIndex].mutationSerial = serial++;
        }
        character.nextInventorySerial = static_cast<std::uint32_t>(serial);
    }
}

/** Loads and validates one complete store. Missing files are not failures. */
void load(AccountState& active) noexcept {
    if (!g_status.available || active.primarySoid == 0) {
        return;
    }
    const HANDLE file = CreateFileW(g_storePath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            g_status.rejected = true;
            report("load", "open_fail");
        }
        return;
    }

    DiskHeader header{};
    DWORD copied = 0;
    bool ok = ReadFile(file, &header, static_cast<DWORD>(sizeof header), &copied, nullptr) != FALSE
              && copied == static_cast<DWORD>(sizeof header) && header.magic == kMagic
              && header.headerSize == static_cast<std::uint32_t>(sizeof(DiskHeader))
              && header.characterCount <= kCharacterCapacity
              && ((header.version == kVersion
                   && header.characterSize == static_cast<std::uint32_t>(sizeof(DiskCharacter)))
                  || (header.version == kCreatorVersion
                      && header.characterSize
                             == static_cast<std::uint32_t>(sizeof(DiskCharacterV3)))
                  || (header.version == kPresentationVersion
                      && header.characterSize
                             == static_cast<std::uint32_t>(sizeof(DiskCharacterV2)))
                  || (header.version == kLegacyVersion
                      && header.characterSize
                             == static_cast<std::uint32_t>(sizeof(DiskCharacterV1))));
    std::array<std::byte, sizeof(DiskCharacter) * kCharacterCapacity> rawRows{};
    const std::size_t payloadBytes =
        static_cast<std::size_t>(header.characterCount) * header.characterSize;
    if (ok && payloadBytes > rawRows.size()) {
        ok = false;
    }
    if (ok && payloadBytes != 0) {
        copied = 0;
        ok = ReadFile(file,
                      rawRows.data(),
                      static_cast<DWORD>(payloadBytes),
                      &copied,
                      nullptr)
                 != FALSE
             && copied == static_cast<DWORD>(payloadBytes);
    }
    LARGE_INTEGER position{};
    LARGE_INTEGER fileSize{};
    if (ok) {
        ok = SetFilePointerEx(file, {}, &position, FILE_CURRENT) != FALSE && GetFileSizeEx(file, &fileSize)
             && position.QuadPart == fileSize.QuadPart;
    }
    (void)CloseHandle(file);
    ok = ok && checksum(std::span(rawRows).first(payloadBytes)) == header.checksum;

    AccountState candidate = active;
    candidate.characters = {};
    candidate.characterCount = header.characterCount;
    for (std::size_t index = 0; ok && index < candidate.characterCount; ++index) {
        const std::byte* row = rawRows.data() + index * header.characterSize;
        if (header.version == kVersion) {
            DiskCharacter disk{};
            std::memcpy(&disk, row, sizeof disk);
            ok = decode_character_common(disk, candidate.primarySoid, index, candidate.characters[index]);
        } else if (header.version == kCreatorVersion) {
            DiskCharacterV3 disk{};
            std::memcpy(&disk, row, sizeof disk);
            ok = decode_character_common(disk, candidate.primarySoid, index, candidate.characters[index]);
        } else if (header.version == kPresentationVersion) {
            DiskCharacterV2 disk{};
            std::memcpy(&disk, row, sizeof disk);
            ok = decode_character_common(disk, candidate.primarySoid, index, candidate.characters[index]);
        } else {
            DiskCharacterV1 disk{};
            std::memcpy(&disk, row, sizeof disk);
            ok = decode_character_common(disk, candidate.primarySoid, index, candidate.characters[index]);
        }
    }
    ok = ok && account::valid(candidate);
    if (!ok) {
        g_status.rejected = true;
        report("load", "rejected");
        return;
    }
    active = candidate;
    normalize_inventory_generations(active);
    g_status.loaded = true;
    report(header.version == kLegacyVersion
               ? "load_v1"
               : (header.version == kPresentationVersion
                      ? "load_v2"
                      : (header.version == kCreatorVersion ? "load_v3" : "load")),
           "ok",
           candidate.characterCount);
}

/** Writes one exact buffer. */
[[nodiscard]] bool write_exact(HANDLE file, const void* data, DWORD size) noexcept {
    DWORD copied = 0;
    return WriteFile(file, data, size, &copied, nullptr) != FALSE && copied == size;
}

/** Skips JSON whitespace without interpreting any value. */
void skip_whitespace(std::string_view input, std::size_t& cursor) noexcept {
    while (cursor < input.size()) {
        const char value = input[cursor];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            break;
        }
        ++cursor;
    }
}

/** Scans one JSON string and returns its raw, unescaped payload bounds. */
[[nodiscard]] bool scan_json_string(std::string_view input,
                                    std::size_t& cursor,
                                    std::size_t& contentBegin,
                                    std::size_t& contentEnd) noexcept {
    skip_whitespace(input, cursor);
    if (cursor >= input.size() || input[cursor] != '"') {
        return false;
    }
    contentBegin = ++cursor;
    bool escaped = false;
    while (cursor < input.size()) {
        const char value = input[cursor++];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (value == '\\') {
            escaped = true;
            continue;
        }
        if (value == '"') {
            contentEnd = cursor - 1U;
            return true;
        }
        if (static_cast<unsigned char>(value) < 0x20U) {
            return false;
        }
    }
    return false;
}

/** Advances across one complete JSON value. */
[[nodiscard]] bool skip_json_value(std::string_view input,
                                   std::size_t& cursor,
                                   unsigned depth) noexcept {
    if (depth > kJsonDepthLimit) {
        return false;
    }
    skip_whitespace(input, cursor);
    if (cursor >= input.size()) {
        return false;
    }
    if (input[cursor] == '"') {
        std::size_t begin = 0;
        std::size_t end = 0;
        return scan_json_string(input, cursor, begin, end);
    }
    if (input[cursor] == '{') {
        ++cursor;
        skip_whitespace(input, cursor);
        if (cursor < input.size() && input[cursor] == '}') {
            ++cursor;
            return true;
        }
        for (;;) {
            std::size_t keyBegin = 0;
            std::size_t keyEnd = 0;
            if (!scan_json_string(input, cursor, keyBegin, keyEnd)) {
                return false;
            }
            skip_whitespace(input, cursor);
            if (cursor >= input.size() || input[cursor++] != ':') {
                return false;
            }
            if (!skip_json_value(input, cursor, depth + 1U)) {
                return false;
            }
            skip_whitespace(input, cursor);
            if (cursor >= input.size()) {
                return false;
            }
            const char delimiter = input[cursor++];
            if (delimiter == '}') {
                return true;
            }
            if (delimiter != ',') {
                return false;
            }
        }
    }
    if (input[cursor] == '[') {
        ++cursor;
        skip_whitespace(input, cursor);
        if (cursor < input.size() && input[cursor] == ']') {
            ++cursor;
            return true;
        }
        for (;;) {
            if (!skip_json_value(input, cursor, depth + 1U)) {
                return false;
            }
            skip_whitespace(input, cursor);
            if (cursor >= input.size()) {
                return false;
            }
            const char delimiter = input[cursor++];
            if (delimiter == ']') {
                return true;
            }
            if (delimiter != ',') {
                return false;
            }
        }
    }

    const std::size_t start = cursor;
    while (cursor < input.size()) {
        const char value = input[cursor];
        if (value == ',' || value == ']' || value == '}' || value == ' ' || value == '\t'
            || value == '\r' || value == '\n') {
            break;
        }
        ++cursor;
    }
    return cursor != start;
}

/** Finds one direct member inside a JSON object and returns the complete value span. */
[[nodiscard]] bool find_json_member(std::string_view input,
                                    std::size_t objectBegin,
                                    std::string_view target,
                                    std::size_t& valueBegin,
                                    std::size_t& valueEnd) noexcept {
    std::size_t cursor = objectBegin;
    skip_whitespace(input, cursor);
    if (cursor >= input.size() || input[cursor++] != '{') {
        return false;
    }
    skip_whitespace(input, cursor);
    if (cursor < input.size() && input[cursor] == '}') {
        return false;
    }
    for (;;) {
        std::size_t keyBegin = 0;
        std::size_t keyEnd = 0;
        if (!scan_json_string(input, cursor, keyBegin, keyEnd)) {
            return false;
        }
        skip_whitespace(input, cursor);
        if (cursor >= input.size() || input[cursor++] != ':') {
            return false;
        }
        skip_whitespace(input, cursor);
        const std::size_t begin = cursor;
        if (!skip_json_value(input, cursor, 0)) {
            return false;
        }
        const std::size_t end = cursor;
        if (input.substr(keyBegin, keyEnd - keyBegin) == target) {
            valueBegin = begin;
            valueEnd = end;
            return true;
        }
        skip_whitespace(input, cursor);
        if (cursor >= input.size()) {
            return false;
        }
        const char delimiter = input[cursor++];
        if (delimiter == '}') {
            return false;
        }
        if (delimiter != ',') {
            return false;
        }
    }
}

/** Fixed-capacity writer used to keep the settings mirror beneath Core's 64 KiB limit. */
struct JsonWriter final {
    std::array<char, kSettingsCapacity>& bytes;
    std::size_t size{};

    [[nodiscard]] bool append(std::string_view value) noexcept {
        if (value.size() > bytes.size() - size) {
            return false;
        }
        std::memcpy(bytes.data() + size, value.data(), value.size());
        size += value.size();
        return true;
    }

    [[nodiscard]] bool character(char value) noexcept {
        if (size == bytes.size()) {
            return false;
        }
        bytes[size++] = value;
        return true;
    }
};

/** Writes one formatted scalar into the fixed JSON buffer. */
template <typename... Args>
[[nodiscard]] bool append_format(JsonWriter& output, const char* format, Args... args) noexcept {
    std::array<char, 768> text{};
    const int count = std::snprintf(text.data(), text.size(), format, args...);
    return count > 0 && static_cast<std::size_t>(count) < text.size()
           && output.append({text.data(), static_cast<std::size_t>(count)});
}

/** Converts a live character key back onto the authored account key used by settings.json. */
[[nodiscard]] bool authored_character_soid(const AccountState& account,
                                           const CharacterState& character,
                                           std::uint64_t& output) noexcept {
    const std::uint64_t authoredPrimary =
        g_configuredAccount.primarySoid != 0 ? g_configuredAccount.primarySoid : account.primarySoid;
    if (authoredPrimary == 0 || account.primarySoid == 0 || character.soid <= account.primarySoid) {
        return false;
    }
    const std::uint64_t offset = character.soid - account.primarySoid;
    if (authoredPrimary > (std::numeric_limits<std::uint64_t>::max)() - offset) {
        return false;
    }
    output = authoredPrimary + offset;
    return output != 0 && output != authoredPrimary;
}

/** Serializes one optional runtime item using the stable Sunrise settings schema. */
[[nodiscard]] bool append_item(JsonWriter& output,
                               const std::optional<account::inventory::Item>& item) noexcept {
    if (!item.has_value()) {
        return output.append("null");
    }
    if (!account::inventory::valid(*item)
        || !append_format(output,
                          "{\"instance_soid\":\"0x%016llX\",\"definition_hash\":\"0x%08X\",\"level\":%d,\"quantity\":%d,\"plugs\":",
                          static_cast<unsigned long long>(item->instanceSoid),
                          static_cast<unsigned>(item->definitionHash),
                          static_cast<int>(item->level),
                          static_cast<int>(item->quantity))) {
        return false;
    }
    if (item->sockets.policy == account::inventory::SocketPolicy::nativeDefaults) {
        if (!output.append("null")) {
            return false;
        }
    } else {
        if (!output.character('[')) {
            return false;
        }
        for (std::size_t lane = 0; lane < item->sockets.plugCount; ++lane) {
            if (lane != 0 && !output.character(',')) {
                return false;
            }
            if (!item->sockets.plugs[lane].has_value()) {
                if (!output.append("null")) {
                    return false;
                }
            } else if (!append_format(output,
                                      "\"0x%08X\"",
                                      static_cast<unsigned>(*item->sockets.plugs[lane]))) {
                return false;
            }
        }
        if (!output.character(']')) {
            return false;
        }
    }
    return output.character('}');
}

/** Serializes the account-wide profile inventory using Core's stable settings schema. */
[[nodiscard]] bool append_profile_items(JsonWriter& output, const AccountState& account) noexcept {
    if (!account::valid(account) || !output.character('[')) {
        return false;
    }
    for (std::size_t index = 0; index < account.profileItemCount; ++index) {
        const account::inventory::ProfileItem& item = account.profileItems[index];
        if ((index != 0 && !output.character(','))
            || !append_format(output,
                              "{\"instance_soid\":\"0x%016llX\",\"definition_hash\":\"0x%08X\","
                              "\"quantity\":%d,\"mutation_serial\":%d}",
                              static_cast<unsigned long long>(item.instanceSoid),
                              static_cast<unsigned>(item.definitionHash),
                              static_cast<int>(item.quantity),
                              static_cast<int>(item.mutationSerial))) {
            return false;
        }
    }
    return output.character(']');
}

/** Serializes all runtime characters and equipment into the settings-compatible array. */
[[nodiscard]] bool append_characters(JsonWriter& output, const AccountState& account) noexcept {
    if (!account::valid(account) || !output.character('[')) {
        return false;
    }
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        const CharacterState& character = account.characters[index];
        std::uint64_t persistedSoid = 0;
        if (!authored_character_soid(account, character, persistedSoid)
            || (index != 0 && !output.character(','))
            || !append_format(output,
                              "{\"soid\":\"0x%016llX\",\"race\":%u,\"gender\":%u,\"class\":%u,"
                              "\"movement_ability\":%u,\"grenade_ability\":%u,\"super_ability\":%u,"
                              "\"melee_ability\":%u,\"class_ability\":%u,\"level\":%u,\"accepted\":%s,"
                              "\"preview_available\":%s,\"appearance_value\":%.9g,"
                              "\"last_orbited_destination\":\"0x%08X\",\"content_bypass\":%s,\"equipment\":{",
                              static_cast<unsigned long long>(persistedSoid),
                              static_cast<unsigned>(character.race),
                              static_cast<unsigned>(character.gender),
                              static_cast<unsigned>(character.characterClass),
                              static_cast<unsigned>(character.movementAbilityEntry),
                              static_cast<unsigned>(character.grenadeAbilityEntry),
                              static_cast<unsigned>(character.superAbilityEntry),
                              static_cast<unsigned>(character.meleeAbilityEntry),
                              static_cast<unsigned>(character.classAbilityEntry),
                              static_cast<unsigned>(character.level),
                              character.accepted ? "true" : "false",
                              character.previewAvailable ? "true" : "false",
                              static_cast<double>(character.appearanceValue),
                              static_cast<unsigned>(character.lastOrbitedDestination),
                              character.contentBypass ? "true" : "false")) {
            return false;
        }
        for (std::size_t slotIndex = 0; slotIndex < account::inventory::kEquipmentSlotCount;
             ++slotIndex) {
            const auto slot = static_cast<account::inventory::EquipmentSlot>(slotIndex);
            const std::string_view name = account::inventory::slot_name(slot);
            if ((slotIndex != 0 && !output.character(',')) || !output.character('"')
                || !output.append(name) || !output.append("\":")
                || !append_item(output, character.equipment.slots[slotIndex])) {
                return false;
            }
        }
        if (!output.append("}}")) {
            return false;
        }
    }
    return output.character(']');
}

/** Reads the current settings document into bounded storage. */
[[nodiscard]] bool read_settings(std::array<char, kSettingsCapacity>& bytes,
                                 std::size_t& size) noexcept {
    size = 0;
    const HANDLE file = CreateFileW(g_settingsPath.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER fileSize{};
    bool ok = GetFileSizeEx(file, &fileSize) != FALSE && fileSize.QuadPart > 0
              && static_cast<std::uint64_t>(fileSize.QuadPart) <= bytes.size();
    DWORD copied = 0;
    if (ok) {
        const DWORD requested = static_cast<DWORD>(fileSize.QuadPart);
        ok = ReadFile(file, bytes.data(), requested, &copied, nullptr) != FALSE
             && copied == requested;
        size = ok ? copied : 0;
    }
    ok = CloseHandle(file) != FALSE && ok;
    return ok;
}

/** Atomically writes a verified settings mirror. */
[[nodiscard]] bool write_settings(std::string_view document) noexcept {
    core::path::Buffer stage = g_settingsPath;
    if (!core::path::append(stage, kSettingsStageSuffix)) {
        return false;
    }
    const HANDLE file = CreateFileW(stage.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD size = static_cast<DWORD>(document.size());
    bool ok = write_exact(file, document.data(), size) && FlushFileBuffers(file) != FALSE;
    ok = CloseHandle(file) != FALSE && ok;
    if (ok) {
        ok = MoveFileExW(stage.chars.data(),
                         g_settingsPath.chars.data(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             != FALSE;
    }
    if (!ok) {
        (void)DeleteFileW(stage.chars.data());
    }
    return ok;
}

} // namespace

/** Captures templates and overlays a valid persistent character store when present. */
void initialize(void* module, const AccountState& authored, AccountState& active) noexcept {
    g_storePath = {};
    g_settingsPath = {};
    g_creationCaptureDirectory = {};
    g_configuredAccount = authored;
    for (std::size_t index = 0; index < g_configuredAccount.characterCount; ++index) {
        g_configuredAccount.characters[index].selected = false;
    }
    normalize_inventory_generations(g_configuredAccount);
    // Class factory templates are separate from the user's authored roster. Native creation and
    // package prefetch can therefore rely on all three classes even when settings.json only keeps
    // one or two characters. The authored account remains available for reset/compatibility.
    if (load_factory_account(module, g_factoryAccount)) {
        normalize_inventory_generations(g_factoryAccount);
        report("factory_templates", "ok", g_factoryAccount.characterCount);
    } else {
        g_factoryAccount = {};
        report("factory_templates", "fallback_settings", g_configuredAccount.characterCount);
    }
    g_status = {};
    g_status.lastSaveOk = true;
    g_status.lastSettingsMirrorOk = true;
    active = authored;
    normalize_inventory_generations(active);
    core::path::Buffer directory{};
    if (module != nullptr && core::path::artifact_directory(module, directory)) {
        g_storePath = directory;
        g_settingsPath = directory;
        g_creationCaptureDirectory = directory;
        if (core::path::append(g_storePath, kStoreSuffix)
            && core::path::append(g_settingsPath, kSettingsSuffix)) {
            g_status.available = true;
            load(active);
            return;
        }
    }
    report("init", "unavailable");
}

/** Drops process-local store metadata. */
void shutdown() noexcept {
    g_storePath = {};
    g_settingsPath = {};
    g_creationCaptureDirectory = {};
    g_configuredAccount = {};
    g_factoryAccount = {};
    g_status = {};
}

/** Saves one native create-character payload under the first unused numbered capture name. */
bool capture_creation_request(std::span<const std::byte> payload,
                              std::uint32_t& captureIndex) noexcept {
    captureIndex = 0;
    if (!g_status.available || g_creationCaptureDirectory.chars[0] == L'\0' || payload.empty()
        || payload.size() > (std::numeric_limits<DWORD>::max)()) {
        report("create501_capture", "unavailable", payload.size());
        return false;
    }

    for (std::uint32_t index = 1; index <= kMaximumCreationCaptures; ++index) {
        std::array<wchar_t, 64> suffix{};
        const int suffixLength = std::swprintf(suffix.data(),
                                               suffix.size(),
                                               L"\\character_create_501_%03u.bin",
                                               static_cast<unsigned>(index));
        if (suffixLength <= 0 || static_cast<std::size_t>(suffixLength) >= suffix.size()) {
            report("create501_capture", "path_fail", payload.size());
            return false;
        }

        core::path::Buffer path = g_creationCaptureDirectory;
        if (!core::path::append(path,
                                {suffix.data(), static_cast<std::size_t>(suffixLength)})) {
            report("create501_capture", "path_fail", payload.size());
            return false;
        }

        // CREATE_NEW makes concurrent/restarted captures collision-safe without replacing history.
        const HANDLE file = CreateFileW(path.chars.data(),
                                        GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
                continue;
            }
            report("create501_capture", "open_fail", payload.size());
            return false;
        }

        const DWORD size = static_cast<DWORD>(payload.size());
        bool ok = write_exact(file, payload.data(), size) && FlushFileBuffers(file) != FALSE;
        ok = CloseHandle(file) != FALSE && ok;
        if (!ok) {
            (void)DeleteFileW(path.chars.data());
            report("create501_capture", "write_fail", payload.size());
            return false;
        }

        captureIndex = index;
        report("create501_capture", "ok", payload.size());
        return true;
    }

    report("create501_capture", "full", payload.size());
    return false;
}

/** Atomically writes the complete dense character array. */
bool persist(const AccountState& account) noexcept {
    if (!g_status.available || !account::valid(account)) {
        g_status.lastSaveOk = false;
        return false;
    }
    std::array<DiskCharacter, kCharacterCapacity> rows{};
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        encode_character(account.characters[index], account.primarySoid, rows[index]);
        if (rows[index].accountOffset == 0) {
            g_status.lastSaveOk = false;
            return false;
        }
    }
    const auto payload = std::as_bytes(std::span(rows).first(account.characterCount));
    DiskHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.headerSize = static_cast<std::uint32_t>(sizeof(DiskHeader));
    header.characterSize = static_cast<std::uint32_t>(sizeof(DiskCharacter));
    header.characterCount = static_cast<std::uint32_t>(account.characterCount);
    header.checksum = checksum(payload);

    core::path::Buffer stage = g_storePath;
    if (!core::path::append(stage, kStageSuffix)) {
        g_status.lastSaveOk = false;
        return false;
    }
    const HANDLE file = CreateFileW(stage.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        g_status.lastSaveOk = false;
        return false;
    }
    const DWORD payloadSize = static_cast<DWORD>(payload.size());
    bool ok = write_exact(file, &header, static_cast<DWORD>(sizeof header))
              && (payloadSize == 0 || write_exact(file, payload.data(), payloadSize))
              && FlushFileBuffers(file) != FALSE;
    ok = CloseHandle(file) != FALSE && ok;
    if (ok) {
        ok = MoveFileExW(stage.chars.data(),
                         g_storePath.chars.data(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
             != FALSE;
    }
    if (!ok) {
        (void)DeleteFileW(stage.chars.data());
        g_status.lastSaveOk = false;
        report("save", "fail", account.characterCount);
        return false;
    }
    g_status.loaded = true;
    g_status.rejected = false;
    g_status.lastSaveOk = true;
    report("save", "ok", account.characterCount);
    return true;
}

/** Mirrors runtime profile inventory plus character/equipment rows into settings.json. */
bool mirror_settings(const AccountState& account) noexcept {
    g_status.lastSettingsMirrorOk = false;
    if (!g_status.available || !account::valid(account) || g_settingsPath.chars[0] == L'\0') {
        report("settings_mirror", "unavailable", account.characterCount);
        return false;
    }

    std::array<char, kSettingsCapacity> currentBytes{};
    std::size_t currentSize = 0;
    if (!read_settings(currentBytes, currentSize)) {
        report("settings_mirror", "read_fail", account.characterCount);
        return false;
    }
    const std::string_view current(currentBytes.data(), currentSize);
    std::size_t stateBegin = 0;
    std::size_t stateEnd = 0;
    std::size_t accountBegin = 0;
    std::size_t accountEnd = 0;
    std::size_t profileBegin = 0;
    std::size_t profileEnd = 0;
    std::size_t charactersBegin = 0;
    std::size_t charactersEnd = 0;
    if (!find_json_member(current, 0, "state", stateBegin, stateEnd)
        || stateBegin >= stateEnd || current[stateBegin] != '{'
        || !find_json_member(current, stateBegin, "account", accountBegin, accountEnd)
        || accountBegin >= accountEnd || current[accountBegin] != '{'
        || !find_json_member(current, accountBegin, "profile_items", profileBegin, profileEnd)
        || profileBegin >= profileEnd
        || !find_json_member(current, stateBegin, "characters", charactersBegin, charactersEnd)
        || charactersBegin >= charactersEnd
        || !((profileEnd <= charactersBegin) || (charactersEnd <= profileBegin))) {
        report("settings_mirror", "locate_fail", account.characterCount);
        return false;
    }

    std::array<char, kSettingsCapacity> profileBytes{};
    JsonWriter profiles{profileBytes};
    std::array<char, kSettingsCapacity> characterBytes{};
    JsonWriter characters{characterBytes};
    if (!append_profile_items(profiles, account) || !append_characters(characters, account)) {
        report("settings_mirror", "encode_fail", account.characterCount);
        return false;
    }

    struct ReplacementSpan final {
        std::size_t begin{};
        std::size_t end{};
        std::string_view bytes{};
    };
    std::array<ReplacementSpan, 2> spans{{
        {profileBegin, profileEnd, {profileBytes.data(), profiles.size}},
        {charactersBegin, charactersEnd, {characterBytes.data(), characters.size}},
    }};
    if (spans[1].begin < spans[0].begin) {
        std::swap(spans[0], spans[1]);
    }

    std::array<char, kSettingsCapacity> replacement{};
    std::size_t replacementSize = 0;
    std::size_t cursor = 0;
    for (const ReplacementSpan& span : spans) {
        if (cursor > span.begin || span.end > current.size()) {
            report("settings_mirror", "locate_fail", account.characterCount);
            return false;
        }
        const std::size_t prefixSize = span.begin - cursor;
        if (prefixSize > replacement.size() - replacementSize
            || span.bytes.size() > replacement.size() - replacementSize - prefixSize) {
            report("settings_mirror", "too_large", account.characterCount);
            return false;
        }
        if (prefixSize != 0) {
            std::memcpy(replacement.data() + replacementSize, current.data() + cursor, prefixSize);
            replacementSize += prefixSize;
        }
        if (!span.bytes.empty()) {
            std::memcpy(replacement.data() + replacementSize, span.bytes.data(), span.bytes.size());
            replacementSize += span.bytes.size();
        }
        cursor = span.end;
    }
    const std::size_t suffixSize = current.size() - cursor;
    if (suffixSize > replacement.size() - replacementSize) {
        report("settings_mirror", "too_large", account.characterCount);
        return false;
    }
    if (suffixSize != 0) {
        std::memcpy(replacement.data() + replacementSize, current.data() + cursor, suffixSize);
        replacementSize += suffixSize;
    }

    core::settings::Settings verified{};
    const std::string_view document(replacement.data(), replacementSize);
    if (!core::settings::parse(document, verified) || !write_settings(document)) {
        report("settings_mirror", "verify_or_write_fail", account.characterCount);
        return false;
    }
    g_status.lastSettingsMirrorOk = true;
    report("settings_mirror", "ok", account.characterCount);
    return true;
}

/** Finds one immutable class starter template, preferring the bundled three-class factory set. */
bool template_for_class(CharacterClass characterClass, CharacterState& output) noexcept {
    const auto find = [characterClass](const AccountState& account, CharacterState& result) noexcept {
        const std::size_t count = (std::min)(account.characterCount, account.characters.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (account.characters[index].characterClass == characterClass) {
                result = account.characters[index];
                result.selected = false;
                return true;
            }
        }
        return false;
    };
    if (find(g_factoryAccount, output) || find(g_configuredAccount, output)) {
        return true;
    }
    output = {};
    return false;
}

/** Returns the complete immutable factory account for package detail/ability prefetch when present. */
AccountState configured_account() noexcept {
    return g_factoryAccount.primarySoid != 0 ? g_factoryAccount : g_configuredAccount;
}

/** Returns the user's boot-authored account for the explicit reset-to-settings action. */
AccountState authored_account() noexcept {
    return g_configuredAccount;
}

/** Returns persistence status. */
StoreStatus status() noexcept {
    return g_status;
}

} // namespace sunrise::state::account::characters
