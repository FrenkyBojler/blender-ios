/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_ghash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_sys_types.h"
#include "BLI_utildefines.h"

#include "BLI_function_ref.hh"

struct Object;
struct ID;
struct LightLinking;
namespace blender::bke {
struct GeometrySet;
}
struct DRWContext;
namespace blender::draw {
class ObjectRef;
}

namespace blender::draw {

enum class DrawObjectFlags : uint8_t {
  IsNegativeScale = 1 << 0,
};
ENUM_OPERATORS(DrawObjectFlags, DrawObjectFlags::IsNegativeScale);

struct DrawObjectKey {
  uint64_t hash_value;

  Object *object;
  ID *ob_data;
  const blender::bke::GeometrySet *preview_base_geometry;
  int preview_instance_index;
  DrawObjectFlags flags;

  DrawObjectKey(Object *object,
                ID *ob_data,
                DrawObjectFlags flags,
                const blender::bke::GeometrySet *preview_base_geometry,
                int preview_instance_index)
      : object(object),
        ob_data(ob_data),
        preview_base_geometry(preview_base_geometry),
        preview_instance_index(preview_instance_index),
        flags(flags)
  {
    hash_value = get_default_hash(object);
    hash_value = get_default_hash(hash_value, ob_data);
    hash_value = get_default_hash(hash_value, preview_base_geometry);
    hash_value = get_default_hash(hash_value, preview_instance_index);
    hash_value = get_default_hash(hash_value, uint8_t(flags));
  }

  uint64_t hash() const
  {
    return hash_value;
  }

  bool operator<(const DrawObjectKey &k) const
  {
    if (hash_value != k.hash_value) {
      return hash_value < k.hash_value;
    }
    if (object != k.object) {
      return object < k.object;
    }
    if (ob_data != k.ob_data) {
      return ob_data < k.ob_data;
    }
    if (flags != k.flags) {
      return flags < k.flags;
    }
    if (preview_base_geometry != k.preview_base_geometry) {
      return preview_base_geometry < k.preview_base_geometry;
    }
    if (preview_instance_index != k.preview_instance_index) {
      return preview_instance_index < k.preview_instance_index;
    }
    return false;
  }

  bool operator==(const DrawObjectKey &k) const
  {
    if (hash_value != k.hash_value) {
      return false;
    }
    if (object != k.object) {
      return false;
    }
    if (ob_data != k.ob_data) {
      return false;
    }
    if (flags != k.flags) {
      return false;
    }
    if (preview_base_geometry != k.preview_base_geometry) {
      return false;
    }
    if (preview_instance_index != k.preview_instance_index) {
      return false;
    }
    return true;
  }
};

void foreach_obref_in_scene(DRWContext &draw_ctx,
                            FunctionRef<bool(Object &)> should_draw_object_cb,
                            FunctionRef<void(ObjectRef &)> draw_object_cb);

}  // namespace blender::draw
