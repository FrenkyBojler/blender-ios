/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"

#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph_query.hh"

#include "multires_reshape.hh"

#include "BKE_mesh_types.hh"
#include "BKE_paint.hh"
#include "BLI_array_utils.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "CLG_log.h"
#include "mikk_float3.hh"

/* -------------------------------------------------------------------- */
/** \name Reshape from object
 * \{ */

static CLG_LogRef LOG = {"multires.prototype"};

static constexpr int bad_vertex_idx = -1;
/* Left ear spike
static constexpr int bad_vertex_idx = 2524920;
*/
/* Right ear "spike"
static constexpr int bad_vertex_idx = 135949;
*/

static bool multiresModifier_reshapeFromVertcos(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                blender::Span<blender::float3> positions)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return false;
  }
  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(static_cast<Mesh *>(object->data), reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_vertcos(&reshape_context, positions)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

bool multiresModifier_reshapeFromObject(Depsgraph *depsgraph,
                                        MultiresModifierData *mmd,
                                        Object *dst,
                                        Object *src)
{
  const Object *ob_eval = DEG_get_evaluated(depsgraph, src);
  if (!ob_eval) {
    return false;
  }
  const Mesh *src_mesh_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (!src_mesh_eval) {
    return false;
  }

  return multiresModifier_reshapeFromVertcos(depsgraph, dst, mmd, src_mesh_eval->vert_positions());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from modifier
 * \{ */

bool multiresModifier_reshapeFromDeformModifier(Depsgraph *depsgraph,
                                                Object *object,
                                                MultiresModifierData *mmd,
                                                ModifierData *deform_md)
{
  using namespace blender;
  MultiresModifierData highest_mmd = blender::dna::shallow_copy(*mmd);
  highest_mmd.sculptlvl = highest_mmd.totlvl;
  highest_mmd.lvl = highest_mmd.totlvl;
  highest_mmd.renderlvl = highest_mmd.totlvl;

  /* Create mesh for the multires, ignoring any further modifiers (leading
   * deformation modifiers will be applied though). */
  Mesh *multires_mesh = BKE_multires_create_mesh(depsgraph, object, &highest_mmd);
  Array<float3> deformed_verts(multires_mesh->vert_positions());

  /* Apply deformation modifier on the multires, */
  ModifierEvalContext modifier_ctx{};
  modifier_ctx.depsgraph = depsgraph;
  modifier_ctx.object = object;
  modifier_ctx.flag = MOD_APPLY_USECACHE | MOD_APPLY_IGNORE_SIMPLIFY;

  const bool deform_success = BKE_modifier_deform_verts(
      deform_md, &modifier_ctx, multires_mesh, deformed_verts);
  BKE_id_free(nullptr, multires_mesh);
  if (!deform_success) {
    return false;
  }

  /* Reshaping */
  bool result = multiresModifier_reshapeFromVertcos(
      depsgraph, object, &highest_mmd, deformed_verts);

  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reshape from grids
 * \{ */

bool multiresModifier_reshapeFromCCG(const int tot_level, Mesh *coarse_mesh, SubdivCCG *subdiv_ccg)
{
  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, subdiv_ccg, coarse_mesh, tot_level))
  {
    return false;
  }

  multires_ensure_external_read(coarse_mesh, reshape_context.top.level);

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, subdiv_ccg)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }
  multires_reshape_smooth_object_grids_with_details(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);
  return true;
}

/* TODO: This doesn't work at the moment, only kept in to avoid re-writing more code if further
 * experimentation is needed */
#define SAMPLE_HIGHER_LEVEL_LIMIT_SURFACE 0

/* When lowering Sculpt subdivision levels, set the CCG positions to the limit surface of the
 * lower level instead of using the higher level positions. Probably not needed */
#define USE_LIMIT_SURFACE_POSITIONS 0

/* Only store the object space delta for odd vertices when switching down levels. Results in
 * shrinkage of the mesh when it has boundary elements. */
#define ONLY_AFFECT_ODD_VERTICES 0

static blender::MutableSpan<blender::float3> multires_ensure_delta_storage(
    Object &object, SubdivCCG &higher_subdiv_ccg, const int level)
{
  SculptSession &ss = *object.sculpt;
  if (ss.multires.runtime.disp_at_level[level - 1].is_empty()) {
    ss.multires.runtime.disp_at_level[level - 1].resize(higher_subdiv_ccg.positions.size());
  }
  return ss.multires.runtime.disp_at_level[level - 1];
}

static blender::MutableSpan<blender::float3> multires_get_delta_storage(Object &object,
                                                                        const int level)
{
  SculptSession &ss = *object.sculpt;
  return ss.multires.runtime.disp_at_level[level - 1];
}

static void multires_clear_delta_storage(Object &object, const int level)
{
  CLOG_DEBUG(&LOG, "Removing storage at level %d", level);
  SculptSession &ss = *object.sculpt;
  ss.multires.runtime.disp_at_level[level - 1].clear_and_shrink();
  ss.multires.runtime.positions_at_level[level - 1].clear_and_shrink();
}

static void multires_level_calc_object_delta(blender::Span<blender::float3> &old_positions,
                                             blender::MutableSpan<blender::float3> object_delta,
                                             blender::Span<bool> odd_vertices)
{
  CLOG_DEBUG(&LOG, "(ELEM) SUBDIV - LIMIT = DELTA:");
  BLI_assert(old_positions.size() == object_delta.size());
  for (const int i : old_positions.index_range()) {
    const blender::float3 limit_surf_position = object_delta[i];
#if ONLY_AFFECT_ODD_VERTICES
    if (odd_vertices[i]) {
#endif
      object_delta[i] = old_positions[i] - limit_surf_position;
      CLOG_TRACE(&LOG,
                 "M - (%d) (%f %f %f) - (%f %f %f) = (%f %f %f) (%f)",
                 i,
                 old_positions[i].x,
                 old_positions[i].y,
                 old_positions[i].z,
                 limit_surf_position.x,
                 limit_surf_position.y,
                 limit_surf_position.z,
                 object_delta[i].x,
                 object_delta[i].y,
                 object_delta[i].z,
                 blender::math::length(object_delta[i]));
      if (i == bad_vertex_idx) {
        CLOG_INFO(&LOG,
                  "M - (%d) (%f %f %f) - (%f %f %f) = (%f %f %f) (%f)",
                  i,
                  old_positions[i].x,
                  old_positions[i].y,
                  old_positions[i].z,
                  limit_surf_position.x,
                  limit_surf_position.y,
                  limit_surf_position.z,
                  object_delta[i].x,
                  object_delta[i].y,
                  object_delta[i].z,
                  blender::math::length(object_delta[i]));
      }
#if ONLY_AFFECT_ODD_VERTICES
    }
    else {
      object_delta[i] = blender::float3(0.0f);
    }
#endif
  }
}

static void print_level_stats(blender::Span<float> data,
                              blender::Span<int> sorted_indices,
                              blender::StringRefNull label,
                              const int elems_to_print = 3)
{
  float avg = 0.0f;
  for (int i : data.index_range()) {
    avg += data[i];
    if (std::isnan(avg)) {
      printf("%d, %f, %f\n", i, data[i], avg);
      BLI_assert(false);
    }
  }
  avg = avg / data.size();

  int median = int(data.size() / 2);
  int index_90th = int(data.size() * .90f);
  int index_95th = int(data.size() * .95f);
  int index_99th = int(data.size() * .99f);
  int index_99_9th = int(data.size() * .999f);
  int index_99_99th = int(data.size() * .9999f);

  CLOG_INFO(&LOG,
            "%s: MAX: (%ld) %.15f, MEAN: %.15f, MEDIAN: (%d) %.15f, 90th: (%d) %.15f, 95th: (%d) "
            "%.15f, 99th: (%d) %.15f, 99.9th: (%d) %.15f, 99.99th: (%d) %.15f",
            label.c_str(),
            data.size() - 1,
            data[sorted_indices[data.size() - 1]],
            avg,
            median,
            data[sorted_indices[median]],
            index_90th,
            data[sorted_indices[index_90th]],
            index_95th,
            data[sorted_indices[index_95th]],
            index_99th,
            data[sorted_indices[index_99th]],
            index_99_9th,
            data[sorted_indices[index_99_9th]],
            index_99_99th,
            data[sorted_indices[index_99_99th]]);
  for (int i = 0; i < elems_to_print; i++) {
    const int idx = data.size() - 1 - i;
    CLOG_INFO(&LOG, "%d - %.15f", sorted_indices[idx], data[sorted_indices[idx]]);
  }
}

static float euclidean_norm(const blender::float3x3 mat)
{
  blender::Span<float> values(mat.base_ptr(), 9);
  float sum = 0.0f;
  for (int i = 0; i < 9; i++) {
    sum += values[i] * values[i];
  }
  return sqrt(sum);
}

static float conditional_value(const blender::float3x3 mat)
{
  const float mat_val = euclidean_norm(mat);
  const float inv_mat_val = euclidean_norm(blender::math::invert(mat));
  return mat_val * inv_mat_val;
}

static void print_matrix(const blender::float3x3 &mat)
{
  CLOG_INFO(&LOG, "Matrix: ");
  CLOG_INFO(&LOG, "%.15f %.15f %.15f", mat.x_axis()[0], mat.x_axis()[1], mat.x_axis()[2]);
  CLOG_INFO(&LOG, "%.15f %.15f %.15f", mat.y_axis()[0], mat.y_axis()[1], mat.y_axis()[2]);
  CLOG_INFO(&LOG, "%.15f %.15f %.15f", mat.z_axis()[0], mat.z_axis()[1], mat.z_axis()[2]);
  float determinant = blender::math::determinant(mat);
  CLOG_INFO(&LOG, "Determinant: %.15f, (%e)", determinant, determinant);

  blender::float3x3 inv_mat = blender::math::invert(mat);

  CLOG_INFO(&LOG, "Inverse Matrix: ");
  CLOG_INFO(
      &LOG, "%.15f %.15f %.15f", inv_mat.x_axis()[0], inv_mat.x_axis()[1], inv_mat.x_axis()[2]);
  CLOG_INFO(
      &LOG, "%.15f %.15f %.15f", inv_mat.y_axis()[0], inv_mat.y_axis()[1], inv_mat.y_axis()[2]);
  CLOG_INFO(
      &LOG, "%.15f %.15f %.15f", inv_mat.z_axis()[0], inv_mat.z_axis()[1], inv_mat.z_axis()[2]);
  determinant = blender::math::determinant(inv_mat);
  CLOG_INFO(&LOG, "Determinant: %.15f, (%e)", determinant, determinant);

  blender::float3x3 result = mat * inv_mat;
  CLOG_INFO(&LOG, "\"Identity\" :");
  CLOG_INFO(&LOG, "%.15f %.15f %.15f", result.x_axis()[0], result.x_axis()[1], result.x_axis()[2]);
  CLOG_INFO(&LOG, "%.15f %.15f %.15f", result.y_axis()[0], result.y_axis()[1], result.y_axis()[2]);
  CLOG_INFO(&LOG, "%.15f %.15f %.15f", result.z_axis()[0], result.z_axis()[1], result.z_axis()[2]);
  CLOG_INFO(&LOG, "Conditional Value: %.15f", conditional_value(mat));
}

#define DEBUG_STATS 1

static void multires_level_object_delta_to_tangent_delta(
    blender::Span<blender::float3x3> tmat_storage,
    blender::MutableSpan<blender::float3> delta_storage)
{
  for (const int i : delta_storage.index_range()) {
    blender::float3 tangent_vector = delta_storage[i];
    blender::double3x3 mat(tmat_storage[i]);
    bool success;
    delta_storage[i] = blender::math::transform_direction(
        blender::float3x3(blender::math::invert(mat)), delta_storage[i]);
    if (i == bad_vertex_idx) {
      blender::float3x3 inv_mat = blender::math::invert(tmat_storage[i], success, 0.0f);
      CLOG_INFO(&LOG, "(Store) Spike data: %d", bad_vertex_idx);
      CLOG_INFO(
          &LOG, "(Tangent) VEC: %f %f %f", tangent_vector.x, tangent_vector.y, tangent_vector.z);

      print_matrix(tmat_storage[i]);

      CLOG_INFO(
          &LOG, "(Object): %f %f %f", delta_storage[i].x, delta_storage[i].y, delta_storage[i].z);

      blender::float3x3 final_mat = inv_mat * tmat_storage[i];
      CLOG_INFO(&LOG, "Final Matrix: ");
      CLOG_INFO(
          &LOG, "%f %f %f", final_mat.x_axis()[0], final_mat.x_axis()[1], final_mat.x_axis()[2]);
      CLOG_INFO(
          &LOG, "%f %f %f", final_mat.y_axis()[0], final_mat.y_axis()[1], final_mat.y_axis()[2]);
      CLOG_INFO(
          &LOG, "%f %f %f", final_mat.z_axis()[0], final_mat.z_axis()[1], final_mat.z_axis()[2]);
    }
  }
#if DEBUG_STATS
  blender::Array<int> sorted_indices(tmat_storage.size());
  blender::array_utils::fill_index_range(sorted_indices.as_mutable_span());

  blender::Array<float> condition_values(tmat_storage.size());
  for (const int i : tmat_storage.index_range()) {
    condition_values[i] = conditional_value(tmat_storage[i]);
  }
  std::sort(sorted_indices.begin(), sorted_indices.end(), [&](int a, int b) {
    return condition_values[a] < condition_values[b];
  });
  print_level_stats(condition_values, sorted_indices, "Condition Value");

  {
    blender::Array<float> determinants(tmat_storage.size());
    for (const int i : tmat_storage.index_range()) {
      determinants[i] = blender::math::determinant(tmat_storage[i]);
    }
    print_level_stats(determinants, sorted_indices, "Determinant");
  }

  {
    blender::Array<float> angles(tmat_storage.size());
    for (const int i : tmat_storage.index_range()) {
      const blender::float3x3& tangent_matrix = tmat_storage[i];
      if (blender::math::is_zero(tangent_matrix)) {
        angles[i] = 0.0f;
        continue;
      }

      double length;
      blender::float3 N = blender::float3(blender::math::normalize_and_get_length(
          blender::math::cross(blender::double3(tangent_matrix.x_axis()), blender::double3(tangent_matrix.y_axis())), length));
      const double denominator = blender::math::length(blender::double3(tangent_matrix.x_axis())) *
                                blender::math::length(blender::double3(tangent_matrix.y_axis()));

      const double angle_between = RAD2DEG(blender::math::asin(double(length) / denominator));
      if (std::isnan(angle_between)) {
        printf("%.15f, %.15f, %.15f\n", length, denominator, blender::math::asin(length / denominator));
        print_matrix(tangent_matrix);
        BLI_assert(false);
      }

      angles[i] = angle_between;
    }
    print_level_stats(angles, sorted_indices, "Angles");

    int less_than_1 = 0;
    int less_than_5 = 0;
    int less_than_15 = 0;
    int less_than_30 = 0;
    int less_than_45 = 0;
    for (const int i : tmat_storage.index_range()) {
      float diff = blender::math::abs(90 - angles[i]);
      if (diff < 1.0) {
        less_than_1++;
      }
      if (diff < 5.0) {
        less_than_5++;
      }
      if (diff < 15.0) {
        less_than_15++;
      }
      if (diff < 30.0) {
        less_than_30++;
      }
      if (diff < 45.0) {
        less_than_45++;
      }
    }
    auto to_percent = [&](int val) { return (float(val) / float(tmat_storage.size())) * 100.0f; };
    printf(
        "Angle Analysis: Total: %ld, <1: %d(%f), <5: %d(%f), <15: %d(%f), <30: %d(%f), <45: "
        "%d(%f)\n",
        tmat_storage.size(),
        less_than_1,
        to_percent(less_than_1),
        less_than_5,
        to_percent(less_than_5),
        less_than_15,
        to_percent(less_than_15),
        less_than_30,
        to_percent(less_than_30),
        less_than_45,
        to_percent(less_than_45));
  }

  {
    blender::Array<float> tangent_lengths(delta_storage.size());
    for (const int i : delta_storage.index_range()) {
      tangent_lengths[i] = blender::math::length(delta_storage[i]);
    }
    print_level_stats(tangent_lengths, sorted_indices, "Tangent Length");
  }
  CLOG_INFO(&LOG, "Matrix @ %d", sorted_indices[sorted_indices.size() - 1]);
  print_matrix(tmat_storage[sorted_indices[sorted_indices.size() - 1]]);
#endif
}

static void multires_copy_from_old_ccg(const SubdivCCG &higher_subdiv_ccg,
                                       blender::Span<blender::float3> old_positions,
                                       SubdivCCG &subdiv_ccg,
                                       blender::MutableSpan<bool> odd_vertices)
{
  BLI_assert(higher_subdiv_ccg.positions.size() == old_positions.size());
  const float higher_grid_1 = higher_subdiv_ccg.grid_size - 1;
  const float grid_1_inv = 1.0f / (subdiv_ccg.grid_size - 1);
  for (const int i : blender::IndexRange(subdiv_ccg.grids_num)) {
    for (const int y : blender::IndexRange(subdiv_ccg.grid_size)) {
      for (const int x : blender::IndexRange(subdiv_ccg.grid_size)) {
        blender::float2 uv(float(x) * grid_1_inv, float(y) * grid_1_inv);
        const int new_x = (int)(uv.x * higher_grid_1);
        const int new_y = (int)(uv.y * higher_grid_1);

        const int curr_idx = i * subdiv_ccg.grid_area + y * subdiv_ccg.grid_size + x;
        const int higher_idx = i * higher_subdiv_ccg.grid_area +
                               new_y * higher_subdiv_ccg.grid_size + new_x;
        CLOG_TRACE(&LOG,
                   "Assigning %d: %d %d (%d) to %d %d (%d)",
                   i,
                   new_x,
                   new_y,
                   higher_idx,
                   x,
                   y,
                   curr_idx);

        subdiv_ccg.positions[curr_idx] = old_positions[higher_idx];
        odd_vertices[higher_idx] = false;
      }
    }
  }
}

static void multires_copy_from_limit_surface(
    const SubdivCCG &higher_subdiv_ccg,
    blender::Span<blender::float3> limit_surface_positions,
    SubdivCCG &subdiv_ccg)
{
  BLI_assert(higher_subdiv_ccg.positions.size() == limit_surface_positions.size());
  const float higher_grid_1 = higher_subdiv_ccg.grid_size - 1;
  const float grid_1_inv = 1.0f / (subdiv_ccg.grid_size - 1);
  for (const int i : blender::IndexRange(subdiv_ccg.grids_num)) {
    for (const int y : blender::IndexRange(subdiv_ccg.grid_size)) {
      for (const int x : blender::IndexRange(subdiv_ccg.grid_size)) {
        blender::float2 uv(float(x) * grid_1_inv, float(y) * grid_1_inv);
        const int new_x = (int)(uv.x * higher_grid_1);
        const int new_y = (int)(uv.y * higher_grid_1);

        const int curr_idx = i * subdiv_ccg.grid_area + y * subdiv_ccg.grid_size + x;
        const int higher_idx = i * higher_subdiv_ccg.grid_area +
                               new_y * higher_subdiv_ccg.grid_size + new_x;

        subdiv_ccg.positions[curr_idx] = limit_surface_positions[higher_idx];
      }
    }
  }
}

bool multiresModifier_storeHigherLevelDelta(Object &object,
                                            Mesh &coarse_mesh,
                                            SubdivCCG &higher_subdiv_ccg,
                                            SubdivCCG &subdiv_ccg)
{
  /* When switching to lower levels... */

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, &subdiv_ccg, &coarse_mesh, higher_subdiv_ccg.level))
  {
    return false;
  }

  CLOG_DEBUG(&LOG, "Retrieving old positions:");
  blender::Span<blender::float3> old_positions =
      object.sculpt->multires.runtime.positions_at_level[higher_subdiv_ccg.level - 1];
  blender::Array<bool> odd_vertices(old_positions.size(), true);
  multires_copy_from_old_ccg(higher_subdiv_ccg, old_positions, subdiv_ccg, odd_vertices);
  /* At this point, the subdiv_ccg has the correct positions of M(n - 1) */

  blender::MutableSpan<blender::float3> delta_storage = multires_ensure_delta_storage(
      object, higher_subdiv_ccg, reshape_context.top.level);
  blender::Array<blender::float3x3> tmat_storage(delta_storage.size());
  BLI_assert(delta_storage.size() == higher_subdiv_ccg.positions.size());

  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, &subdiv_ccg, delta_storage))
  {
    multires_reshape_context_free(&reshape_context);
    return false;
  }

  CLOG_DEBUG(&LOG,
             "SIZES -> HIGHER: %ld, LOWER: %ld, Storage: %ld",
             higher_subdiv_ccg.positions.size(),
             subdiv_ccg.positions.size(),
             delta_storage.size());

  /* For each vertex, V of N, MV = SubdivCCG position (object space), LV = Limit position (object
   * space) */
  multires_reshape_store_limit_positions(
      &reshape_context, MultiresSubdivideModeType::CatmullClark, delta_storage, tmat_storage);
  CLOG_DEBUG(&LOG, "STORED LIMIT POS");
  for (const int i : delta_storage.index_range()) {
    CLOG_TRACE(&LOG,
               "%d - (%f, %f, %f) - %f",
               i,
               delta_storage[i].x,
               delta_storage[i].y,
               delta_storage[i].z,
               blender::math::length(delta_storage[i]));
  }
