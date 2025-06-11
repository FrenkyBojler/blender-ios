/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_ghash.h"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_sys_types.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

struct Object;
struct LightLinking;
namespace blender::bke {
struct GeometrySet;
}

namespace blender::draw {

enum class DrawObjectFlags : uint8_t {
  IsActive = 1 << 0,
  IsDupli = 1 << 1,
  IsNegaticeScale = 1 << 2,
  RecalcTransform = 1 << 3,
  RecalcGeometry = 1 << 4,
  RecalcShading = 1 << 5,
  ParentInEditPaintMode = 1 << 6,
};
ENUM_OPERATORS(DrawObjectFlags, DrawObjectFlags::ParentInEditPaintMode);

struct DrawObjectKey {
  Object *object = nullptr;
  LightLinking *light_linking = nullptr;
  const blender::bke::GeometrySet *preview_base_geometry = nullptr;
  int preview_instance_index = -1;
  DrawObjectFlags flags = DrawObjectFlags(0);
  uint64_t hash_value = 0;

  DrawObjectKey(Object *object,
                LightLinking *light_linking,
                blender::bke::GeometrySet *preview_base_geometry,
                int preview_instance_index,
                DrawObjectFlags flags)
      : object(object),
        light_linking(light_linking),
        preview_base_geometry(preview_base_geometry),
        preview_instance_index(preview_instance_index),
        flags(flags)
  {
    hash_value = BLI_ghashutil_ptrhash(object);
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_ptrhash(light_linking));
    hash_value = BLI_ghashutil_combine_hash(hash_value,
                                            BLI_ghashutil_ptrhash(preview_base_geometry));
    hash_value = BLI_ghashutil_combine_hash(hash_value,
                                            BLI_ghashutil_inthash(preview_instance_index));
    hash_value = BLI_ghashutil_combine_hash(hash_value, BLI_ghashutil_uinthash(uint8_t(flags)));
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
    if (flags != k.flags) {
      return flags < k.flags;
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
    if (flags != k.flags) {
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
};

class DrawScene {
  Map<DrawObjectKey, DrawInstances> instances;
};

}  // namespace blender::draw
