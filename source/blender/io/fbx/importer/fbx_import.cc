/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include <cstdio>

#include "BKE_attribute.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "BLI_color.hh"
#include "BLI_fileops.h"
#include "BLI_math_matrix.h"
#include "BLI_set.hh"
#include "BLI_string.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_collection_types.h"
#include "DNA_scene_types.h"

#include "IO_fbx.hh"

#include "fbx_import.hh"

#include "ufbx.h"

#include "CLG_log.h"
static CLG_LogRef LOG = {"io.fbx"};

namespace blender::io::fbx {

struct FbxImportContext {
  Main *bmain;
  const ufbx_scene &fbx;
  const FBXImportParams &params;
  Set<Object *> created_objects;

  FbxImportContext(Main *main, const ufbx_scene *fbx, const FBXImportParams &params)
      : bmain(main), fbx(*fbx), params(params)
  {
  }

  void import_meshes();
};

void FbxImportContext::import_meshes()
{
  for (ufbx_mesh *fmesh : this->fbx.meshes) {

    /* Create Mesh outside of main. */
    Mesh *mesh = BKE_mesh_new_nomain(
        fmesh->num_vertices, fmesh->num_edges, fmesh->num_faces, fmesh->num_indices);
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();

    /* Vertex positions. */
    MutableSpan<float3> positions = mesh->vert_positions_for_write();
    BLI_assert(positions.size() == fmesh->vertex_position.values.count);
    for (int i = 0; i < fmesh->vertex_position.values.count; i++) {
      ufbx_vec3 val = fmesh->vertex_position.values[i];
      positions[i] = float3(val.x, val.y, val.z);
    }

    /* Faces. */
    MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
    MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
    BLI_assert(face_offsets.size() == fmesh->num_faces + 1);
    for (int face_idx = 0; face_idx < fmesh->num_faces; face_idx++) {
      //@TODO: skip < 3 vertex faces?
      const ufbx_face &fface = fmesh->faces[face_idx];
      face_offsets[face_idx] = fface.index_begin;
      for (int i = 0; i < fface.num_indices; i++) {
        int corner_idx = fface.index_begin + i;
        int vidx = fmesh->vertex_indices[corner_idx];
        corner_verts[corner_idx] = vidx;
      }
    }

    /* Face material indices. */
    if (fmesh->face_material.count == fmesh->num_faces) {
      bke::SpanAttributeWriter<int> materials = attributes.lookup_or_add_for_write_only_span<int>(
          "material_index", bke::AttrDomain::Face);
      for (int i = 0; i < fmesh->face_material.count; i++) {
        materials.span[i] = fmesh->face_material[i];
      }
      materials.finish();
    }

    /* Edges. */
    MutableSpan<int2> edges = mesh->edges_for_write();
    BLI_assert(edges.size() == fmesh->num_edges);
    for (int edge_idx = 0; edge_idx < fmesh->num_edges; edge_idx++) {
      const ufbx_edge &fedge = fmesh->edges[edge_idx];
      int va = fmesh->vertex_indices[fedge.a];
      int vb = fmesh->vertex_indices[fedge.b];
      edges[edge_idx] = int2(va, vb);
    }
    bke::mesh_calc_edges(*mesh, true, false);

    /* UVs. */
    for (const ufbx_uv_set &fuv_set : fmesh->uv_sets) {
      bke::SpanAttributeWriter<float2> uvs = attributes.lookup_or_add_for_write_only_span<float2>(
          fuv_set.name.data, bke::AttrDomain::Corner);
      BLI_assert(fuv_set.vertex_uv.indices.count == uvs.span.size());
      for (int i = 0; i < fuv_set.vertex_uv.indices.count; i++) {
        int val_idx = fuv_set.vertex_uv.indices[i];
        const ufbx_vec2 &uv = fuv_set.vertex_uv.values[val_idx];
        uvs.span[i] = float2(uv.x, uv.y);
      }
      uvs.finish();
    }

    /* Colors. */
    const char *first_color_name = nullptr;
    for (const ufbx_color_set &fcol_set : fmesh->color_sets) {
      if (first_color_name == nullptr) {
        first_color_name = fcol_set.name.data;
      }
      bke::SpanAttributeWriter<ColorGeometry4f> cols =
          attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(fcol_set.name.data,
                                                                        bke::AttrDomain::Corner);
      BLI_assert(fcol_set.vertex_color.indices.count == cols.span.size());
      for (int i = 0; i < fcol_set.vertex_color.indices.count; i++) {
        int val_idx = fcol_set.vertex_color.indices[i];
        const ufbx_vec4 &col = fcol_set.vertex_color.values[val_idx];
        //@TODO: linear/sRGB conversions if needed; use byte color for sRGB
        cols.span[i] = ColorGeometry4f(col.x, col.y, col.z, col.w);
      }
      cols.finish();
    }
    mesh->active_color_attribute = BLI_strdup(first_color_name);
    mesh->default_color_attribute = BLI_strdup(first_color_name);

    /* Normals. */
    if (this->params.use_custom_normals && fmesh->vertex_normal.exists) {
      BLI_assert(fmesh->vertex_normal.indices.count == mesh->corners_num);
      Array<float3> normals(mesh->corners_num);
      for (int i = 0; i < mesh->corners_num; i++) {
        int val_idx = fmesh->vertex_normal.indices[i];
        const ufbx_vec3 &normal = fmesh->vertex_normal.values[val_idx];
        normals[i] = float3(normal.x, normal.y, normal.z);
      }
      BKE_mesh_set_custom_normals(mesh, reinterpret_cast<float(*)[3]>(normals.data()));
    }

    /* Validate if needed. */
    if (this->params.validate_meshes) {
      bool verbose_validate = false;
#ifndef NDEBUG
      verbose_validate = true;
#endif
      BKE_mesh_validate(mesh, verbose_validate, false);
    }

    /* Determine object name. Use name of first user (only if that
     * is not present, use mesh name). */
    std::string ob_name;
    if (fmesh->instances.count > 0) {
      ob_name = fmesh->instances[0]->name.data;
    }
    if (ob_name.empty()) {
      ob_name = fmesh->name.data;
    }
    if (ob_name.empty()) {
      ob_name = "Untitled";
    }
    Object *obj = BKE_object_add_only_object(this->bmain, OB_MESH, ob_name.c_str());
    obj->data = BKE_object_obdata_add_from_type(this->bmain, OB_MESH, ob_name.c_str());
    BKE_mesh_nomain_to_mesh(mesh, static_cast<Mesh *>(obj->data), obj);

    /* Transform matrix. */
    if (fmesh->instances.count > 0) {
      const ufbx_matrix &mtx = fmesh->instances[0]->geometry_to_world;
      float obmat[4][4];
      unit_m4(obmat);
      obmat[0][0] = mtx.m00;
      obmat[1][0] = mtx.m01;
      obmat[2][0] = mtx.m02;
      obmat[3][0] = mtx.m03;
      obmat[0][1] = mtx.m10;
      obmat[1][1] = mtx.m11;
      obmat[2][1] = mtx.m12;
      obmat[3][1] = mtx.m13;
      obmat[0][2] = mtx.m20;
      obmat[1][2] = mtx.m21;
      obmat[2][2] = mtx.m22;
      obmat[3][2] = mtx.m23;
      BKE_object_apply_mat4(obj, obmat, true, false);
    }

    this->created_objects.add(obj);
  }
}

void importer_main(Main *bmain, Scene *scene, ViewLayer *view_layer, const FBXImportParams &params)
{
  UNUSED_VARS(bmain, scene, view_layer, params);

  FILE *file = BLI_fopen(params.filepath, "rb");
  if (!file) {
    CLOG_ERROR(&LOG, "Failed to open FBX file '%s'\n", params.filepath);
    BKE_reportf(params.reports, RPT_ERROR, "FBX Import: Cannot open file '%s'", params.filepath);
    return;
  }

  ufbx_load_opts opts = {};
  opts.evaluate_skinning = false;
  opts.evaluate_caches = false;
  opts.load_external_files = false;
  opts.clean_skin_weights = true;
  opts.use_blender_pbr_material = true;
  opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
  opts.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Z;
  opts.target_axes.front = UFBX_COORDINATE_AXIS_NEGATIVE_Y;
  opts.target_unit_meters = 1.0f;

  ufbx_error fbx_error;
  ufbx_scene *fbx = ufbx_load_stdio(file, &opts, &fbx_error);
  fclose(file);

  if (!fbx) {
    CLOG_ERROR(&LOG,
               "Failed to import FBX file '%s': '%s'\n",
               params.filepath,
               fbx_error.description.data);
    BKE_reportf(params.reports,
                RPT_ERROR,
                "FBX Import: Cannot import file '%s': '%s'",
                params.filepath,
                fbx_error.description.data);
    return;
  }

  LayerCollection *lc = BKE_layer_collection_get_active(view_layer);
  //@TODO: do we need to sort objects by name? (faster to create within blender)

  FbxImportContext ctx(bmain, fbx, params);
  ctx.import_meshes();

  ufbx_free_scene(fbx);

  /* Add objects to collection. */
  for (Object *obj : ctx.created_objects) {
    BKE_collection_object_add(bmain, lc->collection, obj);
  }

  /* Select objects, sync layers etc. */
  BKE_view_layer_base_deselect_all(scene, view_layer);
  BKE_view_layer_synced_ensure(scene, view_layer);
  for (Object *obj : ctx.created_objects) {
    Base *base = BKE_view_layer_base_find(view_layer, obj);
    BKE_view_layer_base_select_and_set_active(view_layer, base);

    int flags = ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY | ID_RECALC_ANIMATION |
                ID_RECALC_BASE_FLAGS;
    DEG_id_tag_update_ex(bmain, &obj->id, flags);
  }
  DEG_id_tag_update(&lc->collection->id, ID_RECALC_SYNC_TO_EVAL);

  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
  DEG_relations_tag_update(bmain);
}

}  // namespace blender::io::fbx