#if USE_LIMIT_SURFACE_POSITIONS
  multires_copy_from_limit_surface(higher_subdiv_ccg, delta_storage, subdiv_ccg);
#endif
  /* Delta = (MV - LV) * LMat */
  multires_level_calc_object_delta(old_positions, delta_storage, odd_vertices);
  CLOG_DEBUG(&LOG, "STORED HIGHER POS - LIMIT POS");
  multires_reshape_context_free(&reshape_context);
#if SAMPLE_HIGHER_LEVEL_LIMIT_SURFACE
  MultiresReshapeContext higher_reshape_context;
  multires_reshape_context_create_from_ccg(
      &higher_reshape_context, &higher_subdiv_ccg, &coarse_mesh, higher_subdiv_ccg.level + 1);

  CLOG_DEBUG(&LOG, "Retrieving matrices");
  multires_reshape_store_higher_limit_surface_tangent_matrices(
      &higher_reshape_context,
      MultiresSubdivideModeType::CatmullClark,
      old_positions,
      tmat_storage);
  multires_reshape_context_free(&higher_reshape_context);
#endif

  multires_level_object_delta_to_tangent_delta(tmat_storage, delta_storage);
  for (const int i : delta_storage.index_range()) {
    CLOG_TRACE(&LOG,
               "%d - (%f, %f, %f) - %f",
               i,
               delta_storage[i].x,
               delta_storage[i].y,
               delta_storage[i].z,
               blender::math::length(delta_storage[i]));
  }

  CLOG_DEBUG(&LOG, "CONVERTED TO TANGENT SPACE");

  return true;
}

