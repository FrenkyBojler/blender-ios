/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"
#include "BKE_editmesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_wrapper.hh"
#include "BKE_modifier.hh"

#include "BLI_bounds.hh"
#include "BLI_math_matrix.hh"
#include "BLI_task.hh"

#include "GEO_reverse_uv_sampler.hh"

#include "DEG_depsgraph_query.hh"

#include "node_geometry_util.hh"

#include <fmt/format.h>
#include <mutex>

namespace blender::nodes::node_geo_deform_curves_on_surface_cc {

using bke::CurvesGeometry;
using bke::attribute_math::mix3;
using geometry::ReverseUVSampler;

NODE_STORAGE_FUNCS(NodeGeometryDeformCurvesOnSurface)

struct NodeGeometryDeformCurvesOnSurfaceCacheEntry {
  /* BVH-only cache: keep the ReverseUVSampler instances built from
   * (uv_map, corner_tris, uv_bounds) between frames so the BVH-build cost is
   * paid once per topology / curve-region change, not every frame.
   * sample_many still runs each frame against current curve attach UVs, so
   * per-curve UV value changes (within the same uv_bounds) are picked up
   * correctly.  Each call passes fresh uv_map / corner_tris Spans into
   * sample_many — the cached samplers' internal Spans would otherwise dangle
   * after the depsgraph re-allocates the mesh's runtime caches. */
  std::unique_ptr<ReverseUVSampler> sampler_orig;
  std::unique_ptr<ReverseUVSampler> sampler_eval;  /* null when same_mesh */
  uint2 tri_counts = {0, 0};
  uint2 uv_map_counts = {0, 0};
  Bounds<float2> uv_bounds = {{0, 0}, {0, 0}};
  std::mutex mutex;
};

struct NodeGeometryDeformCurvesOnSurfaceCache
    : public Map<uint2, NodeGeometryDeformCurvesOnSurfaceCacheEntry *> {
  ~NodeGeometryDeformCurvesOnSurfaceCache()
  {
    this->foreach_item(
        []([[maybe_unused]] const uint2 & /*key*/,
           NodeGeometryDeformCurvesOnSurfaceCacheEntry *entry) { MEM_delete(entry); });
  }
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Curves"_ustr)
      .supported_type(GeometryComponent::Type::Curve)
      .description("Curves to deform");
  b.add_output<decl::Geometry>("Curves"_ustr).propagate_all().align_with_previous();
}

static void deform_curves(const CurvesGeometry &curves,
                          const Mesh &surface_mesh_old,
                          const Mesh &surface_mesh_new,
                          const Span<ReverseUVSampler::Result> surface_samples_old,
                          const Span<ReverseUVSampler::Result> surface_samples_new,
                          const Span<float3> corner_normals_old,
                          const Span<float3> corner_normals_new,
                          const Span<float3> rest_positions,
                          const float4x4 &surface_to_curves,
                          const Span<float3> r_orig_positions,
                          MutableSpan<float3> r_positions,
                          MutableSpan<float3x3> r_rotations,
                          std::atomic<int> &r_invalid_uv_count)
{
  const float4x4 curves_to_surface = math::invert(surface_to_curves);

  const Span<float3> surface_positions_old = surface_mesh_old.vert_positions();
  const Span<int> surface_corner_verts_old = surface_mesh_old.corner_verts();
  const Span<int3> surface_corner_tris_old = surface_mesh_old.corner_tris();

  const Span<float3> surface_positions_new = surface_mesh_new.vert_positions();
  const Span<int> surface_corner_verts_new = surface_mesh_new.corner_verts();
  const Span<int3> surface_corner_tris_new = surface_mesh_new.corner_tris();

  const OffsetIndices points_by_curve = curves.points_by_curve();

  threading::parallel_for(curves.curves_range(), 256, [&](const IndexRange range) {
    /* Track the root position of the last successfully deformed curve in this chunk so that
     * invalid curves can be collapsed near a valid neighbour rather than left in mid-air. */
    int last_known_good_point = -1;

    for (const int curve_i : range) {
      const ReverseUVSampler::Result &surface_sample_old = surface_samples_old[curve_i];
      const ReverseUVSampler::Result &surface_sample_new = surface_samples_new[curve_i];

      const bool invalid = (surface_sample_old.type != ReverseUVSampler::ResultType::Ok) ||
                           (surface_sample_new.type != ReverseUVSampler::ResultType::Ok);

      const IndexRange points = points_by_curve[curve_i];

      if (invalid) {
        r_invalid_uv_count++;
        /* Collapse all points of the invalid curve to a single position so it becomes
         * invisible in strand/strip mode instead of hanging at its rest-pose location. */
        if (!points.is_empty()) {
          const float3 collapse_pos = (last_known_good_point >= 0) ?
                                          r_positions[last_known_good_point] :
                                          r_orig_positions[points[0]];
          for (const int point_i : points) {
            r_positions[point_i] = collapse_pos;
          }
        }
        continue;
      }

      if (!points.is_empty()) {
        last_known_good_point = points[0];
      }

      const int3 &tri_old = surface_corner_tris_old[surface_sample_old.tri_index];
      const int3 &tri_new = surface_corner_tris_new[surface_sample_new.tri_index];
      const float3 &bary_weights_old = surface_sample_old.bary_weights;
      const float3 &bary_weights_new = surface_sample_new.bary_weights;

      const int corner_0_old = tri_old[0];
      const int corner_1_old = tri_old[1];
      const int corner_2_old = tri_old[2];

      const int corner_0_new = tri_new[0];
      const int corner_1_new = tri_new[1];
      const int corner_2_new = tri_new[2];

      const int vert_0_old = surface_corner_verts_old[corner_0_old];
      const int vert_1_old = surface_corner_verts_old[corner_1_old];
      const int vert_2_old = surface_corner_verts_old[corner_2_old];

      const int vert_0_new = surface_corner_verts_new[corner_0_new];
      const int vert_1_new = surface_corner_verts_new[corner_1_new];
      const int vert_2_new = surface_corner_verts_new[corner_2_new];

      const float3 &normal_0_old = corner_normals_old[corner_0_old];
      const float3 &normal_1_old = corner_normals_old[corner_1_old];
      const float3 &normal_2_old = corner_normals_old[corner_2_old];
      const float3 normal_old = math::normalize(
          mix3(bary_weights_old, normal_0_old, normal_1_old, normal_2_old));

      const float3 &normal_0_new = corner_normals_new[corner_0_new];
      const float3 &normal_1_new = corner_normals_new[corner_1_new];
      const float3 &normal_2_new = corner_normals_new[corner_2_new];
      const float3 normal_new = math::normalize(
          mix3(bary_weights_new, normal_0_new, normal_1_new, normal_2_new));

      const float3 &pos_0_old = surface_positions_old[vert_0_old];
      const float3 &pos_1_old = surface_positions_old[vert_1_old];
      const float3 &pos_2_old = surface_positions_old[vert_2_old];
      const float3 pos_old = mix3(bary_weights_old, pos_0_old, pos_1_old, pos_2_old);

      const float3 &pos_0_new = surface_positions_new[vert_0_new];
      const float3 &pos_1_new = surface_positions_new[vert_1_new];
      const float3 &pos_2_new = surface_positions_new[vert_2_new];
      const float3 pos_new = mix3(bary_weights_new, pos_0_new, pos_1_new, pos_2_new);

      /* The translation is just the difference between the old and new position on the surface. */
      const float3 translation = pos_new - pos_old;

      const float3 &rest_pos_0 = rest_positions[vert_0_new];
      const float3 &rest_pos_1 = rest_positions[vert_1_new];

      /* The tangent reference direction is used to determine the rotation of the surface point
       * around its normal axis. It's important that the old and new tangent reference are computed
       * in a consistent way. If the surface has not been rotated, the old and new tangent
       * reference have to have the same direction. For that reason, the old tangent reference is
       * computed based on the rest position attribute instead of positions on the old mesh. This
       * way the old and new tangent reference use the same topology.
       *
       * TODO: Figure out if this can be smoothly interpolated across the surface as well.
       * Currently, this is a source of discontinuity in the deformation, because the vector
       * changes instantly from one triangle to the next. */
      const float3 tangent_reference_dir_old = rest_pos_1 - rest_pos_0;
      const float3 tangent_reference_dir_new = pos_1_new - pos_0_new;

      /* Compute first local tangent based on the (potentially smoothed) normal and the tangent
       * reference. */
      const float3 tangent_x_old = math::normalize(
          math::cross(normal_old, tangent_reference_dir_old));
      const float3 tangent_x_new = math::normalize(
          math::cross(normal_new, tangent_reference_dir_new));

      /* The second tangent defined by the normal and first tangent. */
      const float3 tangent_y_old = math::normalize(math::cross(normal_old, tangent_x_old));
      const float3 tangent_y_new = math::normalize(math::cross(normal_new, tangent_x_new));

      /* Construct rotation matrix that encodes the orientation of the old surface position. */
      float3x3 rotation_old(tangent_x_old, tangent_y_old, normal_old);

      /* Construct rotation matrix that encodes the orientation of the new surface position. */
      float3x3 rotation_new(tangent_x_new, tangent_y_new, normal_new);

      /* Can use transpose instead of inverse because the matrix is orthonormal. In the case of
       * zero-area triangles, the matrix would not be orthonormal, but in this case, none of this
       * works anyway. */
      const float3x3 rotation_old_inv = math::transpose(rotation_old);

      /* Compute a rotation matrix that rotates points from the old to the new surface
       * orientation. */
      const float3x3 rotation = rotation_new * rotation_old_inv;

      /* Construction transformation matrix for this surface position that includes rotation and
       * translation. */
      /* Subtract and add #pos_old, so that the rotation origin is the position on the surface. */
      float4x4 surface_transform = math::from_origin_transform<float4x4>(float4x4(rotation),
                                                                         pos_old);
      surface_transform.location() += translation;

      /* Change the basis of the transformation so to that it can be applied in the local space of
       * the curves. */
      const float4x4 curve_transform = surface_to_curves * surface_transform * curves_to_surface;

      /* Actually transform all points. */
      for (const int point_i : points) {
        const float3 old_point_pos = r_orig_positions[point_i];
        const float3 new_point_pos = math::transform_point(curve_transform, old_point_pos);
        r_positions[point_i] = new_point_pos;
      }

      if (!r_rotations.is_empty()) {
        for (const int point_i : points) {
          r_rotations[point_i] = rotation * r_rotations[point_i];
        }
      }
    }
  });
}

template<typename T> static bool arrays_equal(const Span<T> a, const Span<T> b)
{
  if (a.size() != b.size()) {
    return false;
  }
  std::atomic<bool> different = false;
  threading::parallel_for(IndexRange(a.size()), 1024, [&](const IndexRange range) {
    if (different) {
      return;
    }
    for (const int i : range) {
      if (a[i] != b[i]) {
        different = true;
        return;
      }
    }
  });
  return !different;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet curves_geometry = params.extract_input<GeometrySet>("Curves"_ustr);

  Mesh *surface_mesh_orig = nullptr;
  bool free_suface_mesh_orig = false;
  BLI_SCOPED_DEFER([&]() {
    if (free_suface_mesh_orig) {
      BKE_id_free(nullptr, surface_mesh_orig);
    }
  });

  auto pass_through_input = [&]() {
    params.set_output("Curves"_ustr, std::move(curves_geometry));
  };

  const Object *self_ob_eval = params.self_object();
  if (self_ob_eval == nullptr || self_ob_eval->type != OB_CURVES) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Node only works for curves objects"));
    return;
  }
  const Curves *self_curves_eval = id_cast<const Curves *>(self_ob_eval->data);
  if (self_curves_eval->surface_uv_map == nullptr || self_curves_eval->surface_uv_map[0] == '\0') {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Surface UV map not defined"));
    return;
  }
  /* Take surface information from self-object. */
  Object *surface_ob_eval = self_curves_eval->surface;
  const StringRefNull uv_map_name = self_curves_eval->surface_uv_map;
  const StringRefNull rest_position_name = "rest_position";

