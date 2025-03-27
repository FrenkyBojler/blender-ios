/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Convert material node-trees to GLSL.
 */

#include "MEM_guardedalloc.h"

#include "DNA_customdata_types.h"
#include "DNA_material_types.h"

#include "BLI_ghash.h"
#include "BLI_hash_mm2a.hh"
#include "BLI_link_utils.h"
#include "BLI_listbase.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_time.h"

#include "BKE_cryptomatte.hh"
#include "BKE_material.hh"

#include "IMB_colormanagement.hh"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "GPU_material.hh"
#include "GPU_shader.hh"
#include "GPU_uniform_buffer.hh"
#include "GPU_vertex_format.hh"

#include "BLI_map.hh"
#include "BLI_sys_types.h" /* for intptr_t support */
#include "BLI_vector.hh"

#include "gpu_codegen.hh"
#include "gpu_node_graph.hh"
#include "gpu_shader_create_info.hh"
#include "gpu_shader_dependency_private.hh"

#include <cstdarg>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

using namespace blender;
using namespace blender::gpu::shader;

static bool gpu_pass_shader_validate(struct GPUCodegenCreateInfo *create_info, GPUShader *shader);

struct GPUCodegenCreateInfo : ShaderCreateInfo {
  struct NameBuffer {
    using NameEntry = std::array<char, 32>;

    /** Duplicate attribute names to avoid reference the GPUNodeGraph directly. */
    char attr_names[16][GPU_MAX_SAFE_ATTR_NAME + 1];
    char var_names[16][8];
    Vector<std::unique_ptr<NameEntry>, 16> sampler_names;

    /* Returns the appended name memory location */
    const char *append_sampler_name(const char name[32])
    {
      auto index = sampler_names.size();
      sampler_names.append(std::make_unique<NameEntry>());
      char *name_buffer = sampler_names[index]->data();
      memcpy(name_buffer, name, 32);
      return name_buffer;
    }
  };

  /** Optional generated interface. */
  StageInterfaceInfo *interface_generated = nullptr;
  /** Optional name buffer containing names referenced by StringRefNull. */
  NameBuffer name_buffer;

  GPUCodegenCreateInfo(const char *name) : ShaderCreateInfo(name){};
  ~GPUCodegenCreateInfo()
  {
    MEM_delete(interface_generated);
  };
};

/* -------------------------------------------------------------------- */
/** \name GPUPass
 * \{ */

