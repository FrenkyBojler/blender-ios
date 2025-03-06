/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Manages materials, lights and textures.
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_world_types.h"

#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"

#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"

#include "NOD_shader.h"

#include "GPU_material.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_uniform_buffer.hh"

#include "DRW_engine.hh"

#include "gpu_codegen.hh"
#include "gpu_node_graph.hh"

#include "atomic_ops.h"

void gpu_material_ramp_texture_build(GPUMaterial *mat);
void gpu_material_sky_texture_build(GPUMaterial *mat);

/* Structs */
#define MAX_COLOR_BAND 128
#define MAX_GPU_SKIES 8

struct GPUColorBandBuilder {
  float pixels[MAX_COLOR_BAND][CM_TABLE + 1][4];
  int current_layer;
};

struct GPUSkyBuilder {
  float pixels[MAX_GPU_SKIES][GPU_SKY_WIDTH * GPU_SKY_HEIGHT][4];
  int current_layer;
};

struct GPUMaterial {
  /* Contains #GPUShader and source code for deferred compilation.
   * Can be shared between materials sharing same node-tree topology. */
  GPUPass *pass;
  /* Optimized GPUPass, situationally compiled after initial pass for optimal realtime performance.
   * This shader variant bakes dynamic uniform data as constant. This variant will not use
   * the ubo, and instead bake constants directly into the shader source. */
  GPUPass *optimized_pass;

  double creation_time;

  /** UBOs for this material parameters. */
  GPUUniformBuf *ubo;
  /** Some flags about the nodetree & the needed resources. */
  eGPUMaterialFlag flag;
  /** The engine type this material is compiled for. */
  eGPUMaterialEngine engine;
  /* Identify shader variations (shadow, probe, world background...) */
  uint64_t uuid;
  /* Number of generated function. */
  int generated_function_len;

  /** Source material, might be null. */
  Material *ma;
  /** 1D Texture array containing all color bands. */
  GPUTexture *coba_tex;
  /** Builder for coba_tex. */
  GPUColorBandBuilder *coba_builder;
  /** 2D Texture array containing all sky textures. */
  GPUTexture *sky_tex;
  /** Builder for sky_tex. */
  GPUSkyBuilder *sky_builder;
  /* Low level node graph(s). Also contains resources needed by the material. */
  GPUNodeGraph graph;

  bool has_surface_output;
  bool has_volume_output;
  bool has_displacement_output;

  // TODO: Remove?
  uint32_t refcount;

  std::string name;
};

/* Functions */

void GPU_material_free_single(GPUMaterial *material)
{
  bool do_free = atomic_sub_and_fetch_uint32(&material->refcount, 1) == 0;
  if (!do_free) {
    return;
  }

  gpu_node_graph_free(&material->graph);

  if (material->optimized_pass != nullptr) {
    GPU_pass_release(material->optimized_pass);
  }
  if (material->pass != nullptr) {
    GPU_pass_release(material->pass);
  }
  if (material->ubo != nullptr) {
    GPU_uniformbuf_free(material->ubo);
  }
  if (material->coba_builder != nullptr) {
    MEM_freeN(material->coba_builder);
  }
  if (material->coba_tex != nullptr) {
    GPU_texture_free(material->coba_tex);
  }
  if (material->sky_tex != nullptr) {
    GPU_texture_free(material->sky_tex);
  }
  MEM_delete(material);
}

void GPU_material_free(ListBase *gpumaterial)
{
  LISTBASE_FOREACH (LinkData *, link, gpumaterial) {
    GPUMaterial *material = static_cast<GPUMaterial *>(link->data);
    GPU_material_free_single(material);
  }
  BLI_freelistN(gpumaterial);
}

void GPU_materials_free(Main *bmain)
{
  LISTBASE_FOREACH (Material *, ma, &bmain->materials) {
    GPU_material_free(&ma->gpumaterial);
  }

  LISTBASE_FOREACH (World *, wo, &bmain->worlds) {
    GPU_material_free(&wo->gpumaterial);
  }

  BKE_material_defaults_free_gpu();
}

