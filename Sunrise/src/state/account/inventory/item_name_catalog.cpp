#include "item_name_catalog.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"

namespace sunrise::state::account::inventory::item_names {
namespace {

/** d2loadouts catalogue copied by the user into Sunrise's owned artifact directory. */
constexpr std::wstring_view kCatalogSuffix = L"\\items.js";
struct NameEntry final {
    std::uint32_t definitionHash{};
    std::string name{};
};

core::path::Buffer g_path{};
std::vector<Option> g_options{};
std::vector<NameEntry> g_names{};
Status g_status{};

/** Emits one compact catalogue lifecycle line. */
void report(const char* result) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=item_names result=%s options=%zu names=%zu",
                                      result,
                                      g_options.size(),
                                      g_names.size());
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Skips ASCII whitespace inside the JS/JSON-compatible data section. */
void skip_space(std::string_view text, std::size_t& cursor) noexcept {
    while (cursor < text.size()) {
        const char value = text[cursor];
        if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
            break;
        }
        ++cursor;
    }
}

/** Appends one Unicode scalar as UTF-8. Invalid scalar values are refused. */
[[nodiscard]] bool append_utf8(std::uint32_t value, std::string& output) {
    if (value <= 0x7FU) {
        output.push_back(static_cast<char>(value));
        return true;
    }
    if (value <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        return true;
    }
    if (value >= 0xD800U && value <= 0xDFFFU) {
        return false;
    }
    if (value <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        return true;
    }
    if (value <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        return true;
    }
    return false;
}

/** Reads four hexadecimal digits used by a JSON `\\uXXXX` escape. */
[[nodiscard]] bool parse_hex4(std::string_view text,
                              std::size_t cursor,
                              std::uint32_t& output) noexcept {
    if (cursor > text.size() || text.size() - cursor < 4U) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        const char digit = text[cursor + index];
        value <<= 4U;
        if (digit >= '0' && digit <= '9') {
            value |= static_cast<std::uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value |= static_cast<std::uint32_t>(10 + digit - 'a');
        } else if (digit >= 'A' && digit <= 'F') {
            value |= static_cast<std::uint32_t>(10 + digit - 'A');
        } else {
            return false;
        }
    }
    output = value;
    return true;
}

