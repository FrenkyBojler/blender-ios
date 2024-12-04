/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_kdtree.h"
#include "BLI_math_geom.h"
#include "BLI_math_rotation.h"
#include "BLI_noise.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"

#include "DNA_pointcloud_types.h"

#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_sample.hh"
#include "BKE_pointcloud.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "GEO_randomize.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_distribute_points_on_faces_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  auto enable_random = [](bNode &node) {
    node.custom1 = GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_RANDOM;
  };
  auto enable_poisson = [](bNode &node) {
    node.custom1 = GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_POISSON;
  };

  b.add_input<decl::Geometry>("Mesh").supported_type(GeometryComponent::Type::Mesh);
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  auto &distance_min = b.add_input<decl::Float>("Distance Min")
                           .min(0.0f)
                           .subtype(PROP_DISTANCE)
                           .make_available(enable_poisson)
                           .available(false);
  auto &density_max = b.add_input<decl::Float>("Density Max")
                          .default_value(10.0f)
                          .min(0.0f)
                          .make_available(enable_poisson)
                          .available(false);
  auto &density = b.add_input<decl::Float>("Density")
                      .default_value(10.0f)
                      .min(0.0f)
                      .field_on_all()
                      .make_available(enable_random)
                      .available(false);
  auto &density_factor = b.add_input<decl::Float>("Density Factor")
                             .default_value(1.0f)
                             .min(0.0f)
                             .max(1.0f)
                             .subtype(PROP_FACTOR)
                             .field_on_all()
                             .make_available(enable_poisson)
                             .available(false);
  b.add_input<decl::Int>("Seed");

  b.add_output<decl::Geometry>("Points").propagate_all();
  b.add_output<decl::Vector>("Normal").field_on_all();
  b.add_output<decl::Rotation>("Rotation").field_on_all();

  const bNode *node = b.node_or_null();
  if (node != nullptr) {
    switch (node->custom1) {
      case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_POISSON:
        distance_min.available(true);
        density_max.available(true);
        density_factor.available(true);
        break;
      case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_RANDOM:
        density.available(true);
        break;
    }
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "distribute_method", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_layout_ex(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "use_legacy_normal", UI_ITEM_NONE, nullptr, ICON_NONE);
}

/**
 * Use an arbitrary choice of axes for a usable rotation attribute directly out of this node.
 */
static math::Quaternion normal_to_rotation(const float3 normal)
{
  float quat[4];
  vec_to_quat(quat, normal, OB_NEGZ, OB_POSY);
  return math::normalize(math::Quaternion(quat));
}

BLI_NOINLINE static KDTree_3d *build_kdtree(Span<float3> positions)
{
  KDTree_3d *kdtree = BLI_kdtree_3d_new(positions.size());

  int i_point = 0;
  for (const float3 position : positions) {
    BLI_kdtree_3d_insert(kdtree, i_point, position);
    i_point++;
  }

  BLI_kdtree_3d_balance(kdtree);
  return kdtree;
}

BLI_NOINLINE static void update_elimination_mask_for_close_points(
    Span<float3> positions, const float minimum_distance, MutableSpan<bool> elimination_mask)
{
  KDTree_3d *kdtree = build_kdtree(positions);
  BLI_SCOPED_DEFER([&]() { BLI_kdtree_3d_free(kdtree); });

  for (const int i : positions.index_range()) {
    if (!elimination_mask[i]) {
      continue;
    }

    struct CallbackData {
      int index;
      MutableSpan<bool> elimination_mask;
    } callback_data = {i, elimination_mask};

    BLI_kdtree_3d_range_search_cb(
        kdtree,
        positions[i],
        minimum_distance,
        [](void *user_data, int index, const float * /*co*/, float /*dist_sq*/) {
          CallbackData &callback_data = *static_cast<CallbackData *>(user_data);
          if (index != callback_data.index) {
            callback_data.elimination_mask[index] = false;
          }
          return true;
        },
        &callback_data);
  }
}

