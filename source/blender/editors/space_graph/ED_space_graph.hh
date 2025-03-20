/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_vector.hh"

struct bAnimListElem;
struct bContext;

namespace blender::ed::graph {
/**
 * Return all bAnimListElem (they contain only FCurves) for which the keyframes are visible in the
 * GUI. This excludes FCurves that are drawn as curves but whose keyframes are NOT shown.
 *
 * The reason a Vector of `bAnimListElem` is returned is that this struct is needed to do NLA
 * mapping.
 */
blender::Vector<bAnimListElem *> get_editable_fcurves(bContext *C);

}  // namespace blender::ed::graph