/** Parses one JSON-compatible quoted UTF-8 string and advances past its closing quote. */
[[nodiscard]] bool parse_string(std::string_view text,
                                std::size_t& cursor,
                                std::string& output) {
    output.clear();
    skip_space(text, cursor);
    if (cursor >= text.size() || text[cursor] != '"') {
        return false;
    }
    ++cursor;
    while (cursor < text.size()) {
        const char value = text[cursor++];
        if (value == '"') {
            return true;
        }
        if (value != '\\') {
            output.push_back(value);
            continue;
        }
        if (cursor >= text.size()) {
            return false;
        }
        const char escape = text[cursor++];
        switch (escape) {
        case '"':
        case '\\':
        case '/':
            output.push_back(escape);
            break;
        case 'b':
            output.push_back('\b');
            break;
        case 'f':
            output.push_back('\f');
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        case 'u': {
            std::uint32_t scalar = 0;
            if (!parse_hex4(text, cursor, scalar)) {
                return false;
            }
            cursor += 4U;
            if (!append_utf8(scalar, output)) {
                return false;
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

/** Finds the matching close token while ignoring bracket-like bytes inside strings. */
[[nodiscard]] std::size_t matching(std::string_view text,
                                   std::size_t open,
                                   char openToken,
                                   char closeToken) noexcept {
    if (open >= text.size() || text[open] != openToken) {
        return std::string_view::npos;
    }
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t cursor = open; cursor < text.size(); ++cursor) {
        const char value = text[cursor];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                inString = false;
            }
            continue;
        }
        if (value == '"') {
            inString = true;
        } else if (value == openToken) {
            ++depth;
        } else if (value == closeToken) {
            if (depth == 0) {
                return std::string_view::npos;
            }
            --depth;
            if (depth == 0) {
                return cursor;
            }
        }
    }
    return std::string_view::npos;
}

/** Locates one quoted field key inside an object slice. */
[[nodiscard]] std::size_t field_value(std::string_view object, std::string_view key) noexcept {
    std::string needle;
    try {
        needle.reserve(key.size() + 2U);
        needle.push_back('"');
        needle.append(key);
        needle.push_back('"');
    } catch (...) {
        return std::string_view::npos;
    }
    const std::size_t found = object.find(needle);
    if (found == std::string_view::npos) {
        return found;
    }
    std::size_t cursor = found + needle.size();
    skip_space(object, cursor);
    if (cursor >= object.size() || object[cursor] != ':') {
        return std::string_view::npos;
    }
    ++cursor;
    skip_space(object, cursor);
    return cursor;
}

/** Reads one unsigned 32-bit decimal field. */
[[nodiscard]] bool u32_field(std::string_view object,
                             std::string_view key,
                             std::uint32_t& output) noexcept {
    const std::size_t start = field_value(object, key);
    if (start == std::string_view::npos) {
        return false;
    }
    const char* first = object.data() + start;
    const char* last = object.data() + object.size();
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(first, last, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr == first || value > 0xFFFFFFFFULL) {
        return false;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

/** Reads one signed small integer field. */
[[nodiscard]] bool int_field(std::string_view object,
                             std::string_view key,
                             int& output) noexcept {
    const std::size_t start = field_value(object, key);
    if (start == std::string_view::npos) {
        return false;
    }
    const char* first = object.data() + start;
    const char* last = object.data() + object.size();
    const auto parsed = std::from_chars(first, last, output, 10);
    return parsed.ec == std::errc{} && parsed.ptr != first;
}

/** Reads one string field. */
[[nodiscard]] bool string_field(std::string_view object,
                                std::string_view key,
                                std::string& output) {
    std::size_t cursor = field_value(object, key);
    return cursor != std::string_view::npos && parse_string(object, cursor, output);
}

/** Converts d2loadouts' stable slot text into Sunrise's semantic slot. */
[[nodiscard]] bool option_slot(std::string_view value, EquipmentSlot& output) noexcept {
    const std::optional<EquipmentSlot> slot = slot_from_name(value);
    if (!slot.has_value()) {
        return false;
    }
    output = *slot;
    return true;
}

/** Parses one weapon/armour/subclass object into a selectable option. */
[[nodiscard]] bool parse_option(std::string_view object,
                                bool subclass,
                                Option& output) {
    std::uint32_t hash = 0;
    std::string name;
    std::string slotName;
    if (!u32_field(object, "decimal", hash) || hash == 0 || !string_field(object, "name", name)) {
        return false;
    }
    EquipmentSlot slot = EquipmentSlot::subclass;
    if (!subclass && (!string_field(object, "slot", slotName) || !option_slot(slotName, slot))) {
        return false;
    }
    int characterClass = -1;
    int parsedClass = -1;
    if (int_field(object, "class", parsedClass) && parsedClass >= 0 && parsedClass <= 2) {
        characterClass = parsedClass;
    }
    output.definitionHash = hash;
    output.slot = slot;
    output.characterClass = static_cast<std::int8_t>(characterClass);
    output.name = std::move(name);
    return true;
}

/** Parses one named option array. */
[[nodiscard]] bool parse_option_array(std::string_view text,
                                      std::string_view section,
                                      bool subclass,
                                      std::vector<Option>& output) {
    std::string needle;
    needle.reserve(section.size() + 2U);
    needle.push_back('"');
    needle.append(section);
    needle.push_back('"');
    const std::size_t sectionPos = text.find(needle);
    if (sectionPos == std::string_view::npos) {
        return false;
    }
    const std::size_t open = text.find('[', sectionPos + needle.size());
    const std::size_t close = matching(text, open, '[', ']');
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return false;
    }
    std::size_t cursor = open + 1U;
    while (cursor < close) {
        const std::size_t objectOpen = text.find('{', cursor);
        if (objectOpen == std::string_view::npos || objectOpen >= close) {
            break;
        }
        const std::size_t objectClose = matching(text, objectOpen, '{', '}');
        if (objectClose == std::string_view::npos || objectClose > close) {
            return false;
        }
        Option option{};
        if (parse_option(text.substr(objectOpen, objectClose - objectOpen + 1U), subclass, option)) {
            output.push_back(std::move(option));
        }
        cursor = objectClose + 1U;
    }
    return true;
}

/** Parses the decimal-hash -> display-name map. */
[[nodiscard]] bool parse_names(std::string_view text, std::vector<NameEntry>& output) {
    constexpr std::string_view kNames = "\"names\"";
    const std::size_t section = text.find(kNames);
    if (section == std::string_view::npos) {
        return false;
    }
    const std::size_t open = text.find('{', section + kNames.size());
    const std::size_t close = matching(text, open, '{', '}');
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return false;
    }
    std::size_t cursor = open + 1U;
    std::string key;
    std::string name;
    while (cursor < close) {
        skip_space(text, cursor);
        if (cursor < close && text[cursor] == ',') {
            ++cursor;
            continue;
        }
        if (cursor >= close) {
            break;
        }
        if (!parse_string(text, cursor, key)) {
            return false;
        }
        skip_space(text, cursor);
        if (cursor >= close || text[cursor] != ':') {
            return false;
        }
        ++cursor;
        if (!parse_string(text, cursor, name)) {
            return false;
        }
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), value, 10);
        if (parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size()
            && value <= 0xFFFFFFFFULL) {
            output.push_back(NameEntry{static_cast<std::uint32_t>(value), name});
        }
    }
    return !output.empty();
}