BLI_NOINLINE static void update_elimination_mask_based_on_density_factors(
    const Mesh &mesh,
    const VArray<float> &density_factors,
    const GroupedSpan<float3> tri_bary_coords,
    const MutableSpan<bool> elimination_mask)
{
  BLI_assert(tri_bary_coords.data.size() == elimination_mask.size());
  const Span<int3> corner_tris = mesh.corner_tris();

  if (density_factors.is_single()) {
    const float density_factor = std::max(0.0f, density_factors.get_internal_single());
    threading::parallel_for(tri_bary_coords.data.index_range(), 2048, [&](const IndexRange range) {
      for (const int i : range) {
        const float hash = noise::hash_float_to_float(tri_bary_coords.data[i]);
        elimination_mask[i] &= hash <= density_factor;
      }
    });
    return;
  }

  VArraySpan<float> density_factors_span(density_factors);
  threading::parallel_for(tri_bary_coords.index_range(), 2048, [&](const IndexRange range) {
    for (const int tri_i : range) {
      const int3 &tri = corner_tris[tri_i];
      const float a = std::max(0.0f, density_factors_span[tri[0]]);
      const float b = std::max(0.0f, density_factors_span[tri[1]]);
      const float c = std::max(0.0f, density_factors_span[tri[2]]);

      for (const int i : tri_bary_coords.offsets[tri_i]) {
        if (!elimination_mask[i]) {
          continue;
        }

        const float3 bary_coord = tri_bary_coords.data[i];
        const float probability = bke::attribute_math::mix3<float>(bary_coord, a, b, c);
        const float hash = noise::hash_float_to_float(bary_coord);
        if (hash > probability) {
          elimination_mask[i] = false;
        }
      }
    }
  });
}

static void interpolate_vert_attribute(const Mesh &mesh,
                                       const GroupedSpan<float3> tri_bary_coords,
                                       const GVArray &src,
                                       GMutableSpan dst)
{
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  bke::attribute_math::convert_to_static_type(dst.type(), [&](auto dummy) {
    using T = decltype(dummy);
    const VArraySpan<T> src_typed(src.typed<T>());
    MutableSpan<T> dst_typed = dst.typed<T>();

    threading::parallel_for(corner_tris.index_range(), 2048, [&](const IndexRange range) {
      for (const int64_t tri_i : range) {
        const int3 tri_verts = corner_tris[tri_i];
        const T &a = src_typed[corner_verts[tri_verts[0]]];
        const T &b = src_typed[corner_verts[tri_verts[1]]];
        const T &c = src_typed[corner_verts[tri_verts[2]]];

        const Span<float3> bary_coords = tri_bary_coords[tri_i];
        MutableSpan<T> dst = dst_typed.slice(tri_bary_coords.offsets[tri_i]);

        std::transform(
            bary_coords.begin(), bary_coords.end(), dst.begin(), [&](const float3 &bary_coord) {
              return bke::attribute_math::mix3(bary_coord, a, b, c);
            });
      }
    });
  });
}

template<typename T>
inline void gather_to_groups(const OffsetIndices<int> dst_offsets,
                             const Span<int> indices,
                             const Span<T> src,
                             MutableSpan<T> dst)
{
  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int64_t i : range) {
      dst.slice(dst_offsets[i]).fill(src[indices[i]]);
    }
  });
}

static void interpolate_face_attribute(const Mesh &mesh,
                                       const GroupedSpan<float3> tri_bary_coords,
                                       const GVArray &src,
                                       GMutableSpan dst)
{
  const Span<int> face_index = mesh.corner_tri_faces();
  const Span<int3> corner_tris = mesh.corner_tris();

  bke::attribute_math::convert_to_static_type(dst.type(), [&](auto dummy) {
    using T = decltype(dummy);
    const VArraySpan<T> src_typed(src.typed<T>());
    MutableSpan<T> dst_typed = dst.typed<T>();
    gather_to_groups(tri_bary_coords.offsets, mesh.corner_tri_faces(), src_typed, dst_typed);
  });
}

static void interpolate_corner_attribute(const Mesh &mesh,
                                         const GroupedSpan<float3> tri_bary_coords,
                                         const GVArray &src,
                                         GMutableSpan dst)
{
  const Span<int3> corner_tris = mesh.corner_tris();

  bke::attribute_math::convert_to_static_type(dst.type(), [&](auto dummy) {
    using T = decltype(dummy);
    const VArraySpan<T> src_typed(src.typed<T>());
    MutableSpan<T> dst_typed = dst.typed<T>();
    threading::parallel_for(corner_tris.index_range(), 2048, [&](const IndexRange range) {
      for (const int64_t tri_i : range) {
        const int3 face_tri = corner_tris[tri_i];
        const T &a = src_typed[face_tri[0]];
        const T &b = src_typed[face_tri[1]];
        const T &c = src_typed[face_tri[2]];

        const Span<float3> bary_coords = tri_bary_coords[tri_i];
        MutableSpan<T> dst = dst_typed.slice(tri_bary_coords.offsets[tri_i]);

        std::transform(
            bary_coords.begin(), bary_coords.end(), dst.begin(), [&](const float3 &bary_coord) {
              return bke::attribute_math::mix3(bary_coord, a, b, c);
            });
      }
    });
  });
}