static void multires_level_tangent_delta_to_object_delta(
    blender::MutableSpan<blender::float3> delta_storage,
    blender::Span<blender::float3x3> tmat_storage)
{
  for (const int i : delta_storage.index_range()) {
    if (i == bad_vertex_idx) {
      blender::float3 tangent_vector = delta_storage[i];
      blender::float3x3 tangent_matrix = tmat_storage[i];
      CLOG_INFO(&LOG, "(Apply) Spike data: %d", bad_vertex_idx);
      CLOG_INFO(&LOG,
                "(Tangent) Vector: %f %f %f",
                tangent_vector.x,
                tangent_vector.y,
                tangent_vector.z);

      CLOG_INFO(&LOG, "Matrix: ");
      CLOG_INFO(&LOG,
                "%.15f %.15f %.15f",
                tangent_matrix.x_axis()[0],
                tangent_matrix.x_axis()[1],
                tangent_matrix.x_axis()[2]);
      CLOG_INFO(&LOG,
                "%.15f %.15f %.15f",
                tangent_matrix.y_axis()[0],
                tangent_matrix.y_axis()[1],
                tangent_matrix.y_axis()[2]);
      CLOG_INFO(&LOG,
                "%.15f %.15f %.15f",
                tangent_matrix.z_axis()[0],
                tangent_matrix.z_axis()[1],
                tangent_matrix.z_axis()[2]);
      CLOG_INFO(&LOG, "Determinant: %f", blender::math::determinant(tangent_matrix));

      blender::float3 obj_vector = blender::math::transform_direction(tmat_storage[i],
                                                                      delta_storage[i]);
      CLOG_INFO(&LOG, "(Object) Vector: %f %f %f", obj_vector.x, obj_vector.y, obj_vector.z);
    }
    delta_storage[i] = blender::math::transform_direction(tmat_storage[i], delta_storage[i]);
  }
}