/** Reads a complete optional catalogue file. */
[[nodiscard]] bool read_file(std::string& output) {
    if (!g_status.pathAvailable) {
        return false;
    }
    std::ifstream file(std::filesystem::path{g_path.chars.data()}, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff end = file.tellg();
    if (end <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    output.resize(static_cast<std::size_t>(end));
    file.read(output.data(), static_cast<std::streamsize>(output.size()));
    return static_cast<std::size_t>(file.gcount()) == output.size();
}

/** Sorts selectable options for stable slot/class/name presentation. */
void finalize_options() {
    std::sort(g_options.begin(), g_options.end(), [](const Option& left, const Option& right) {
        if (left.slot != right.slot) {
            return static_cast<std::uint8_t>(left.slot) < static_cast<std::uint8_t>(right.slot);
        }
        if (left.characterClass != right.characterClass) {
            return left.characterClass < right.characterClass;
        }
        return left.name < right.name;
    });
}

/** Sorts the names for binary-search lookup and keeps the first name for duplicate hashes. */
void finalize_names() {
    std::sort(g_names.begin(), g_names.end(), [](const NameEntry& left, const NameEntry& right) {
        return left.definitionHash < right.definitionHash;
    });
    g_names.erase(std::unique(g_names.begin(),
                              g_names.end(),
                              [](const NameEntry& left, const NameEntry& right) {
                                  return left.definitionHash == right.definitionHash;
                              }),
                  g_names.end());
}

} // namespace

/** Loads the optional d2loadouts catalogue without making State initialization depend on it. */
void initialize(void* module) noexcept {
    shutdown();
    if (module == nullptr || !core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kCatalogSuffix)) {
        report("path_fail");
        return;
    }
    g_status.pathAvailable = true;
    try {
        std::string text;
        if (!read_file(text)) {
            report("missing");
            return;
        }
        std::vector<Option> options;
        std::vector<NameEntry> names;
        const bool parsed = parse_option_array(text, "weapons", false, options)
                            && parse_option_array(text, "armor", false, options)
                            && parse_option_array(text, "subclasses", true, options)
                            && parse_names(text, names);
        if (!parsed || options.empty() || names.empty()) {
            report("parse_fail");
            return;
        }
        g_options = std::move(options);
        g_names = std::move(names);
        finalize_options();
        finalize_names();
        g_status.loaded = true;
        g_status.optionCount = g_options.size();
        g_status.nameCount = g_names.size();
        report("ok");
    } catch (...) {
        g_options.clear();
        g_names.clear();
        g_status.loaded = false;
        report("exception");
    }
}

/** Clears all process-local catalogue storage. */
void shutdown() noexcept {
    g_path = {};
    g_options.clear();
    g_names.clear();
    g_status = {};
}

Status status() noexcept {
    Status value = g_status;
    value.optionCount = g_options.size();
    value.nameCount = g_names.size();
    return value;
}

std::span<const Option> options() noexcept {
    return g_options;
}

std::string_view name_for_hash(std::uint32_t definitionHash) noexcept {
    const auto found = std::lower_bound(g_names.begin(),
                                        g_names.end(),
                                        definitionHash,
                                        [](const NameEntry& entry, std::uint32_t value) {
                                            return entry.definitionHash < value;
                                        });
    return found != g_names.end() && found->definitionHash == definitionHash
               ? std::string_view{found->name}
               : std::string_view{};
}

} // namespace sunrise::state::account::inventory::item_names
