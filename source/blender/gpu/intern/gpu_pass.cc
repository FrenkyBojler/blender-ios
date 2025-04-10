/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Convert material node-trees to GLSL.
 */

#include "MEM_guardedalloc.h"

#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "GPU_capabilities.hh"
#include "GPU_pass.hh"
#include "GPU_vertex_format.hh"
#include "gpu_codegen.hh"

#include <mutex>
#include <string>

using namespace blender;
using namespace blender::gpu::shader;

static bool gpu_pass_shader_validate(GPUCodegenCreateInfo *create_info, GPUShader *shader);

/* -------------------------------------------------------------------- */
/** \name GPUPass
 * \{ */

struct GPUPass {
  static inline std::atomic<uint64_t> compilation_counts = 0;

  GPUCodegenCreateInfo *create_info = nullptr;
  BatchHandle compilation_handle = 0;
  std::atomic<GPUShader *> shader = nullptr;
  std::atomic<eGPUPassStatus> status = GPU_PASS_QUEUED;
  /* Orphaned GPUPasses gets freed by the garbage collector. */
  std::atomic<int> refcount = 1;
  /* The last time the refcount was greater than 0. */
  int gc_timestamp = 0;

  uint64_t compilation_timestamp = 0;

  /** Hint that an optimized variant of this pass should be created.
   *  Based on a complexity heuristic from pass code generation. */
  bool should_optimize = false;
  bool is_optimization_pass = false;

  GPUPass(GPUCodegenCreateInfo *info, bool deferred_compilation, bool is_optimization_pass)
      : create_info(info), is_optimization_pass(is_optimization_pass)
  {

    if (is_optimization_pass && deferred_compilation) {
      // Defer until all non optimization passes are compiled.
      return;
    }

    GPUShaderCreateInfo *base_info = reinterpret_cast<GPUShaderCreateInfo *>(create_info);

    if (deferred_compilation) {
      compilation_handle = GPU_shader_batch_create_from_infos(
          Span<GPUShaderCreateInfo *>(&base_info, 1));
    }
    else {
      shader = GPU_shader_create_from_info(base_info);
      finalize_compilation();
    }
  }

  ~GPUPass()
  {
    if (compilation_handle) {
      // TODO: Add a way to remove handles from the compilation queue.
      finalize_compilation();
    }
    BLI_assert(create_info == nullptr || (is_optimization_pass && status == GPU_PASS_QUEUED));
    MEM_delete(create_info);
    GPU_SHADER_FREE_SAFE(shader);
  }

  void finalize_compilation()
  {
    BLI_assert_msg(create_info, "GPUPass::finalize_compilation() called more than once.");

    if (compilation_handle) {
      shader = GPU_shader_batch_finalize(compilation_handle).first();
    }

    compilation_timestamp = ++compilation_counts;

    if (shader && !gpu_pass_shader_validate(create_info, shader)) {
      fprintf(stderr, "GPUShader: error: too many samplers in shader.\n");
      GPU_shader_free(shader);
      shader = nullptr;
    }

    status = shader ? GPU_PASS_SUCCESS : GPU_PASS_FAILED;

    MEM_delete(create_info);
    create_info = nullptr;
  }

  void update()
  {
    update_compilation();
    update_gc_timestamp();
  }

  void update_compilation()
  {
    if (compilation_handle) {
      if (GPU_shader_batch_is_ready(compilation_handle)) {
        finalize_compilation();
      }
    }
    else if (status == GPU_PASS_QUEUED && refcount > 0) {
      BLI_assert(is_optimization_pass);
      GPUShaderCreateInfo *base_info = reinterpret_cast<GPUShaderCreateInfo *>(create_info);
      compilation_handle = GPU_shader_batch_create_from_infos(
          Span<GPUShaderCreateInfo *>(&base_info, 1));
    }
  }

  void update_gc_timestamp()
  {
    if (refcount == 0) {
      gc_timestamp++;
    }
    else {
      gc_timestamp = 0;
    }
  }

