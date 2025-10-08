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

static const int multires_grid_tot[] = {
    0, 4, 9, 25, 81, 289, 1089, 4225, 16641, 66049, 263169, 1050625, 4198401, 16785409};

/* -------------------------------------------------------------------- */
/** \name Reshape from object
 * \{ */

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

static blender::MutableSpan<blender::float3> multires_ensure_higher_delta_storage(Object &object, Mesh &coarse_mesh, const int level)
{
  /* TODO: Ensure storage on the object, sized appropriately. */
  return {};
}

static void multires_reshape_calculate_object_delta(MultiresReshapeContext *reshape_context, SubdivCCG &higher_subdiv_ccg, blender::MutableSpan<blender::float3> object_delta)
{
  /* TODO: Calculate object space delta for all vertices of higher_subdiv_ccg and store into object_delta */
}

static void multires_reshape_object_delta_to_tangent_delta(MultiresReshapeContext *reshape_context, Object &object)
{
  /* TODO: Convert each object_delta into tangent_delta */
}

bool multiresModifier_storeHigherLevelDelta(Object &object, Mesh &coarse_mesh, SubdivCCG &higher_subdiv_ccg, SubdivCCG &lower_subdiv_ccg)
{
  /* When switching to lower levels... */
  /* Evaluate this twice, once for M(n - 1) and once for M(n)
  /* At this point, the subdiv_ccg has the correct positions of M(n - 1) */
  /* Construct an evaluator from this subdiv ccg. */
  /* Use the evaluator get the limit surface positions and the tangent matrices */
  /* For each vertex, V of N, MV = SubdivCCG position (object space), LV = Limit position (object space) */
  /* Delta = (MV - LV) * LMat */

  MultiresReshapeContext reshape_context;
  if (!multires_reshape_context_create_from_ccg( &reshape_context, &lower_subdiv_ccg, &coarse_mesh, higher_subdiv_ccg.level))
  {
    return false;
  }

  blender::MutableSpan<blender::float3> higher_storage = multires_ensure_higher_delta_storage(object, coarse_mesh, reshape_context.top.level);

  if (!multires_reshape_assign_final_coords_from_ccg(&reshape_context, &lower_subdiv_ccg)) {
    multires_reshape_context_free(&reshape_context);
    return false;
  }

  multires_reshape_smooth_object_grids_v2(&reshape_context, MultiresSubdivideModeType::CatmullClark);
  multires_reshape_calculate_object_delta(&reshape_context, higher_subdiv_ccg, higher_storage);
  multires_reshape_object_delta_to_tangent_delta(&reshape_context, object);
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
    blender::Vector<blender::float3> level_disp(
        multires_grid_tot[top_level] * coarse_mesh->corners_num, blender::float3(0.0f));
    multires_runtime.disp_at_level.append(std::move(level_disp));
#if 0
    printf("Creating new runtime layer (Current: %lld, Requested: %d)\n", multires_runtime.disp_at_level.size(), top_level);
    printf("Resulting size: %lld, Addr: %p\n", multires_runtime.disp_at_level.size(), &multires_runtime);
#endif
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
  printf("Flush updates to tangent displacements\n");

  if (!multires_reshape_context_create_from_modifier(&reshape_context, object, mmd, top_level)) {
    return;
  }

  multires_reshape_store_original_grids(&reshape_context);
  multires_reshape_ensure_grids(coarse_mesh, reshape_context.top.level);
  printf("Allocate grids\n");
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
  printf("Stored tangent displacements as object coordinates\n");

  /* Smooth the reshape CCG and use that to get the new tangent displacments */
  /* TODO: Have this read from the runtime data */
  multires_reshape_smooth_object_grids_v2(&reshape_context, mode);
  multires_reshape_object_grids_to_tangent_displacement(&reshape_context);
  printf("Assign from MDisps\n");

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
