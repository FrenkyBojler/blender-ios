/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"

struct Object;
struct DRWContext;
namespace blender::draw {
class ObjectRef;
}

namespace blender::draw {

void foreach_obref_in_scene(DRWContext &draw_ctx,
                            FunctionRef<bool(Object &)> should_draw_object_cb,
                            FunctionRef<void(ObjectRef &)> draw_object_cb);

}  // namespace blender::draw