  if (!curves_geometry.has_curves()) {
    pass_through_input();
    return;
  }
  if (surface_ob_eval == nullptr || surface_ob_eval->type != OB_MESH) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Curves not attached to a surface"));
    return;
  }
  Object *surface_ob_orig = DEG_get_original(surface_ob_eval);
  Mesh &surface_object_data = *id_cast<Mesh *>(surface_ob_orig->data);

  if (BMEditMesh *em = surface_object_data.runtime->edit_mesh.get()) {
    surface_mesh_orig = BKE_mesh_from_bmesh_for_eval_nomain(em->bm, nullptr, &surface_object_data);
    free_suface_mesh_orig = true;
  }
  else {
    surface_mesh_orig = &surface_object_data;
  }
  Mesh *surface_mesh_eval = BKE_modifier_get_evaluated_mesh_from_evaluated_object(surface_ob_eval);
  if (surface_mesh_eval == nullptr) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error, TIP_("Surface has no mesh"));
    return;
  }

  BKE_mesh_wrapper_ensure_mdata(surface_mesh_eval);

  const AttributeAccessor mesh_attributes_eval = surface_mesh_eval->attributes();
  const AttributeAccessor mesh_attributes_orig = surface_mesh_orig->attributes();

  Curves &curves_id = *curves_geometry.get_curves_for_write();
  CurvesGeometry &curves = curves_id.geometry.wrap();

  if (!mesh_attributes_eval.contains(uv_map_name)) {
    pass_through_input();
    const std::string message = fmt::format(
        fmt::runtime(TIP_("Evaluated surface missing UV map: \"{}\"")), uv_map_name);
    params.error_message_add(NodeWarningType::Error, message);
    return;
  }
  if (!mesh_attributes_orig.contains(uv_map_name)) {
    pass_through_input();
    const std::string message = fmt::format(
        fmt::runtime(TIP_("Original surface missing UV map: \"{}\"")), uv_map_name);
    params.error_message_add(NodeWarningType::Error, message);
    return;
  }
  if (!mesh_attributes_eval.contains(rest_position_name)) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Evaluated surface missing attribute: \"rest_position\""));
    return;
  }
  if (!curves.surface_uv_coords() && curves.curves_num() > 0) {
    pass_through_input();
    params.error_message_add(NodeWarningType::Error,
                             TIP_("Curves are not attached to any UV map"));
    return;
  }
  const VArraySpan uv_map_orig = *mesh_attributes_orig.lookup<float2>(uv_map_name,
                                                                      AttrDomain::Corner);
  const VArraySpan uv_map_eval = *mesh_attributes_eval.lookup<float2>(uv_map_name,
                                                                      AttrDomain::Corner);
  const VArraySpan rest_positions = *mesh_attributes_eval.lookup<float3>(rest_position_name,
                                                                         AttrDomain::Point);
  const VArraySpan surface_uv_coords = *curves.attributes().lookup_or_default<float2>(
      "surface_uv_coordinate", AttrDomain::Curve, float2(0));

  const Span<int3> corner_tris_orig = surface_mesh_orig->corner_tris();
  const Span<int3> corner_tris_eval = surface_mesh_eval->corner_tris();

  Bounds<float2> uv_bounds(float2{0, 0});
  if (surface_uv_coords.size() > 0) {
    uv_bounds = *bounds::min_max(surface_uv_coords);
  }

  /* Skip building a second sampler when both meshes share the same corner topology and UV map.
   * This is the common case: armature/shape-key deformation leaves topology unchanged. */
  const bool same_mesh = arrays_equal<int3>(corner_tris_orig, corner_tris_eval) &&
                         arrays_equal<float2>(uv_map_orig, uv_map_eval);

  /* Cache the ReverseUVSampler BVHs (not the sample results) keyed by
   * (surface session_uid, curves_num).  Topology fingerprints
   * (tri_counts, uv_map_counts) guard against stale BVHs.  sample_many still
   * runs every frame against current curve attach UVs, so per-curve UV
   * changes (sculpt, paint) are picked up correctly. */
  NodeGeometryDeformCurvesOnSurface &storage = node_storage(
      const_cast<bNode &>(params.node()));
  if (storage.cache == 0) {
    storage.cache = reinterpret_cast<uint64_t>(
        MEM_new<NodeGeometryDeformCurvesOnSurfaceCache>(__func__));
  }
  NodeGeometryDeformCurvesOnSurfaceCache &cache =
      *reinterpret_cast<NodeGeometryDeformCurvesOnSurfaceCache *>(storage.cache);

  const int curves_num = curves.curves_num();
  const uint2 cache_key{uint(surface_mesh_orig->id.session_uid), uint(curves_num)};
  const uint2 tri_counts{uint(corner_tris_orig.size()), uint(corner_tris_eval.size())};
  const uint2 uv_map_counts{uint(uv_map_orig.size()), uint(uv_map_eval.size())};

  NodeGeometryDeformCurvesOnSurfaceCacheEntry **entry_slot = cache.lookup_ptr(cache_key);
  NodeGeometryDeformCurvesOnSurfaceCacheEntry *entry = entry_slot ? *entry_slot : nullptr;
  if (entry == nullptr) {
    entry = MEM_new<NodeGeometryDeformCurvesOnSurfaceCacheEntry>(__func__);
    cache.add(cache_key, entry);
  }
  {
    std::lock_guard lock(entry->mutex);
    /* uv_bounds determines which triangles get pruned out of the BVH (see
     * known_uv_bounds handling in ReverseUVSampler).  When the curves'
     * attach-UV region shifts, the cached BVH may be missing the now-needed
     * triangles, so rebuild on bounds change.  Bounds are exact-compared:
     * computing them every frame is unconditional anyway. */
    const bool cache_valid = (entry->tri_counts == tri_counts) &&
                             (entry->uv_map_counts == uv_map_counts) &&
                             entry->uv_bounds.min == uv_bounds.min &&
                             entry->uv_bounds.max == uv_bounds.max &&
                             entry->sampler_orig != nullptr &&
                             (same_mesh || entry->sampler_eval != nullptr);
    if (!cache_valid) {
      entry->sampler_orig = std::make_unique<ReverseUVSampler>(
          uv_map_orig, corner_tris_orig, uv_bounds, surface_uv_coords.size());
      if (!same_mesh) {
        entry->sampler_eval = std::make_unique<ReverseUVSampler>(
            uv_map_eval, corner_tris_eval, uv_bounds, surface_uv_coords.size());
      }
      else {
        entry->sampler_eval.reset();
      }
      entry->tri_counts = tri_counts;
      entry->uv_map_counts = uv_map_counts;
      entry->uv_bounds = uv_bounds;
    }
  }

  const ReverseUVSampler &reverse_uv_sampler_orig = *entry->sampler_orig;
  const ReverseUVSampler *reverse_uv_sampler_eval_ptr = entry->sampler_eval.get();

  /* Always run sample_many against current curve attach UVs.  Pass current-
   * frame uv_map / corner_tris into sample_many — the cached samplers'
   * internal Spans were captured at construction and may now point at freed
   * mesh runtime data. */
  Array<ReverseUVSampler::Result> samples_old(curves_num);
  Array<ReverseUVSampler::Result> samples_new_storage(same_mesh ? 0 : curves_num);
  threading::parallel_invoke(
      1024 < curves_num && !same_mesh,
      [&]() {
        reverse_uv_sampler_orig.sample_many(
            surface_uv_coords, samples_old, uv_map_orig, corner_tris_orig);
      },
      [&]() {
        if (!same_mesh) {
          reverse_uv_sampler_eval_ptr->sample_many(
              surface_uv_coords, samples_new_storage, uv_map_eval, corner_tris_eval);
        }
      });

  const Span<ReverseUVSampler::Result> cached_old = samples_old.as_span();
  const Span<ReverseUVSampler::Result> cached_new = same_mesh ? cached_old
                                                              : samples_new_storage.as_span();

  /* Retrieve face corner normals from each mesh. It's necessary to use face corner normals
   * because face normals or vertex normals may lose information (custom normals, auto smooth) in
   * some cases. */
  const Span<float3> corner_normals_orig = surface_mesh_orig->corner_normals();
  const Span<float3> corner_normals_eval = surface_mesh_eval->corner_normals();

  std::atomic<int> invalid_uv_count = 0;

  const bke::CurvesSurfaceTransforms transforms{*self_ob_eval, surface_ob_eval};

  bke::CurvesEditHints *edit_hints = curves_geometry.get_curve_edit_hints_for_write();
  MutableSpan<float3> edit_hint_positions;
  MutableSpan<float3x3> edit_hint_rotations;
  if (edit_hints != nullptr) {
    if (const std::optional<MutableSpan<float3>> positions = edit_hints->positions_for_write()) {
      edit_hint_positions = *positions;
    }
    if (!edit_hints->deform_mats.has_value()) {
      edit_hints->deform_mats.emplace(edit_hints->curves_id_orig.geometry.point_num,
                                      float3x3::identity());
      edit_hints->deform_mats->fill(float3x3::identity());
    }
    edit_hint_rotations = *edit_hints->deform_mats;
  }

  MutableSpan<float3> curve_positions = curves.positions_for_write();

  if (edit_hint_positions.is_empty()) {
    deform_curves(curves,
                  *surface_mesh_orig,
                  *surface_mesh_eval,
                  cached_old,
                  cached_new,
                  corner_normals_orig,
                  corner_normals_eval,
                  rest_positions,
                  transforms.surface_to_curves,
                  curve_positions,
                  curve_positions,
                  edit_hint_rotations,
                  invalid_uv_count);
  }
  else {
    /* First deform the actual curves in the input geometry. */
    deform_curves(curves,
                  *surface_mesh_orig,
                  *surface_mesh_eval,
                  cached_old,
                  cached_new,
                  corner_normals_orig,
                  corner_normals_eval,
                  rest_positions,
                  transforms.surface_to_curves,
                  curve_positions,
                  curve_positions,
                  {},
                  invalid_uv_count);
    /* Then also deform edit curve information for use in sculpt mode. */
    const CurvesGeometry &curves_orig = edit_hints->curves_id_orig.geometry.wrap();
    const VArraySpan<float2> surface_uv_coords_orig = *curves_orig.attributes().lookup_or_default(
        "surface_uv_coordinate", AttrDomain::Curve, float2(0));
    if (!surface_uv_coords_orig.is_empty()) {
      const int hint_curves_num = curves_orig.curves_num();
      Array<ReverseUVSampler::Result> hint_samples_old(hint_curves_num);
      Array<ReverseUVSampler::Result> hint_samples_new_storage(same_mesh ? 0 : hint_curves_num);
      threading::parallel_invoke(
          1024 < hint_curves_num && !same_mesh,
          [&]() {
            reverse_uv_sampler_orig.sample_many(
                surface_uv_coords_orig, hint_samples_old, uv_map_orig, corner_tris_orig);
          },
          [&]() {
            if (!same_mesh) {
              reverse_uv_sampler_eval_ptr->sample_many(
                  surface_uv_coords_orig, hint_samples_new_storage, uv_map_eval, corner_tris_eval);
            }
          });
      const Span<ReverseUVSampler::Result> hint_samples_new = same_mesh ?
                                                                  hint_samples_old.as_span() :
                                                                  hint_samples_new_storage.as_span();
      deform_curves(curves_orig,
                    *surface_mesh_orig,
                    *surface_mesh_eval,
                    hint_samples_old,
                    hint_samples_new,
                    corner_normals_orig,
                    corner_normals_eval,
                    rest_positions,
                    transforms.surface_to_curves,
                    edit_hint_positions,
                    edit_hint_positions,
                    edit_hint_rotations,
                    invalid_uv_count);
    }
  }

  curves.tag_positions_changed();

  if (invalid_uv_count) {
    const std::string message = fmt::format(fmt::runtime(TIP_("Invalid surface UVs on {} curves")),
                                            invalid_uv_count.load());
    params.error_message_add(NodeWarningType::Warning, message);
  }

  params.set_output("Curves"_ustr, curves_geometry);
}