static void multires_level_apply_object_delta(blender::Span<blender::float3> position_storage,
                                              blender::Span<blender::float3> delta_storage,
                                              SubdivCCG &subdiv_ccg)
{
  BLI_assert(subdiv_ccg.positions.size() == delta_storage.size());
  BLI_assert(subdiv_ccg.positions.size() == position_storage.size());

  CLOG_DEBUG(&LOG, "APPLY OBJ DELTA");
  for (const int i : subdiv_ccg.positions.index_range()) {
    subdiv_ccg.positions[i] = position_storage[i] + delta_storage[i];
    float length = blender::math::length(delta_storage[i]);
    if (length > 0.3f) {
      CLOG_WARN(&LOG, "%d, %f", i, length);
    }
    CLOG_TRACE(&LOG,
               "(%d) (%f %f %f) = (%f %f %f) + (%f %f %f)",
               i,
               subdiv_ccg.positions[i].x,
               subdiv_ccg.positions[i].y,
               subdiv_ccg.positions[i].z,
               position_storage[i].x,
               position_storage[i].y,
               position_storage[i].z,
               delta_storage[i].x,
               delta_storage[i].y,
               delta_storage[i].z);
  }
#if DEBUG_STATS
  float avg = 0.0f;
  blender::Array<float> lengths(delta_storage.size());
  for (const int i : delta_storage.index_range()) {
    lengths[i] = blender::math::length(delta_storage[i]);
    avg += lengths[i];
  }
  avg /= lengths.size();
  std::sort(lengths.begin(), lengths.end());

  int median = int(lengths.size() / 2);
  int index_90th = int(lengths.size() * .90f);
  int index_95th = int(lengths.size() * .95f);
  int index_99th = int(lengths.size() * .99f);
  int index_99_9th = int(lengths.size() * .999f);
  int index_99_99th = int(lengths.size() * .9999f);

  CLOG_INFO(
      &LOG,
      "Object Space Delta: MAX: (%ld) %f, MEAN: %f, MEDIAN: (%d) %f, 90th: (%d) %f, 95th: (%d) "
      "%f, 99th: (%d) %f, 99.9th: (%d) %f, 99.99th: (%d) %f",
      lengths.size() - 1,
      lengths[lengths.size() - 1],
      avg,
      median,
      lengths[median],
      index_90th,
      lengths[index_90th],
      index_95th,
      lengths[index_95th],
      index_99th,
      lengths[index_99th],
      index_99_9th,
      lengths[index_99_9th],
      index_99_99th,
      lengths[index_99_99th]);

  for (int i = 0; i < 10; i++) {
    CLOG_INFO(&LOG, "%ld, %f", lengths.size() - 1 - i, lengths[lengths.size() - 1 - i]);
  }
#endif
}

