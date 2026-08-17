#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sunrise::state::account::inventory {

/** Authored equipment exposes the 16 named slots the first State supports. */
enum class EquipmentSlot : std::uint8_t {
    kinetic,
    energy,
    heavy,
    helmet,
    gauntlets,
    chest,
    legs,
    classItem,
    ghost,
    vehicle,
    ship,
    subclass,
    clanBanner,
    emblem,
    emote,
    finisher,
    count,
};

/** One fixed entry exists for every semantic equipment slot. */
inline constexpr std::size_t kEquipmentSlotCount = static_cast<std::size_t>(EquipmentSlot::count);
/** An authored item can pick at most 12 ordinary socket lanes. */
inline constexpr std::size_t kPlugCapacity = 12;
/** The engine no-definition hash cannot identify an authored item or plug. */
inline constexpr std::uint32_t kNoDefinitionHash = 0x811C9DC5U;

/** Says whether Middleware uses native socket defaults or authored lanes. */
enum class SocketPolicy : std::uint8_t {
    nativeDefaults,
    authored,
};

/** Fixed authored socket choices. An empty optional is an explicit empty lane. */
struct Sockets {
    SocketPolicy policy{SocketPolicy::nativeDefaults};
    std::array<std::optional<std::uint32_t>, kPlugCapacity> plugs{};
    std::size_t plugCount{};
};

/** Account-wide stacks can occupy every row of the native 701-row profile inventory. */
inline constexpr std::size_t kProfileItemCapacity = 701;
/** Mod and shader profile buckets reserve up to 100 resident action-source identities. */
inline constexpr std::size_t kProfileActionSourceCapacity = 100;
/** Runtime-owned profile action sources use a namespace separate from character item SOIDs. */
inline constexpr std::uint64_t kFirstProfileItemInstanceSoid = 0x5000000000000001ULL;
/**
 * The 16 supported character equipment buckets reserve 151 native rows in this build. One row
 * per semantic slot can be equipped, leaving at most 135 simultaneously unequipped instances.
 */
inline constexpr std::size_t kCharacterItemCapacity = 135;

/** One authored account-wide item, placed by the inventory bucket its definition names. */
struct ProfileItem {
    /** Optional stable runtime identity for profile rows that are also Family-4 action sources. */
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t quantity{};
    /** Rising generation copied into the native row when profile inventory is mutated. */
    std::int32_t mutationSerial{};
};

/** One authored equipment item without native table or wire-layout fields. */
struct Item {
    std::uint64_t instanceSoid{};
    std::uint32_t definitionHash{};
    std::int32_t level{};
    std::int32_t quantity{};
    /** Rising per-character generation assigned when the item changes inventory rows. */
    std::int32_t mutationSerial{};
    /** Native accumulated item-state bits such as favorite markers. */
    std::uint32_t flags{};
    Sockets sockets;
};

/** Ordered unequipped items placed into their installed character-inventory bucket ranges. */
struct CharacterItems {
    std::array<Item, kCharacterItemCapacity> values{};
    std::size_t count{};
};

/** One optional authored item for every semantic equipment slot. */
struct Equipment {
    std::array<std::optional<Item>, kEquipmentSlotCount> slots{};
};

/**
 * Finds the semantic State slot for one exact JSON equipment name.
 * @param name Borrowed case-sensitive equipment name.
 * @return Matching slot, or no value for an unknown name.
 */
[[nodiscard]] std::optional<EquipmentSlot> slot_from_name(std::string_view name) noexcept;

/** @return Stable configuration/display name for one semantic equipment slot. */
[[nodiscard]] std::string_view slot_name(EquipmentSlot slot) noexcept;

/**
 * Checks the canonical socket policy and every authored plug hash.
 * @return True when the policy, count and fixed tail agree.
 */
[[nodiscard]] bool valid(const Sockets& sockets) noexcept;

/**
 * Checks one whole authored item without reading installed build data.
 * @return True when the id, scalar and socket fields are valid.
 */
[[nodiscard]] bool valid(const Item& item) noexcept;

/**
 * Checks every item present in the fixed semantic equipment array.
 * @return True when every used slot holds a whole item.
 */
[[nodiscard]] bool valid(const Equipment& equipment) noexcept;

/** Checks the used prefix and empty tail of one character's unequipped item array. */
[[nodiscard]] bool valid(const CharacterItems& items) noexcept;

} // namespace sunrise::state::account::inventory