BLI_NOINLINE static void propagate_existing_attributes(
    const Mesh &mesh,
    const GroupedSpan<float3> bary_coords,
    const bke::AttributeAccessor src_attributes,
    const Map<StringRef, AttributeDomainAndType> &attributes,
    bke::MutableAttributeAccessor dst_attributes)
{
  for (MapItem<StringRef, AttributeDomainAndType> entry : attributes.items()) {
    const StringRef attribute_id = entry.key;

    const eCustomDataType output_data_type = entry.value.data_type;
    if (output_data_type == CD_PROP_STRING) {
      continue;
    }

    GAttributeReader src = src_attributes.lookup(attribute_id);
    if (!src) {
      continue;
    }
    if (src.domain == AttrDomain::Edge) {
      continue;
    }

    GSpanAttributeWriter dst = dst_attributes.lookup_or_add_for_write_only_span(
        attribute_id, AttrDomain::Point, output_data_type);
    if (!dst) {
      continue;
    }

    switch (src.domain) {
      case AttrDomain::Point: {
        interpolate_vert_attribute(mesh, bary_coords, src.varray, dst.span);
        break;
      }
      case AttrDomain::Face: {
        interpolate_face_attribute(mesh, bary_coords, src.varray, dst.span);
        break;
      }
      case AttrDomain::Corner: {
        interpolate_corner_attribute(mesh, bary_coords, src.varray, dst.span);
        break;
      }
      default: {
        break;
      }
    }

    dst.finish();
  }
}

namespace {
struct AttributeOutputs {
  std::optional<std::string> normal_id;
  std::optional<std::string> rotation_id;
};
}  // namespace

static void compute_rotation_output(const Span<float3> normals,
                                    MutableSpan<math::Quaternion> r_rotations)
{
  threading::parallel_for(normals.index_range(), 512, [&](const IndexRange range) {
    for (const int i : range) {
      r_rotations[i] = normal_to_rotation(normals[i]);
    }
  });
}

static void normalize(MutableSpan<float3> data)
{
  threading::parallel_for(data.index_range(), 4096, [&](const IndexRange range) {
    MutableSpan<float3> local_data = data.slice(range);
    std::transform(local_data.begin(),
                   local_data.end(),
                   local_data.begin(),
                   [](const float3 &vector) { return math::normalize(vector); });
  });
}

static void compute_normal_outputs(const Mesh &mesh,
                                   const GroupedSpan<float3> tri_bary_coords,
                                   MutableSpan<float3> r_normals)
{
  switch (mesh.normals_domain()) {
    case bke::MeshNormalDomain::Point: {
      const Span<float3> vert_normals = mesh.vert_normals();
      interpolate_vert_attribute(
          mesh, tri_bary_coords, VArray<float3>::ForSpan(vert_normals), r_normals);
      normalize(r_normals);
      break;
    }
    case bke::MeshNormalDomain::Face: {
      const Span<float3> face_normals = mesh.face_normals();
      interpolate_face_attribute(
          mesh, tri_bary_coords, VArray<float3>::ForSpan(face_normals), r_normals);
      break;
    }
    case bke::MeshNormalDomain::Corner: {
      const Span<float3> corner_normals = mesh.corner_normals();
      interpolate_corner_attribute(
          mesh, tri_bary_coords, VArray<float3>::ForSpan(corner_normals), r_normals);
      normalize(r_normals);
      break;
    }
  }
}

static void compute_legacy_normal_outputs(const Mesh &mesh,
                                          const OffsetIndices<int> tris,
                                          MutableSpan<float3> r_normals)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  threading::parallel_for(tris.index_range(), 2048, [&](const IndexRange range) {
    for (const int tri_i : range) {
      const int3 tri = corner_tris[tri_i];

      const float3 a = positions[corner_verts[tri[0]]];
      const float3 b = positions[corner_verts[tri[1]]];
      const float3 c = positions[corner_verts[tri[2]]];

      float3 normal;
      normal_tri_v3(normal, a, b, c);

      r_normals.slice(tris[tri_i]).fill(normal);
    }
  });
}