GPUPass *GPU_material_get_pass(GPUMaterial *material)
{
  /* If an optimized pass variant is available, and optimization is
   * flagged as complete, we use this one instead. */
  return GPU_material_optimization_status(material) == GPU_MAT_OPTIMIZATION_SUCCESS ?
             material->optimized_pass :
             material->pass;
}

GPUShader *GPU_material_get_shader(GPUMaterial *material)
{
  return GPU_pass_shader_get(GPU_material_get_pass(material));
}

const char *GPU_material_get_name(GPUMaterial *material)
{
  return material->name.c_str();
}

Material *GPU_material_get_material(GPUMaterial *material)
{
  return material->ma;
}

GPUUniformBuf *GPU_material_uniform_buffer_get(GPUMaterial *material)
{
  return material->ubo;
}

void GPU_material_uniform_buffer_create(GPUMaterial *material, ListBase *inputs)
{
  material->ubo = GPU_uniformbuf_create_from_list(inputs, material->name.c_str());
}

ListBase GPU_material_attributes(const GPUMaterial *material)
{
  return material->graph.attributes;
}

ListBase GPU_material_textures(GPUMaterial *material)
{
  return material->graph.textures;
}

const GPUUniformAttrList *GPU_material_uniform_attributes(const GPUMaterial *material)
{
  const GPUUniformAttrList *attrs = &material->graph.uniform_attrs;
  return attrs->count > 0 ? attrs : nullptr;
}

const ListBase *GPU_material_layer_attributes(const GPUMaterial *material)
{
  const ListBase *attrs = &material->graph.layer_attrs;
  return !BLI_listbase_is_empty(attrs) ? attrs : nullptr;
}

GPUNodeGraph *gpu_material_node_graph(GPUMaterial *material)
{
  return &material->graph;
}