  bool should_gc(int gc_collect_rate)
  {
    return !compilation_handle && status != GPU_PASS_FAILED && gc_timestamp >= gc_collect_rate;
  }
};

eGPUPassStatus GPU_pass_status(GPUPass *pass)
{
  return pass->status;
}

bool GPU_pass_should_optimize(GPUPass *pass)
{
  /* Returns optimization heuristic prepared during initial codegen.
   * NOTE: Optimization currently limited to parallel backend as repeated compilations required for
   * material specialization causes impactful CPU stalls otherwise. */

  return pass->should_optimize && GPU_use_parallel_compilation();
}

GPUShader *GPU_pass_shader_get(GPUPass *pass)
{
  return pass->shader;
}

void GPU_pass_acquire(GPUPass *pass)
{
  int previous_refcount = pass->refcount++;
  BLI_assert(previous_refcount > 0);
}

void GPU_pass_release(GPUPass *pass)
{
  int previous_refcount = pass->refcount--;
  BLI_assert(previous_refcount > 0);
}

uint64_t GPU_pass_global_compilation_count()
{
  return GPUPass::compilation_counts;
}

uint64_t GPU_pass_compilation_timestamp(GPUPass *pass)
{
  if (pass) {
    return pass->compilation_timestamp;
  }

  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPUPass Cache
 *
 * Internal shader cache: This prevent the shader recompilation / stall when
 * using undo/redo AND also allows for GPUPass reuse if the Shader code is the
 * same for 2 different Materials. Unused GPUPasses are free by Garbage collection.
 * \{ */

class GPUPassCache {

  /* Number of updates with 0 users required before garbage collecting a pass.*/
  static constexpr int gc_collect_rate_ = 60;
  /* Number of updates without base compilations required before starting to compile optimization
   * passes.*/
  static constexpr int optimization_delay_ = 60;

  int updates_without_base_compilations_ = 0;

  Map<uint32_t, std::unique_ptr<GPUPass>> passes_[GPU_MAT_ENGINE_MAX][2 /*is_optimization_pass*/];
  std::mutex mutex_;

 public:
  void add(eGPUMaterialEngine engine,
           size_t hash,
           GPUCodegenCreateInfo *info,
           bool deferred_compilation,
           bool is_optimization_pass)
  {
    std::lock_guard lock(mutex_);

    // TODO: info->name was assigned in GPU_pass_compile,
    // but there can be more than one material per pass.
    passes_[engine][is_optimization_pass].add(
        hash, std::make_unique<GPUPass>(info, deferred_compilation, is_optimization_pass));
  };

  GPUPass *get(eGPUMaterialEngine engine,
               size_t hash,
               bool allow_deferred,
               bool is_optimization_pass)
  {
    std::lock_guard lock(mutex_);
    std::unique_ptr<GPUPass> *pass = passes_[engine][is_optimization_pass].lookup_ptr(hash);
    if (!allow_deferred && pass && pass->get()->status == GPU_PASS_QUEUED) {
      pass->get()->finalize_compilation();
    }
    return pass ? pass->get() : nullptr;
  }

  void update()
  {
    std::lock_guard lock(mutex_);

    bool base_passes_ready = true;

    /* Base Passes. */
    for (auto &engine_passes : passes_) {
      for (std::unique_ptr<GPUPass> &pass : engine_passes[false].values()) {
        pass->update();
        if (pass->status == GPU_PASS_QUEUED) {
          base_passes_ready = false;
        }
      }

      engine_passes[false].remove_if(
          [&](auto item) { return item.value->should_gc(gc_collect_rate_); });
    }

    /* Optimization Passes GC. */
    for (auto &engine_passes : passes_) {
      for (std::unique_ptr<GPUPass> &pass : engine_passes[true].values()) {
        pass->update_gc_timestamp();
      }

      engine_passes[true].remove_if(
          /* TODO: Use lower rate for optimization passes? */
          [&](auto item) { return item.value->should_gc(gc_collect_rate_); });
    }

    if (!base_passes_ready) {
      updates_without_base_compilations_ = 0;
      return;
    }

    if (++updates_without_base_compilations_ < optimization_delay_) {
      return;
    }

    /* Optimization Passes Compilation. */
    for (auto &engine_passes : passes_) {
      for (std::unique_ptr<GPUPass> &pass : engine_passes[true].values()) {
        pass->update_compilation();
      }
    }
  }
};

static GPUPassCache *g_cache = nullptr;

void GPU_pass_cache_init()
{
  g_cache = MEM_new<GPUPassCache>(__func__);
}

void GPU_pass_cache_update()
{
  g_cache->update();
}

void GPU_pass_cache_free()
{
  MEM_delete(g_cache);
  g_cache = nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compilation
 * \{ */

static bool gpu_pass_shader_validate(GPUCodegenCreateInfo *create_info, GPUShader *shader)
{
  /* NOTE: The only drawback of this method is that it will count a sampler
   * used in the fragment shader and only declared (but not used) in the vertex
   * shader as used by both. But this corner case is not happening for now. */
  // TODO: No longer true with vertex displacement? ^
  int active_samplers_len = 0;
  for (const ShaderCreateInfo::Resource &res : create_info->pass_resources_) {
    if (res.bind_type == ShaderCreateInfo::Resource::BindType::SAMPLER) {
      if (GPU_shader_get_uniform(shader, res.sampler.name.c_str()) != -1) {
        active_samplers_len++;
      }
    }
  }

  /* Validate against GPU limit. */
  if ((active_samplers_len > GPU_max_textures_frag()) ||
      (active_samplers_len > GPU_max_textures_vert()))
  {
    return false;
  }

  if (create_info->geometry_source_.is_empty() == false) {
    if (active_samplers_len > GPU_max_textures_geom()) {
      return false;
    }
  }

  return (active_samplers_len * 3 <= GPU_max_textures());
}

GPUPass *GPU_generate_pass(GPUMaterial *material,
                           GPUNodeGraph *graph,
                           eGPUMaterialEngine engine,
                           bool deferred_compilation,
                           GPUCodegenCallbackFn finalize_source_cb,
                           void *thunk,
                           bool optimize_graph)
{
  gpu_node_graph_prune_unused(graph);

  /* If Optimize flag is passed in, we are generating an optimized
   * variant of the GPUMaterial's GPUPass. */
  if (optimize_graph) {
    gpu_node_graph_optimize(graph);
  }

  /* Extract attributes before compiling so the generated VBOs are ready to accept the future
   * shader. */
  gpu_node_graph_finalize_uniform_attrs(graph);

  GPUCodegen codegen(material, graph);
  codegen.generate_graphs();
  codegen.generate_cryptomatte();

  GPUPass *pass = nullptr;

  if (!optimize_graph) {
    /* The optimized version of the shader should not re-generate a UBO.
     * The UBO will not be used for this variant. */
    codegen.generate_uniform_buffer();
  }

  /* Cache lookup: Reuse shaders already compiled. */
  pass = g_cache->get(engine, codegen.hash_get(), deferred_compilation, optimize_graph);

  if (pass) {
    pass->refcount++;
    return pass;
  }

  /* The shader is not compiled, continue generating the shader strings. */
  codegen.generate_attribs();
  codegen.generate_resources();
  codegen.generate_library();

  /* Make engine add its own code and implement the generated functions. */
  finalize_source_cb(thunk, material, &codegen.output);

  codegen.create_info->finalize();
  g_cache->add(
      engine, codegen.hash_get(), codegen.create_info, deferred_compilation, optimize_graph);
  codegen.create_info = nullptr;

  return g_cache->get(engine, codegen.hash_get(), deferred_compilation, optimize_graph);
}

/** \} */
