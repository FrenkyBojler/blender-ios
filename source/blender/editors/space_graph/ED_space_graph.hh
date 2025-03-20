/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_vector.hh"

struct FCurve;
struct bContext;

namespace blender::ed::graph {
/**
 * Return all bAnimListElem (they contain only FCurves) for which the keyframes are visible in the
 * GUI. This excludes FCurves that are drawn as curves but whose keyframes are NOT shown.
 */
blender::Vector<bAnimListElem *> get_editable_fcurves(bContext *C);

}  // namespace blender::ed::graph