eGPUMaterialStatus GPU_material_status(GPUMaterial *mat)
{
  switch (GPU_pass_status(mat->pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_QUEUED;
    default:
      return GPU_MAT_FAILED;
  }
}

eGPUMaterialOptimizationStatus GPU_material_optimization_status(GPUMaterial *mat)
{
  if (!GPU_pass_should_optimize(mat->pass)) {
    return GPU_MAT_OPTIMIZATION_SKIP;
  }

  if (!mat->optimized_pass) {
    return GPU_MAT_OPTIMIZATION_READY;
  }

  switch (GPU_pass_status(mat->optimized_pass)) {
    case GPU_PASS_SUCCESS:
      return GPU_MAT_OPTIMIZATION_SUCCESS;
    case GPU_PASS_QUEUED:
      return GPU_MAT_OPTIMIZATION_QUEUED;
    default:
      BLI_assert_unreachable();
      return GPU_MAT_OPTIMIZATION_SKIP;
  }
}

bool GPU_material_optimization_ready(GPUMaterial *mat)
{
  /* Timer threshold before optimizations will be queued.
   * When materials are frequently being modified, optimization
   * can incur CPU overhead from excessive compilation.
   *
   * As the optimization is entirely asynchronous, it is still beneficial
   * to do this quickly to avoid build-up and improve runtime performance.
   * The threshold just prevents compilations being queued frame after frame. */
  const double optimization_time_threshold_s = 1.2;
  return ((BLI_time_now_seconds() - mat->creation_time) >= optimization_time_threshold_s);
}

bool GPU_material_has_surface_output(GPUMaterial *mat)
{
  return mat->has_surface_output;
}

bool GPU_material_has_volume_output(GPUMaterial *mat)
{
  return mat->has_volume_output;
}

bool GPU_material_has_displacement_output(GPUMaterial *mat)
{
  return mat->has_displacement_output;
}

bool GPU_material_flag_get(const GPUMaterial *mat, eGPUMaterialFlag flag)
{
  return (mat->flag & flag) != 0;
}

eGPUMaterialFlag GPU_material_flag(const GPUMaterial *mat)
{
  return mat->flag;
}

void GPU_material_flag_set(GPUMaterial *mat, eGPUMaterialFlag flag)
{
  if ((flag & GPU_MATFLAG_GLOSSY) && (mat->flag & GPU_MATFLAG_GLOSSY)) {
    /* Tag material using multiple glossy BSDF as using clear coat. */
    mat->flag |= GPU_MATFLAG_COAT;
  }
  mat->flag |= flag;
}

bool GPU_material_recalc_flag_get(GPUMaterial *mat)
{
  /* NOTE: Consumes the flags. */
  bool updated = (mat->flag & GPU_MATFLAG_UPDATED) != 0;
  mat->flag &= ~GPU_MATFLAG_UPDATED;
  return updated;
}

uint64_t GPU_material_uuid_get(GPUMaterial *mat)
{
  return mat->uuid;
}

/* Code generation */

GPUMaterial *GPU_material_from_nodetree(Material *ma,
                                        bNodeTree *ntree,
                                        ListBase *gpumaterials,
                                        const char *name,
                                        eGPUMaterialEngine engine,
                                        uint64_t shader_uuid,
                                        bool is_lookdev,
                                        GPUCodegenCallbackFn callback,
                                        void *thunk,
                                        GPUMaterialPassReplacementCallbackFn pass_replacement_cb)
{
  // TODO: This should lock?

  /* Search if this material is not already compiled. */
  LISTBASE_FOREACH (LinkData *, link, gpumaterials) {
    GPUMaterial *mat = (GPUMaterial *)link->data;
    if (mat->uuid == shader_uuid && mat->engine == engine) {
      // TODO: mat->flag |= GPU_MATFLAG_UPDATED; on finished compilation.
      return mat;
    }
  }

  GPUMaterial *mat = MEM_new<GPUMaterial>(__func__);
  mat->ma = ma;
  mat->engine = engine;
  mat->uuid = shader_uuid;
  mat->flag = GPU_MATFLAG_UPDATED;
  mat->graph.used_libraries = BLI_gset_new(
      BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "GPUNodeGraph.used_libraries");
  mat->refcount = 1;
  mat->name = name;
  if (is_lookdev) {
    mat->flag |= GPU_MATFLAG_LOOKDEV_HACK;
  }

  /* Localize tree to create links for reroute and mute. */
  bNodeTree *localtree = blender::bke::node_tree_localize(ntree, nullptr);
  ntreeGPUMaterialNodes(localtree, mat);

  gpu_material_ramp_texture_build(mat);
  gpu_material_sky_texture_build(mat);

  /* Use default material pass when possible. */
  if (GPUPass *default_pass = pass_replacement_cb ? pass_replacement_cb(thunk, mat) : nullptr) {
    mat->pass = default_pass;
    GPU_pass_acquire(mat->pass);
    /** WORKAROUND:
     * The node tree code is never executed in default replaced passes,
     * but the GPU validation will still complain if the node tree UBO is not bound.
     * So we create a dummy UBO with (at least) the size of the default material one (192 bytes).
     * We allocate 256 bytes to leave some room for future changes. */
    mat->ubo = GPU_uniformbuf_create_ex(256, nullptr, "Dummy UBO");
  }
  else {
    /* Create source code and search pass cache for an already compiled version. */
    mat->pass = GPU_generate_pass(mat, &mat->graph, engine, callback, thunk, false);
  }

  /* Determine whether we should generate an optimized variant of the graph.
   * Heuristic is based on complexity of default material pass and shader node graph. */
  if (GPU_pass_should_optimize(mat->pass)) {
    // TODO: Defer
    mat->optimized_pass = GPU_generate_pass(mat, &mat->graph, engine, callback, thunk, true);
  }

  // TODO: Is this safe to do here?
  gpu_node_graph_free_nodes(&mat->graph);
  /* Only free after GPU_pass_shader_get where GPUUniformBuf read data from the local tree. */
  blender::bke::node_tree_free_local_tree(localtree);
  BLI_assert(!localtree->id.py_instance); /* Or call #BKE_libblock_free_data_py. */
  MEM_freeN(localtree);

  /* Note that even if building the shader fails in some way, we want to keep
   * it to avoid trying to compile again and again, and simply do not use
   * the actual shader on drawing. */
  LinkData *link = static_cast<LinkData *>(MEM_callocN(sizeof(LinkData), "GPUMaterialLink"));
  link->data = mat;
  BLI_addtail(gpumaterials, link);

  return mat;
}

GPUMaterial *GPU_material_from_callbacks(eGPUMaterialEngine engine,
                                         ConstructGPUMaterialFn construct_function_cb,
                                         GPUCodegenCallbackFn generate_code_function_cb,
                                         void *thunk)
{
  /* Allocate a new material and its material graph, and initialize its reference count. */
  GPUMaterial *material = MEM_new<GPUMaterial>(__func__);
  material->graph.used_libraries = BLI_gset_new(
      BLI_ghashutil_ptrhash, BLI_ghashutil_ptrcmp, "GPUNodeGraph.used_libraries");
  material->refcount = 1;
  material->optimized_pass = nullptr;
  material->engine = engine;

  /* Construct the material graph by adding and linking the necessary GPU material nodes. */
  construct_function_cb(thunk, material);

  /* Create and initialize the texture storing color bands used by Ramp and Curve nodes. */
  gpu_material_ramp_texture_build(material);

  /* Lookup an existing pass in the cache or generate a new one. */
  material->pass = GPU_generate_pass(
      material, &material->graph, engine, generate_code_function_cb, thunk, false);
  material->optimized_pass = nullptr;

  /* Determine whether we should generate an optimized variant of the graph.
   * Heuristic is based on complexity of default material pass and shader node graph. */
  if (GPU_pass_should_optimize(material->pass)) {
    // TODO: Defer
    material->optimized_pass = GPU_generate_pass(
        material, &material->graph, engine, generate_code_function_cb, thunk, true);
  }

  // TODO: Is this safe to do here?
  gpu_node_graph_free_nodes(&material->graph);

  return material;
}

GPUTexture **gpu_material_sky_texture_layer_set(
    GPUMaterial *mat, int width, int height, const float *pixels, float *row)
{
  /* In order to put all sky textures into one 2D array texture,
   * we need them to be the same size. */
  BLI_assert(width == GPU_SKY_WIDTH);
  BLI_assert(height == GPU_SKY_HEIGHT);
  UNUSED_VARS_NDEBUG(width, height);

  if (mat->sky_builder == nullptr) {
    mat->sky_builder = static_cast<GPUSkyBuilder *>(
        MEM_mallocN(sizeof(GPUSkyBuilder), "GPUSkyBuilder"));
    mat->sky_builder->current_layer = 0;
  }

  int layer = mat->sky_builder->current_layer;
  *row = float(layer);

  if (*row == MAX_GPU_SKIES) {
    printf("Too many sky textures in shader!\n");
  }
  else {
    float *dst = (float *)mat->sky_builder->pixels[layer];
    memcpy(dst, pixels, sizeof(float) * GPU_SKY_WIDTH * GPU_SKY_HEIGHT * 4);
    mat->sky_builder->current_layer += 1;
  }

  return &mat->sky_tex;
}

GPUTexture **gpu_material_ramp_texture_row_set(GPUMaterial *mat,
                                               int size,
                                               const float *pixels,
                                               float *r_row)
{
  /* In order to put all the color-bands into one 1D array texture,
   * we need them to be the same size. */
  BLI_assert(size == CM_TABLE + 1);
  UNUSED_VARS_NDEBUG(size);

  if (mat->coba_builder == nullptr) {
    mat->coba_builder = static_cast<GPUColorBandBuilder *>(
        MEM_mallocN(sizeof(GPUColorBandBuilder), "GPUColorBandBuilder"));
    mat->coba_builder->current_layer = 0;
  }

  int layer = mat->coba_builder->current_layer;
  *r_row = float(layer);

  if (*r_row == MAX_COLOR_BAND) {
    printf("Too many color band in shader! Remove some Curve, Black Body or Color Ramp Node.\n");
  }
  else {
    float *dst = (float *)mat->coba_builder->pixels[layer];
    memcpy(dst, pixels, sizeof(float) * (CM_TABLE + 1) * 4);
    mat->coba_builder->current_layer += 1;
  }

  return &mat->coba_tex;
}

static void gpu_material_ramp_texture_build(GPUMaterial *mat)
{
  if (mat->coba_builder == nullptr) {
    return;
  }

  GPUColorBandBuilder *builder = mat->coba_builder;

  mat->coba_tex = GPU_texture_create_1d_array("mat_ramp",
                                              CM_TABLE + 1,
                                              builder->current_layer,
                                              1,
                                              GPU_RGBA16F,
                                              GPU_TEXTURE_USAGE_SHADER_READ,
                                              (float *)builder->pixels);

  MEM_freeN(builder);
  mat->coba_builder = nullptr;
}

static void gpu_material_sky_texture_build(GPUMaterial *mat)
{
  if (mat->sky_builder == nullptr) {
    return;
  }

  mat->sky_tex = GPU_texture_create_2d_array("mat_sky",
                                             GPU_SKY_WIDTH,
                                             GPU_SKY_HEIGHT,
                                             mat->sky_builder->current_layer,
                                             1,
                                             GPU_RGBA32F,
                                             GPU_TEXTURE_USAGE_SHADER_READ,
                                             (float *)mat->sky_builder->pixels);

  MEM_freeN(mat->sky_builder);
  mat->sky_builder = nullptr;
}

void GPU_material_output_surface(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_surface) {
    material->graph.outlink_surface = link;
    material->has_surface_output = true;
  }
}