bool multiresModifier_applyHigherLevelDelta(Object &object,
                                            Mesh &coarse_mesh,
                                            SubdivCCG &lower_subdiv_ccg,
                                            SubdivCCG &subdiv_ccg)
{
  /* When switching to higher levels... */
  /* Take the stored higher level tangent displacements */
  blender::Array<blender::float3> ccg_storage(lower_subdiv_ccg.positions.size());
  blender::MutableSpan<blender::float3> delta_storage = multires_get_delta_storage(
      object, subdiv_ccg.level);
  blender::Array<blender::float3> position_storage(delta_storage.size());
  blender::Array<blender::float3x3> tmat_storage(delta_storage.size());
  BLI_assert(delta_storage.size() == subdiv_ccg.positions.size());

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg(
          &reshape_context, &lower_subdiv_ccg, &coarse_mesh, subdiv_ccg.level))
  {
    return false;
  }

  CLOG_DEBUG(&LOG, "Retrieving old positions:");
  blender::Span<blender::float3> old_positions =
      object.sculpt->multires.runtime.positions_at_level[lower_subdiv_ccg.level - 1];
  for (const int i : old_positions.index_range()) {
    CLOG_TRACE(
        &LOG, "(%d) - %f %f %f", i, old_positions[i].x, old_positions[i].y, old_positions[i].z);
  }

  multires_reshape_store_positions_and_matrices(
      &reshape_context,
      MultiresSubdivideModeType::CatmullClark,
      object.sculpt->multires.runtime.positions_at_level[lower_subdiv_ccg.level - 1],
      position_storage,
      tmat_storage);

  /* Convert them to object space */
  multires_level_tangent_delta_to_object_delta(delta_storage, tmat_storage);
