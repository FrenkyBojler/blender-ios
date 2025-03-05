/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include <cstdio>

#include "BKE_armature.hh"
#include "BKE_attribute.hh"
#include "BKE_camera.h"
#include "BKE_deform.hh"
#include "BKE_idprop.hh"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_light.h"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"
#include "BKE_report.hh"

#include "BLI_color.hh"
#include "BLI_fileops.h"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_string.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_camera_types.h"
#include "DNA_collection_types.h"
#include "DNA_key_types.h"
#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"

#include "ED_armature.hh"

#include "IO_fbx.hh"

#include "fbx_import.hh"
#include "fbx_import_anim.hh"
#include "fbx_import_material.hh"
#include "fbx_import_util.hh"

#include "CLG_log.h"
static CLG_LogRef LOG = {"io.fbx"};

namespace blender::io::fbx {

struct FbxImportContext {
  Main *bmain;
  const ufbx_scene &fbx;
  const FBXImportParams &params;
  std::string base_dir;
  FbxElementMapping mapping;

  FbxImportContext(Main *main, const ufbx_scene *fbx, const FBXImportParams &params)
      : bmain(main), fbx(*fbx), params(params)
  {
    char basedir[FILE_MAX];
    BLI_path_split_dir_part(params.filepath, basedir, sizeof(basedir));
    base_dir = basedir;
  }

  void import_globals(Scene *scene);
  void import_materials();
  void import_meshes();
  void import_cameras();
  void import_lights();
  void import_empties();
  void import_animation(double fps);