void GPU_material_output_volume(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_volume) {
    material->graph.outlink_volume = link;
    material->has_volume_output = true;
  }
}

void GPU_material_output_displacement(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_displacement) {
    material->graph.outlink_displacement = link;
    material->has_displacement_output = true;
  }
}

void GPU_material_output_thickness(GPUMaterial *material, GPUNodeLink *link)
{
  if (!material->graph.outlink_thickness) {
    material->graph.outlink_thickness = link;
  }
}

void GPU_material_add_output_link_aov(GPUMaterial *material, GPUNodeLink *link, int hash)
{
  GPUNodeGraphOutputLink *aov_link = static_cast<GPUNodeGraphOutputLink *>(
      MEM_callocN(sizeof(GPUNodeGraphOutputLink), __func__));
  aov_link->outlink = link;
  aov_link->hash = hash;
  BLI_addtail(&material->graph.outlink_aovs, aov_link);
}

void GPU_material_add_output_link_composite(GPUMaterial *material, GPUNodeLink *link)
{
  GPUNodeGraphOutputLink *compositor_link = static_cast<GPUNodeGraphOutputLink *>(
      MEM_callocN(sizeof(GPUNodeGraphOutputLink), __func__));
  compositor_link->outlink = link;
  BLI_addtail(&material->graph.outlink_compositor, compositor_link);
}

char *GPU_material_split_sub_function(GPUMaterial *material,
                                      eGPUType return_type,
                                      GPUNodeLink **link)
{
  /* Force cast to return type. */
  switch (return_type) {
    case GPU_FLOAT:
      GPU_link(material, "set_value", *link, link);
      break;
    case GPU_VEC3:
      GPU_link(material, "set_rgb", *link, link);
      break;
    case GPU_VEC4:
      GPU_link(material, "set_rgba", *link, link);
      break;
    default:
      BLI_assert(0);
      break;
  }

  GPUNodeGraphFunctionLink *func_link = static_cast<GPUNodeGraphFunctionLink *>(
      MEM_callocN(sizeof(GPUNodeGraphFunctionLink), __func__));
  func_link->outlink = *link;
  SNPRINTF(func_link->name, "ntree_fn%d", material->generated_function_len++);
  BLI_addtail(&material->graph.material_functions, func_link);

  return func_link->name;
}