struct GPUPass {
  static inline std::atomic<uint64_t> compilation_counts = 0;
  static inline std::atomic<uint32_t> in_flight_compilation_counts = 0;

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
    if (!deferred_compilation) {
      GPUShaderCreateInfo *base_info = reinterpret_cast<GPUShaderCreateInfo *>(create_info);
      shader = GPU_shader_create_from_info(base_info);
      finalize_compilation();
    }
    else if (!is_optimization_pass) {
      /* Optimization passes are deferred until all base passes are compiled. */
      try_setup_deferred_compilation();
    }
  }

  ~GPUPass()
  {
    if (compilation_handle) {
      finalize_compilation();
    }
    BLI_assert(create_info == nullptr || (is_optimization_pass && status == GPU_PASS_QUEUED));
    MEM_delete(create_info);
    GPU_SHADER_FREE_SAFE(shader);
  }

  void try_setup_deferred_compilation()
  {
    BLI_assert(status == GPU_PASS_QUEUED && compilation_handle == 0 && refcount > 0);

    /* Avoid having too many compilations in flight, in case they need to be cancelled. */
    /* TODO: Add a way to cancel compilation batches. */
    int max_in_flight_compilations = std::max(16, GPU_max_parallel_compilations() * 2);
    if (in_flight_compilation_counts >= max_in_flight_compilations) {
      return;
    }

    GPUShaderCreateInfo *base_info = reinterpret_cast<GPUShaderCreateInfo *>(create_info);
    compilation_handle = GPU_shader_batch_create_from_infos(
        Span<GPUShaderCreateInfo *>(&base_info, 1));

    in_flight_compilation_counts++;
  }

  void finalize_compilation()
  {
    BLI_assert_msg(create_info, "GPUPass::finalize_compilation() called more than once.");

    if (compilation_handle) {
      shader = GPU_shader_batch_finalize(compilation_handle).first();
      in_flight_compilation_counts--;
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
    if (status != GPU_PASS_QUEUED) {
      return;
    }

    if (compilation_handle) {
      if (GPU_shader_batch_is_ready(compilation_handle)) {
        finalize_compilation();
      }
    }
    else if (refcount != 0) {
      try_setup_deferred_compilation();
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

    // TODO: info->name (Was assigned in GPU_pass_compile)
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
/** \name Type > string conversion
 * \{ */

#if 0
#  define SRC_NAME(io, link, list, type) \
    link->node->name << "_" << io << BLI_findindex(&link->node->list, (const void *)link) << "_" \
                     << type
#else
#  define SRC_NAME(io, list, link, type) type
#endif

static std::ostream &operator<<(std::ostream &stream, const GPUInput *input)
{
  switch (input->source) {
    case GPU_SOURCE_FUNCTION_CALL:
    case GPU_SOURCE_OUTPUT:
      return stream << SRC_NAME("in", input, inputs, "tmp") << input->id;
    case GPU_SOURCE_CONSTANT:
      return stream << SRC_NAME("in", input, inputs, "cons") << input->id;
    case GPU_SOURCE_UNIFORM:
      return stream << "node_tree.u" << input->id;
    case GPU_SOURCE_ATTR:
      return stream << "var_attrs.v" << input->attr->id;
    case GPU_SOURCE_UNIFORM_ATTR:
      return stream << "UNI_ATTR(unf_attrs[resource_id].attr" << input->uniform_attr->id << ")";
    case GPU_SOURCE_LAYER_ATTR:
      return stream << "attr_load_layer(" << input->layer_attr->hash_code << ")";
    case GPU_SOURCE_STRUCT:
      return stream << "strct" << input->id;
    case GPU_SOURCE_TEX:
      return stream << input->texture->sampler_name;
    case GPU_SOURCE_TEX_TILED_MAPPING:
      return stream << input->texture->tiled_mapping_name;
    default:
      BLI_assert(0);
      return stream;
  }
}

static std::ostream &operator<<(std::ostream &stream, const GPUOutput *output)
{
  return stream << SRC_NAME("out", output, outputs, "tmp") << output->id;
}

/* Print data constructor (i.e: vec2(1.0f, 1.0f)). */
static std::ostream &operator<<(std::ostream &stream, const Span<float> &span)
{
  stream << (eGPUType)span.size() << "(";
  /* Use uint representation to allow exact same bit pattern even if NaN. This is
   * because we can pass UINTs as floats for constants. */
  const Span<uint32_t> uint_span = span.cast<uint32_t>();
  for (const uint32_t &element : uint_span) {
    char formatted_float[32];
    SNPRINTF(formatted_float, "uintBitsToFloat(%uu)", element);
    stream << formatted_float;
    if (&element != &uint_span.last()) {
      stream << ", ";
    }
  }
  stream << ")";
  return stream;
}

/* Trick type to change overload and keep a somewhat nice syntax. */
struct GPUConstant : public GPUInput {};

static std::ostream &operator<<(std::ostream &stream, const GPUConstant *input)
{
  stream << Span<float>(input->vec, input->type);
  return stream;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GLSL code generation
 * \{ */

class GPUCodegen {
 public:
  GPUMaterial &mat;
  GPUNodeGraph &graph;
  GPUCodegenOutput output = {};
  GPUCodegenCreateInfo *create_info = nullptr;

 private:
  uint32_t hash_ = 0;
  BLI_HashMurmur2A hm2a_;
  ListBase ubo_inputs_ = {nullptr, nullptr};
  GPUInput *cryptomatte_input_ = nullptr;

  /** Cache parameters for complexity heuristic. */
  uint nodes_total_ = 0;
  uint textures_total_ = 0;
  uint uniforms_total_ = 0;

 public:
  GPUCodegen(GPUMaterial *mat_, GPUNodeGraph *graph_) : mat(*mat_), graph(*graph_)
  {
    BLI_hash_mm2a_init(&hm2a_, GPU_material_uuid_get(&mat));
    BLI_hash_mm2a_add_int(&hm2a_, GPU_material_flag(&mat));
    create_info = MEM_new<GPUCodegenCreateInfo>(__func__, "codegen");
    output.create_info = reinterpret_cast<GPUShaderCreateInfo *>(
        static_cast<ShaderCreateInfo *>(create_info));
  }

  ~GPUCodegen()
  {
    MEM_SAFE_FREE(cryptomatte_input_);
    MEM_delete(create_info);
    BLI_freelistN(&ubo_inputs_);
  };

  void generate_graphs();
  void generate_cryptomatte();
  void generate_uniform_buffer();
  void generate_attribs();
  void generate_resources();
  void generate_library();

  uint32_t hash_get() const
  {
    return hash_;
  }

  /* Heuristic determined during pass codegen for whether a
   * more optimal variant of this material should be compiled. */
  bool should_optimize_heuristic() const
  {
    /* If each of the maximal attributes are exceeded, we can optimize, but we should also ensure
     * the baseline is met. */
    bool do_optimize = (nodes_total_ >= 60 || textures_total_ >= 4 || uniforms_total_ >= 64) &&
                       (textures_total_ >= 1 && uniforms_total_ >= 8 && nodes_total_ >= 4);
    return do_optimize;
  }

 private:
  void set_unique_ids();

  void node_serialize(std::stringstream &eval_ss, const GPUNode *node);
  std::string graph_serialize(eGPUNodeTag tree_tag,
                              GPUNodeLink *output_link,
                              const char *output_default = nullptr);
  std::string graph_serialize(eGPUNodeTag tree_tag);
};

void GPUCodegen::generate_attribs()
{
  if (BLI_listbase_is_empty(&graph.attributes)) {
    output.attr_load.clear();
    return;
  }

  GPUCodegenCreateInfo &info = *create_info;

  info.interface_generated = MEM_new<StageInterfaceInfo>(__func__, "codegen_iface", "var_attrs");
  StageInterfaceInfo &iface = *info.interface_generated;
  info.vertex_out(iface);

  /* Input declaration, loading / assignment to interface and geometry shader passthrough. */
  std::stringstream load_ss;

  int slot = GPU_shader_draw_parameters_support() ? 15 : 14;
  LISTBASE_FOREACH (GPUMaterialAttribute *, attr, &graph.attributes) {
    if (slot == -1) {
      BLI_assert_msg(0, "Too many attributes");
      break;
    }
    STRNCPY(info.name_buffer.attr_names[slot], attr->input_name);
    SNPRINTF(info.name_buffer.var_names[slot], "v%d", attr->id);

    StringRefNull attr_name = info.name_buffer.attr_names[slot];
    StringRefNull var_name = info.name_buffer.var_names[slot];

    eGPUType input_type, iface_type;

    load_ss << "var_attrs." << var_name;
    if (attr->is_hair_length) {
      iface_type = input_type = GPU_FLOAT;
      load_ss << " = attr_load_" << input_type << "(" << attr_name << ");\n";
    }
    else {
      switch (attr->type) {
        case CD_ORCO:
          /* Need vec4 to detect usage of default attribute. */
          input_type = GPU_VEC4;
          iface_type = GPU_VEC3;
          load_ss << " = attr_load_orco(" << attr_name << ");\n";
          break;
        case CD_TANGENT:
          iface_type = input_type = GPU_VEC4;
          load_ss << " = attr_load_tangent(" << attr_name << ");\n";
          break;
        default:
          iface_type = input_type = GPU_VEC4;
          load_ss << " = attr_load_" << input_type << "(" << attr_name << ");\n";
          break;
      }
    }

    info.vertex_in(slot--, to_type(input_type), attr_name);
    iface.smooth(to_type(iface_type), var_name);
  }

  output.attr_load = load_ss.str();
}

void GPUCodegen::generate_resources()
{
  GPUCodegenCreateInfo &info = *create_info;

  std::stringstream ss;

  /* Textures. */
  int slot = 0;
  LISTBASE_FOREACH (GPUMaterialTexture *, tex, &graph.textures) {
    if (tex->colorband) {
      const char *name = info.name_buffer.append_sampler_name(tex->sampler_name);
      info.sampler(slot++, ImageType::FLOAT_1D_ARRAY, name, Frequency::BATCH);
    }
    else if (tex->sky) {
      const char *name = info.name_buffer.append_sampler_name(tex->sampler_name);
      info.sampler(0, ImageType::FLOAT_2D_ARRAY, name, Frequency::BATCH);
    }
    else if (tex->tiled_mapping_name[0] != '\0') {
      const char *name = info.name_buffer.append_sampler_name(tex->sampler_name);
      info.sampler(slot++, ImageType::FLOAT_2D_ARRAY, name, Frequency::BATCH);

      const char *name_mapping = info.name_buffer.append_sampler_name(tex->tiled_mapping_name);
      info.sampler(slot++, ImageType::FLOAT_1D_ARRAY, name_mapping, Frequency::BATCH);
    }
    else {
      const char *name = info.name_buffer.append_sampler_name(tex->sampler_name);
      info.sampler(slot++, ImageType::FLOAT_2D, name, Frequency::BATCH);
    }
  }

  /* Increment heuristic. */
  textures_total_ = slot;

  if (!BLI_listbase_is_empty(&ubo_inputs_)) {
    /* NOTE: generate_uniform_buffer() should have sorted the inputs before this. */
    ss << "struct NodeTree {\n";
    LISTBASE_FOREACH (LinkData *, link, &ubo_inputs_) {
      GPUInput *input = (GPUInput *)(link->data);
      if (input->source == GPU_SOURCE_CRYPTOMATTE) {
        ss << input->type << " crypto_hash;\n";
      }
      else {
        ss << input->type << " u" << input->id << ";\n";
      }
    }
    ss << "};\n\n";

    info.uniform_buf(GPU_NODE_TREE_UBO_SLOT, "NodeTree", GPU_UBO_BLOCK_NAME, Frequency::BATCH);
  }

  if (!BLI_listbase_is_empty(&graph.uniform_attrs.list)) {
    ss << "struct UniformAttrs {\n";
    LISTBASE_FOREACH (GPUUniformAttr *, attr, &graph.uniform_attrs.list) {
      ss << "vec4 attr" << attr->id << ";\n";
    }
    ss << "};\n\n";

    /* TODO(fclem): Use the macro for length. Currently not working for EEVEE. */
    /* DRW_RESOURCE_CHUNK_LEN = 512 */
    info.uniform_buf(2, "UniformAttrs", GPU_ATTRIBUTE_UBO_BLOCK_NAME "[512]", Frequency::BATCH);
  }

  if (!BLI_listbase_is_empty(&graph.layer_attrs)) {
    info.additional_info("draw_layer_attributes");
  }

  info.typedef_source_generated = ss.str();
}

void GPUCodegen::generate_library()
{
  GPUCodegenCreateInfo &info = *create_info;

  void *value;
  Vector<std::string> source_files;

  /* Iterate over libraries. We need to keep this struct intact in case it is required for the
   * optimization pass. The first pass just collects the keys from the GSET, given items in a GSET
   * are unordered this can cause order differences between invocations, so we collect the keys
   * first, and sort them before doing actual work, to guarantee stable behavior while still
   * having cheap insertions into the GSET */
  GHashIterator *ihash = BLI_ghashIterator_new((GHash *)graph.used_libraries);
  while (!BLI_ghashIterator_done(ihash)) {
    value = BLI_ghashIterator_getKey(ihash);
    source_files.append((const char *)value);
    BLI_ghashIterator_step(ihash);
  }
  BLI_ghashIterator_free(ihash);

  std::sort(source_files.begin(), source_files.end());
  for (auto &key : source_files) {
    auto deps = gpu_shader_dependency_get_resolved_source(key.c_str());
    info.dependencies_generated.extend_non_duplicates(deps);
  }
}

void GPUCodegen::node_serialize(std::stringstream &eval_ss, const GPUNode *node)
{
  /* Declare constants. */
  LISTBASE_FOREACH (GPUInput *, input, &node->inputs) {
    switch (input->source) {
      case GPU_SOURCE_FUNCTION_CALL:
        eval_ss << input->type << " " << input << "; " << input->function_call << input << ");\n";
        break;
      case GPU_SOURCE_STRUCT:
        eval_ss << input->type << " " << input << " = CLOSURE_DEFAULT;\n";
        break;
      case GPU_SOURCE_CONSTANT:
        eval_ss << input->type << " " << input << " = " << (GPUConstant *)input << ";\n";
        break;
      default:
        break;
    }
  }
  /* Declare temporary variables for node output storage. */
  LISTBASE_FOREACH (GPUOutput *, output, &node->outputs) {
    eval_ss << output->type << " " << output << ";\n";
  }

  /* Function call. */
  eval_ss << node->name << "(";
  /* Input arguments. */
  LISTBASE_FOREACH (GPUInput *, input, &node->inputs) {
    switch (input->source) {
      case GPU_SOURCE_OUTPUT:
      case GPU_SOURCE_ATTR: {
        /* These inputs can have non matching types. Do conversion. */
        eGPUType to = input->type;
        eGPUType from = (input->source == GPU_SOURCE_ATTR) ? input->attr->gputype :
                                                             input->link->output->type;
        if (from != to) {
          /* Use defines declared inside codegen_lib (i.e: vec4_from_float). */
          eval_ss << to << "_from_" << from << "(";
        }

        if (input->source == GPU_SOURCE_ATTR) {
          eval_ss << input;
        }
        else {
          eval_ss << input->link->output;
        }

        if (from != to) {
          /* Special case that needs luminance coefficients as argument. */
          if (from == GPU_VEC4 && to == GPU_FLOAT) {
            float coefficients[3];
            IMB_colormanagement_get_luminance_coefficients(coefficients);
            eval_ss << ", " << Span<float>(coefficients, 3);
          }

          eval_ss << ")";
        }
        break;
      }
      default:
        eval_ss << input;
        break;
    }
    eval_ss << ", ";
  }
  /* Output arguments. */
  LISTBASE_FOREACH (GPUOutput *, output, &node->outputs) {
    eval_ss << output;
    if (output->next) {
      eval_ss << ", ";
    }
  }
  eval_ss << ");\n\n";

  /* Increment heuristic. */
  nodes_total_++;
}

std::string GPUCodegen::graph_serialize(eGPUNodeTag tree_tag,
                                        GPUNodeLink *output_link,
                                        const char *output_default)
{
  if (output_link == nullptr && output_default == nullptr) {
    return "";
  }

  std::stringstream eval_ss;
  bool has_nodes = false;
  /* NOTE: The node order is already top to bottom (or left to right in node editor)
   * because of the evaluation order inside ntreeExecGPUNodes(). */
  LISTBASE_FOREACH (GPUNode *, node, &graph.nodes) {
    if ((node->tag & tree_tag) == 0) {
      continue;
    }
    node_serialize(eval_ss, node);
    has_nodes = true;
  }

  if (!has_nodes) {
    return "";
  }

  if (output_link) {
    eval_ss << "return " << output_link->output << ";\n";
  }
  else {
    /* Default output in case there are only AOVs. */
    eval_ss << "return " << output_default << ";\n";
  }

  std::string str = eval_ss.str();
  BLI_hash_mm2a_add(&hm2a_, reinterpret_cast<const uchar *>(str.c_str()), str.size());
  return str;
}

std::string GPUCodegen::graph_serialize(eGPUNodeTag tree_tag)
{
  std::stringstream eval_ss;
  LISTBASE_FOREACH (GPUNode *, node, &graph.nodes) {
    if (node->tag & tree_tag) {
      node_serialize(eval_ss, node);
    }
  }
  std::string str = eval_ss.str();
  BLI_hash_mm2a_add(&hm2a_, reinterpret_cast<const uchar *>(str.c_str()), str.size());
  return str;
}

void GPUCodegen::generate_cryptomatte()
{
  cryptomatte_input_ = static_cast<GPUInput *>(MEM_callocN(sizeof(GPUInput), __func__));
  cryptomatte_input_->type = GPU_FLOAT;
  cryptomatte_input_->source = GPU_SOURCE_CRYPTOMATTE;

  float material_hash = 0.0f;
  Material *material = GPU_material_get_material(&mat);
  if (material) {
    bke::cryptomatte::CryptomatteHash hash(material->id.name + 2,
                                           BLI_strnlen(material->id.name + 2, MAX_NAME - 2));
    material_hash = hash.float_encoded();
  }
  cryptomatte_input_->vec[0] = material_hash;

  BLI_addtail(&ubo_inputs_, BLI_genericNodeN(cryptomatte_input_));
}

void GPUCodegen::generate_uniform_buffer()
{
  /* Extract uniform inputs. */
  LISTBASE_FOREACH (GPUNode *, node, &graph.nodes) {
    LISTBASE_FOREACH (GPUInput *, input, &node->inputs) {
      if (input->source == GPU_SOURCE_UNIFORM && !input->link) {
        /* We handle the UBO uniforms separately. */
        BLI_addtail(&ubo_inputs_, BLI_genericNodeN(input));
        uniforms_total_++;
      }
    }
  }
  if (!BLI_listbase_is_empty(&ubo_inputs_)) {
    /* This sorts the inputs based on size. */
    GPU_material_uniform_buffer_create(&mat, &ubo_inputs_);
  }
}

/* Sets id for unique names for all inputs, resources and temp variables. */
void GPUCodegen::set_unique_ids()
{
  int id = 1;
  LISTBASE_FOREACH (GPUNode *, node, &graph.nodes) {
    LISTBASE_FOREACH (GPUInput *, input, &node->inputs) {
      input->id = id++;
    }
    LISTBASE_FOREACH (GPUOutput *, output, &node->outputs) {
      output->id = id++;
    }
  }
}

void GPUCodegen::generate_graphs()
{
  set_unique_ids();

  output.surface = graph_serialize(
      GPU_NODE_TAG_SURFACE | GPU_NODE_TAG_AOV, graph.outlink_surface, "CLOSURE_DEFAULT");
  output.volume = graph_serialize(GPU_NODE_TAG_VOLUME, graph.outlink_volume, "CLOSURE_DEFAULT");
  output.displacement = graph_serialize(
      GPU_NODE_TAG_DISPLACEMENT, graph.outlink_displacement, nullptr);
  output.thickness = graph_serialize(GPU_NODE_TAG_THICKNESS, graph.outlink_thickness, nullptr);
  if (!BLI_listbase_is_empty(&graph.outlink_compositor)) {
    output.composite = graph_serialize(GPU_NODE_TAG_COMPOSITOR);
  }

  if (!BLI_listbase_is_empty(&graph.material_functions)) {
    std::stringstream eval_ss;
    eval_ss << "\n/* Generated Functions */\n\n";
    LISTBASE_FOREACH (GPUNodeGraphFunctionLink *, func_link, &graph.material_functions) {
      /* Untag every node in the graph to avoid serializing nodes from other functions */
      LISTBASE_FOREACH (GPUNode *, node, &graph.nodes) {
        node->tag &= ~GPU_NODE_TAG_FUNCTION;
      }
      /* Tag only the nodes needed for the current function */
      gpu_nodes_tag(func_link->outlink, GPU_NODE_TAG_FUNCTION);
      const std::string fn = graph_serialize(GPU_NODE_TAG_FUNCTION, func_link->outlink);
      eval_ss << "float " << func_link->name << "() {\n" << fn << "}\n\n";
    }
    output.material_functions = eval_ss.str();
    /* Leave the function tags as they were before serialization */
    LISTBASE_FOREACH (GPUNodeGraphFunctionLink *, funclink, &graph.material_functions) {
      gpu_nodes_tag(funclink->outlink, GPU_NODE_TAG_FUNCTION);
    }
  }

  LISTBASE_FOREACH (GPUMaterialAttribute *, attr, &graph.attributes) {
    BLI_hash_mm2a_add(&hm2a_, (uchar *)attr->name, strlen(attr->name));
  }

  hash_ = BLI_hash_mm2a_end(&hm2a_);
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

/* -------------------------------------------------------------------- */
/** \name Module
 * \{ */

void gpu_codegen_init() {}

void gpu_codegen_exit()
{
  BKE_material_defaults_free_gpu();
  GPU_shader_free_builtin_shaders();
}

/** \} */
