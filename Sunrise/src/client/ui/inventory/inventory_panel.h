#pragma once

namespace sunrise::client::ui::inventory {

/** Draws equipped and unequipped owned items for the selected runtime character. */
void draw_inventory() noexcept;

/** Draws the installed native item catalogue and existing-equipment replacement editor. */
void draw_item_catalog() noexcept;

} // namespace sunrise::client::ui::inventory
