/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_vector.hh"

struct bAnimListElem;
struct bContext;

namespace blender::ed::action {
/**
 * Return all bAnimListElem for which the keyframes are visible in the
 * GUI.
 */
blender::Vector<bAnimListElem *> get_visible_elements(bContext *C);

}  // namespace blender::ed::action