static void compute_hashes(const GroupedSpan<float3> tri_bary_coords, MutableSpan<int> hashs)
{
  threading::parallel_for(tri_bary_coords.index_range(), 1024, [&](const IndexRange range) {
    for (const int tri_i : range) {
      const Span<float3> coords = tri_bary_coords[tri_i];
      MutableSpan<int> local_hashs = hashs.slice(tri_bary_coords.offsets[tri_i]);

      std::transform(
          coords.begin(), coords.end(), local_hashs.begin(), [tri_i](const float3 &coord) {
            return noise::hash(noise::hash_float(coord), tri_i);
          });
    }
  });
}

static void compute_attribute_outputs(const Mesh &mesh,
                                      const GroupedSpan<float3> tri_bary_coords,
                                      const AttributeOutputs &attribute_outputs,
                                      const bool use_legacy_normal,
                                      MutableAttributeAccessor dst_attributes)
{
  SpanAttributeWriter<int> ids = dst_attributes.lookup_or_add_for_write_only_span<int>(
      "id", AttrDomain::Point);
  compute_hashes(tri_bary_coords, ids.span);
  ids.finish();

  if (attribute_outputs.normal_id) {
    SpanAttributeWriter<float3> normals = dst_attributes.lookup_or_add_for_write_only_span<float3>(
        *attribute_outputs.normal_id, AttrDomain::Point);
    if (use_legacy_normal) {
      compute_legacy_normal_outputs(mesh, tri_bary_coords.offsets, normals.span);
    }
    else {
      compute_normal_outputs(mesh, tri_bary_coords, normals.span);
    }
    normals.finish();
  }

  if (attribute_outputs.rotation_id) {
    BLI_assert(attribute_outputs.normal_id);
    const VArraySpan<float3> normals = *dst_attributes.lookup<float3>(*attribute_outputs.normal_id,
                                                                      AttrDomain::Point);
    SpanAttributeWriter<math::Quaternion> rotations =
        dst_attributes.lookup_or_add_for_write_only_span<math::Quaternion>(
            *attribute_outputs.rotation_id, AttrDomain::Point);
    compute_rotation_output(normals, rotations.span);
    rotations.finish();
  }
}

static Array<float> calc_full_density_factors_with_selection(const Mesh &mesh,
                                                             const Field<float> &density_field,
                                                             const Field<bool> &selection_field)
{
  const AttrDomain domain = AttrDomain::Corner;
  const int domain_size = mesh.attributes().domain_size(domain);
  Array<float> densities(domain_size, 0.0f);

  const bke::MeshFieldContext field_context{mesh, domain};
  fn::FieldEvaluator evaluator{field_context, domain_size};
  evaluator.set_selection(selection_field);
  evaluator.add_with_destination(density_field, densities.as_mutable_span());
  evaluator.evaluate();
  return densities;
}

static void tris_points_count_for_densities(const Mesh &mesh,
                                            const VArray<float> &densities,
                                            const int seed,
                                            MutableSpan<int> r_count)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<int3> corner_tris = mesh.corner_tris();

  if (densities.is_single()) {
    const float density = std::max(0.0f, densities.get_internal_single());
    threading::parallel_for(corner_tris.index_range(), 2048, [&](const IndexRange range) {
      for (const int64_t tri_i : range) {
        const int3 tri = corner_tris[tri_i];
        const int v0_loop = tri[0];
        const int v1_loop = tri[1];
        const int v2_loop = tri[2];
        const float3 &v0_pos = positions[corner_verts[v0_loop]];
        const float3 &v1_pos = positions[corner_verts[v1_loop]];
        const float3 &v2_pos = positions[corner_verts[v2_loop]];

        const float area = area_tri_v3(v0_pos, v1_pos, v2_pos);

        const int corner_tri_seed = noise::hash(tri_i, seed);
        RandomNumberGenerator corner_tri_rng(corner_tri_seed);

        const int point_amount = corner_tri_rng.round_probabilistic(area * density);
        r_count[tri_i] = point_amount;
      }
    });
    return;
  }

  VArraySpan<float> densities_span(densities);
  threading::parallel_for(corner_tris.index_range(), 2048, [&](const IndexRange range) {
    for (const int64_t tri_i : range) {
      const int3 tri = corner_tris[tri_i];
      const int v0_loop = tri[0];
      const int v1_loop = tri[1];
      const int v2_loop = tri[2];
      const float3 &v0_pos = positions[corner_verts[v0_loop]];
      const float3 &v1_pos = positions[corner_verts[v1_loop]];
      const float3 &v2_pos = positions[corner_verts[v2_loop]];

      const float v0_density_factor = std::max(0.0f, densities_span[v0_loop]);
      const float v1_density_factor = std::max(0.0f, densities_span[v1_loop]);
      const float v2_density_factor = std::max(0.0f, densities_span[v2_loop]);
      const float tri_mean_density = (v0_density_factor + v1_density_factor + v2_density_factor) /
                                     3.0f;

      const float area = area_tri_v3(v0_pos, v1_pos, v2_pos);

      const int corner_tri_seed = noise::hash(tri_i, seed);
      RandomNumberGenerator corner_tri_rng(corner_tri_seed);

      const int point_amount = corner_tri_rng.round_probabilistic(area * tri_mean_density);
      r_count[tri_i] = point_amount;
    }
  });
}

