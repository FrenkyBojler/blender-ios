/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "MEM_guardedalloc.h"  // IWYU pragma: export

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"  // IWYU pragma: export
#include "BKE_node_socket_value.hh"  // IWYU pragma: export

#include "NOD_geometry_exec.hh"                 // IWYU pragma: export
#include "NOD_register.hh"                      // IWYU pragma: export
#include "NOD_socket_declarations.hh"           // IWYU pragma: export
#include "NOD_socket_declarations_geometry.hh"  // IWYU pragma: export

#include "node_util.hh"  // IWYU pragma: export

#include "BKE_attribute.hh"
#include "BLI_task.hh"

namespace blender {
namespace bke {
struct BVHTreeFromMesh;
}
namespace nodes {
class GatherAddNodeSearchParams;
class GatherLinkSearchOpParams;
}  // namespace nodes
}  // namespace blender

void geo_node_type_base(blender::bke::bNodeType *ntype,
                        std::string idname,
                        std::optional<int16_t> legacy_type = std::nullopt);
bool geo_node_poll_default(const blender::bke::bNodeType *ntype,
                           const bNodeTree *ntree,
                           const char **r_disabled_hint);

/* Same as geo_node_type_base but allows node use in the compositor by allowing compositor node
 * trees in the poll function. */
void geo_cmp_node_type_base(blender::bke::bNodeType *ntype,
                            std::string idname,
                            std::optional<int16_t> legacy_type = std::nullopt);

namespace blender::nodes {

bool check_tool_context_and_error(GeoNodeExecParams &params);
void search_link_ops_for_tool_node(GatherLinkSearchOpParams &params);
void search_link_ops_for_volume_grid_node(GatherLinkSearchOpParams &params);

void get_closest_in_bvhtree(bke::BVHTreeFromMesh &tree_data,
                            const VArray<float3> &positions,
                            const IndexMask &mask,
                            MutableSpan<int> r_indices,
                            MutableSpan<float> r_distances_sq,
                            MutableSpan<float3> r_positions);

void mix_baked_data_item(eNodeSocketDatatype socket_type,
                         SocketValueVariant &prev,
                         const SocketValueVariant &next,
                         const float factor);

namespace enums {

const EnumPropertyItem *attribute_type_type_with_socket_fn(bContext * /*C*/,
                                                           PointerRNA * /*ptr*/,
                                                           PropertyRNA * /*prop*/,
                                                           bool *r_free);

bool generic_attribute_type_supported(const EnumPropertyItem &item);

}  // namespace enums

const EnumPropertyItem *grid_data_type_socket_items_filter_fn(bContext *C,
                                                              PointerRNA *ptr,
                                                              PropertyRNA *prop,
                                                              bool *r_free);
const EnumPropertyItem *grid_socket_type_items_filter_fn(bContext *C,
                                                         PointerRNA *ptr,
                                                         PropertyRNA *prop,
                                                         bool *r_free);

void node_geo_exec_with_missing_openvdb(GeoNodeExecParams &params);

void draw_data_blocks(const bContext *C, uiLayout *layout, PointerRNA &bake_rna);


class Grid3DFieldContext : public FieldContext {
 private:
  int3 resolution_;
  float3 bounds_min_;
  float3 bounds_max_;

  static float grid_map_coordinate(const float x,
                                   const float in_min,
                                   const float in_max,
                                   const float out_min,
                                   const float out_max)
  {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
  }

 public:
  Grid3DFieldContext(const int3 resolution, const float3 bounds_min, const float3 bounds_max)
      : resolution_(resolution), bounds_min_(bounds_min), bounds_max_(bounds_max)
  {
  }

  int64_t voxel_num() const
  {
    return int64_t(resolution_.x) * int64_t(resolution_.y) * int64_t(resolution_.z);
  }

  GVArray get_varray_for_input(const FieldInput &field_input,
                               const IndexMask & /*mask*/,
                               ResourceScope & /*scope*/) const override
  {
    const bke::AttributeFieldInput *attribute_field_input =
        dynamic_cast<const bke::AttributeFieldInput *>(&field_input);
    if (attribute_field_input == nullptr) {
      return {};
    }
    if (attribute_field_input->attribute_name() != "position") {
      return {};
    }

    Array<float3> positions(this->voxel_num());

    threading::parallel_for(IndexRange(resolution_.x), 1, [&](const IndexRange x_range) {
      /* Start indexing at current X slice. */
      int64_t index = x_range.start() * resolution_.y * resolution_.z;
      for (const int64_t x_i : x_range) {
        const float x = grid_map_coordinate(
            x_i, 0.0f, resolution_.x - 1, bounds_min_.x, bounds_max_.x);
        for (const int64_t y_i : IndexRange(resolution_.y)) {
          const float y = grid_map_coordinate(
              y_i, 0.0f, resolution_.y - 1, bounds_min_.y, bounds_max_.y);
          for (const int64_t z_i : IndexRange(resolution_.z)) {
            const float z = grid_map_coordinate(
                z_i, 0.0f, resolution_.z - 1, bounds_min_.z, bounds_max_.z);
            positions[index] = float3(x, y, z);
            index++;
          }
        }
      }
    });
    return VArray<float3>::from_container(std::move(positions));
  }
};

}  // namespace blender::nodes
