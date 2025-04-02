/* SPDX-FileCopyrightText: 2013 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include "BKE_screen.hh"
#include "BLI_vector.hh"
#include "DNA_space_types.h"
#include "DNA_workspace_types.h"

struct ScrArea;
struct SpaceProperties;
struct bContext;
struct PointerRNA;

namespace blender::ed::properties {

constexpr std::array<blender::StringRefNull, BCONTEXT_TOT> filter_items = {
    "show_properties_tool",
    "show_properties_scene",
    "show_properties_render",
    "show_properties_output",
    "show_properties_view_layer",
    "show_properties_world",
    "show_properties_collection",
    "show_properties_object",
    "show_properties_constraints",
    "show_properties_modifiers",
    "show_properties_data",
    "show_properties_bone",
    "show_properties_bone_constraints",
    "show_properties_material",
    "show_properties_texture",
    "show_properties_particles",
    "show_properties_physics",
    "show_properties_effects",
};

}  // namespace blender::ed::properties

/**
 * Fills an array with the tab context values for the properties editor. -1 signals a separator.
 *
 * \return The total number of items in the array returned.
 */
blender::Vector<eSpaceButtons_Context> ED_buttons_tabs_list(const SpaceProperties *sbuts,
                                                            bool apply_filter = true);
void ED_buttons_visible_tabs_menu(bContext *C, uiLayout *layout, void * /*arg*/);
bool ED_buttons_tab_has_search_result(SpaceProperties *sbuts, int index);

void ED_buttons_search_string_set(SpaceProperties *sbuts, const char *value);
int ED_buttons_search_string_length(SpaceProperties *sbuts);
const char *ED_buttons_search_string_get(SpaceProperties *sbuts);

bool ED_buttons_should_sync_with_outliner(const bContext *C,
                                          const SpaceProperties *sbuts,
                                          ScrArea *area);
void ED_buttons_set_context(const bContext *C,
                            SpaceProperties *sbuts,
                            PointerRNA *ptr,
                            int context);
