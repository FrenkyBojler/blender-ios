/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_ghash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_sys_types.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include <array>
#include <functional>

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
  IsActive = 1 << 0,
  IsNegativeScale = 1 << 1,
  RecalcTransform = 1 << 2,
  RecalcGeometry = 1 << 3,
  RecalcShading = 1 << 4,
  ParentInEditPaintMode = 1 << 5,
};
ENUM_OPERATORS(DrawObjectFlags, DrawObjectFlags::ParentInEditPaintMode);

struct DrawObjectKey {
  uint64_t hash_value;

  Object *object;
  ID *ob_data;
  LightLinking *light_linking;
  const blender::bke::GeometrySet *preview_base_geometry;
  int preview_instance_index;
  short base_flags;
  DrawObjectFlags flags;
  char draw_type;

  DrawObjectKey(Object *object,
                ID *ob_data,
                short base_flags,
                DrawObjectFlags flags,
                char draw_type,
                LightLinking *light_linking,
                const blender::bke::GeometrySet *preview_base_geometry,
                int preview_instance_index)
      : object(object),
        ob_data(ob_data),
        light_linking(light_linking),
        preview_base_geometry(preview_base_geometry),
        preview_instance_index(preview_instance_index),
        base_flags(base_flags),
        flags(flags),
        draw_type(draw_type)
  {
    hash_value = BLI_ghashutil_ptrhash(object);
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_ptrhash(ob_data));
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_ptrhash(light_linking));
    hash_value = BLI_ghashutil_combine_hash(hash_value,
                                            BLI_ghashutil_ptrhash(preview_base_geometry));
    hash_value = BLI_ghashutil_combine_hash(hash_value,
                                            BLI_ghashutil_inthash(preview_instance_index));
    /* TODO: Single hash for these ? */
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_uinthash(base_flags));
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_uinthash(uint8_t(flags)));
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_uinthash(draw_type));
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
    if (base_flags != k.base_flags) {
      return base_flags < k.base_flags;
    }
    if (flags != k.flags) {
      return flags < k.flags;
    }
    if (draw_type != k.draw_type) {
      return draw_type < k.draw_type;
    }
    if (light_linking != k.light_linking) {
      return light_linking < k.light_linking;
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
    if (base_flags != k.base_flags) {
      return false;
    }
    if (flags != k.flags) {
      return false;
    }
    if (draw_type != k.draw_type) {
      return false;
    }
    if (light_linking != k.light_linking) {
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

struct DrawInstances {
  Vector<float4x4, 0> object_to_world;
  Vector<float4x4, 0> particles_object_to_world;
  /* Persistent identifier for a dupli object, for inter-frame matching of
   * objects with motion blur, or inter-update matching for syncing. */
  Vector<std::array<int, /*MAX_DUPLI_RECUR*/ 8>, 0> persistent_id;
  /* Random ID for shading */
  Vector<unsigned int, 0> random_id;
  Vector<int> select_id;
};

void foreach_obref_in_scene(DRWContext &draw_ctx, std::function<void(ObjectRef &)> callback);

}  // namespace blender::draw
