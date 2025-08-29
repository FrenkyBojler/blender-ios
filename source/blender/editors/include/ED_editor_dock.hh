#pragma once

#include <optional>

#include "DNA_screen_types.h"
#include "DNA_space_enums.h"

struct bContext;
struct rcti;
struct wmWindow;

namespace blender::ed::editor_dock {

ScrArea *add_docked_area(bScreen *screen, const rcti &area_rect, DockedAreaPosition position);
SpaceLink *add_docked_space(ScrArea *area,
                            const eSpace_Type type,
                            std::optional<int> subtype,
                            const Scene *scene);
void activate_docked_space(bContext *C, ScrArea *docked_area, SpaceLink *space);
/**
 * Make sure \a space is visible and active, or if already visible and active, hide it.
 *
 * This is exactly the behavior of clicking the editor tab in the editor dock.
 *
 * Hiding/unhiding a space means hiding/unhiding the area it contains (\a docked_area here).
 */
void toggle_docked_space(bContext *C, ScrArea *docked_area, SpaceLink *space);
void hide_docked_area(const wmWindow *win, ScrArea *docked_area);
void unhide_docked_area(const wmWindow *win, ScrArea *docked_area);

}  // namespace blender::ed::editor_dock
