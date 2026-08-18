#pragma once

#include <string_view>

namespace sunrise::server::script::activity_registry {

/** Script pair selected for one authored activity destination. */
struct Definition final {
    std::string_view destination{};
    std::wstring_view typeMain{};
    std::wstring_view activityMain{};
    std::string_view typeName{};
    std::string_view activityName{};
};

/** Finds the reusable activity-type script and the small activity-specific script. */
[[nodiscard]] bool find(std::string_view destination, Definition& output) noexcept;

} // namespace sunrise::server::script::activity_registry