static void node_free_deform_curves_on_surface_storage(bNode *node)
{
  NodeGeometryDeformCurvesOnSurface &storage = node_storage(*node);
  MEM_delete(reinterpret_cast<NodeGeometryDeformCurvesOnSurfaceCache *>(storage.cache));
  MEM_delete(reinterpret_cast<NodeGeometryDeformCurvesOnSurface *>(node->storage));
}

static void node_copy_deform_curves_on_surface_storage(bNodeTree * /*dst_ntree*/,
                                                        bNode *dst_node,
                                                        const bNode *src_node)
{
  const NodeGeometryDeformCurvesOnSurface &src = node_storage(*src_node);
  auto *dst_storage = MEM_new<NodeGeometryDeformCurvesOnSurface>(__func__,
                                                                 dna::shallow_copy(src));
  dst_storage->cache = 0;
  dst_node->storage = dst_storage;
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  NodeGeometryDeformCurvesOnSurface *data = MEM_new<NodeGeometryDeformCurvesOnSurface>(__func__);
  node->storage = data;
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(
      &ntype, "GeometryNodeDeformCurvesOnSurface"_ustr, GEO_NODE_DEFORM_CURVES_ON_SURFACE);
  ntype.ui_name = "Deform Curves on Surface";
  ntype.ui_description =
      "Translate and rotate curves based on changes between the object's original and evaluated "
      "surface mesh";
  ntype.enum_name_legacy = "DEFORM_CURVES_ON_SURFACE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.initfunc = node_init;
  bke::node_type_size(ntype, 170, 120, 700);
  bke::node_type_storage(ntype,
                         "NodeGeometryDeformCurvesOnSurface",
                         node_free_deform_curves_on_surface_storage,
                         node_copy_deform_curves_on_surface_storage);
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_deform_curves_on_surface_cc