  void setup_hierarchy();
  Object *create_armature_for_deformer(const ufbx_skin_deformer &fskin);
};

static void matrix_to_m44(const ufbx_matrix &src, float dst[4][4])
{
  dst[0][0] = src.m00;
  dst[1][0] = src.m01;
  dst[2][0] = src.m02;
  dst[3][0] = src.m03;
  dst[0][1] = src.m10;
  dst[1][1] = src.m11;
  dst[2][1] = src.m12;
  dst[3][1] = src.m13;
  dst[0][2] = src.m20;
  dst[1][2] = src.m21;
  dst[2][2] = src.m22;
  dst[3][2] = src.m23;
  dst[0][3] = 0.0f;
  dst[1][3] = 0.0f;
  dst[2][3] = 0.0f;
  dst[3][3] = 1.0f;
}

static void node_matrix_to_obj(const ufbx_node *node, Object *obj)
{
  ufbx_matrix mtx = ufbx_matrix_mul(node->is_root ? &node->node_to_world : &node->node_to_parent,
                                    &node->geometry_to_node);
  float obmat[4][4];
  matrix_to_m44(mtx, obmat);
  BKE_object_apply_mat4(obj, obmat, true, false);
}

static void read_custom_properties(const ufbx_props &props, ID &id)
{
  for (const ufbx_prop &prop : props.props) {
    if ((prop.flags & UFBX_PROP_FLAG_USER_DEFINED) == 0) {
      continue;
    }

    IDProperty *idgroup = IDP_EnsureProperties(&id);
    IDProperty *idprop = nullptr;
    IDPropertyTemplate val = {0};
    //@TODO: validate_blend_names on the property name
    const char *name = prop.name.data;

    switch (prop.type) {
      case UFBX_PROP_BOOLEAN:
        val.i = prop.value_int;
        idprop = IDP_New(IDP_BOOLEAN, &val, name);
        break;
      case UFBX_PROP_INTEGER:
        val.i = prop.value_int;
        idprop = IDP_New(IDP_INT, &val, name);
        break;
      case UFBX_PROP_NUMBER:
        val.d = prop.value_real;
        idprop = IDP_New(IDP_DOUBLE, &val, name);
        break;
      case UFBX_PROP_STRING:
        val.string.str = prop.value_str.data;
        val.string.len = prop.value_str.length + 1; /* Length includes null terminator. */
        val.string.subtype = IDP_STRING_SUB_UTF8;
        idprop = IDP_New(IDP_STRING, &val, name);
        break;
      //@TODO: vector, color, color_with_alpha, translation, rotation, ...
      default:
        break;
    }

    if (idprop != nullptr) {
      IDP_AddToGroup(idgroup, idprop);
    }
  }
}

void FbxImportContext::import_globals(Scene *scene)
{
  /* Set scene framerate to that of FBX file. */
  double fps = this->fbx.settings.frames_per_second;
  scene->r.frs_sec = roundf(fps);
  scene->r.frs_sec_base = scene->r.frs_sec / fps;
}

void FbxImportContext::import_materials()
{
  for (const ufbx_material *fmat : this->fbx.materials) {
    Material *mat = io::fbx::import_material(this->bmain, this->base_dir, *fmat);
    if (this->params.use_custom_props) {
      read_custom_properties(fmat->props, mat->id);
    }
    this->mapping.mat_to_material.add(fmat, mat);
  }
}

void FbxImportContext::import_meshes()
{
  for (const ufbx_mesh *fmesh : this->fbx.meshes) {
    if (fmesh->instances.count == 0) {
      continue; /* Ignore if not used by any objects. */
    }

    /* Create Mesh outside of main. */
    Mesh *mesh = BKE_mesh_new_nomain(
        fmesh->num_vertices, fmesh->num_edges, fmesh->num_faces, fmesh->num_indices);
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    AttributeOwner attr_owner = AttributeOwner::from_id(&mesh->id);

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
    BLI_assert(face_offsets.size() == fmesh->num_faces + 1 ||
               face_offsets.is_empty() && fmesh->num_faces == 0);
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
    /* Face smoothing. */
    if (fmesh->face_smoothing.count > 0 && fmesh->face_smoothing.count == fmesh->num_faces) {
      bke::SpanAttributeWriter<bool> smooth = attributes.lookup_or_add_for_write_only_span<bool>(
          "sharp_face", bke::AttrDomain::Face);
      for (int i = 0; i < fmesh->face_smoothing.count; i++) {
        smooth.span[i] = !fmesh->face_smoothing[i];
      }
      smooth.finish();
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

    /* Calculate any remaining edges, and add them to explicitly imported ones.
     * Note that this clears any per-edge data, so we have to setup edge creases etc.
     * after that. */
    bke::mesh_calc_edges(*mesh, true, false);

    const bool has_edge_creases = fmesh->edge_crease.count > 0 &&
                                  fmesh->edge_crease.count == fmesh->num_edges;
    const bool has_edge_smooth = fmesh->edge_smoothing.count > 0 &&
                                 fmesh->edge_smoothing.count == fmesh->num_edges;
    if (has_edge_creases || has_edge_smooth) {
      /* The total number of edges in mesh now might be different from number of explicitly
       * imported ones; we have to build mapping from vertex pairs to edge index. */
      Span<int2> edges = mesh->edges();
      Map<OrderedEdge, int> edge_map;
      edge_map.reserve(edges.size());
      for (const int i : edges.index_range()) {
        edge_map.add(edges[i], i);
      }

      if (has_edge_creases) {
        bke::SpanAttributeWriter<float> creases =
            attributes.lookup_or_add_for_write_only_span<float>("crease_edge",
                                                                bke::AttrDomain::Edge);
        creases.span.fill(0.0f);
        for (int i = 0; i < fmesh->num_edges; i++) {
          const ufbx_edge &fedge = fmesh->edges[i];
          int va = fmesh->vertex_indices[fedge.a];
          int vb = fmesh->vertex_indices[fedge.b];
          int edge_i = edge_map.lookup_default({va, vb}, -1);
          if (edge_i >= 0) {
            /* Python fbx importer was squaring the incoming crease values. */
            creases.span[edge_i] = sqrtf(fmesh->edge_crease[i]);
          }
        }
        creases.finish();
      }

      if (has_edge_smooth) {
        bke::SpanAttributeWriter<bool> sharp = attributes.lookup_or_add_for_write_only_span<bool>(
            "sharp_edge", bke::AttrDomain::Edge);
        sharp.span.fill(false);
        for (int i = 0; i < fmesh->num_edges; i++) {
          const ufbx_edge &fedge = fmesh->edges[i];
          int va = fmesh->vertex_indices[fedge.a];
          int vb = fmesh->vertex_indices[fedge.b];
          int edge_i = edge_map.lookup_default({va, vb}, -1);
          if (edge_i >= 0) {
            sharp.span[edge_i] = !fmesh->edge_smoothing[i];
          }
        }
        sharp.finish();
      }
    }

    /* UVs. */
    for (const ufbx_uv_set &fuv_set : fmesh->uv_sets) {
      std::string attr_name = BKE_attribute_calc_unique_name(attr_owner, fuv_set.name.data);
      bke::SpanAttributeWriter<float2> uvs = attributes.lookup_or_add_for_write_only_span<float2>(
          attr_name, bke::AttrDomain::Corner);
      BLI_assert(fuv_set.vertex_uv.indices.count == uvs.span.size());
      for (int i = 0; i < fuv_set.vertex_uv.indices.count; i++) {
        int val_idx = fuv_set.vertex_uv.indices[i];
        const ufbx_vec2 &uv = fuv_set.vertex_uv.values[val_idx];
        uvs.span[i] = float2(uv.x, uv.y);
      }
      uvs.finish();
    }

    /* Colors. */
    std::string first_color_name;
    for (const ufbx_color_set &fcol_set : fmesh->color_sets) {
      if (this->params.vertex_colors == eFBXVertexColorMode::None) {
        continue;
      }
      std::string attr_name = BKE_attribute_calc_unique_name(attr_owner, fcol_set.name.data);
      if (first_color_name.empty()) {
        first_color_name = attr_name;
      }
      if (this->params.vertex_colors == eFBXVertexColorMode::sRGB) {
        /* sRGB colors, use 4 bytes per color. */
        bke::SpanAttributeWriter<ColorGeometry4b> cols =
            attributes.lookup_or_add_for_write_only_span<ColorGeometry4b>(attr_name,
                                                                          bke::AttrDomain::Corner);
        BLI_assert(fcol_set.vertex_color.indices.count == cols.span.size());
        for (int i = 0; i < fcol_set.vertex_color.indices.count; i++) {
          int val_idx = fcol_set.vertex_color.indices[i];
          const ufbx_vec4 &col = fcol_set.vertex_color.values[val_idx];
          /* Note: color values are expected to already be in sRGB space. */
          float4 fcol = float4(col.x, col.y, col.z, col.w);
          uchar4 bcol;
          rgba_float_to_uchar(bcol, fcol);
          cols.span[i] = ColorGeometry4b(bcol);
        }
        cols.finish();
      }
      else if (this->params.vertex_colors == eFBXVertexColorMode::Linear) {
        /* Linear colors, use 4 floats per color. */
        bke::SpanAttributeWriter<ColorGeometry4f> cols =
            attributes.lookup_or_add_for_write_only_span<ColorGeometry4f>(attr_name,
                                                                          bke::AttrDomain::Corner);
        BLI_assert(fcol_set.vertex_color.indices.count == cols.span.size());
        for (int i = 0; i < fcol_set.vertex_color.indices.count; i++) {
          int val_idx = fcol_set.vertex_color.indices[i];
          const ufbx_vec4 &col = fcol_set.vertex_color.values[val_idx];
          cols.span[i] = ColorGeometry4f(col.x, col.y, col.z, col.w);
        }
        cols.finish();
      }
      else {
        BLI_assert_unreachable();
      }
    }
    if (!first_color_name.empty()) {
      mesh->active_color_attribute = BLI_strdup(first_color_name.c_str());
      mesh->default_color_attribute = BLI_strdup(first_color_name.c_str());
    }

    /* Normals. */
    if (this->params.use_custom_normals && fmesh->vertex_normal.exists) {
      BLI_assert(fmesh->vertex_normal.indices.count == mesh->corners_num);
      Array<float3> normals(mesh->corners_num);
      for (int i = 0; i < mesh->corners_num; i++) {
        int val_idx = fmesh->vertex_normal.indices[i];
        const ufbx_vec3 &normal = fmesh->vertex_normal.values[val_idx];
        normals[i] = float3(normal.x, normal.y, normal.z);
      }
      bke::mesh_set_custom_normals(*mesh, normals);
    }

    /* Skinning (vertex group) information. */
    if (fmesh->skin_deformers.count > 0) {
      const ufbx_skin_deformer *skin = fmesh->skin_deformers[0];
      if (skin != nullptr && fmesh->num_vertices > 0 &&
          skin->vertices.count == fmesh->num_vertices)
      {
        MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
        for (int i = 0; i < fmesh->num_vertices; i++) {
          const ufbx_skin_vertex &fvertex = skin->vertices[i];
          int num_weights = fvertex.num_weights;
          if (num_weights > 0) {
            dverts[i].dw = static_cast<MDeformWeight *>(
                MEM_mallocN(sizeof(MDeformWeight) * num_weights, __func__));
            dverts[i].totweight = num_weights;
            for (int j = 0; j < num_weights; j++) {
              const ufbx_skin_weight &fweight = skin->weights[fvertex.weight_begin + j];
              dverts[i].dw[j].def_nr = fweight.cluster_index;
              dverts[i].dw[j].weight = fweight.weight;
            }
          }
        }
      }
    }

    /* Validate if needed. */
    if (this->params.validate_meshes) {
      bool verbose_validate = false;
#ifndef NDEBUG
      verbose_validate = true;
#endif
      BKE_mesh_validate(mesh, verbose_validate, false);
    }

    /* Steps below have to be done on the final mesh in Main. */
    Mesh *mesh_main = static_cast<Mesh *>(
        BKE_object_obdata_add_from_type(this->bmain, OB_MESH, get_fbx_name(fmesh->name, "Mesh")));
    BKE_mesh_nomain_to_mesh(mesh, mesh_main, nullptr);
    mesh = mesh_main;
    if (this->params.use_custom_props) {
      read_custom_properties(fmesh->props, mesh->id);
    }

    /* Blend shapes. */
    Key *mesh_key = nullptr;
    for (const ufbx_blend_deformer *fdeformer : fmesh->blend_deformers) {
      for (const ufbx_blend_channel *fchan : fdeformer->channels) {
        /* In theory fbx supports multiple keyframes within one blend shape
         * channel; we only take the final target keyframe. */
        if (fchan->target_shape == nullptr) {
          continue;
        }

        if (mesh_key == nullptr) {
          mesh_key = BKE_key_add(this->bmain, &mesh->id);
          mesh_key->type = KEY_RELATIVE;
          mesh->key = mesh_key;

          KeyBlock *kb = BKE_keyblock_add(mesh_key, "Basis");
          BKE_keyblock_convert_from_mesh(mesh, mesh_key, kb);
        }

        KeyBlock *kb = BKE_keyblock_add(mesh_key, fchan->target_shape->name.data);
        kb->curval = fchan->weight;
        BKE_keyblock_convert_from_mesh(mesh, mesh_key, kb);
        float3 *kb_data = (float3 *)kb->data;
        for (int i = 0; i < fchan->target_shape->num_offsets; i++) {
          int idx = fchan->target_shape->offset_vertices[i];
          const ufbx_vec3 &delta = fchan->target_shape->position_offsets[i];
          kb_data[idx] += float3(delta.x, delta.y, delta.z);
        }

        this->mapping.el_to_shape_key.add(&fchan->element, mesh_key);
      }
    }

    /* Create objects that use this mesh. */
    for (const ufbx_node *node : fmesh->instances) {
      Object *obj = BKE_object_add_only_object(this->bmain, OB_MESH, get_fbx_name(node->name));
      obj->data = mesh_main;

      if (mesh_key != nullptr) {
        obj->shapenr = 1;
      }

      /* Add vertex groups to object. */
      if (fmesh->skin_deformers.count > 0) {
        const ufbx_skin_deformer *skin = fmesh->skin_deformers[0];
        if (skin != nullptr) {
          for (const ufbx_skin_cluster *fcluster : skin->clusters) {
            const char *bone_name = get_fbx_name(fcluster->bone_node->name, "Bone");
            BKE_object_defgroup_add_name(obj, bone_name);
          }
        }

        /* Add armature modifier, and create the armature object. */
        Object *arm_obj = this->create_armature_for_deformer(*skin);
        BLI_assert(arm_obj == this->mapping.el_to_object.lookup_default(&skin->element, nullptr));
        ModifierData *md = BKE_modifier_new(eModifierType_Armature);
        STRNCPY(md->name, get_fbx_name(skin->name, "Armature"));
        BLI_addtail(&obj->modifiers, md);
        BKE_modifiers_persistent_uid_init(*obj, *md);

        ArmatureModifierData *ad = reinterpret_cast<ArmatureModifierData *>(md);
        ad->object = arm_obj;

        obj->parent = arm_obj;
      }

      /* Assign materials. */
      if (fmesh->materials.count > 0 && node->materials.count == fmesh->materials.count) {
        int mat_index = 0;
        for (int mi = 0; mi < fmesh->materials.count; mi++) {
          const ufbx_material *mesh_fmat = fmesh->materials[mi];
          const ufbx_material *node_fmat = node->materials[mi];
          Material *mesh_mat = this->mapping.mat_to_material.lookup_default(mesh_fmat, nullptr);
          Material *node_mat = this->mapping.mat_to_material.lookup_default(node_fmat, nullptr);
          if (mesh_mat != nullptr) {
            mat_index++;
            /* Assign material to the data block. */
            BKE_object_material_assign_single_obdata(this->bmain, obj, mesh_mat, mat_index);

            /* If object material is different, assign that to object. */
            if (node_mat != nullptr && node_mat != mesh_mat) {
              BKE_object_material_assign(
                  this->bmain, obj, node_mat, mat_index, BKE_MAT_ASSIGN_OBJECT);
            }
          }
        }
        if (mat_index > 0) {
          obj->actcol = 1;
        }
      }

      /* Subdivision. */
      if (this->params.use_subsurf &&
          fmesh->subdivision_display_mode != UFBX_SUBDIVISION_DISPLAY_DISABLED &&
          (fmesh->subdivision_preview_levels > 0 || fmesh->subdivision_render_levels > 0))
      {
        ModifierData *md = BKE_modifier_new(eModifierType_Subsurf);
        STRNCPY(md->name, "subsurf");
        BLI_addtail(&obj->modifiers, md);
        BKE_modifiers_persistent_uid_init(*obj, *md);

        SubsurfModifierData *ssd = reinterpret_cast<SubsurfModifierData *>(md);
        ssd->subdivType = SUBSURF_TYPE_CATMULL_CLARK;
        ssd->levels = fmesh->subdivision_preview_levels;
        ssd->renderLevels = fmesh->subdivision_render_levels;
        ssd->boundary_smooth = fmesh->subdivision_boundary ==
                                       UFBX_SUBDIVISION_BOUNDARY_SHARP_CORNERS ?
                                   SUBSURF_BOUNDARY_SMOOTH_PRESERVE_CORNERS :
                                   SUBSURF_BOUNDARY_SMOOTH_ALL;
      }

      if (this->params.use_custom_props) {
        read_custom_properties(node->props, obj->id);
      }
      node_matrix_to_obj(node, obj);
      this->mapping.el_to_object.add(&node->element, obj);
    }
  }
}

void FbxImportContext::import_cameras()
{
  for (ufbx_camera *fcam : this->fbx.cameras) {
    if (fcam->instances.count == 0) {
      continue; /* Ignore if not used by any objects. */
    }
    const ufbx_node *node = fcam->instances[0];

    Camera *bcam = BKE_camera_add(this->bmain, get_fbx_name(fcam->name, "Camera"));
    if (this->params.use_custom_props) {
      read_custom_properties(fcam->props, bcam->id);
    }

    bcam->type = fcam->projection_mode == UFBX_PROJECTION_MODE_ORTHOGRAPHIC ? CAM_ORTHO :
                                                                              CAM_PERSP;
    bcam->dof.focus_distance = ufbx_find_real(&fcam->props, "FocusDistance", 10.0f) *
                               this->fbx.metadata.geometry_scale * this->fbx.metadata.root_scale;
    if (ufbx_find_bool(&fcam->props, "UseDepthOfField", false)) {
      bcam->dof.flag |= CAM_DOF_ENABLED;
    }
    bcam->lens = fcam->focal_length_mm;
    constexpr double m_to_in = 0.0393700787;
    bcam->sensor_x = fcam->film_size_inch.x / m_to_in;
    bcam->sensor_y = fcam->film_size_inch.y / m_to_in;

    /* Note: do not use `fcam->orthographic_extent` to match Python importer behavior, which was
     * not taking ortho units into account. */
    bcam->ortho_scale = ufbx_find_real(&fcam->props, "OrthoZoom", 1.0);

    bcam->shiftx = ufbx_find_real(&fcam->props, "FilmOffsetX", 0.0) / (m_to_in * bcam->sensor_x);
    bcam->shifty = ufbx_find_real(&fcam->props, "FilmOffsetY", 0.0) / (m_to_in * bcam->sensor_x);
    bcam->clip_start = fcam->near_plane * this->fbx.metadata.root_scale;
    bcam->clip_end = fcam->far_plane * this->fbx.metadata.root_scale;

    Object *obj = BKE_object_add_only_object(this->bmain, OB_CAMERA, get_fbx_name(node->name));
    obj->data = bcam;

    if (this->params.use_custom_props) {
      read_custom_properties(node->props, obj->id);
    }
    node_matrix_to_obj(node, obj);
    this->mapping.el_to_object.add(&node->element, obj);
  }
}

void FbxImportContext::import_lights()
{
  for (const ufbx_light *flight : this->fbx.lights) {
    if (flight->instances.count == 0) {
      continue; /* Ignore if not used by any objects. */
    }
    const ufbx_node *node = flight->instances[0];

    Light *lamp = BKE_light_add(this->bmain, get_fbx_name(flight->name, "Light"));
    if (this->params.use_custom_props) {
      read_custom_properties(flight->props, lamp->id);
    }
    switch (flight->type) {
      case UFBX_LIGHT_POINT:
        lamp->type = LA_LOCAL;
        break;
      case UFBX_LIGHT_DIRECTIONAL:
        lamp->type = LA_SUN;
        break;
      case UFBX_LIGHT_SPOT:
        lamp->type = LA_SPOT;
        lamp->spotsize = DEG2RAD(flight->outer_angle);
        lamp->spotblend = 1.0f - flight->inner_angle / flight->outer_angle;
        break;
      default:
        break;
    }

    lamp->r = flight->color.x;
    lamp->g = flight->color.y;
    lamp->b = flight->color.z;
    lamp->energy = flight->intensity;
    if (flight->cast_shadows) {
      lamp->mode |= LA_SHADOW;
    }
    //@TODO: if hasattr(lamp, "cycles"): lamp.cycles.cast_shadow = lamp.use_shadow

    Object *obj = BKE_object_add_only_object(this->bmain, OB_LAMP, get_fbx_name(node->name));
    obj->data = lamp;

    if (this->params.use_custom_props) {
      read_custom_properties(node->props, obj->id);
    }
    node_matrix_to_obj(node, obj);
    this->mapping.el_to_object.add(&node->element, obj);
  }
}

Object *FbxImportContext::create_armature_for_deformer(const ufbx_skin_deformer &fskin)
{
  Object *obj = this->mapping.el_to_object.lookup_default(&fskin.element, nullptr);
  if (obj != nullptr && obj->type == OB_ARMATURE) {
    return obj;
  }

  /* Get list of bones sorted in a way so that parents are before children. */
  Vector<const ufbx_skin_cluster *> fbones(fskin.clusters.begin(), fskin.clusters.end());
  std::sort(
      fbones.begin(), fbones.end(), [](const ufbx_skin_cluster *a, const ufbx_skin_cluster *b) {
        return a->bone_node->node_depth < b->bone_node->node_depth;
      });
  /* Figure out "most root" parent of all the bones. */
  const ufbx_node *armature_parent = nullptr;
  const ufbx_node *armature_root = nullptr;
  if (!fbones.is_empty()) {
    armature_root = fbones[0]->bone_node;
    armature_parent = fbones[0]->bone_node->parent;
  }

  /* Create armature. */
  bArmature *arm = BKE_armature_add(bmain, get_fbx_name(fskin.name, "Armature"));
  obj = BKE_object_add_only_object(
      this->bmain,
      OB_ARMATURE,
      get_fbx_name(armature_parent ? armature_parent->name : fskin.name, "Armature"));
  if (this->params.use_custom_props) {
    read_custom_properties(fskin.props, arm->id);
  }
  obj->data = arm;

  /* Set armature object transform. */
  const ufbx_node *armature_xform_node = nullptr;
  if (armature_parent != nullptr && armature_parent->attrib_type == UFBX_ELEMENT_EMPTY) {
    /* If the root-most parent of the bones is an empty, use that as the armature object transform,
     * and mark that empty as processed. */
    armature_xform_node = armature_parent;
    node_matrix_to_obj(armature_parent, obj);
    this->mapping.el_to_object.add(&armature_parent->element, obj);
  }
  else if (armature_root != nullptr) {
    /* Otherwise use root bone node transform as armature transform. */
    armature_xform_node = armature_root;
    node_matrix_to_obj(armature_root, obj);
  }

  ufbx_matrix world_to_arm = ufbx_identity_matrix;
  if (armature_xform_node != nullptr) {
    world_to_arm = ufbx_matrix_invert(&armature_xform_node->node_to_world);
  }

  /* Create bones. */
  ED_armature_to_edit(arm);

  Map<const ufbx_node *, EditBone *> node_to_bone;
  node_to_bone.reserve(fbones.size());

  for (const ufbx_skin_cluster *fbone : fbones) {

    EditBone *bone = ED_armature_ebone_add(arm, get_fbx_name(fbone->bone_node->name, "Bone"));
    node_to_bone.add(fbone->bone_node, bone);
    /* For all bones, record the whole armature as the owning object. */
    this->mapping.el_to_object.add(&fbone->bone_node->element, obj);

    //@TODO: custom props
    bone->flag |= BONE_SELECTED;

    /* Get average distance to children. */
    float bone_size = 0.0f;
    int child_bone_count = 0;
    for (const ufbx_node *child : fbone->bone_node->children) {
      if (child->bone) {
        ufbx_vec3 pos = child->local_transform.translation;
        bone_size += math::length(float3(pos.x, pos.y, pos.z));
        child_bone_count++;
      }
    }
    if (child_bone_count > 0) {
      bone_size /= child_bone_count;
    }

    /* Zero length bones are automatically collapsed into their parent when you leave edit mode,
     * so enforce a minimum length. */
    bone_size = math::max(bone_size, 0.01f);
    bone->tail[0] = 0.0f;
    bone->tail[1] = bone_size;
    bone->tail[2] = 0.0f;

    /* Set bind matrix. */
    float bind_matrix[4][4];
    ufbx_matrix bone_to_arm = ufbx_matrix_mul(&world_to_arm, &fbone->bind_to_world);
    matrix_to_m44(bone_to_arm, bind_matrix);
    normalize_m4(bind_matrix);
    ED_armature_ebone_from_mat4(bone, bind_matrix);

    bool added = this->mapping.bone_to_bind_matrix.add(fbone->bone_node,
                                                       /*fbone->bind_to_world*/ bone_to_arm);
    BLI_assert_msg(added, "fbx: same bone node used more than once?");
    UNUSED_VARS(added);

    /* Set bone parent. */
    const ufbx_node *parent = fbone->bone_node->parent;
    if (parent != nullptr) {
      EditBone *parent_bone = node_to_bone.lookup_default(parent, nullptr);
      if (parent_bone != nullptr) {
        bone->parent = parent_bone;
      }
    }
  }

  ED_armature_from_edit(this->bmain, arm);
  ED_armature_edit_free(arm);

  this->mapping.el_to_object.add(&fskin.element, obj);
  return obj;
}

void FbxImportContext::import_empties()
{
  /* Make sure that objects we have already created have their parent hierachy as empties. */
  Map<const ufbx_node *, Object *> node_to_empty;
  for (const auto &item : this->mapping.el_to_object.items()) {
    const ufbx_node *node = ufbx_as_node(item.key);
    if (node == nullptr) {
      continue;
    }
    if (node->bone != nullptr) {
      /* No need to create empty parents for bones. */
      continue;
    }
    while (node != nullptr && !node->is_root) {
      if (!this->mapping.el_to_object.contains(&node->element) && !node_to_empty.contains(node)) {
        Object *obj = BKE_object_add_only_object(this->bmain, OB_EMPTY, get_fbx_name(node->name));
        obj->data = nullptr;
        if (this->params.use_custom_props) {
          read_custom_properties(node->props, obj->id);
        }
        node_matrix_to_obj(node, obj);
        node_to_empty.add(node, obj);
      }
      node = node->parent;
    }
  }

  /* Create all the empties. */
  for (const ufbx_empty *fempty : this->fbx.empties) {
    if (fempty->instances.count == 0) {
      continue; /* Ignore if not used by any objects. */
    }
    const ufbx_node *node = fempty->instances[0];
    if (!this->mapping.el_to_object.contains(&node->element) && !node_to_empty.contains(node)) {
      Object *obj = BKE_object_add_only_object(this->bmain, OB_EMPTY, get_fbx_name(node->name));
      obj->data = nullptr;
      if (this->params.use_custom_props) {
        read_custom_properties(node->props, obj->id);
      }
      node_matrix_to_obj(node, obj);
      node_to_empty.add(node, obj);
    }
  }

  /* Add all the created empties to the node->object map. */
  for (const auto &item : node_to_empty.items()) {
    this->mapping.el_to_object.add(&item.key->element, item.value);
  }
}

void FbxImportContext::import_animation(double fps)
{
  if (this->params.use_anim) {
    io::fbx::import_animations(
        *this->bmain, this->fbx, this->mapping, fps, this->params.anim_offset);
  }
}

void FbxImportContext::setup_hierarchy()
{
  for (const auto &item : this->mapping.el_to_object.items()) {
    if (item.value->parent != nullptr) {
      continue; /* Parent is already set up (e.g. armature). */
    }
    const ufbx_node *node = ufbx_as_node(item.key);
    if (node == nullptr) {
      continue;
    }
    if (node->bone != nullptr) {
      /* If this node is for a bone, do not try to setup object parenting for it
       * (the object for bone bones is whole armature). */
      continue;
    }
    if (node->parent) {
      Object *obj_par = this->mapping.el_to_object.lookup_default(&node->parent->element, nullptr);
      if (obj_par != nullptr && obj_par != item.value) {
        item.value->parent = obj_par;
      }
    }
  }
}

void importer_main(Main *bmain, Scene *scene, ViewLayer *view_layer, const FBXImportParams &params)
{
  UNUSED_VARS(bmain, view_layer, params);

  FILE *file = BLI_fopen(params.filepath, "rb");
  if (!file) {
    CLOG_ERROR(&LOG, "Failed to open FBX file '%s'\n", params.filepath);
    BKE_reportf(params.reports, RPT_ERROR, "FBX Import: Cannot open file '%s'", params.filepath);
    return;
  }

  ufbx_load_opts opts = {};
  opts.filename.data = params.filepath;
  opts.filename.length = strlen(params.filepath);
  opts.evaluate_skinning = false;
  opts.evaluate_caches = false;
  opts.load_external_files = false;
  opts.clean_skin_weights = true;
  opts.use_blender_pbr_material = true;
  //@TODO: axes according to import settings
  opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
  opts.target_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
  opts.target_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Z;
  opts.target_axes.front = UFBX_COORDINATE_AXIS_NEGATIVE_Y;
  opts.target_unit_meters = 1.0f;

  opts.target_camera_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
  opts.target_camera_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
  opts.target_camera_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;
  opts.target_light_axes.right = UFBX_COORDINATE_AXIS_POSITIVE_X;
  opts.target_light_axes.up = UFBX_COORDINATE_AXIS_POSITIVE_Y;
  opts.target_light_axes.front = UFBX_COORDINATE_AXIS_POSITIVE_Z;

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
  ctx.import_globals(scene);
  ctx.import_materials();
  ctx.import_meshes();
  ctx.import_cameras();
  ctx.import_lights();
  ctx.import_empties();
  ctx.import_animation(FPS);
  ctx.setup_hierarchy();

  ufbx_free_scene(fbx);

  /* Add objects to collection. */
  for (Object *obj : ctx.mapping.el_to_object.values()) {
    BKE_collection_object_add(bmain, lc->collection, obj);
  }

  /* Select objects, sync layers etc. */
  BKE_view_layer_base_deselect_all(scene, view_layer);
  BKE_view_layer_synced_ensure(scene, view_layer);
  for (Object *obj : ctx.mapping.el_to_object.values()) {
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