#if DEBUG_STATS
  blender::Array<int> sorted_indices(tmat_storage.size());
  blender::array_utils::fill_index_range(sorted_indices.as_mutable_span());

  blender::Array<float> condition_values(tmat_storage.size());
  for (const int i : tmat_storage.index_range()) {
    condition_values[i] = conditional_value(tmat_storage[i]);
  }
  std::sort(sorted_indices.begin(), sorted_indices.end(), [&](int a, int b) {
    return condition_values[a] < condition_values[b];
  });
  print_level_stats(condition_values, sorted_indices, "Condition Value");

  {
    blender::Array<float> determinants(tmat_storage.size());
    for (const int i : tmat_storage.index_range()) {
      determinants[i] = blender::math::determinant(tmat_storage[i]);
    }
    print_level_stats(determinants, sorted_indices, "Determinant");
  }

#if 0
  {
    blender::Array<float> angles(tmat_storage.size());
    for (const int i : tmat_storage.index_range()) {
      angles[i] = blender::math::dot(tmat_storage[i].x_axis(), tmat_storage[i].y_axis()) /
                  (blender::math::length(tmat_storage[i].x_axis()) *
                   blender::math::length(tmat_storage[i].y_axis()));
      angles[i] = RAD2DEGF(blender::math::acos(angles[i]));
      BLI_assert(std::isfinite(angles[i]));
    }
    print_level_stats(angles, sorted_indices, "Angles");
  }
