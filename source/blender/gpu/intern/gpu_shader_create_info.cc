/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Descriptor type used to define shader structure, resources and interfaces.
 */

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"

#include "BKE_global.hh"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "GPU_platform.hh"
#include "GPU_shader.hh"

#include "gpu_shader_create_info.hh"
#include "gpu_shader_create_info_private.hh"
#include "gpu_shader_dependency_private.hh"

#undef GPU_SHADER_NAMED_INTERFACE_INFO
#undef GPU_SHADER_INTERFACE_INFO
#undef GPU_SHADER_CREATE_INFO
#undef GPU_SHADER_NAMED_INTERFACE_END
#undef GPU_SHADER_INTERFACE_END
#undef GPU_SHADER_CREATE_END

namespace blender {

namespace gpu::shader {

using CreateInfoDictionary = Map<StringRef, ShaderCreateInfo *>;
using InterfaceDictionary = Map<StringRef, StageInterfaceInfo *>;

static CreateInfoDictionary *g_create_infos = nullptr;
static InterfaceDictionary *g_interfaces = nullptr;

/* -------------------------------------------------------------------- */
/** \name Check Backend Support
 *
 * \{ */

static bool is_vulkan_compatible_interface(const StageInterfaceInfo &iface)
{
  if (iface.instance_name.is_empty()) {
    return true;
  }

  bool use_flat = false;
  bool use_smooth = false;
  bool use_noperspective = false;
  for (const StageInterfaceInfo::InOut &attr : iface.inouts) {
    switch (attr.interp) {
      case Interpolation::FLAT:
        use_flat = true;
        break;
      case Interpolation::SMOOTH:
        use_smooth = true;
        break;
      case Interpolation::NO_PERSPECTIVE:
        use_noperspective = true;
        break;
    }
  }
  int num_used_interpolation_types = (use_flat ? 1 : 0) + (use_smooth ? 1 : 0) +
                                     (use_noperspective ? 1 : 0);

#if 0
  if (num_used_interpolation_types > 1) {
    std::cout << "'" << iface.name << "' uses multiple interpolation types\n";
  }
#endif

  return num_used_interpolation_types <= 1;
}

bool ShaderCreateInfo::is_vulkan_compatible() const
{
  /* Vulkan doesn't support setting an interpolation mode per attribute in a struct. */
  for (const StageInterfaceInfo *iface : vertex_out_interfaces_) {
    if (!is_vulkan_compatible_interface(*iface)) {
      return false;
    }
  }
  for (const StageInterfaceInfo *iface : geometry_out_interfaces_) {
    if (!is_vulkan_compatible_interface(*iface)) {
      return false;
    }
  }

  return true;
}

std::string ShaderCreateInfo::buffer_typename(StringRefNull type_name, bool uniform_buffer) const
{
  if (bool(this->builtins_ & BuiltinBits::NO_BUFFER_TYPE_LINTING) || type_name.startswith("int") ||
      type_name.startswith("uint") || type_name.startswith("float") ||
      type_name.startswith("packed_"))
  {
    return type_name;
  }
  return type_name + "_host_shared_" + (uniform_buffer ? "uniform_" : "");
}

/** \} */

ShaderCreateInfo::ShaderCreateInfo(const char *name) : name_(name)
{
  bool last_char_is_underscore = false;
  /* Escape the shader name to be able to use it inside an identifier. */
  for (char &c : name_) {
    if (!std::isalnum(c)) {
      c = '_';
    }
    if (c == '_' && last_char_is_underscore) {
      /* Avoid GLSL warning about double underscore being reserved. */
      c = 'w';
    }
    last_char_is_underscore = c == '_';
  }
}

std::string ShaderCreateInfo::resource_guard_defines(Span<CompilationConstant> constants) const
{
  std::string defines;
  defines += "#define CREATE_INFO_" + name_ + "\n";
  for (const auto &additional_info : additional_infos_) {
    const ShaderCreateInfo &info = *reinterpret_cast<const ShaderCreateInfo *>(
        gpu_shader_create_info_get(additional_info.name.c_str()));

    if (additional_info.conditions.evaluate(constants)) {
      defines += info.resource_guard_defines(constants);
    }
  }
  return defines;
}

void ShaderCreateInfo::finalize(const bool recursive)
{
  if (finalized_) {
    return;
  }

  finalized_ = true;

  /* Define a stack frame to track the state of each node traversal. */
  struct StackFrame {
    ShaderCreateInfo *info;
    size_t child_idx;
    std::unique_ptr<Set<StringRefNull>> deps_merged;
  };

  std::vector<StackFrame> stack;
  /* Initialize the root node frame */
  stack.push_back({this, 0, std::make_unique<Set<StringRefNull>>()});
  validate_vertex_attributes();

  while (!stack.empty()) {
    /* Safely look up index and pointer to avoid referencing issues during vector resizing. */
    const size_t top_idx = stack.size() - 1;
    ShaderCreateInfo &node = *stack[top_idx].info;
    const size_t child_idx = stack[top_idx].child_idx;

    /*  Processing dependencies. */
    if (child_idx < node.additional_infos_.size()) {
      const auto &additional_info = node.additional_infos_[child_idx];

      /* Fetch create info. */
      const ShaderCreateInfo &info_const = *reinterpret_cast<const ShaderCreateInfo *>(
          gpu_shader_create_info_get(additional_info.name.c_str()));
      /* We need to tag the info as finalized. */
      ShaderCreateInfo &dep = const_cast<ShaderCreateInfo &>(info_const);

      /* Recursive finalize is only supported at startup. We expect any runtime shader compilation
       * to only finalize one level. */
      BLI_assert(recursive || dep.finalized_);

      if (!dep.finalized_) {
        /* Emulate the recursive call: Prepare the dependency, push to stack,
         * and defer merging until the dep is fully finalized. */
        dep.finalized_ = true;
        dep.validate_vertex_attributes();
        stack.push_back({&dep, 0, std::make_unique<Set<StringRefNull>>()});
        continue;
      }

      node.interface_names_size_ += dep.interface_names_size_;

      /* NOTE: EEVEE Materials can result in nested includes. To avoid duplicate
       * shader resources, we need to avoid inserting duplicates.
       * TODO: Optimize create info preparation to include each individual "additional_info"
       * only a single time. */
      node.vertex_inputs_.extend_non_duplicates(dep.vertex_inputs_);
      node.fragment_outputs_.extend_non_duplicates(dep.fragment_outputs_);
      node.vertex_out_interfaces_.extend_non_duplicates(dep.vertex_out_interfaces_);
      node.geometry_out_interfaces_.extend_non_duplicates(dep.geometry_out_interfaces_);
      node.subpass_inputs_.extend_non_duplicates(dep.subpass_inputs_);
      node.specialization_constants_.extend_non_duplicates(dep.specialization_constants_);
      node.compilation_constants_.extend_non_duplicates(dep.compilation_constants_);

      node.shared_variables_.extend(dep.shared_variables_);

      validate_vertex_attributes(&dep);

      /* Insert with duplicate check. */
      node.push_constants_.extend_non_duplicates(dep.push_constants_);
      node.defines_.extend_non_duplicates(dep.defines_);
      node.typedef_sources_.extend_non_duplicates(dep.typedef_sources_);

      for (const auto &res : dep.pass_resources_) {
        extend_predicate(node.pass_resources_, res, additional_info.conditions);
      }
      for (const auto &res : dep.batch_resources_) {
        extend_predicate(node.batch_resources_, res, additional_info.conditions);
      }
      for (const auto &res : dep.geometry_resources_) {
        extend_predicate(node.geometry_resources_, res, additional_info.conditions);
      }

      /* API-specific parameters.
       * We will only copy API-specific parameters if they are otherwise unassigned. */
#ifdef WITH_METAL_BACKEND
      if (mtl_max_threads_per_threadgroup_ == 0) {
        node.mtl_max_threads_per_threadgroup_ = dep.mtl_max_threads_per_threadgroup_;
      }
#endif

      if (dep.early_fragment_test_) {
        node.early_fragment_test_ = true;
        node.depth_write_ = DepthWrite::UNCHANGED;
      }
      /* Modify depth write if has been changed from default.
       * `UNCHANGED` implies gl_FragDepth is not used at all. */
      if (dep.depth_write_ != DepthWrite::UNCHANGED) {
        node.depth_write_ = dep.depth_write_;
      }

      /* Inherit builtin bits from additional info. */
      node.builtins_ |= dep.builtins_;

      /* TODO(fclem): We need to reintroduce this check before compiling.
       * The issue is that the new SRT paradigm allows for conflicting resources if they are not
       * defined at the same time (using compilation constants). */
      // validate_merge(info);

      if (!stack[top_idx].deps_merged->add(dep.name_)) {
        assert_no_overlap(node, dep, false, "additional info already merged via another info");
      }

      if (dep.compute_layout_.local_size_x != -1) {
        assert_no_overlap(
            node, dep, compute_layout_.local_size_x == -1, "Compute layout already defined");
        node.compute_layout_ = dep.compute_layout_;
      }

      if (!dep.vertex_source_.is_empty()) {
        assert_no_overlap(node, dep, vertex_source_.is_empty(), "Vertex source already existing");
        node.vertex_source_ = dep.vertex_source_;
      }
      if (!dep.geometry_source_.is_empty()) {
        assert_no_overlap(
            node, dep, geometry_source_.is_empty(), "Geometry source already existing");
        node.geometry_source_ = dep.geometry_source_;
        node.geometry_layout_ = dep.geometry_layout_;
      }
      if (!dep.fragment_source_.is_empty()) {
        assert_no_overlap(
            node, dep, fragment_source_.is_empty(), "Fragment source already existing");
        node.fragment_source_ = dep.fragment_source_;
      }
      if (!dep.compute_source_.is_empty()) {
        assert_no_overlap(
            node, dep, compute_source_.is_empty(), "Compute source already existing");
        node.compute_source_ = dep.compute_source_;
      }

      if (dep.vertex_entry_fn_ != "main") {
        assert_no_overlap(
            node, dep, vertex_entry_fn_ == "main", "Vertex function already existing");
        node.vertex_entry_fn_ = dep.vertex_entry_fn_;
      }
      if (dep.geometry_entry_fn_ != "main") {
        assert_no_overlap(
            node, dep, geometry_entry_fn_ == "main", "Geometry function already existing");
        node.geometry_entry_fn_ = dep.geometry_entry_fn_;
      }
      if (dep.fragment_entry_fn_ != "main") {
        assert_no_overlap(
            node, dep, fragment_entry_fn_ == "main", "Fragment function already existing");
        node.fragment_entry_fn_ = dep.fragment_entry_fn_;
      }
      if (dep.compute_entry_fn_ != "main") {
        assert_no_overlap(
            node, dep, compute_entry_fn_ == "main", "Compute function already existing");
        node.compute_entry_fn_ = dep.compute_entry_fn_;
      }

      /* Progress to the next dependency for this node. */
      stack[top_idx].child_idx++;
      continue;
    }

    /* Executed once all dependencies are processed  */

    if (!geometry_source_.is_empty() && bool(builtins_ & BuiltinBits::LAYER)) {
      std::cout
          << name_
          << ": Validation failed. BuiltinBits::LAYER shouldn't be used with geometry shaders."
          << std::endl;
      BLI_assert(0);
    }

    if (auto_resource_location_) {
      int images = 0, samplers = 0, ubos = 0, ssbos = 0;

      for (auto &res : batch_resources_) {
        set_resource_slot(res, images, samplers, ubos, ssbos);
      }
      for (auto &res : pass_resources_) {
        set_resource_slot(res, images, samplers, ubos, ssbos);
      }
      for (auto &res : geometry_resources_) {
        set_resource_slot(res, images, samplers, ubos, ssbos);
      }
    }

    /* Info complete. Pop it off the stack. */
    stack.pop_back();
  }
}

void ShaderCreateInfo::extend_predicate(Vector<Resource, 0> &resource_vector,
                                        ShaderCreateInfo::Resource res_copy,
                                        Span<ConditionFn> additional_conditions)
{
  res_copy.conditions.extend(additional_conditions);
  /** IMPORTANT: We keep duplicates until we evaluate the conditions. */
  resource_vector.append(res_copy);
};

void ShaderCreateInfo::assert_no_overlap(const ShaderCreateInfo &info,
                                         const ShaderCreateInfo &dep,
                                         const bool test,
                                         const StringRefNull error)
{
  if (!test) {
    std::cout << info.name_ << ": Validation failed while merging " << dep.name_ << " : ";
    std::cout << error << std::endl;
    BLI_assert(0);
  }
}

void ShaderCreateInfo::set_resource_slot(
    Resource &res, int &images, int &samplers, int &ubos, int &ssbos)
{
  switch (res.bind_type) {
    case Resource::BindType::UNIFORM_BUFFER:
      res.slot = ubos++;
      break;
    case Resource::BindType::STORAGE_BUFFER:
      res.slot = ssbos++;
      break;
    case Resource::BindType::SAMPLER:
      res.slot = samplers++;
      break;
    case Resource::BindType::IMAGE:
      res.slot = images++;
      break;
  }
}

std::string ShaderCreateInfo::check_error() const
{
  std::string error;

  /* At least a vertex shader and a fragment shader are required, or only a compute shader. */
  if (this->compute_source_.is_empty()) {
    if (this->vertex_source_.is_empty()) {
      error += "Missing vertex shader in " + this->name_ + ".\n";
    }
    if (this->fragment_source_.is_empty()) {
      error += "Missing fragment shader in " + this->name_ + ".\n";
    }
  }
  else {
    if (!this->vertex_source_.is_empty()) {
      error += "Compute shader has vertex_source_ shader attached in " + this->name_ + ".\n";
    }
    if (!this->geometry_source_.is_empty()) {
      error += "Compute shader has geometry_source_ shader attached in " + this->name_ + ".\n";
    }
    if (!this->fragment_source_.is_empty()) {
      error += "Compute shader has fragment_source_ shader attached in " + this->name_ + ".\n";
    }
  }

  if (!this->geometry_source_.is_empty()) {
    if (flag_is_set(this->builtins_, BuiltinBits::BARYCENTRIC_COORD)) {
      error += "Shader " + this->name_ +
               " has geometry stage and uses barycentric coordinates. This is not allowed as "
               "fallback injects a geometry stage.\n";
    }
    if (flag_is_set(this->builtins_, BuiltinBits::VIEWPORT_INDEX)) {
      error += "Shader " + this->name_ +
               " has geometry stage and uses multi-viewport. This is not allowed as "
               "fallback injects a geometry stage.\n";
    }
    if (flag_is_set(this->builtins_, BuiltinBits::LAYER)) {
      error += "Shader " + this->name_ +
               " has geometry stage and uses layer output. This is not allowed as "
               "fallback injects a geometry stage.\n";
    }
  }

  if ((G.debug & G_DEBUG_GPU) == 0) {
    return error;
  }

  if (flag_is_set(this->builtins_,
                  BuiltinBits::BARYCENTRIC_COORD | BuiltinBits::VIEWPORT_INDEX |
                      BuiltinBits::LAYER))
  {
    for (const StageInterfaceInfo *interface : this->vertex_out_interfaces_) {
      if (interface->instance_name.is_empty()) {
        error += "Shader " + this->name_ + " uses interface " + interface->name +
                 " that doesn't contain an instance name, but is required for the fallback "
                 "geometry shader.\n";
      }
    }
  }

  for (const StageInterfaceInfo *interface : this->vertex_out_interfaces_) {
    for (const StageInterfaceInfo::InOut &inout : interface->inouts) {
      if (inout.name.is_array()) {
        error += "Shader " + this->name_ + " : \"" + interface->name + "." + inout.name + "\":";
        error += " Array types are not allowed in shader stage interfaces.\n";
      }
      if (ELEM(inout.type, Type::float3x3_t, Type::float4x4_t)) {
        error += "Shader " + this->name_ + " : \"" + interface->name + "." + inout.name + "\":";
        error += " Matrix types are not allowed in shader stage interfaces.\n";
      }
    }
  }

  if (!this->is_vulkan_compatible()) {
    error += this->name_ +
             " contains a stage interface using an instance name and mixed interpolation modes. "
             "This is not compatible with Vulkan and need to be adjusted.\n";
  }

  /* Validate specialization constants. */
  for (int i = 0; i < specialization_constants_.size(); i++) {
    for (int j = i + 1; j < specialization_constants_.size(); j++) {
      if (specialization_constants_[i].name == specialization_constants_[j].name) {
        error += this->name_ + " contains two specialization constants with the name: " +
                 std::string(specialization_constants_[i].name);
      }
    }
  }

  /* Validate compilation constants. */
  for (int i = 0; i < compilation_constants_.size(); i++) {
    for (int j = i + 1; j < compilation_constants_.size(); j++) {
      if (compilation_constants_[i].name == compilation_constants_[j].name) {
        error += this->name_ + " contains two compilation constants with the name: " +
                 std::string(compilation_constants_[i].name);
      }
    }
  }

  /* Validate shared variables. */
  for (int i = 0; i < shared_variables_.size(); i++) {
    for (int j = i + 1; j < shared_variables_.size(); j++) {
      if (shared_variables_[i].name == shared_variables_[j].name) {
        error += this->name_ + " contains two specialization constants with the name: " +
                 std::string(shared_variables_[i].name);
      }
    }
  }

  /* Check same bind-points usage. */
  Set<int> images, samplers, ubos, ssbos;

  auto register_resource = [&](const Resource &res) -> bool {
    switch (res.bind_type) {
      case Resource::BindType::UNIFORM_BUFFER:
        return ubos.add(res.slot);
      case Resource::BindType::STORAGE_BUFFER:
        return ssbos.add(res.slot);
      case Resource::BindType::SAMPLER:
        return samplers.add(res.slot);
      case Resource::BindType::IMAGE:
        return images.add(res.slot);
      default:
        return false;
    }
  };

  auto print_error_msg = [&](const Resource &res, const Vector<Resource> &resources) {
    auto print_resource_name = [&](const Resource &res) {
      switch (res.bind_type) {
        case Resource::BindType::UNIFORM_BUFFER:
          error += "Uniform Buffer " + res.uniformbuf.name;
          break;
        case Resource::BindType::STORAGE_BUFFER:
          error += "Storage Buffer " + res.storagebuf.name;
          break;
        case Resource::BindType::SAMPLER:
          error += "Sampler " + res.sampler.name;
          break;
        case Resource::BindType::IMAGE:
          error += "Image " + res.image.name;
          break;
        default:
          error += "Unknown Type";
          break;
      }
    };

    for (const Resource &_res : resources) {
      if (&res != &_res && res.bind_type == _res.bind_type && res.slot == _res.slot) {
        error += name_ + ": Validation failed : Overlapping ";
        print_resource_name(res);
        error += " and ";
        print_resource_name(_res);
        error += " at binding location " + std::to_string(res.slot) + "\n";
      }
    }
  };

  for (const auto &res : batch_resources_) {
    if (register_resource(res) == false) {
      print_error_msg(res, resources_get_all_());
    }
  }

  for (const auto &res : pass_resources_) {
    if (register_resource(res) == false) {
      print_error_msg(res, resources_get_all_());
    }
  }

  for (const auto &res : geometry_resources_) {
    if (register_resource(res) == false) {
      print_error_msg(res, resources_get_all_());
    }
  }

  return error;
}

void ShaderCreateInfo::validate_merge(const ShaderCreateInfo &other_info)
{
  if (!auto_resource_location_) {
    /* Check same bind-points usage in OGL. */
    Set<int> images, samplers, ubos, ssbos;

    auto register_resource = [&](const Resource &res) -> bool {
      switch (res.bind_type) {
        case Resource::BindType::UNIFORM_BUFFER:
          return images.add(res.slot);
        case Resource::BindType::STORAGE_BUFFER:
          return samplers.add(res.slot);
        case Resource::BindType::SAMPLER:
          return ubos.add(res.slot);
        case Resource::BindType::IMAGE:
          return ssbos.add(res.slot);
        default:
          return false;
      }
    };

    auto print_error_msg = [&](const Resource &res, const Vector<Resource> &resources) {
      auto print_resource_name = [&](const Resource &res) {
        switch (res.bind_type) {
          case Resource::BindType::UNIFORM_BUFFER:
            std::cout << "Uniform Buffer " << res.uniformbuf.name;
            break;
          case Resource::BindType::STORAGE_BUFFER:
            std::cout << "Storage Buffer " << res.storagebuf.name;
            break;
          case Resource::BindType::SAMPLER:
            std::cout << "Sampler " << res.sampler.name;
            break;
          case Resource::BindType::IMAGE:
            std::cout << "Image " << res.image.name;
            break;
          default:
            std::cout << "Unknown Type";
            break;
        }
      };

      for (const Resource &_res : resources) {
        if (&res != &_res && res.bind_type == _res.bind_type && res.slot == _res.slot) {
          std::cout << name_ << ": Validation failed : Overlapping ";
          print_resource_name(res);
          std::cout << " and ";
          print_resource_name(_res);
          std::cout << " at (" << res.slot << ") while merging " << other_info.name_ << std::endl;
        }
      }
    };

    for (auto &res : batch_resources_) {
      if (register_resource(res) == false) {
        print_error_msg(res, resources_get_all_());
      }
    }

    for (auto &res : pass_resources_) {
      if (register_resource(res) == false) {
        print_error_msg(res, resources_get_all_());
      }
    }

    for (auto &res : geometry_resources_) {
      if (register_resource(res) == false) {
        print_error_msg(res, resources_get_all_());
      }
    }
  }
}

void ShaderCreateInfo::validate_vertex_attributes(const ShaderCreateInfo *other_info)
{
  uint32_t attr_bits = 0;
  for (auto &attr : vertex_inputs_) {
    if (attr.type == Type::float3x3_t) {
      std::cout << name_ << ": \"" << attr.name << "\" : float3x3 unsupported as vertex attribute."
                << std::endl;
      BLI_assert(0);
    }
    if (attr.type == Type::float4x4_t) {
      std::cout << name_ << ": \"" << attr.name << "\" : float4x4 unsupported as vertex attribute."
                << std::endl;
      BLI_assert(0);
    }
    if (attr.name.is_array()) {
      std::cout << name_ << ": \"" << attr.name
                << "\" : arrays are unsupported as vertex attribute." << std::endl;
      BLI_assert(0);
    }
    if (attr.index >= 16 || attr.index < 0) {
      std::cout << name_ << ": Invalid index for attribute \"" << attr.name << "\"" << std::endl;
      BLI_assert(0);
    }
    uint32_t attr_new = 0;
    if (attr.type == Type::float4x4_t) {
      for (int i = 0; i < 4; i++) {
        attr_new |= 1 << (attr.index + i);
      }
    }
    else {
      attr_new |= 1 << attr.index;
    }

    if ((attr_bits & attr_new) != 0) {
      std::cout << name_ << ": Attribute \"" << attr.name
                << "\" overlap one or more index from another attribute."
                   " Note that mat4 takes up 4 indices.";
      if (other_info) {
        std::cout << " While merging " << other_info->name_ << std::endl;
      }
      std::cout << std::endl;
      BLI_assert(0);
    }
    attr_bits |= attr_new;
  }
}

}  // namespace gpu::shader

using namespace blender::gpu::shader;

#ifdef _MSC_VER
/* Disable optimization for this function with MSVC. It does not like the fact
 * shaders info are declared in the same function (same basic block or not does
 * not change anything).
 * Since it is just a function called to register shaders (once),
 * the fact it's optimized or not does not matter, it's not on any hot
 * code path. */
#  pragma optimize("", off)
#endif
void gpu_shader_create_info_init()
{
  g_create_infos = new CreateInfoDictionary();
  g_interfaces = new InterfaceDictionary();

#define GPU_SHADER_NAMED_INTERFACE_INFO(_interface, _inst_name) \
  StageInterfaceInfo *ptr_##_interface = new StageInterfaceInfo(#_interface, #_inst_name); \
  StageInterfaceInfo &_interface = *ptr_##_interface; \
  g_interfaces->add_new(#_interface, ptr_##_interface); \
  _interface

#define GPU_SHADER_INTERFACE_INFO(_interface) \
  StageInterfaceInfo *ptr_##_interface = new StageInterfaceInfo(#_interface); \
  StageInterfaceInfo &_interface = *ptr_##_interface; \
  g_interfaces->add_new(#_interface, ptr_##_interface); \
  _interface

#define GPU_SHADER_CREATE_INFO(_info) \
  ShaderCreateInfo *ptr_##_info = new ShaderCreateInfo(#_info); \
  ShaderCreateInfo &_info = *ptr_##_info; \
  g_create_infos->add_new(#_info, ptr_##_info); \
  _info

#define GPU_SHADER_NAMED_INTERFACE_END(_inst_name) ;
#define GPU_SHADER_INTERFACE_END() ;
#define GPU_SHADER_CREATE_END() ;

/* Declare, register and construct the infos. */
#include "glsl_compositor_infos_list.hh"
#include "glsl_draw_infos_list.hh"
#include "glsl_gpu_infos_list.hh"
#include "glsl_ocio_infos_list.hh"
#ifdef WITH_OPENSUBDIV
#  include "glsl_osd_infos_list.hh"
#endif

  if (GPU_stencil_clasify_buffer_workaround()) {
    /* WORKAROUND: Adding a dummy buffer that isn't used fixes a bug inside the Qualcomm driver. */
    eevee_deferred_tile_classify.storage_buf(
        12, Qualifier::read_write, "uint", "dummy_workaround_buf[]");
  }

  for (ShaderCreateInfo *info : g_create_infos->values()) {
    info->is_generated_ = false;

    info->builtins_ |= gpu_shader_dependency_get_builtins(info->vertex_source_);
    info->builtins_ |= gpu_shader_dependency_get_builtins(info->fragment_source_);
    info->builtins_ |= gpu_shader_dependency_get_builtins(info->geometry_source_);
    info->builtins_ |= gpu_shader_dependency_get_builtins(info->compute_source_);

    if (!info->compute_source_.is_empty()) {
      info->shared_variables_.extend(
          gpu_shader_dependency_get_shared_variables(info->compute_source_));
    }

#if GPU_SHADER_PRINTF_ENABLE
    const bool is_material_shader = StringRefNull(info->name_).startswith("eevee_surf_");
    if (flag_is_set(info->builtins_, BuiltinBits::USE_PRINTF) ||
        (gpu_shader_dependency_force_gpu_print_injection() && is_material_shader))
    {
      info->additional_info("gpu_print");
    }
#endif

#ifndef NDEBUG
    /* Automatically amend the create info for ease of use of the debug feature. */
    if (flag_is_set(info->builtins_, BuiltinBits::USE_DEBUG_DRAW)) {
      info->additional_info("draw_debug_draw");
    }
#endif
  }

  for (ShaderCreateInfo *info : g_create_infos->values()) {
    info->finalize(true);
  }

  /* TEST */
  // gpu_shader_create_info_compile(nullptr);
}
#ifdef _MSC_VER
#  pragma optimize("", on)
#endif

void gpu_shader_create_info_exit()
{
  for (auto *value : g_create_infos->values()) {
    delete value;
  }
  delete g_create_infos;

  for (auto *value : g_interfaces->values()) {
    delete value;
  }
  delete g_interfaces;
}

bool gpu_shader_create_info_compile_all(const char *name_starts_with_filter)
{
  using namespace blender::gpu;
  int success = 0;
  int skipped_filter = 0;
  int skipped = 0;
  int total = 0;

  Vector<AsyncCompilationHandle> handles;

  for (ShaderCreateInfo *info : g_create_infos->values()) {
    info->finalize();
    if (info->do_static_compilation_) {
      if (name_starts_with_filter &&
          !StringRefNull(info->name_).startswith(StringRefNull(name_starts_with_filter)))
      {
        skipped_filter++;
        continue;
      }
      if ((info->metal_backend_only_ && GPU_backend_get_type() != GPU_BACKEND_METAL) ||
          (GPU_geometry_shader_support() == false && info->geometry_source_ != nullptr))
      {
        skipped++;
        continue;
      }
      total++;

      handles.append(
          GPU_shader_async_compilation(reinterpret_cast<const GPUShaderCreateInfo *>(info)));
    }
  }

  GPU_shader_compiler_wait_for_all();

  for (AsyncCompilationHandle handle : handles) {
    if (gpu::Shader *result = GPU_shader_async_compilation_finalize(handle)) {
      success++;
#if 0 /* TODO(fclem): This is too verbose for now. Make it a cmake option. */
        /* Test if any resource is optimized out and print a warning if that's the case. */
        /* TODO(fclem): Limit this to OpenGL backend. */
        const ShaderInterface *interface = shader->interface;

        Vector<ShaderCreateInfo::Resource> all_resources = info->resources_get_all_();

        for (ShaderCreateInfo::Resource &res : all_resources) {
          StringRefNull name = "";
          const ShaderInput *input = nullptr;

          switch (res.bind_type) {
            case ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER:
              input = interface->ubo_get(res.slot);
              name = res.uniformbuf.name;
              break;
            case ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER:
              input = interface->ssbo_get(res.slot);
              name = res.storagebuf.name;
              break;
            case ShaderCreateInfo::Resource::BindType::SAMPLER:
              input = interface->texture_get(res.slot);
              name = res.sampler.name;
              break;
            case ShaderCreateInfo::Resource::BindType::IMAGE:
              input = interface->texture_get(res.slot);
              name = res.image.name;
              break;
          }

          if (input == nullptr) {
            std::cerr << "Error: " << info->name_;
            std::cerr << ": Resource « " << name << " » not found in the shader interface\n";
          }
          else if (input->location == -1) {
            std::cerr << "Warning: " << info->name_;
            std::cerr << ": Resource « " << name << " » is optimized out\n";
          }
        }
#endif
      GPU_shader_free(result);
    }
  }

  printf("Shader Test compilation result: %d / %d passed", success, total);
  if (skipped_filter > 0) {
    printf(" (skipped %d when filtering)", skipped_filter);
  }
  if (skipped > 0) {
    printf(" (skipped %d for compatibility reasons)", skipped);
  }
  printf("\n");
  return success == total;
}

const GPUShaderCreateInfo *gpu_shader_create_info_get(const char *info_name)
{
  if (g_create_infos->contains(info_name) == false) {
    printf("Error: Cannot find shader create info named \"%s\"\n", info_name);
    return nullptr;
  }
  ShaderCreateInfo *info = g_create_infos->lookup(info_name);
  return reinterpret_cast<const GPUShaderCreateInfo *>(info);
}

}  // namespace blender