static void random_barycentric_points(const int seed,
                                      const OffsetIndices<int> groups,
                                      MutableSpan<float3> r_bary_coords)
{
  threading::parallel_for(
      groups.index_range(),
      2048,
      [&](const IndexRange range) {
        for (const int64_t group_i : range) {
          RandomNumberGenerator corner_tri_rng(noise::hash(group_i, seed));
          /* Extra step to keep legacy result. */
          corner_tri_rng.skip(1);

          MutableSpan<float3> bary_coords = r_bary_coords.slice(groups[group_i]);
          std::generate(bary_coords.begin(), bary_coords.end(), [&]() {
            return corner_tri_rng.get_barycentric_coordinates();
          });
        }
      },
      threading::accumulated_task_sizes(
          [&](const IndexRange range) { return groups[range].size(); }));
}

static void reduce_offsets_by_mask_selections(const OffsetIndices<int> offsets,
                                              const IndexMask &mask,
                                              MutableSpan<int> r_offsets)
{
  BLI_assert(offsets.data().size() == r_offsets.size());
  threading::parallel_for(offsets.index_range(), 1024, [&](const IndexRange range) {
    const IndexMask local_mask = mask.slice_content(offsets[range]);
    if (local_mask.is_empty()) {
      r_offsets.slice(range).fill(0);
      return;
    }
    for (const int i : range) {
      r_offsets[i] = local_mask.slice_content(offsets[i]).size();
    }
  });
  offset_indices::accumulate_counts_to_offsets(r_offsets);
}

static Field<float> switch_with_default(Field<float> values, Field<bool> selection)
{
  const static auto switch_fn = mf::build::SI2_SO<float, bool, float>(
      "Switch Selection",
      [](const float value, const bool selection) { return selection ? value : 0.0f; },
      mf::build::exec_presets::AllSpanOrSingle());

  return Field<float>(
      FieldOperation::Create(switch_fn, {std::move(values), std::move(selection)}));
}