#endif

  {
    blender::Array<float> tangent_lengths(delta_storage.size());
    for (const int i : delta_storage.index_range()) {
      tangent_lengths[i] = blender::math::length(delta_storage[i]);
    }
    print_level_stats(tangent_lengths, sorted_indices, "Object Length");
  }

  if (bad_vertex_idx != -1 && bad_vertex_idx < tmat_storage.size()) {
    print_matrix(tmat_storage[bad_vertex_idx]);
  }
#endif

  /* Re-add them to the new subdiv CCG */
  multires_level_apply_object_delta(position_storage, delta_storage, subdiv_ccg);
  for (const int i : subdiv_ccg.positions.index_range()) {
    CLOG_TRACE(&LOG,
               "(%d) - %f %f %f",
               i,
               subdiv_ccg.positions[i].x,
               subdiv_ccg.positions[i].y,
               subdiv_ccg.positions[i].z);
  }

  BKE_subdiv_ccg_recalc_normals(subdiv_ccg);

  /* Delete the data */
  multires_clear_delta_storage(object, subdiv_ccg.level);

  multires_reshape_context_free(&reshape_context);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Subdivision
 * \{ */

void multiresModifier_subdivide(Object *object,
                                MultiresModifierData *mmd,
                                const MultiresSubdivideModeType mode)
{
  const int top_level = mmd->totlvl + 1;
  multiresModifier_subdivide_to_level_v2(object, mmd, top_level, mode);
}

void multiresModifier_subdivide_to_level(Object *object,
                                         MultiresModifierData *mmd,
                                         const int top_level,
                                         const MultiresSubdivideModeType mode)
{
  if (top_level <= mmd->totlvl) {
    return;
  }

  Mesh *coarse_mesh = static_cast<Mesh *>(object->data);
  if (coarse_mesh->corners_num == 0) {
    /* If there are no loops in the mesh implies there is no CD_MDISPS as well. So can early output
     * from here as there is nothing to subdivide. */
    return;
  }

  MultiresReshapeContext reshape_context;

  /* There was no multires at all, all displacement is at 0. Can simply make sure all mdisps grids
   * are allocated at a proper level and return. */
  const bool has_mdisps = CustomData_has_layer(&coarse_mesh->corner_data, CD_MDISPS);
  if (!has_mdisps) {
    CustomData_add_layer(
        &coarse_mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, coarse_mesh->corners_num);
  }

  /* NOTE: Subdivision happens from the top level of the existing multires modifier. If it is set
   * to 0 and there is mdisps layer it would mean that the modifier went out of sync with the data.
   * This happens when, for example, linking modifiers from one object to another.
   *
   * In such cases simply ensure grids to be the proper level.
   *
   * If something smarter is needed it is up to the operators which does data synchronization, so
   * that the mdisps layer is also synchronized. */
  if (!has_mdisps || top_level == 1 || mmd->totlvl == 0) {
    multires_reshape_ensure_grids(coarse_mesh, top_level);
    if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
      multires_subdivide_create_tangent_displacement_linear_grids(object, mmd);
    }
    else {
      multires_set_tot_level(object, mmd, top_level);
    }
    return;
  }

  multires_flush_sculpt_updates(object);

  if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, top_level)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  multires_reshape_assign_final_elements_from_orig_mdisps(&reshape_context);

  /* Free original grids which makes it so smoothing with details thinks all the details were
   * added against base mesh's limit surface. This is similar behavior to as if we've done all
   * displacement in sculpt mode at the old top level and then propagated to the new top level. */
  multires_reshape_free_original_grids(&reshape_context);

  if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
    multires_reshape_smooth_object_grids(&reshape_context, mode);
  }
  else {
    multires_reshape_smooth_object_grids_with_details(&reshape_context);
  }

  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  multires_reshape_context_free(&reshape_context);

  multires_set_tot_level(object, mmd, top_level);
}

