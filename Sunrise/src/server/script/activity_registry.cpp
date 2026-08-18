#include "activity_registry.h"

#include <array>

namespace sunrise::server::script::activity_registry {
namespace {

/**
 * Activity scripts compose a large reusable type layer with a small authored per-activity layer.
 * Adding a strike, mission, raid, Gambit mode, or PvP map adds rows rather than loader branches.
 */
constexpr std::array<Definition, 1> kDefinitions{{
    {"strike_bond",
     L"activities\\strikes\\main.lua",
     L"activities\\strikes\\garden_world\\main.lua",
     "strikes",
     "garden_world"},
}};

} // namespace

bool find(std::string_view destination, Definition& output) noexcept {
    output = {};
    for (const Definition& definition : kDefinitions) {
        if (definition.destination == destination) {
            output = definition;
            return true;
        }
    }
    return false;
}

} // namespace sunrise::server::script::activity_registry
