/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_vector.hh"

struct bAnimListElem;
struct bContext;
struct ListBase;

namespace blender::ed::action {
/**
 * Return all bAnimListElem for which the keyframes are visible in the
 * GUI.
 */
void get_visible_elements(bContext *C, ListBase &r_anim_data);

}  // namespace blender::ed::action
