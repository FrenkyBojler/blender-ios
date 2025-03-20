/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

 #include "BLI_vector.hh"

 struct FCurve;
 struct bContext;

namespace blender::ed::graph{

    blender::Vector<FCurve *> get_visible_fcurves(bContext *C);
    
}