void multiresModifier_subdivide_to_level_v2(Object *object,
                                            MultiresModifierData *mmd,
                                            int top_level,
                                            MultiresSubdivideModeType mode)
{

  if (top_level <= mmd->totlvl) {
    return;
  }
  if (ELEM(mode, MultiresSubdivideModeType::Linear, MultiresSubdivideModeType::Simple)) {
    /* Ignore non-catmull clark subdivision for now */
    return;
  }

  Mesh *coarse_mesh = static_cast<Mesh *>(object->data);
  if (coarse_mesh->corners_num == 0) {
    /* If there are no loops in the mesh implies there is no CD_MDISPS as well. So can early output
     * from here as there is nothing to subdivide. */
    return;
  }

  MultiresReshapeContext reshape_context;

  /* There was no multires at all, all displacement is at 0. Can simply make sure all mdisps grids
   * are allocated at a proper level and return. */
  const bool has_mdisps = CustomData_has_layer(&coarse_mesh->corner_data, CD_MDISPS);
  if (!has_mdisps) {
    CustomData_add_layer(
        &coarse_mesh->corner_data, CD_MDISPS, CD_SET_DEFAULT, coarse_mesh->corners_num);
  }

  /* Create layer for new level. */
  MultiresRuntime &multires_runtime = object->sculpt->multires.runtime;
  const int level_idx = top_level - 1;
  BLI_assert(level_idx >= 0);
  if (top_level > multires_runtime.disp_at_level.size()) {
    multires_runtime.disp_at_level.resize(top_level);
  }
  if (top_level > multires_runtime.positions_at_level.size()) {
    multires_runtime.positions_at_level.resize(top_level);
  }

  /* NOTE: Subdivision happens from the top level of the existing multires modifier. If it is set
   * to 0 and there is mdisps layer it would mean that the modifier went out of sync with the data.
   * This happens when, for example, linking modifiers from one object to another.
   *
   * In such cases simply ensure grids to be the proper level.
   *
   * If something smarter is needed it is up to the operators which does data synchronization, so
   * that the mdisps layer is also synchronized. */
  if (!has_mdisps || top_level == 1 || mmd->totlvl == 0) {
    multires_set_tot_level(object, mmd, top_level);
    return;
  }

  /* After this call this point, we have the current SubdivCCG data stored as tangent displacements
   */
  /* TODO: Have this write to `multires_runtime` in tangent space of the base mesh. */
  /* TODO: Potentially write the object space positions too. */
  multires_flush_sculpt_updates(object);
  CLOG_DEBUG(&LOG, "Flush updates to tangent displacements");

  if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, top_level)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  CLOG_DEBUG(&LOG, "Allocate grids");
  /* The refine CCG should be the "current" / flushed displacements. These are *object space*
   * locations of the grid elements.*/
  /* TODO: Implement a "multires_reshape_assign_base_coords_from_runtime" */
  /* TODO: Can this be shortcut? We have a very expensive set of calls here that effectively go:
   *
   * Sculpt Mode (SubdivCCG) (object space)
   * CD_MDISP (tangent space)
   * Limit evaluation and displacement addition (object space)
   * Subdivide to create new level
   * Object space *back to* tangent space in MDisps for modifier evaluation (tangent space)
   * Back in sculpt mode: MDisp back to SubdivCCG (objet space)
   */
  multires_reshape_assign_final_elements_from_orig_mdisps(&reshape_context);
  multires_reshape_free_original_grids(&reshape_context);
  CLOG_DEBUG(&LOG, "Stored tangent displacements as object coordinates");

  /* Smooth the reshape CCG and use that to get the new tangent displacments */
  /* TODO: Have this read from the runtime data */
  multires_reshape_smooth_object_grids_v2(&reshape_context, mode);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  CLOG_DEBUG(&LOG, "Assign from MDisps");

  /* At this point, the "canonical" MDisp data should be updated so that later when the subdiv CCG
   * is created it can use that to create the new object space positions */
  /* All levels of `multires_runtime` should be zeroed out */
  /* TODO: Is there a simpler way of storing the current object space data such that we don't have
   * to do this many round trip conversions? */

  multires_reshape_context_free(&reshape_context);

  /* The final CCG level should be 1 + the current level */
  multires_set_tot_level(object, mmd, top_level);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply base
 * \{ */

void multiresModifier_base_apply(Depsgraph *depsgraph,
                                 Object *object,
                                 MultiresModifierData *mmd,
                                 const ApplyBaseMode mode)
{
  multires_force_sculpt_rebuild(object);

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_object(&reshape_context, depsgraph, object, mmd)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);

  /* At this point base_mesh is object's mesh, the subdiv is initialized to the deformed state of
   * the base mesh.
   * Store coordinates of top level grids in object space which will define true shape we would
   * want to reshape to after modifying the base mesh. */
  multires_reshape_assign_final_coords_from_mdisps(&reshape_context);

  /* For modifying base mesh we only want to consider deformation caused by multires displacement
   * and ignore all deformation which might be caused by deformation modifiers leading the multires
   * one.
   * So refine the subdiv to the original mesh vertices positions, which will also need to make
   * it so object space displacement is re-evaluated for them (as in, can not re-use any knowledge
   * from the final coordinates in the object space ). */
  multires_reshape_apply_base_refine_from_base(&reshape_context);

  /* Modify original mesh coordinates. This happens in two steps:
   * - Coordinates are set to their final location, where they are intended to be in the final
   *   result.
   * - Heuristic moves them a bit, kind of canceling out the effect of subsurf (so then when
   *   multires modifier applies subsurf vertices are placed at the desired location). */
  multires_reshape_apply_base_update_mesh_coords(&reshape_context);
  if (mode == ApplyBaseMode::ForSubdivision) {
    multires_reshape_apply_base_refit_base_mesh(&reshape_context);
  }

  /* Reshape to the stored final state.
   * Not that the base changed, so the subdiv is to be refined to the new positions. Unfortunately,
   * this can not be done foe entirely cheap: if there were deformation modifiers prior to the
   * multires they need to be re-evaluated for the new base mesh. */
  multires_reshape_apply_base_refine_from_deform(&reshape_context);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);

  multires_reshape_context_free(&reshape_context);
}

/** \} */
