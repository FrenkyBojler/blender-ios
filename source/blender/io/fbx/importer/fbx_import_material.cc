/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "DNA_material_types.h"

#include "NOD_shader.h"

#include "IMB_imbuf_types.hh"

#include "fbx_import_material.hh"

#include "ufbx.h"

namespace blender::io::fbx {

/* Nodes are arranged in columns by type, with manually placed x coordinates
 * based on node widths. */
static constexpr float node_locx_texcoord = -880.0f;
static constexpr float node_locx_mapping = -680.0f;
static constexpr float node_locx_image = -480.0f;
static constexpr float node_locx_normalmap = -200.0f;
static constexpr float node_locx_bsdf = 0.0f;
static constexpr float node_locx_output = 280.0f;

/* Nodes are arranged in rows; one row for each image being used. */
static constexpr float node_locy_top = 300.0f;
static constexpr float node_locy_step = 300.0f;

/* Add a node of the given type at the given location. */
static bNode *add_node(bNodeTree *ntree, int type, float x, float y)
{
  bNode *node = bke::node_add_static_node(nullptr, *ntree, type);
  node->location[0] = x;
  node->location[1] = y;
  return node;
}

static void link_sockets(bNodeTree *ntree,
                         bNode *from_node,
                         const char *from_node_id,
                         bNode *to_node,
                         const char *to_node_id)
{
  bNodeSocket *from_sock{bke::node_find_socket(*from_node, SOCK_OUT, from_node_id)};
  bNodeSocket *to_sock{bke::node_find_socket(*to_node, SOCK_IN, to_node_id)};
  BLI_assert(from_sock && to_sock);
  bke::node_add_link(*ntree, *from_node, *from_sock, *to_node, *to_sock);
}

static void set_socket_float(const char *socket_id, const float value, bNode *node)
{
  bNodeSocket *socket{bke::node_find_socket(*node, SOCK_IN, socket_id)};
  BLI_assert(socket && socket->type == SOCK_FLOAT);
  bNodeSocketValueFloat *dst = socket->default_value_typed<bNodeSocketValueFloat>();
  dst->value = value;
}

static void set_socket_rgb(const char *socket_id, float vr, float vg, float vb, bNode *node)
{
  bNodeSocket *socket{bke::node_find_socket(*node, SOCK_IN, socket_id)};
  BLI_assert(socket && socket->type == SOCK_RGBA);
  bNodeSocketValueRGBA *dst = socket->default_value_typed<bNodeSocketValueRGBA>();
  dst->value[0] = vr;
  dst->value[1] = vg;
  dst->value[2] = vb;
  dst->value[3] = 1.0f;
}

static void set_socket_vector(const char *socket_id, float vx, float vy, float vz, bNode *node)
{
  bNodeSocket *socket{bke::node_find_socket(*node, SOCK_IN, socket_id)};
  BLI_assert(socket && socket->type == SOCK_VECTOR);
  bNodeSocketValueVector *dst = socket->default_value_typed<bNodeSocketValueVector>();
  dst->value[0] = vx;
  dst->value[1] = vy;
  dst->value[2] = vz;
}

static void set_bsdf_socket_values(bNode *bsdf, Material *mat, const ufbx_material &fmat)
{
  /* It would be better to use ufbx_material_pbr_maps to get better import
   * of PBR properties from various applications. However for now we do it
   * manually to match the FBX importer behavior. */

  /* Base color. */
  ufbx_vec3 diff_color = {1, 1, 1};
  if (fmat.fbx.diffuse_color.has_value) {
    diff_color = fmat.fbx.diffuse_color.value_vec3;
  }
  diff_color.x = math::clamp(diff_color.x, 0.0, 1.0);
  diff_color.y = math::clamp(diff_color.y, 0.0, 1.0);
  diff_color.z = math::clamp(diff_color.z, 0.0, 1.0);
  set_socket_rgb("Base Color", diff_color.x, diff_color.y, diff_color.z, bsdf);
  mat->r = diff_color.x; /* For viewport shading. */
  mat->g = diff_color.y;
  mat->b = diff_color.z;

  /* Specular IOR. */
  float specular = 0.25f;
  if (fmat.fbx.specular_factor.has_value) {
    specular = fmat.fbx.specular_factor.value_real;
  }
  specular *= 2.0f;
  specular = math::clamp(specular, 0.0f, 1.0f);
  set_socket_float("Specular IOR Level", specular, bsdf);
  mat->spec = specular; /* For viewport shading. */

  /* Rougness: empirical map from FBX shininess (0..100) to (1..0) rougness. */
  /* Note: to match python importer, only manually query for Shininess property;
   * ufbx queries for both Shininess and ShininessExponent for mat.fbx.specular_exponent */
  float shininess = ufbx_find_real(&fmat.props, "Shininess", 20.0f);
  shininess = math::clamp(shininess, 0.0f, 100.0f);
  float roughness = 1.0f - (sqrtf(shininess)) / 10.0f;
  roughness = math::clamp(roughness, 0.0f, 1.0f);
  set_socket_float("Roughness", roughness, bsdf);
  mat->roughness = roughness; /* For viewport shading. */

  /* Alpha: complex logic based on TransparencyFactor, Opacity or TransparentColor. */
  float alpha = 1.0f;
  if (fmat.fbx.specular_exponent.has_value) {
    alpha = 1.0f - fmat.fbx.transparency_factor.value_real;
  }
  if (alpha == 0.0f || alpha == 1.0f) {
    /*@TODO: handle "Opacity" being present like in Python importer. */
    if (fmat.fbx.transparency_color.has_value) {
      alpha = 1.0f - fmat.fbx.transparency_color.value_vec3.x;
    }
  }
  set_socket_float("Alpha", alpha, bsdf);

  /* Metallic. */
  float metallic = 0.0f;
  if (fmat.fbx.reflection_factor.has_value) {
    metallic = fmat.fbx.reflection_factor.value_real;
  }
  metallic = math::clamp(metallic, 0.0f, 1.0f);
  set_socket_float("Metallic", metallic, bsdf);
  mat->metallic = metallic; /* For viewport shading. */

  /* Emission. */
  float emis_strength = 1.0f;
  if (fmat.fbx.emission_factor.has_value) {
    emis_strength = math::clamp(fmat.fbx.emission_factor.value_real, 0.0, 1000000.0);
  }
  set_socket_float("Emission Strength", emis_strength, bsdf);

  ufbx_vec3 emis_color = {0, 0, 0};
  if (fmat.fbx.emission_color.has_value) {
    emis_color = fmat.fbx.emission_color.value_vec3;
    emis_color.x = math::clamp(emis_color.x, 0.0, 1000000.0);
    emis_color.y = math::clamp(emis_color.y, 0.0, 1000000.0);
    emis_color.z = math::clamp(emis_color.z, 0.0, 1000000.0);
  }
  set_socket_rgb("Emission Color", emis_color.x, emis_color.y, emis_color.z, bsdf);
}

static Image *create_placeholder_image(Main *bmain, const std::string &path)
{
  const float color[4] = {0, 0, 0, 1};
  const char *name = BLI_path_basename(path.c_str());
  Image *image = BKE_image_add_generated(
      bmain, 32, 32, name, 24, false, IMA_GENTYPE_BLANK, color, false, false, false);
  STRNCPY(image->filepath, path.c_str());
  image->source = IMA_SRC_FILE;
  return image;
}

static Image *load_texture_image(Main *bmain, const std::string &file_dir, const ufbx_texture &tex)
{
  /* Check with filename directly. */
  Image *image = BKE_image_load_exists(bmain, tex.filename.data);
  if (image == nullptr) {
    /* Try loading as a relative path. */
    std::string path = file_dir + "/" + tex.filename.data;
    image = BKE_image_load_exists(bmain, path.c_str());
    if (image == nullptr) {
      /* Try loading with absolute path from FBX. */
      image = BKE_image_load_exists(bmain, tex.absolute_filename.data);
    }
  }

  /* Use embedded data for this image, if we haven't done that yet. */
  if (tex.content.size > 0 && (image == nullptr || !BKE_image_has_packedfile(image))) {
    if (image == nullptr) {
      image = create_placeholder_image(bmain, tex.filename.data);
    }

    char *data_dup = static_cast<char *>(MEM_mallocN(tex.content.size, __func__));
    memcpy(data_dup, tex.content.data, tex.content.size);
    BKE_image_packfiles_from_mem(nullptr, image, data_dup, tex.content.size);
  }

  return image;
}

static const char *ufbx_map_to_node_socket[UFBX_MATERIAL_FBX_MAP_COUNT] = {
    nullptr,              /* UFBX_MATERIAL_FBX_DIFFUSE_FACTOR */
    "Base Color",         /* UFBX_MATERIAL_FBX_DIFFUSE_COLOR */
    "Specular IOR Level", /* UFBX_MATERIAL_FBX_SPECULAR_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_SPECULAR_COLOR */
    "Roughness",          /* UFBX_MATERIAL_FBX_SPECULAR_EXPONENT */
    "Metallic",           /* UFBX_MATERIAL_FBX_REFLECTION_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_REFLECTION_COLOR */
    "Alpha",              /* UFBX_MATERIAL_FBX_TRANSPARENCY_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_TRANSPARENCY_COLOR */
    "Emission Strength",  /* UFBX_MATERIAL_FBX_EMISSION_FACTOR */
    "Emission Color",     /* UFBX_MATERIAL_FBX_EMISSION_COLOR */
    nullptr,              /* UFBX_MATERIAL_FBX_AMBIENT_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_AMBIENT_COLOR */
    "Normal",             /* UFBX_MATERIAL_FBX_NORMAL_MAP */
    nullptr,              /* UFBX_MATERIAL_FBX_BUMP */
    nullptr,              /* UFBX_MATERIAL_FBX_BUMP_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_DISPLACEMENT_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_DISPLACEMENT */
    nullptr,              /* UFBX_MATERIAL_FBX_VECTOR_DISPLACEMENT_FACTOR */
    nullptr,              /* UFBX_MATERIAL_FBX_VECTOR_DISPLACEMENT */
};

static void add_image_textures(Main *bmain,
                               const std::string &file_dir,
                               bNodeTree *ntree,
                               bNode *bsdf,
                               Material *mat,
                               const ufbx_material &fmat)
{
  float node_locy = node_locy_top;
  for (int slot = 0; slot < UFBX_MATERIAL_FBX_MAP_COUNT; slot++) {
    const char *socket_name = ufbx_map_to_node_socket[slot];
    if (socket_name == nullptr) {
      /* We do not support this texture. */
      continue;
    }

    const ufbx_texture *ftex = fmat.fbx.maps[slot].texture;
    if (ftex == nullptr || !fmat.fbx.maps[slot].texture_enabled) {
      /* No texture used for this slot. */
      continue;
    }

    Image *image = load_texture_image(bmain, file_dir, *ftex);
    if (image == nullptr) {
      /* Texture not found. */
      continue;
    }

    /* We have a valid texture, add node for it and any UV transformations if needed. */
    bNode *image_node = add_node(ntree, SH_NODE_TEX_IMAGE, node_locx_image, node_locy);
    BLI_assert(image_node);
    image_node->id = &image->id;
    NodeTexImage *tex_image = static_cast<NodeTexImage *>(image_node->storage);

    /* Wrap mode. */
    tex_image->extension = SHD_IMAGE_EXTENSION_REPEAT;
    if (ftex->wrap_u == UFBX_WRAP_CLAMP || ftex->wrap_v == UFBX_WRAP_CLAMP) {
      tex_image->extension = SHD_IMAGE_EXTENSION_EXTEND;
    }

    /* UV transform. */
    if (ftex->has_uv_transform) {
      /*@TODO: which UV set to use. */
      bNode *uvmap = add_node(ntree, SH_NODE_UVMAP, node_locx_texcoord, node_locy);
      bNode *mapping = add_node(ntree, SH_NODE_MAPPING, node_locx_mapping, node_locy);
      mapping->custom1 = TEXMAP_TYPE_TEXTURE;
      set_socket_vector("Location",
                        ftex->uv_transform.translation.x,
                        ftex->uv_transform.translation.y,
                        ftex->uv_transform.translation.z,
                        mapping);
      ufbx_vec3 rot = ufbx_quat_to_euler(ftex->uv_transform.rotation, UFBX_ROTATION_ORDER_XYZ);
      set_socket_vector("Rotation", -rot.x, -rot.y, -rot.z, mapping);
      set_socket_vector("Scale",
                        1.0f / ftex->uv_transform.scale.x,
                        1.0f / ftex->uv_transform.scale.y,
                        1.0f / ftex->uv_transform.scale.z,
                        mapping);

      link_sockets(ntree, uvmap, "UV", mapping, "Vector");
      link_sockets(ntree, mapping, "Vector", image_node, "Vector");
    }

    if (STREQ(socket_name, "Normal")) {
      bNode *normal_node = add_node(ntree, SH_NODE_NORMAL_MAP, node_locx_normalmap, node_locy);
      link_sockets(ntree, image_node, "Color", normal_node, "Color");
      link_sockets(ntree, normal_node, "Normal", bsdf, "Normal");

      /* Normal strength. */
      float normal_strength = 1.0f;
      if (fmat.fbx.bump_factor.has_value) {
        normal_strength = fmat.fbx.bump_factor.value_real;
      }
      set_socket_float("Strength", normal_strength, normal_node);
    }
    else {
      link_sockets(ntree, image_node, "Color", bsdf, socket_name);

      if (STREQ(socket_name, "Base Color")) {
        /* Link base color alpha (if we have one) to output alpha. */
        void *lock;
        ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, &lock);
        bool has_alpha = ibuf != nullptr && ibuf->planes == R_IMF_PLANES_RGBA;
        BKE_image_release_ibuf(image, ibuf, lock);

        if (has_alpha) {
          //@TODO: add to list of cycles decals
          link_sockets(ntree, image_node, "Alpha", bsdf, "Alpha");
        }
      }
    }

    /* Next layout row: goes downwards on the screen. */
    node_locy -= node_locy_step;
  }
}

Material *import_material(Main *bmain, const std::string &base_dir, const ufbx_material &fmat)
{
  Material *mat = BKE_material_add(bmain, fmat.name.data);
  id_us_min(&mat->id);

  mat->use_nodes = true;
  bNodeTree *ntree = blender::bke::node_tree_add_tree_embedded(
      nullptr, &mat->id, "Shader Nodetree", ntreeType_Shader->idname);
  bNode *bsdf = add_node(ntree, SH_NODE_BSDF_PRINCIPLED, node_locx_bsdf, node_locy_top);
  bNode *output = add_node(ntree, SH_NODE_OUTPUT_MATERIAL, node_locx_output, node_locy_top);
  set_bsdf_socket_values(bsdf, mat, fmat);
  add_image_textures(bmain, base_dir, ntree, bsdf, mat, fmat);
  link_sockets(ntree, bsdf, "BSDF", output, "Surface");
  bke::node_set_active(*ntree, *output);

  mat->nodetree = ntree;

  BKE_ntree_update_after_single_tree_change(*bmain, *mat->nodetree);

  return mat;
}

}  // namespace blender::io::fbx