static void point_distribution_calculate(GeometrySet &geometry_set,
                                         const GeometryNodeDistributePointsOnFacesMode method,
                                         const int seed,
                                         const AttributeOutputs &attribute_outputs,
                                         Field<bool> selection_field,
                                         GeoNodeExecParams &params)
{
  if (!geometry_set.has_mesh()) {
    return;
  }

  const Mesh &mesh = *geometry_set.get_mesh();

  const bke::MeshFieldContext face_corner_context(mesh, AttrDomain::Corner);
  fn::FieldEvaluator evaluator(face_corner_context, mesh.corners_num);

  Array<int> offsets;
  Array<float3> bary_coords;

  switch (method) {
    case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_RANDOM: {
      evaluator.add(switch_with_default(params.extract_input<Field<float>>("Density"),
                                        std::move(selection_field)));
      evaluator.evaluate();
      const VArray<float> densities = evaluator.get_evaluated<float>(0);
      offsets.reinitialize(mesh.corner_tris().size() + 1);
      tris_points_count_for_densities(mesh, densities, seed, offsets);
      const OffsetIndices<int> points_groups = offset_indices::accumulate_counts_to_offsets(
          offsets.as_mutable_span());
      bary_coords.reinitialize(points_groups.total_size());
      random_barycentric_points(seed, points_groups, bary_coords);
      break;
    }
    case GEO_NODE_POINT_DISTRIBUTE_POINTS_ON_FACES_POISSON: {
      const float density_max = params.get_input<float>("Density Max");
      offsets.reinitialize(mesh.corner_tris().size() + 1);
      tris_points_count_for_densities(
          mesh, VArray<float>::ForSingle(density_max, mesh.corners_num), seed, offsets);
      OffsetIndices<int> points_groups = offset_indices::accumulate_counts_to_offsets(
          offsets.as_mutable_span());
      bary_coords.reinitialize(points_groups.total_size());
      random_barycentric_points(seed, offsets.as_span(), bary_coords);

      const GroupedSpan<float3> tris_bary_coords(offsets.as_span(), bary_coords);

      evaluator.add(switch_with_default(params.extract_input<Field<float>>("Density Factor"),
                                        std::move(selection_field)));
      evaluator.evaluate();
      const VArray<float> density_factors = evaluator.get_evaluated<float>(0);

      /* Delete all too-near points. */
      const float minimum_distance = params.get_input<float>("Distance Min");
      const std::optional<float> density_factor = density_factors.get_if_single();
      if (minimum_distance <= 0.0f && density_factor.has_value() && *density_factor == 1.0f) {
        break;
      }

      Array<bool> elimination_mask(bary_coords.size(), true);

      /* Temporary positions. Result point positions will be interpolated just like any other
       * attribute. */
      Array<float3> positions(bary_coords.size());
      interpolate_vert_attribute(mesh,
                                 tris_bary_coords,
                                 VArray<float3>::ForSpan(mesh.vert_positions()),
                                 positions.as_mutable_span());
      update_elimination_mask_for_close_points(positions, minimum_distance, elimination_mask);
      update_elimination_mask_based_on_density_factors(
          mesh, density_factors, tris_bary_coords, elimination_mask.as_mutable_span());

      IndexMaskMemory memory;
      const IndexMask mask = IndexMask::from_bools(elimination_mask.as_span(), memory);

      Array<int> reduced_offsets(offsets.size());
      Array<float3> reduced_bary_coords(mask.size());

      reduce_offsets_by_mask_selections(offsets.as_span(), mask, reduced_offsets);
      array_utils::gather(bary_coords.as_span(), mask, reduced_bary_coords.as_mutable_span());

      offsets = std::move(reduced_offsets);
      bary_coords = std::move(reduced_bary_coords);
      break;
    }
  }

  const GroupedSpan<float3> tri_bary_coords(offsets.as_span(), bary_coords);

  PointCloud *pointcloud = BKE_pointcloud_new_nomain(bary_coords.size());
  bke::MutableAttributeAccessor point_attributes = pointcloud->attributes_for_write();
  bke::SpanAttributeWriter<float> point_radii =
      point_attributes.lookup_or_add_for_write_only_span<float>("radius", AttrDomain::Point);
  point_radii.span.fill(0.05f);
  point_radii.finish();

  geometry_set.replace_pointcloud(pointcloud);

  Map<StringRef, AttributeDomainAndType> attributes;
  geometry_set.gather_attributes_for_propagation({GeometryComponent::Type::Mesh},
                                                 GeometryComponent::Type::PointCloud,
                                                 false,
                                                 params.get_attribute_filter("Points"),
                                                 attributes);

  propagate_existing_attributes(
      mesh, tri_bary_coords, mesh.attributes(), attributes, pointcloud->attributes_for_write());

  const bool use_legacy_normal = params.node().custom2 != 0;
  compute_attribute_outputs(mesh,
                            tri_bary_coords,
                            attribute_outputs,
                            use_legacy_normal,
                            pointcloud->attributes_for_write());

  geometry::debug_randomize_point_order(pointcloud);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Mesh");

  const GeometryNodeDistributePointsOnFacesMode method = GeometryNodeDistributePointsOnFacesMode(
      params.node().custom1);

  const int seed = params.get_input<int>("Seed") * 5383843;
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");

  AttributeOutputs attribute_outputs;
  attribute_outputs.rotation_id = params.get_output_anonymous_attribute_id_if_needed("Rotation");
  attribute_outputs.normal_id = params.get_output_anonymous_attribute_id_if_needed(
      "Normal", bool(attribute_outputs.rotation_id));

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    point_distribution_calculate(
        geometry_set, method, seed, attribute_outputs, selection_field, params);
    /* Keep instances because the original geometry set may contain instances that are processed as
     * well. */
    geometry_set.keep_only_during_modify({GeometryComponent::Type::PointCloud});
  });

  params.set_output("Points", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype,
                     GEO_NODE_DISTRIBUTE_POINTS_ON_FACES,
                     "Distribute Points on Faces",
                     NODE_CLASS_GEOMETRY);
  blender::bke::node_type_size(&ntype, 170, 100, 320);
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.draw_buttons_ex = node_layout_ex;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_distribute_points_on_faces_cc
