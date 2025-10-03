/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_global.hh"

#include "BLI_string.h"

#include "BLI_string.h"
#include <algorithm>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>

#include <cstring>

#include "GPU_platform.hh"
#include "GPU_vertex_format.hh"

#include "gpu_shader_dependency_private.hh"

#include "mtl_common.hh"
#include "mtl_context.hh"
#include "mtl_debug.hh"
#include "mtl_shader.hh"
#include "mtl_shader_generate.hh"
#include "mtl_shader_generator.hh"
#include "mtl_shader_interface.hh"
#include "mtl_texture.hh"

extern char datatoc_mtl_shader_shared_hh[];

using namespace blender;
using namespace blender::gpu;
using namespace blender::gpu::shader;

namespace blender::gpu {

std::mutex msl_patch_default_lock;
char *MSLGeneratorInterface::msl_patch_default = nullptr;

/* Generator names. */
#define FRAGMENT_OUT_STRUCT_NAME "FragmentOut"
#define FRAGMENT_TILE_IN_STRUCT_NAME "FragmentTileIn"

/* -------------------------------------------------------------------- */
/** \name Shader Translation utility functions.
 * \{ */

static void split_array(StringRefNull input, std::string &r_name, std::string &r_array)
{
  size_t array_start = input.find('[');
  if (array_start != std::string::npos) {
    r_name = input.substr(0, array_start);
    r_array = input.substr(array_start);
  }
  else {
    r_name = input;
    r_array = "";
  }
}

static MTLInterfaceDataType to_mtl_type(Type type)
{
  switch (type) {
    case Type::float_t:
      return MTL_DATATYPE_FLOAT;
    case Type::float2_t:
      return MTL_DATATYPE_FLOAT2;
    case Type::float3_t:
      return MTL_DATATYPE_FLOAT3;
    case Type::float4_t:
      return MTL_DATATYPE_FLOAT4;
    case Type::float3x3_t:
      return MTL_DATATYPE_FLOAT3x3;
    case Type::float4x4_t:
      return MTL_DATATYPE_FLOAT4x4;
    case Type::uint_t:
      return MTL_DATATYPE_UINT;
    case Type::uint2_t:
      return MTL_DATATYPE_UINT2;
    case Type::uint3_t:
      return MTL_DATATYPE_UINT3;
    case Type::uint4_t:
      return MTL_DATATYPE_UINT4;
    case Type::int_t:
      return MTL_DATATYPE_INT;
    case Type::int2_t:
      return MTL_DATATYPE_INT2;
    case Type::int3_t:
      return MTL_DATATYPE_INT3;
    case Type::int4_t:
      return MTL_DATATYPE_INT4;
    case Type::float3_10_10_10_2_t:
      return MTL_DATATYPE_INT1010102_NORM;
    case Type::bool_t:
      return MTL_DATATYPE_BOOL;
    case Type::uchar_t:
      return MTL_DATATYPE_UCHAR;
    case Type::uchar2_t:
      return MTL_DATATYPE_UCHAR2;
    case Type::uchar3_t:
      return MTL_DATATYPE_UCHAR3;
    case Type::uchar4_t:
      return MTL_DATATYPE_UCHAR4;
    case Type::char_t:
      return MTL_DATATYPE_CHAR;
    case Type::char2_t:
      return MTL_DATATYPE_CHAR2;
    case Type::char3_t:
      return MTL_DATATYPE_CHAR3;
    case Type::char4_t:
      return MTL_DATATYPE_CHAR4;
    case Type::ushort_t:
      return MTL_DATATYPE_USHORT;
    case Type::ushort2_t:
      return MTL_DATATYPE_USHORT2;
    case Type::ushort3_t:
      return MTL_DATATYPE_USHORT3;
    case Type::ushort4_t:
      return MTL_DATATYPE_USHORT4;
    case Type::short_t:
      return MTL_DATATYPE_SHORT;
    case Type::short2_t:
      return MTL_DATATYPE_SHORT2;
    case Type::short3_t:
      return MTL_DATATYPE_SHORT3;
    case Type::short4_t:
      return MTL_DATATYPE_SHORT4;
    default: {
      BLI_assert_msg(false, "Unexpected data type");
    }
  }
  return MTL_DATATYPE_FLOAT;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name MTLShader builtin shader generation utilities.
 * \{ */

std::string MTLShader::resources_declare(const ShaderCreateInfo & /*info*/) const
{
  /* This is done later inside #generate_entry_point(). */
  return "";
}

std::string MTLShader::vertex_interface_declare(const ShaderCreateInfo & /*info*/) const
{
  /* This is done later inside #generate_entry_point(). */
  return "";
}

std::string MTLShader::fragment_interface_declare(const ShaderCreateInfo & /*info*/) const
{
  /* This is done later inside #generate_entry_point(). */
  return "";
}

std::string MTLShader::MTLShader::geometry_interface_declare(
    const shader::ShaderCreateInfo & /*info*/) const
{
  BLI_assert_msg(false, "Geometry shading unsupported by Metal");
  return "";
}

std::string MTLShader::geometry_layout_declare(const shader::ShaderCreateInfo & /*info*/) const
{
  BLI_assert_msg(false, "Geometry shading unsupported by Metal");
  return "";
}

std::string MTLShader::compute_layout_declare(const ShaderCreateInfo & /*info*/) const
{
  /* This is done later inside #generate_entry_point(). */
  return "";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shader Translation.
 * \{ */

char *MSLGeneratorInterface::msl_patch_default_get()
{
  msl_patch_default_lock.lock();
  if (msl_patch_default != nullptr) {
    msl_patch_default_lock.unlock();
    return msl_patch_default;
  }

  std::stringstream ss_patch;
  ss_patch << datatoc_mtl_shader_shared_hh << std::endl;
  size_t len = strlen(ss_patch.str().c_str()) + 1;

  msl_patch_default = (char *)malloc(len * sizeof(char));
  memcpy(msl_patch_default, ss_patch.str().c_str(), len * sizeof(char));
  msl_patch_default_lock.unlock();
  return msl_patch_default;
}

static void shared_variable_declare(const shader::ShaderCreateInfo &info, std::stringstream &ss)
{
  for (const shader::ShaderCreateInfo::SharedVariable &var : info.shared_variables_) {
    std::string array, name;
    split_array(var.name, name, array);
    ss << "threadgroup " << to_string(var.type) << ' ' << name << array << ";\n";
  }
}

static void shared_variable_pass(const shader::ShaderCreateInfo &info, std::stringstream &ss)
{
  bool first = true;
  if (info.shared_variables_.is_empty()) {
    return;
  }
  ss << "(";
  for (const shader::ShaderCreateInfo::SharedVariable &var : info.shared_variables_) {
    std::string array, name;
    split_array(var.name, name, array);
    ss << (first ? ' ' : ',') << name;
    first = false;
  }
  ss << ")";
}

void MSLGeneratorInterface::prepare_from_createinfo(const shader::ShaderCreateInfo *info)
{
  /** Assign info. */
  create_info_ = info;

  /** Prepare Uniforms. */
  for (const shader::ShaderCreateInfo::PushConst &push_constant : create_info_->push_constants_) {
    MSLUniform uniform(push_constant.type,
                       push_constant.name,
                       bool(push_constant.array_size > 1),
                       push_constant.array_size);
    uniforms.append(uniform);
  }

  /** Prepare Constants. */
  for (const auto &constant : create_info_->specialization_constants_) {
    constants.append(MSLConstant(constant.type, constant.name));
  }

  /* Prepare textures and uniform blocks.
   * Perform across both resource categories and extract both
   * texture samplers and image types. */

  /* NOTE: Metal requires Samplers and images to share slots. We will re-map these.
   * If `auto_resource_location_` is not used, then slot collision could occur and
   * this should be resolved in the original create-info.
   * UBOs and SSBOs also share the same bind table. */
  int texture_slot_id = 0;
  int ubo_buffer_slot_id_ = 0;
  int storage_buffer_slot_id_ = 0;

  uint max_storage_buffer_location = 0;

  const blender::Vector<ShaderCreateInfo::Resource> all_resources = info->resources_get_all_();

  /* Determine max sampler slot for image resource offset, when not using auto resource location,
   * as image resources cannot overlap sampler ranges. */
  int max_sampler_slot = 0;
  if (!create_info_->auto_resource_location_) {
    for (const ShaderCreateInfo::Resource &res : all_resources) {
      if (res.bind_type == shader::ShaderCreateInfo::Resource::BindType::SAMPLER) {
        max_sampler_slot = max_ii(res.slot, max_sampler_slot);
      }
    }
  }

  for (const ShaderCreateInfo::Resource &res : all_resources) {
    /* TODO(Metal): Consider adding stage flags to textures in create info. */
    /* Handle sampler types. */
    switch (res.bind_type) {
      case shader::ShaderCreateInfo::Resource::BindType::SAMPLER: {

        /* Samplers to have access::sample by default. */
        MSLTextureSamplerAccess access = MSLTextureSamplerAccess::TEXTURE_ACCESS_SAMPLE;
        /* TextureBuffers must have read/write/read-write access pattern. */
        if (res.sampler.type == ImageType::FloatBuffer ||
            res.sampler.type == ImageType::IntBuffer || res.sampler.type == ImageType::UintBuffer)
        {
          access = MSLTextureSamplerAccess::TEXTURE_ACCESS_READ;
        }

        MSLTextureResource msl_tex;
        msl_tex.stage = ShaderStage::ANY;
        msl_tex.type = res.sampler.type;
        msl_tex.name = res.sampler.name;
        msl_tex.access = access;
        msl_tex.slot = texture_slot_id++;
        msl_tex.location = (create_info_->auto_resource_location_) ? msl_tex.slot : res.slot;
        msl_tex.is_texture_sampler = true;
        BLI_assert(msl_tex.slot < MTL_MAX_TEXTURE_SLOTS);

        texture_samplers.append(msl_tex);
        max_tex_bind_index = max_ii(max_tex_bind_index, msl_tex.slot);
      } break;

      case shader::ShaderCreateInfo::Resource::BindType::IMAGE: {
        /* Flatten qualifier flags into final access state. */
        MSLTextureSamplerAccess access;
        if ((res.image.qualifiers & Qualifier::read_write) == Qualifier::read_write) {
          access = MSLTextureSamplerAccess::TEXTURE_ACCESS_READWRITE;
        }
        else if (bool(res.image.qualifiers & Qualifier::write)) {
          access = MSLTextureSamplerAccess::TEXTURE_ACCESS_WRITE;
        }
        else {
          access = MSLTextureSamplerAccess::TEXTURE_ACCESS_READ;
        }

        /* Writeable image targets only assigned to Fragment and compute shaders. */
        MSLTextureResource msl_image;
        msl_image.stage = ShaderStage::FRAGMENT | ShaderStage::COMPUTE;
        msl_image.type = res.image.type;
        msl_image.name = res.image.name;
        msl_image.access = access;
        msl_image.slot = texture_slot_id++;
        msl_image.location = (create_info_->auto_resource_location_) ? msl_image.slot : res.slot;
        msl_image.is_texture_sampler = false;
        BLI_assert(msl_image.slot < MTL_MAX_TEXTURE_SLOTS);

        texture_samplers.append(msl_image);
        max_tex_bind_index = max_ii(max_tex_bind_index, msl_image.slot);
      } break;

      case shader::ShaderCreateInfo::Resource::BindType::UNIFORM_BUFFER: {
        MSLBufferBlock ubo;
        BLI_assert(res.uniformbuf.type_name.is_empty() == false);
        BLI_assert(res.uniformbuf.name.is_empty() == false);
        int64_t array_offset = res.uniformbuf.name.find_first_of("[");

        /* We maintain two bind indices. "Slot" refers to the storage index buffer(N) in which
         * we will bind the resource. "Location" refers to the explicit bind index specified
         * in ShaderCreateInfo.
         * NOTE: ubo.slot is offset by one, as first UBO slot is reserved for push constant data.
         */
        ubo.slot = 1 + (ubo_buffer_slot_id_++);
        ubo.location = (create_info_->auto_resource_location_) ? ubo.slot : res.slot;

        BLI_assert(ubo.location >= 0 && ubo.location < MTL_MAX_BUFFER_BINDINGS);

        ubo.qualifiers = shader::Qualifier::read;
        ubo.type_name = res.uniformbuf.type_name;
        ubo.is_texture_buffer = false;
        ubo.is_array = (array_offset > -1);
        if (ubo.is_array) {
          /* If is array UBO, strip out array tag from name. */
          StringRef name_no_array = StringRef(res.uniformbuf.name.c_str(), array_offset);
          ubo.name = name_no_array;
        }
        else {
          ubo.name = res.uniformbuf.name;
        }
        ubo.stage = ShaderStage::ANY;
        uniform_blocks.append(ubo);
      } break;

      case shader::ShaderCreateInfo::Resource::BindType::STORAGE_BUFFER: {
        MSLBufferBlock ssbo;
        BLI_assert(res.storagebuf.type_name.is_empty() == false);
        BLI_assert(res.storagebuf.name.is_empty() == false);
        int64_t array_offset = res.storagebuf.name.find_first_of("[");

        /* We maintain two bind indices. "Slot" refers to the storage index buffer(N) in which
         * we will bind the resource. "Location" refers to the explicit bind index specified
         * in ShaderCreateInfo. */
        ssbo.slot = storage_buffer_slot_id_++;
        ssbo.location = (create_info_->auto_resource_location_) ? ssbo.slot : res.slot;

        max_storage_buffer_location = max_uu(max_storage_buffer_location, ssbo.location);

        BLI_assert(ssbo.location >= 0 && ssbo.location < MTL_MAX_BUFFER_BINDINGS);

        ssbo.qualifiers = res.storagebuf.qualifiers;
        ssbo.type_name = res.storagebuf.type_name;
        ssbo.is_texture_buffer = false;
        ssbo.is_array = (array_offset > -1);
        if (ssbo.is_array) {
          /* If is array UBO, strip out array tag from name. */
          StringRef name_no_array = StringRef(res.storagebuf.name.c_str(), array_offset);
          ssbo.name = name_no_array;
        }
        else {
          ssbo.name = res.storagebuf.name;
        }
        ssbo.stage = ShaderStage::ANY;
        storage_blocks.append(ssbo);
      } break;
    }
  }

  /* For texture atomic fallback support, bind texture source buffers and data buffer as storage
   * blocks. */
  if (!MTLBackend::get_capabilities().supports_texture_atomics) {
    uint atomic_fallback_buffer_count = 0;
    for (MSLTextureResource &tex : texture_samplers) {
      if (ELEM(tex.type,
               ImageType::AtomicUint2D,
               ImageType::AtomicUint2DArray,
               ImageType::AtomicUint3D,
               ImageType::AtomicInt2D,
               ImageType::AtomicInt2DArray,
               ImageType::AtomicInt3D))
      {
        /* Add storage-buffer bind-point. */
        MSLBufferBlock ssbo;

        /* We maintain two bind indices. "Slot" refers to the storage index buffer(N) in which
         * we will bind the resource. "Location" refers to the explicit bind index specified
         * in ShaderCreateInfo.
         * NOTE: For texture buffers, we will accumulate these after all other storage buffers.
         */
        ssbo.slot = storage_buffer_slot_id_++;
        ssbo.location = max_storage_buffer_location + 1 + atomic_fallback_buffer_count;

        /* Flag atomic fallback buffer id and location.
         * ID is used to determine order for accessing parameters, while
         * location is used to extract the explicit bind point for the buffer. */
        tex.atomic_fallback_buffer_ssbo_id = storage_blocks.size();

        BLI_assert(ssbo.location >= 0 && ssbo.location < MTL_MAX_BUFFER_BINDINGS);

        /* Qualifier should be read write and type is either uint or int. */
        ssbo.qualifiers = Qualifier::read_write;
        ssbo.type_name = tex.get_msl_return_type_str();
        ssbo.is_array = false;
        ssbo.name = tex.name + "_storagebuf";
        ssbo.stage = ShaderStage::ANY;
        ssbo.is_texture_buffer = true;
        storage_blocks.append(ssbo);

        /* Add uniform for metadata. */
        MSLUniform uniform(shader::Type::int4_t, tex.name + "_metadata", false, 1);
        uniforms.append(uniform);

        atomic_fallback_buffer_count++;
      }
    }
  }

  /* Assign maximum buffer. */
  max_buffer_slot = storage_buffer_slot_id_ + ubo_buffer_slot_id_ + 1;

  /** Vertex Inputs. */
  bool all_attr_location_assigned = true;
  for (const ShaderCreateInfo::VertIn &attr : info->vertex_inputs_) {

    /* Validate input. */
    BLI_assert(attr.name.is_empty() == false);

    /* NOTE(Metal): Input attributes may not have a location specified.
     * unset locations are resolved during: `resolve_input_attribute_locations`. */
    MSLVertexInputAttribute msl_attr;
    bool attr_location_assigned = (attr.index >= 0);
    all_attr_location_assigned = all_attr_location_assigned && attr_location_assigned;
    msl_attr.layout_location = attr_location_assigned ? attr.index : -1;
    msl_attr.type = attr.type;
    msl_attr.name = attr.name;
    vertex_input_attributes.append(msl_attr);
  }

  /* Ensure all attributes are assigned a location. */
  if (!all_attr_location_assigned) {
    this->resolve_input_attribute_locations();
  }

  /** Fragment outputs. */
  for (const shader::ShaderCreateInfo::FragOut &frag_out : create_info_->fragment_outputs_) {
    /* Validate input. */
    BLI_assert(frag_out.name.is_empty() == false);
    BLI_assert(frag_out.index >= 0);

    /* Populate MSLGenerator attribute. */
    MSLFragmentOutputAttribute mtl_frag_out;
    mtl_frag_out.layout_location = frag_out.index;
    mtl_frag_out.layout_index = (frag_out.blend != DualBlend::NONE) ?
                                    ((frag_out.blend == DualBlend::SRC_0) ? 0 : 1) :
                                    -1;
    mtl_frag_out.type = frag_out.type;
    mtl_frag_out.name = frag_out.name;
    mtl_frag_out.raster_order_group = frag_out.raster_order_group;

    fragment_outputs.append(mtl_frag_out);
  }

  /* Fragment tile inputs. */
  for (const shader::ShaderCreateInfo::SubpassIn &frag_tile_in : create_info_->subpass_inputs_) {

    /* Validate input. */
    BLI_assert(frag_tile_in.name.is_empty() == false);
    BLI_assert(frag_tile_in.index >= 0);

    /* Populate MSLGenerator attribute. */
    MSLFragmentTileInputAttribute mtl_frag_in;
    mtl_frag_in.layout_location = frag_tile_in.index;
    mtl_frag_in.layout_index = -1;
    mtl_frag_in.type = frag_tile_in.type;
    mtl_frag_in.name = frag_tile_in.name;
    mtl_frag_in.raster_order_group = frag_tile_in.raster_order_group;
    mtl_frag_in.is_layered_input = ELEM(frag_tile_in.img_type,
                                        ImageType::Uint2DArray,
                                        ImageType::Int2DArray,
                                        ImageType::Float2DArray);

    fragment_tile_inputs.append(mtl_frag_in);

    /* If we do not support native tile inputs, generate an image-binding per input. */
    if (!MTLBackend::capabilities.supports_native_tile_inputs) {
      /* Generate texture binding resource. */
      MSLTextureResource msl_image;
      msl_image.stage = ShaderStage::FRAGMENT;
      msl_image.type = frag_tile_in.img_type;
      msl_image.name = frag_tile_in.name + "_subpass_img";
      msl_image.access = MSLTextureSamplerAccess::TEXTURE_ACCESS_READ;
      msl_image.slot = texture_slot_id++;
      /* WATCH: We don't have a great place to generate the image bindings.
       * So we will use the subpass binding index and check if it collides with an existing
       * binding. */
      msl_image.location = frag_tile_in.index;
      msl_image.is_texture_sampler = false;
      BLI_assert(msl_image.slot < MTL_MAX_TEXTURE_SLOTS);
      BLI_assert(msl_image.location < MTL_MAX_TEXTURE_SLOTS);

      /* Check existing samplers. */
      for (const auto &tex : texture_samplers) {
        UNUSED_VARS_NDEBUG(tex);
        BLI_assert(tex.location != msl_image.location);
      }

      texture_samplers.append(msl_image);
      max_tex_bind_index = max_ii(max_tex_bind_index, msl_image.slot);
    }
  }
}

bool MSLGeneratorInterface::use_argument_buffer_for_samplers() const
{
  /* We can only use argument buffers IF highest sampler index exceeds static limit of 16,
   * AND we can support more samplers with an argument buffer. */
  bool use_argument_buffer = (texture_samplers.size() >= 15 || max_tex_bind_index >= 14) &&
                             GPU_max_samplers() > 15;

#ifndef NDEBUG
  /* Due to explicit bind location support, we may be below the sampler limit, but forced to offset
   * bindings due to the range being high. Introduce debug check here to issue warning. In these
   * cases, if explicit bind location support is not required, best to use auto_resource_location
   * to optimize bind point packing. */
  if (use_argument_buffer && texture_samplers.size() < 15) {
    MTL_LOG_WARNING(
        "Compiled Shader '%s' is falling back to bindless via argument buffers due to having a "
        "texture sampler of Index: %u Which exceeds the limit of 15+1. However shader only uses "
        "%d textures. Consider optimising bind points with .auto_resource_location(true).",
        parent_shader_.name_get().c_str(),
        max_tex_bind_index,
        (int)texture_samplers.size());
  }
#endif

  return use_argument_buffer;
}

uint32_t MSLGeneratorInterface::num_samplers_for_stage(ShaderStage /*stage*/) const
{
  /* NOTE: Sampler bindings and argument buffer shared across stages,
   * in case stages share texture/sampler bindings. */
  return texture_samplers.size();
}

uint32_t MSLGeneratorInterface::max_sampler_index_for_stage(ShaderStage /*stage*/) const
{
  /* NOTE: Sampler bindings and argument buffer shared across stages,
   * in case stages share texture/sampler bindings. */
  return max_tex_bind_index;
}

uint32_t MSLGeneratorInterface::get_sampler_argument_buffer_bind_index(ShaderStage stage)
{
  /* NOTE: Shader stage must be a singular index. Compound shader masks are not valid for this
   * function. */
  BLI_assert(stage == ShaderStage::VERTEX || stage == ShaderStage::FRAGMENT ||
             stage == ShaderStage::COMPUTE);
  if (sampler_argument_buffer_bind_index[get_shader_stage_index(stage)] >= 0) {
    return sampler_argument_buffer_bind_index[get_shader_stage_index(stage)];
  }

  /* Sampler argument buffer to follow UBOs and PushConstantBlock. */
  sampler_argument_buffer_bind_index[get_shader_stage_index(stage)] = (max_buffer_slot + 1);
  return sampler_argument_buffer_bind_index[get_shader_stage_index(stage)];
}

std::string MSLGeneratorInterface::generate_msl_vertex_entry_stub()
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(ShaderStage::VERTEX);

  std::stringstream out;
  out << std::endl << "/*** AUTO-GENERATED MSL VERETX SHADER STUB. ***/" << std::endl;

  /* Un-define texture defines from main source - avoid conflict with MSL texture. */
  out << "#undef texture" << std::endl;
  out << "#undef textureLod" << std::endl;

  /* Disable special case for booleans being treated as ints in GLSL. */
  out << "#undef bool" << std::endl;

  /* Un-define uniform mappings to avoid name collisions. */
  out << generate_msl_uniform_undefs(ShaderStage::VERTEX);

  /* Generate function entry point signature w/ resource bindings and inputs. */
  out << "vertex ";
  out << get_stage_class_name(ShaderStage::VERTEX) << "::VertexOut ";
#ifndef NDEBUG
  out << "vertex_function_entry_" << parent_shader_.name_get() << "(\n\t";
#else
  out << "vertex_function_entry(\n\t";
#endif

  out << this->generate_msl_vertex_inputs_string();
  out << ") {" << std::endl << std::endl;
  out << "\t" << get_stage_class_name(ShaderStage::VERTEX) << "::VertexOut output;" << std::endl
      << "\t" << get_stage_class_name(ShaderStage::VERTEX) << " " << shader_stage_inst_name << ";"
      << std::endl;

  /* Copy Vertex Globals. */
  if (this->uses_gl_VertexID) {
    out << shader_stage_inst_name << ".gl_VertexID = gl_VertexID;" << std::endl;
  }
  if (this->uses_gl_InstanceID) {
    out << shader_stage_inst_name << ".gl_InstanceID = gl_InstanceID-gl_BaseInstanceARB;"
        << std::endl;
  }
  if (this->uses_gl_BaseInstanceARB) {
    out << shader_stage_inst_name << ".gl_BaseInstanceARB = gl_BaseInstanceARB;" << std::endl;
  }

  /* Copy vertex attributes into local variables. */
  out << this->generate_msl_vertex_attribute_input_population();

  /* Populate Uniforms and uniform blocks. */
  out << this->generate_msl_texture_vars(ShaderStage::VERTEX);
  out << this->generate_msl_global_uniform_population(ShaderStage::VERTEX);
  out << this->generate_msl_uniform_block_population(ShaderStage::VERTEX);

  /* Execute original 'main' function within class scope. */
  out << "\t/* Execute Vertex main function */\t" << std::endl
      << "\t" << shader_stage_inst_name << ".main();" << std::endl
      << std::endl;

  /* Populate Output values. */
  out << this->generate_msl_vertex_output_population();

  /* Final point size,
   * This is only compiled if the `MTL_global_pointsize` is specified
   * as a function specialization in the PSO. This is restricted to
   * point primitive types. */
  out << "if(is_function_constant_defined(MTL_global_pointsize)){ output.pointsize = "
         "(MTL_global_pointsize > 0.0)?MTL_global_pointsize:output.pointsize; }"
      << std::endl;
  out << "\treturn output;" << std::endl;
  out << "}";
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_fragment_entry_stub()
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(
      ShaderStage::FRAGMENT);
  std::stringstream out;
  out << std::endl << "/*** AUTO-GENERATED MSL FRAGMENT SHADER STUB. ***/" << std::endl;

  /* Undefine texture defines from main source - avoid conflict with MSL texture. */
  out << "#undef texture" << std::endl;
  out << "#undef textureLod" << std::endl;

  /* Disable special case for booleans being treated as integers in GLSL. */
  out << "#undef bool" << std::endl;

  /* Undefine uniform mappings to avoid name collisions. */
  out << generate_msl_uniform_undefs(ShaderStage::FRAGMENT);

  /* Early fragment tests. */
  if (uses_early_fragment_test) {
    out << "[[early_fragment_tests]]" << std::endl;
  }

  /* Generate function entry point signature w/ resource bindings and inputs. */
#ifndef NDEBUG
  out << "fragment " << get_stage_class_name(ShaderStage::FRAGMENT)
      << "::" FRAGMENT_OUT_STRUCT_NAME " fragment_function_entry_" << parent_shader_.name_get()
      << "(\n\t";
#else
  out << "fragment " << get_stage_class_name(ShaderStage::FRAGMENT)
      << "::" FRAGMENT_OUT_STRUCT_NAME " fragment_function_entry(\n\t";
#endif
  out << this->generate_msl_fragment_inputs_string();
  out << ") {" << std::endl << std::endl;
  out << "\t" << get_stage_class_name(ShaderStage::FRAGMENT)
      << "::" FRAGMENT_OUT_STRUCT_NAME " output;" << std::endl
      << "\t" << get_stage_class_name(ShaderStage::FRAGMENT) << " " << shader_stage_inst_name
      << ";" << std::endl;

  /* Copy Fragment Globals. */
  if (this->uses_gl_PointCoord) {
    out << shader_stage_inst_name << ".gl_PointCoord = gl_PointCoord;" << std::endl;
  }
  if (this->uses_gl_FrontFacing) {
    out << shader_stage_inst_name << ".gl_FrontFacing = gl_FrontFacing;" << std::endl;
  }
  if (this->uses_gl_PrimitiveID) {
    out << "fragment_shader_instance.gl_PrimitiveID = gl_PrimitiveID;" << std::endl;
  }

  /* Copy vertex attributes into local variable.s */
  out << this->generate_msl_fragment_input_population();

  /* Barycentrics. */
  if (this->uses_barycentrics) {
    out << shader_stage_inst_name << ".gpu_BaryCoord = mtl_barycentric_coord.xyz;" << std::endl;
  }

  /* Populate Uniforms and uniform blocks. */
  out << this->generate_msl_texture_vars(ShaderStage::FRAGMENT);
  out << this->generate_msl_global_uniform_population(ShaderStage::FRAGMENT);
  out << this->generate_msl_uniform_block_population(ShaderStage::FRAGMENT);

  /* Populate fragment tile-in members. */
  if (this->fragment_tile_inputs.is_empty() == false) {
    out << this->generate_msl_fragment_tile_input_population();
  }

  /* Execute original 'main' function within class scope. */
  out << "\t/* Execute Fragment main function */\t" << std::endl
      << "\t" << shader_stage_inst_name << ".main();" << std::endl
      << std::endl;

  /* Populate Output values. */
  out << this->generate_msl_fragment_output_population();
  out << "  return output;" << std::endl << "}";

  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_compute_entry_stub(
    const shader::ShaderCreateInfo &info)
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(ShaderStage::COMPUTE);
  std::stringstream out;
  out << std::endl << "/*** AUTO-GENERATED MSL COMPUTE SHADER STUB. ***/" << std::endl;

  /* Un-define texture defines from main source - avoid conflict with MSL texture. */
  out << "#undef texture" << std::endl;
  out << "#undef textureLod" << std::endl;

  /* Disable special case for booleans being treated as ints in GLSL. */
  out << "#undef bool" << std::endl;

  /* Un-define uniform mappings to avoid name collisions. */
  out << generate_msl_uniform_undefs(ShaderStage::COMPUTE);

  /* Generate function entry point signature w/ resource bindings and inputs. */
  out << "kernel void ";
#ifndef NDEBUG
  out << "compute_function_entry_" << parent_shader_.name_get() << "(\n\t";
#else
  out << "compute_function_entry(\n\t";
#endif

  out << this->generate_msl_compute_inputs_string();
  out << ") {" << std::endl << std::endl;
  if (!info.shared_variables_.is_empty()) {
    shared_variable_declare(info, out);
  }
  else {
    out << "MSL_SHARED_VARS_DECLARE\n";
  }

  out << "\t" << get_stage_class_name(ShaderStage::COMPUTE) << " " << shader_stage_inst_name;
  /* Shared vars should be either all be declared in shader (MSL_SHARED_VARS_* path) or all in
   * create infos (shared_variable_* path). */
  if (!info.shared_variables_.is_empty()) {
    shared_variable_pass(info, out);
  }
  else {
    out << " MSL_SHARED_VARS_PASS ";
  }
  out << ";\n";

  /* Copy global variables. */
  /* Entry point parameters for gl Globals. */
  if (this->uses_gl_GlobalInvocationID) {
    out << shader_stage_inst_name << ".gl_GlobalInvocationID = gl_GlobalInvocationID;"
        << std::endl;
  }
  if (this->uses_gl_WorkGroupID) {
    out << shader_stage_inst_name << ".gl_WorkGroupID = gl_WorkGroupID;" << std::endl;
  }
  if (this->uses_gl_NumWorkGroups) {
    out << shader_stage_inst_name << ".gl_NumWorkGroups = gl_NumWorkGroups;" << std::endl;
  }
  if (this->uses_gl_LocalInvocationIndex) {
    out << shader_stage_inst_name << ".gl_LocalInvocationIndex = gl_LocalInvocationIndex;"
        << std::endl;
  }
  if (this->uses_gl_LocalInvocationID) {
    out << shader_stage_inst_name << ".gl_LocalInvocationID = gl_LocalInvocationID;" << std::endl;
  }

  /* Populate Uniforms and uniform blocks. */
  out << this->generate_msl_texture_vars(ShaderStage::COMPUTE);
  out << this->generate_msl_global_uniform_population(ShaderStage::COMPUTE);
  out << this->generate_msl_uniform_block_population(ShaderStage::COMPUTE);

  /* Execute original 'main' function within class scope. */
  out << "\t/* Execute Compute main function */\t" << std::endl
      << "\t" << shader_stage_inst_name << ".main();" << std::endl
      << std::endl;

  out << "}";
  return out.str();
}

/* If first parameter in function signature, do not print out a comma.
 * Update first parameter flag to false for future invocations. */
static char parameter_delimiter(bool &is_first_parameter)
{
  if (is_first_parameter) {
    is_first_parameter = false;
    return ' ';
  }
  return ',';
}

void MSLGeneratorInterface::generate_msl_textures_input_string(std::stringstream &out,
                                                               ShaderStage stage,
                                                               bool &is_first_parameter)
{
  /* NOTE: Shader stage must be specified as the singular stage index for which the input
   * is generating. Compound stages are not valid inputs. */
  BLI_assert(stage == ShaderStage::VERTEX || stage == ShaderStage::FRAGMENT ||
             stage == ShaderStage::COMPUTE);
  /* Generate texture signatures for textures used by this stage. */
  BLI_assert(this->texture_samplers.size() <= GPU_max_textures_vert());
  for (const MSLTextureResource &tex : this->texture_samplers) {
    if (bool(tex.stage & stage)) {
      out << parameter_delimiter(is_first_parameter) << "\n\t" << tex.get_msl_typestring(false)
          << " [[texture(" << tex.slot << ")]]";
    }
  }

  /* Generate sampler signatures. */
  /* NOTE: Currently textures and samplers share indices across shading stages, so the limit is
   * shared.
   * If we exceed the hardware-supported limit, then follow a bind-less model using argument
   * buffers. */
  if (this->use_argument_buffer_for_samplers()) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconstant SStruct& samplers [[buffer(MTL_uniform_buffer_base_index+"
        << (this->get_sampler_argument_buffer_bind_index(stage)) << ")]]";
  }
  else {
    /* Maximum Limit of samplers defined in the function argument table is
     * `MTL_MAX_DEFAULT_SAMPLERS=16`. */
    BLI_assert(this->texture_samplers.size() <= MTL_MAX_DEFAULT_SAMPLERS);
    for (const MSLTextureResource &tex : this->texture_samplers) {
      if (bool(tex.stage & stage)) {
        out << parameter_delimiter(is_first_parameter) << "\n\tsampler " << tex.name
            << "_sampler [[sampler(" << tex.slot << ")]]";
      }
    }

    /* Fallback. */
    if (this->texture_samplers.size() > 16) {
      shader_debug_printf(
          "[Metal] Warning: Shader exceeds limit of %u samplers on current hardware\n",
          MTL_MAX_DEFAULT_SAMPLERS);
    }
  }
}

void MSLGeneratorInterface::generate_msl_uniforms_input_string(std::stringstream &out,
                                                               ShaderStage stage,
                                                               bool &is_first_parameter)
{
  for (const MSLBufferBlock &ubo : this->uniform_blocks) {
    if (bool(ubo.stage & stage)) {
      /* For literal/existing global types, we do not need the class name-space accessor. */
      out << parameter_delimiter(is_first_parameter) << "\n\tconstant ";
      if (!is_builtin_type(ubo.type_name)) {
        out << get_stage_class_name(stage) << "::";
      }
      /* #UniformBuffer bind indices start at `MTL_uniform_buffer_base_index + 1`, as
       * MTL_uniform_buffer_base_index is reserved for the #PushConstantBlock (push constants).
       * MTL_uniform_buffer_base_index is an offset depending on the number of unique VBOs
       * bound for the current PSO specialization. */
      out << ubo.type_name << "* " << ubo.name << "[[buffer(MTL_uniform_buffer_base_index+"
          << ubo.slot << ")]]";
    }
  }

  /* Storage buffers. */
  for (const MSLBufferBlock &ssbo : this->storage_blocks) {
    if (bool(ssbo.stage & stage)) {
      out << parameter_delimiter(is_first_parameter) << "\n\t";
      if (bool(stage & ShaderStage::VERTEX)) {
        out << "const ";
      }
      /* For literal/existing global types, we do not need the class name-space accessor. */
      bool writeable = (ssbo.qualifiers & shader::Qualifier::write) == shader::Qualifier::write;
      const char *memory_scope = ((writeable) ? "device " : "constant ");
      out << memory_scope;
      if (!is_builtin_type(ssbo.type_name)) {
        out << get_stage_class_name(stage) << "::";
      }
      /* #StorageBuffer bind indices start at `MTL_storage_buffer_base_index`.
       * MTL_storage_buffer_base_index follows immediately after all uniform blocks.
       * such that MTL_storage_buffer_base_index = MTL_uniform_buffer_base_index +
       * uniform_blocks.size() + 1. Where the additional buffer is reserved for the
       * #PushConstantBlock (push constants). */
      out << ssbo.type_name << "* " << ssbo.name << "[[buffer(MTL_storage_buffer_base_index+"
          << (ssbo.slot) << ")]]";
    }
  }
}

std::string MSLGeneratorInterface::generate_msl_vertex_inputs_string()
{
  std::stringstream out;
  bool is_first_parameter = true;

  if (this->vertex_input_attributes.is_empty() == false) {
    /* Vertex Buffers use input assembly. */
    out << get_stage_class_name(ShaderStage::VERTEX) << "::VertexIn v_in [[stage_in]]";
    is_first_parameter = false;
  }

  if (this->uniforms.is_empty() == false) {
    out << parameter_delimiter(is_first_parameter) << "\n\tconstant "
        << get_stage_class_name(ShaderStage::VERTEX)
        << "::PushConstantBlock* uniforms[[buffer(MTL_uniform_buffer_base_index)]]";
    is_first_parameter = false;
  }

  this->generate_msl_uniforms_input_string(out, ShaderStage::VERTEX, is_first_parameter);

  /* Generate texture signatures. */
  this->generate_msl_textures_input_string(out, ShaderStage::VERTEX, is_first_parameter);

  /* Entry point parameters for gl Globals. */
  if (this->uses_gl_VertexID) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint32_t gl_VertexID [[vertex_id]]";
  }
  if (this->uses_gl_InstanceID) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint32_t gl_InstanceID [[instance_id]]";
  }
  if (this->uses_gl_BaseInstanceARB) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint32_t gl_BaseInstanceARB [[base_instance]]";
  }
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_fragment_inputs_string()
{
  bool is_first_parameter = true;
  std::stringstream out;
  out << parameter_delimiter(is_first_parameter) << get_stage_class_name(ShaderStage::FRAGMENT)
      << "::VertexOut v_in [[stage_in]]";

  if (this->uniforms.is_empty() == false) {
    out << parameter_delimiter(is_first_parameter) << "\n\tconstant "
        << get_stage_class_name(ShaderStage::FRAGMENT)
        << "::PushConstantBlock* uniforms[[buffer(MTL_uniform_buffer_base_index)]]";
  }

  this->generate_msl_uniforms_input_string(out, ShaderStage::FRAGMENT, is_first_parameter);

  /* Generate texture signatures. */
  this->generate_msl_textures_input_string(out, ShaderStage::FRAGMENT, is_first_parameter);

  if (this->uses_gl_PointCoord) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst float2 gl_PointCoord [[point_coord]]";
  }
  if (this->uses_gl_FrontFacing) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst bool gl_FrontFacing [[front_facing]]";
  }
  if (this->uses_gl_PrimitiveID) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint gl_PrimitiveID [[primitive_id]]";
  }

  /* Barycentrics. */
  if (this->uses_barycentrics) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst float3 mtl_barycentric_coord [[barycentric_coord]]";
  }

  /* Fragment tile-inputs. */
  if (this->fragment_tile_inputs.is_empty() == false) {
    out << parameter_delimiter(is_first_parameter) << "\n\t"
        << get_stage_class_name(ShaderStage::FRAGMENT)
        << "::" FRAGMENT_TILE_IN_STRUCT_NAME " fragment_tile_in";
  }
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_compute_inputs_string()
{
  bool is_first_parameter = true;
  std::stringstream out;
  if (this->uniforms.is_empty() == false) {
    out << parameter_delimiter(is_first_parameter) << "constant "
        << get_stage_class_name(ShaderStage::COMPUTE)
        << "::PushConstantBlock* uniforms[[buffer(MTL_uniform_buffer_base_index)]]";
  }

  this->generate_msl_uniforms_input_string(out, ShaderStage::COMPUTE, is_first_parameter);

  /* Generate texture signatures. */
  this->generate_msl_textures_input_string(out, ShaderStage::COMPUTE, is_first_parameter);

  /* Entry point parameters for gl Globals. */
  if (this->uses_gl_GlobalInvocationID) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint3 gl_GlobalInvocationID [[thread_position_in_grid]]";
  }
  if (this->uses_gl_WorkGroupID) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint3 gl_WorkGroupID [[threadgroup_position_in_grid]]";
  }
  if (this->uses_gl_NumWorkGroups) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint3 gl_NumWorkGroups [[threadgroups_per_grid]]";
  }
  if (this->uses_gl_LocalInvocationIndex) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]]";
  }
  if (this->uses_gl_LocalInvocationID) {
    out << parameter_delimiter(is_first_parameter)
        << "\n\tconst uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]]";
  }

  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_uniform_structs(ShaderStage shader_stage)
{
  /* Only generate PushConstantBlock if we have uniforms. */
  if (this->uniforms.size() == 0) {
    return "";
  }
  BLI_assert(shader_stage == ShaderStage::VERTEX || shader_stage == ShaderStage::FRAGMENT);
  UNUSED_VARS_NDEBUG(shader_stage);
  std::stringstream out;

  /* Common Uniforms. */
  out << "typedef struct {" << std::endl;

  for (const MSLUniform &uniform : this->uniforms) {
    if (uniform.is_array) {
      out << "\t" << to_string(uniform.type) << " " << uniform.name << "[" << uniform.array_elems
          << "];" << std::endl;
    }
    else {
      out << "\t" << to_string(uniform.type) << " " << uniform.name << ";" << std::endl;
    }
  }
  out << "} PushConstantBlock;\n\n";

  /* Member UBO block reference. */
  out << std::endl << "const constant PushConstantBlock *global_uniforms;" << std::endl;

  /* Macro define chain.
   * To access uniforms, we generate a macro such that the uniform name can
   * be used directly without using the struct's handle. */
  for (const MSLUniform &uniform : this->uniforms) {
    out << "#define " << uniform.name << " global_uniforms->" << uniform.name << std::endl;
  }
  out << std::endl;
  return out.str();
}

/* NOTE: Uniform macro definition vars can conflict with other parameters. */
std::string MSLGeneratorInterface::generate_msl_uniform_undefs(ShaderStage /*shader_stage*/)
{
  std::stringstream out;

  /* Macro undef chain. */
  for (const MSLUniform &uniform : this->uniforms) {
    out << "#undef " << uniform.name << std::endl;
  }
  /* UBO block undef. */
  for (const MSLBufferBlock &ubo : this->uniform_blocks) {
    out << "#undef " << ubo.name << std::endl;
  }
  /* SSBO block undef. */
  for (const MSLBufferBlock &ssbo : this->storage_blocks) {
    out << "#undef " << ssbo.name << std::endl;
  }
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_vertex_in_struct()
{
  std::stringstream out;

  /* Skip struct if no vert attributes. */
  if (this->vertex_input_attributes.size() == 0) {
    return "";
  }

  /* Output */
  out << "typedef struct {" << std::endl;
  for (const MSLVertexInputAttribute &in_attr : this->vertex_input_attributes) {
    /* Matrix and array attributes are not trivially supported and thus
     * require each element to be passed as an individual attribute.
     * This requires shader source generation of sequential elements.
     * The matrix type is then re-packed into a Mat4 inside the entry function.
     *
     * e.g.
     * float4 __internal_modelmatrix_0 [[attribute(0)]];
     * float4 __internal_modelmatrix_1 [[attribute(1)]];
     * float4 __internal_modelmatrix_2 [[attribute(2)]];
     * float4 __internal_modelmatrix_3 [[attribute(3)]];
     */
    if (is_matrix_type(in_attr.type)) {
      for (int elem = 0; elem < get_matrix_location_count(in_attr.type); elem++) {
        out << "\t" << get_matrix_subtype(in_attr.type) << " __internal_" << in_attr.name << elem
            << " [[attribute(" << (in_attr.layout_location + elem) << ")]];" << std::endl;
      }
    }
    else {
      out << "\t" << in_attr.type << " " << in_attr.name << " [[attribute("
          << in_attr.layout_location << ")]];" << std::endl;
    }
  }

  out << "} VertexIn;" << std::endl << std::endl;

  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_vertex_out_struct(ShaderStage shader_stage)
{
  BLI_assert(shader_stage == ShaderStage::VERTEX || shader_stage == ShaderStage::FRAGMENT);
  std::stringstream out;

  /* Vertex output struct. */
  out << "typedef struct {" << std::endl;

  /* If we use GL position, our standard output variable will be mapped to '_default_position_'.
   * Otherwise, we use the FIRST element in the output array. */
  bool first_attr_is_position = false;
  if (this->uses_gl_Position) {

    /* If invariance is available, utilize this to consistently mitigate depth fighting artifacts
     * by ensuring that vertex position is consistently calculated between subsequent passes
     * with maximum precision. */
    out << "\tfloat4 _default_position_ [[position]]";
    out << " [[invariant]]";
    out << ";" << std::endl;
  }
  else {
    /* Use first output element for position. */
    BLI_assert(this->vertex_output_varyings.is_empty() == false);
    BLI_assert(this->vertex_output_varyings[0].type == "vec4");

    /* Use invariance if available. See above for detail. */
    out << "\tfloat4 " << this->vertex_output_varyings[0].name << " [[position]];";
    out << " [[invariant]]";
    out << ";" << std::endl;
    first_attr_is_position = true;
  }

  /* Generate other vertex output members. */
  bool skip_first_index = first_attr_is_position;
  for (const MSLVertexOutputAttribute &v_out : this->vertex_output_varyings) {

    /* Skip first index if used for position. */
    if (skip_first_index) {
      skip_first_index = false;
      continue;
    }

    if (v_out.is_array) {
      /* Array types cannot be trivially passed between shading stages.
       * Instead we pass each component individually. E.g. vec4 pos[2]
       * will be converted to: `vec4 pos_0; vec4 pos_1;`
       * The specified interpolation qualifier will be applied per element. */
      /* TODO(Metal): Support array of matrix in-out types if required
       * e.g. Mat4 out_matrices[3]. */
      for (int i = 0; i < v_out.array_elems; i++) {
        out << "\t" << v_out.type << " " << v_out.instance_name << "_" << v_out.name << i
            << v_out.get_mtl_interpolation_qualifier() << ";" << std::endl;
      }
    }
    else {
      /* Matrix types need to be expressed as their vector sub-components. */
      if (is_matrix_type(v_out.type)) {
        BLI_assert(v_out.get_mtl_interpolation_qualifier() == " [[flat]]" &&
                   "Matrix varying types must have [[flat]] interpolation");
        std::string subtype = get_matrix_subtype(v_out.type);
        for (int elem = 0; elem < get_matrix_location_count(v_out.type); elem++) {
          out << "\t" << subtype << v_out.instance_name << " __matrix_" << v_out.name << elem
              << v_out.get_mtl_interpolation_qualifier() << ";" << std::endl;
        }
      }
      else {
        out << "\t" << v_out.type << " " << v_out.instance_name << "_" << v_out.name
            << v_out.get_mtl_interpolation_qualifier() << ";" << std::endl;
      }
    }
  }

  /* Add gl_PointSize if written to. */
  if (shader_stage == ShaderStage::VERTEX) {
    if (this->uses_gl_PointSize) {
      /* If `gl_PointSize` is explicitly written to,
       * we will output the written value directly.
       * This value can still be overridden by the
       * global point-size value. */
      out << "\tfloat pointsize [[point_size]];" << std::endl;
    }
    else {
      /* Otherwise, if point-size is not written to inside the shader,
       * then its usage is controlled by whether the `MTL_global_pointsize`
       * function constant has been specified.
       * This function constant is enabled for all point primitives being rendered. */
      out << "\tfloat pointsize [[point_size, function_constant(MTL_global_pointsize)]];"
          << std::endl;
    }
  }

  /* Add gl_ClipDistance[n]. */
  if (shader_stage == ShaderStage::VERTEX) {
    out << "#if defined(USE_CLIP_PLANES) || defined(USE_WORLD_CLIP_PLANES)" << std::endl;
    if (this->clip_distances.size() > 1) {
      /* Output array of clip distances if specified. */
      out << "\tfloat clipdistance [[clip_distance, "
             "function_constant(MTL_clip_distances_enabled)]] ["
          << this->clip_distances.size() << "];" << std::endl;
    }
    else if (this->clip_distances.is_empty() == false) {
      out << "\tfloat clipdistance [[clip_distance, "
             "function_constant(MTL_clip_distances_enabled)]];"
          << std::endl;
    }
    out << "#endif" << std::endl;
  }

  /* Add MTL render target array index for multilayered rendering support. */
  if (uses_gpu_layer) {
    out << "\tuint gpu_Layer [[render_target_array_index]];" << std::endl;
  }

  /* Add Viewport Index output */
  if (uses_gpu_viewport_index) {
    out << "\tuint gpu_ViewportIndex [[viewport_array_index]];" << std::endl;
  }

  out << "} VertexOut;" << std::endl << std::endl;

  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_fragment_struct(bool is_input)
{
  std::stringstream out;

  auto &fragment_interface_src = (is_input) ? this->fragment_tile_inputs : this->fragment_outputs;

  /* Output. */
  out << "typedef struct {" << std::endl;
  for (int f_output = 0; f_output < fragment_interface_src.size(); f_output++) {
    out << "\t" << to_string(fragment_interface_src[f_output].type) << " "
        << fragment_interface_src[f_output].name << " [[color("
        << fragment_interface_src[f_output].layout_location << ")";
    if (fragment_interface_src[f_output].layout_index >= 0) {
      out << ", index(" << fragment_interface_src[f_output].layout_index << ")";
    }
    if (fragment_interface_src[f_output].raster_order_group >= 0) {
      out << ", raster_order_group(" << fragment_interface_src[f_output].raster_order_group << ")";
    }
    out << "]]"
        << ";" << std::endl;
  }
  /* Add gl_FragDepth output if used. */
  if (this->uses_gl_FragDepth) {
    std::string out_depth_argument = ((this->depth_write == DepthWrite::GREATER) ?
                                          "greater" :
                                          ((this->depth_write == DepthWrite::LESS) ? "less" :
                                                                                     "any"));
    out << "\tfloat fragdepth [[depth(" << out_depth_argument << ")]];" << std::endl;
  }
  /* Add gl_FragStencilRefARB output if used. */
  if (!is_input && this->uses_gl_FragStencilRefARB) {
    out << "\tuint fragstencil [[stencil]];" << std::endl;
  }
  if (is_input) {
    out << "} " FRAGMENT_TILE_IN_STRUCT_NAME ";" << std::endl;
  }
  else {
    out << "} " FRAGMENT_OUT_STRUCT_NAME ";" << std::endl;
  }
  out << std::endl;
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_global_uniform_population(ShaderStage stage)
{
  if (this->uniforms.size() == 0) {
    return "";
  }
  /* Populate Global Uniforms. */
  std::stringstream out;

  /* Copy UBO block ref. */
  out << "\t/* Copy Uniform block member reference */" << std::endl;
  out << "\t" << get_shader_stage_instance_name(stage) << "."
      << "global_uniforms = uniforms;" << std::endl;

  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_fragment_tile_input_population()
{
  std::stringstream out;

  /* Native tile read is supported on tile-based architectures (Apple Silicon). */
  if (MTLBackend::capabilities.supports_native_tile_inputs) {
    for (const MSLFragmentTileInputAttribute &tile_input : this->fragment_tile_inputs) {
      out << "\t" << get_shader_stage_instance_name(ShaderStage::FRAGMENT) << "."
          << tile_input.name << " = "
          << "fragment_tile_in." << tile_input.name << ";" << std::endl;
    }
  }
  else {
    for (const MSLFragmentTileInputAttribute &tile_input : this->fragment_tile_inputs) {
      /* Get read swizzle mask. */
      char swizzle[] = "xyzw";
      swizzle[to_component_count(tile_input.type)] = '\0';

      bool is_layered_fb = bool(create_info_->builtins_ & BuiltinBits::LAYER);
      std::string texel_co =
          (tile_input.is_layered_input) ?
              ((is_layered_fb)  ? "ivec3(ivec2(v_in._default_position_.xy), int(v_in.gpu_Layer))" :
                                  /* This should fetch the attached layer.
                                   * But this is not simple to set. For now
                                   * assume it is always the first layer. */
                                  "ivec3(ivec2(v_in._default_position_.xy), 0)") :
              "ivec2(v_in._default_position_.xy)";

      out << "\t" << get_shader_stage_instance_name(ShaderStage::FRAGMENT) << "."
          << tile_input.name << " = imageLoad("
          << get_shader_stage_instance_name(ShaderStage::FRAGMENT) << "." << tile_input.name
          << "_subpass_img, " << texel_co << ")." << swizzle << ";\n";
    }
  }
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_uniform_block_population(ShaderStage stage)
{
  /* Populate Global Uniforms. */
  std::stringstream out;
  out << "\t/* Copy UBO block references into local class variables */" << std::endl;
  for (const MSLBufferBlock &ubo : this->uniform_blocks) {

    /* Only include blocks which are used within this stage. */
    if (bool(ubo.stage & stage)) {
      /* Generate UBO reference assignment.
       * NOTE(Metal): We append `_local` post-fix onto the class member name
       * for the ubo to avoid name collision with the UBO accessor macro.
       * We only need to add this post-fix for the non-array access variant,
       * as the array is indexed directly, rather than requiring a dereference. */
      out << "\t" << get_shader_stage_instance_name(stage) << "." << ubo.name;
      if (!ubo.is_array) {
        out << "_local";
      }
      out << " = " << ubo.name << ";" << std::endl;
    }
  }

  /* Populate storage buffer references. */
  out << "\t/* Copy SSBO block references into local class variables */" << std::endl;
  for (const MSLBufferBlock &ssbo : this->storage_blocks) {

    /* Only include blocks which are used within this stage. */
    if (bool(ssbo.stage & stage) && !ssbo.is_texture_buffer) {
      /* Generate UBO reference assignment.
       * NOTE(Metal): We append `_local` post-fix onto the class member name
       * for the ubo to avoid name collision with the UBO accessor macro.
       * We only need to add this post-fix for the non-array access variant,
       * as the array is indexed directly, rather than requiring a dereference. */
      out << "\t" << get_shader_stage_instance_name(stage) << "." << ssbo.name;
      if (!ssbo.is_array) {
        out << "_local";
      }
      out << " = ";

      if (bool(stage & ShaderStage::VERTEX)) {
        bool writeable = bool(ssbo.qualifiers & shader::Qualifier::write);
        const char *memory_scope = ((writeable) ? "device " : "constant ");

        out << "const_cast<" << memory_scope;

        if (!is_builtin_type(ssbo.type_name)) {
          out << get_stage_class_name(stage) << "::";
        }
        out << ssbo.type_name << "*>(";
      }
      out << ssbo.name;
      if (bool(stage & ShaderStage::VERTEX)) {
        out << ")";
      }
      out << ";" << std::endl;
    }
  }

  out << std::endl;
  return out.str();
}

/* Copy input attributes from stage_in into class local variables. */
std::string MSLGeneratorInterface::generate_msl_vertex_attribute_input_population()
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(ShaderStage::VERTEX);

  /* Populate local attribute variables. */
  std::stringstream out;
  out << "\t/* Copy Vertex Stage-in attributes into local variables */" << std::endl;
  for (int attribute = 0; attribute < this->vertex_input_attributes.size(); attribute++) {

    if (is_matrix_type(this->vertex_input_attributes[attribute].type)) {
      /* Reading into an internal matrix from split attributes: Should generate the following:
       * vertex_shader_instance.mat_attribute_type =
       * mat4(v_in.__internal_mat_attribute_type0,
       *      v_in.__internal_mat_attribute_type1,
       *      v_in.__internal_mat_attribute_type2,
       *      v_in.__internal_mat_attribute_type3). */
      out << "\t" << shader_stage_inst_name << "." << this->vertex_input_attributes[attribute].name
          << " = " << this->vertex_input_attributes[attribute].type << "(v_in.__internal_"
          << this->vertex_input_attributes[attribute].name << 0;
      for (int elem = 1;
           elem < get_matrix_location_count(this->vertex_input_attributes[attribute].type);
           elem++)
      {
        out << ",\n"
            << "v_in.__internal_" << this->vertex_input_attributes[attribute].name << elem;
      }
      out << ");";
    }
    else {
      /* OpenGL uses the `GPU_FETCH_*` functions which can alter how an attribute value is
       * interpreted. In Metal, we cannot support all implicit conversions within the vertex
       * descriptor/vertex stage-in, so we need to perform value transformation on-read.
       *
       * This is handled by wrapping attribute reads to local shader registers in a
       * suitable conversion function `attribute_conversion_func_name`.
       * This conversion function performs a specific transformation on the source
       * vertex data, depending on the specified GPU_FETCH_* mode for the current
       * vertex format.
       *
       * The fetch_mode is specified per-attribute using specialization constants
       * on the PSO, wherein a unique set of constants is passed in per vertex
       * buffer/format configuration. Efficiently enabling pass-through reads
       * if no special fetch is required. */
      bool do_attribute_conversion_on_read = false;
      std::string attribute_conversion_func_name = get_attribute_conversion_function(
          &do_attribute_conversion_on_read, this->vertex_input_attributes[attribute].type);

      if (do_attribute_conversion_on_read) {
        BLI_assert(this->vertex_input_attributes[attribute].layout_location >= 0);
        out << "\t" << attribute_conversion_func_name << "(MTL_AttributeConvert"
            << this->vertex_input_attributes[attribute].layout_location << ", v_in."
            << this->vertex_input_attributes[attribute].name << ", " << shader_stage_inst_name
            << "." << this->vertex_input_attributes[attribute].name << ");" << std::endl;
      }
      else {
        out << "\t" << shader_stage_inst_name << "."
            << this->vertex_input_attributes[attribute].name << " = v_in."
            << this->vertex_input_attributes[attribute].name << ";" << std::endl;
      }
    }
  }
  out << std::endl;
  return out.str();
}

/* Copy post-main, modified, local class variables into vertex-output struct. */
std::string MSLGeneratorInterface::generate_msl_vertex_output_population()
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(ShaderStage::VERTEX);
  std::stringstream out;
  out << "\t/* Copy Vertex Outputs into output struct */" << std::endl;

  /* Output gl_Position with conversion to Metal coordinate-space. */
  if (this->uses_gl_Position) {
    out << "\toutput._default_position_ = " << shader_stage_inst_name << ".gl_Position;"
        << std::endl;

    /* Invert Y and rescale depth range.
     * This is an alternative method to modifying all projection matrices. */
    out << "\toutput._default_position_.y = -output._default_position_.y;" << std::endl;
    out << "\toutput._default_position_.z = "
           "(output._default_position_.z+output._default_position_.w)/2.0;"
        << std::endl;
  }

  /* Output Point-size. */
  if (this->uses_gl_PointSize) {
    out << "\toutput.pointsize = " << shader_stage_inst_name << ".gl_PointSize;" << std::endl;
  }

  /* Output render target array Index. */
  if (uses_gpu_layer) {
    out << "\toutput.gpu_Layer = " << shader_stage_inst_name << ".gpu_Layer;" << std::endl;
  }

  /* Output Viewport Index. */
  if (uses_gpu_viewport_index) {
    out << "\toutput.gpu_ViewportIndex = " << shader_stage_inst_name << ".gpu_ViewportIndex;"
        << std::endl;
  }

  /* Output clip-distances.
   * Clip distances are only written to if both clipping planes are turned on for the shader,
   * and the clipping planes are enabled. Enablement is controlled on a per-plane basis
   * via function constants in the shader pipeline state object (PSO). */
  out << "#if defined(USE_CLIP_PLANES) || defined(USE_WORLD_CLIP_PLANES)" << std::endl
      << "if(MTL_clip_distances_enabled) {" << std::endl;
  if (this->clip_distances.size() > 1) {
    for (int cd = 0; cd < this->clip_distances.size(); cd++) {
      /* Default value when clipping is disabled >= 0.0 to ensure primitive is not clipped. */
      out << "\toutput.clipdistance[" << cd
          << "] = (is_function_constant_defined(MTL_clip_distance_enabled" << cd << "))?"
          << shader_stage_inst_name << ".gl_ClipDistance_" << cd << ":1.0;" << std::endl;
    }
  }
  else if (this->clip_distances.is_empty() == false) {
    out << "\toutput.clipdistance = " << shader_stage_inst_name << ".gl_ClipDistance_0;"
        << std::endl;
  }
  out << "}" << std::endl << "#endif" << std::endl;

  /* Populate output vertex variables. */
  int output_id = 0;
  for (const MSLVertexOutputAttribute &v_out : this->vertex_output_varyings) {
    if (v_out.is_array) {

      for (int i = 0; i < v_out.array_elems; i++) {
        out << "\toutput." << v_out.instance_name << "_" << v_out.name << i << " = "
            << shader_stage_inst_name << ".";

        if (v_out.instance_name.empty() == false) {
          out << v_out.instance_name << ".";
        }

        out << v_out.name << "[" << i << "]"
            << ";" << std::endl;
      }
    }
    else {
      /* Matrix types are split into vectors and need to be reconstructed. */
      if (is_matrix_type(v_out.type)) {
        for (int elem = 0; elem < get_matrix_location_count(v_out.type); elem++) {
          out << "\toutput." << v_out.instance_name << "__matrix_" << v_out.name << elem << " = "
              << shader_stage_inst_name << ".";

          if (v_out.instance_name.empty() == false) {
            out << v_out.instance_name << ".";
          }

          out << v_out.name << "[" << elem << "];" << std::endl;
        }
      }
      else {
        /* If we are not using gl_Position, first vertex output is used for position.
         * Ensure it is vec4. */
        if (!this->uses_gl_Position && output_id == 0) {
          out << "\toutput." << v_out.instance_name << "_" << v_out.name << " = to_vec4("
              << shader_stage_inst_name << "." << v_out.name << ");" << std::endl;

          /* Invert Y */
          out << "\toutput." << v_out.instance_name << "_" << v_out.name << ".y = -output."
              << v_out.name << ".y;" << std::endl;
        }
        else {
          /* Assign vertex output. */
          out << "\toutput." << v_out.instance_name << "_" << v_out.name << " = "
              << shader_stage_inst_name << ".";

          if (v_out.instance_name.empty() == false) {
            out << v_out.instance_name << ".";
          }

          out << v_out.name << ";" << std::endl;
        }
      }
    }
    output_id++;
  }
  out << std::endl;
  return out.str();
}

/* Copy fragment stage inputs (Vertex Outputs) into local class variables. */
std::string MSLGeneratorInterface::generate_msl_fragment_input_population()
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(
      ShaderStage::FRAGMENT);
  /* Populate local attribute variables. */
  std::stringstream out;
  out << "\t/* Copy Fragment input into local variables. */" << std::endl;

  /* Special common case for gl_FragCoord, assigning to input position. */
  if (this->uses_gl_Position) {
    out << "\t" << shader_stage_inst_name << ".gl_FragCoord = v_in._default_position_;"
        << std::endl;
  }
  else {
    /* When gl_Position is not set, first VertexIn element is used for position. */
    out << "\t" << shader_stage_inst_name << ".gl_FragCoord = v_in."
        << this->vertex_output_varyings[0].name << ";" << std::endl;
  }

  /* Assign default gl_FragDepth.
   * If gl_FragDepth is used, it should default to the original depth value. Resolves #107159 where
   * overlay_wireframe_frag may not write to gl_FragDepth. */
  if (this->uses_gl_FragDepth) {
    out << "\t" << shader_stage_inst_name << ".gl_FragDepth = " << shader_stage_inst_name
        << ".gl_FragCoord.z;" << std::endl;
  }

  /* Input render target array index received from vertex shader. */
  if (uses_gpu_layer) {
    out << "\t" << shader_stage_inst_name << ".gpu_Layer = v_in.gpu_Layer;" << std::endl;
  }

  /* Input viewport array index received from vertex shader. */
  if (uses_gpu_viewport_index) {
    out << "\t" << shader_stage_inst_name << ".gpu_ViewportIndex = v_in.gpu_ViewportIndex;"
        << std::endl;
  }

  /* NOTE: We will only assign to the intersection of the vertex output and fragment input.
   * Fragment input represents varying variables which are declared (but are not necessarily
   * used). The Vertex out defines the set which is passed into the fragment shader, which
   * contains out variables declared in the vertex shader, though these are not necessarily
   * consumed by the fragment shader.
   *
   * In the cases where the fragment shader expects a variable, but it does not exist in the
   * vertex shader, a warning will be provided. */
  for (int f_input = (this->uses_gl_Position) ? 0 : 1;
       f_input < this->fragment_input_varyings.size();
       f_input++)
  {
    bool exists_in_vertex_output = false;
    for (int v_o = 0; v_o < this->vertex_output_varyings.size() && !exists_in_vertex_output; v_o++)
    {
      if (this->fragment_input_varyings[f_input].name == this->vertex_output_varyings[v_o].name) {
        exists_in_vertex_output = true;
      }
    }
    if (!exists_in_vertex_output) {
      shader_debug_printf(
          "[Warning] Fragment shader expects varying input '%s', but this is not passed from "
          "the "
          "vertex shader\n",
          this->fragment_input_varyings[f_input].name.c_str());
      continue;
    }
    if (this->fragment_input_varyings[f_input].is_array) {
      for (int i = 0; i < this->fragment_input_varyings[f_input].array_elems; i++) {
        out << "\t" << shader_stage_inst_name << ".";

        if (this->fragment_input_varyings[f_input].instance_name.empty() == false) {
          out << this->fragment_input_varyings[f_input].instance_name << ".";
        }

        out << this->fragment_input_varyings[f_input].name << "[" << i << "] = v_in."
            << this->fragment_input_varyings[f_input].instance_name << "_"
            << this->fragment_input_varyings[f_input].name << i << ";" << std::endl;
      }
    }
    else {
      /* Matrix types are split into components and need to be regrouped into a matrix. */
      if (is_matrix_type(this->fragment_input_varyings[f_input].type)) {
        out << "\t" << shader_stage_inst_name << ".";

        if (this->fragment_input_varyings[f_input].instance_name.empty() == false) {
          out << this->fragment_input_varyings[f_input].instance_name << ".";
        }

        out << this->fragment_input_varyings[f_input].name << " = "
            << this->fragment_input_varyings[f_input].type;
        int count = get_matrix_location_count(this->fragment_input_varyings[f_input].type);
        for (int elem = 0; elem < count; elem++) {
          out << ((elem == 0) ? "(" : "") << "v_in."
              << this->fragment_input_varyings[f_input].instance_name << "__matrix_"
              << this->fragment_input_varyings[f_input].name << elem
              << ((elem < count - 1) ? ",\n" : "");
        }
        out << ");" << std::endl;
      }
      else {
        out << "\t" << shader_stage_inst_name << ".";

        if (this->fragment_input_varyings[f_input].instance_name.empty() == false) {
          out << this->fragment_input_varyings[f_input].instance_name << ".";
        }

        out << this->fragment_input_varyings[f_input].name << " = v_in."
            << this->fragment_input_varyings[f_input].instance_name << "_"
            << this->fragment_input_varyings[f_input].name << ";" << std::endl;
      }
    }
  }
  out << std::endl;
  return out.str();
}

/* Copy post-main, modified, local class variables into fragment-output struct. */
std::string MSLGeneratorInterface::generate_msl_fragment_output_population()
{
  static const char *shader_stage_inst_name = get_shader_stage_instance_name(
      ShaderStage::FRAGMENT);
  /* Populate output fragment variables. */
  std::stringstream out;
  out << "\t/* Copy Fragment Outputs into output struct. */" << std::endl;

  /* Output gl_FragDepth. */
  if (this->uses_gl_FragDepth) {
    out << "\toutput.fragdepth = " << shader_stage_inst_name << ".gl_FragDepth;" << std::endl;
  }

  /* Output gl_FragStencilRefARB. */
  if (this->uses_gl_FragStencilRefARB) {
    out << "\toutput.fragstencil = uint(" << shader_stage_inst_name << ".gl_FragStencilRefARB);"
        << std::endl;
  }

  /* Output attributes. */
  for (int f_output = 0; f_output < this->fragment_outputs.size(); f_output++) {

    out << "\toutput." << this->fragment_outputs[f_output].name << " = " << shader_stage_inst_name
        << "." << this->fragment_outputs[f_output].name << ";" << std::endl;
  }
  out << std::endl;
  return out.str();
}

std::string MSLGeneratorInterface::generate_msl_texture_vars(ShaderStage shader_stage)
{
  /* NOTE: Shader stage must be a singular stage index. Compound stage is not valid for this
   * function. */
  BLI_assert(shader_stage == ShaderStage::VERTEX || shader_stage == ShaderStage::FRAGMENT ||
             shader_stage == ShaderStage::COMPUTE);

  std::stringstream out;
  out << "\t/* Populate local texture and sampler members */" << std::endl;
  for (int i = 0; i < this->texture_samplers.size(); i++) {
    if (bool(this->texture_samplers[i].stage & shader_stage)) {

      /* Assign texture reference. */
      out << "\t" << get_shader_stage_instance_name(shader_stage) << "."
          << this->texture_samplers[i].name << ".texture = &" << this->texture_samplers[i].name
          << ";" << std::endl;

      /* Assign sampler reference. */
      if (this->use_argument_buffer_for_samplers()) {
        out << "\t" << get_shader_stage_instance_name(shader_stage) << "."
            << this->texture_samplers[i].name << ".samp = &samplers.sampler_args["
            << this->texture_samplers[i].slot << "];" << std::endl;
      }
      else {
        out << "\t" << get_shader_stage_instance_name(shader_stage) << "."
            << this->texture_samplers[i].name << ".samp = &" << this->texture_samplers[i].name
            << "_sampler;" << std::endl;
      }

      /* Assign texture buffer reference and uniform metadata (if used). */
      int tex_buf_id = this->texture_samplers[i].atomic_fallback_buffer_ssbo_id;
      if (tex_buf_id != -1) {
        MSLBufferBlock &ssbo = this->storage_blocks[tex_buf_id];
        out << "\t" << get_shader_stage_instance_name(shader_stage) << "."
            << this->texture_samplers[i].name << ".atomic.buffer = ";

        if (bool(shader_stage & ShaderStage::VERTEX)) {
          bool writeable = bool(ssbo.qualifiers & shader::Qualifier::write);
          const char *memory_scope = ((writeable) ? "device " : "constant ");

          out << "const_cast<" << memory_scope;

          if (!is_builtin_type(ssbo.type_name)) {
            out << get_stage_class_name(shader_stage) << "::";
          }
          out << ssbo.type_name << "*>(";
        }
        out << ssbo.name;
        if (bool(shader_stage & ShaderStage::VERTEX)) {
          out << ")";
        }
        out << ";" << std::endl;

        out << "\t" << get_shader_stage_instance_name(shader_stage) << "."
            << this->texture_samplers[i].name << ".atomic.aligned_width = uniforms->"
            << this->texture_samplers[i].name << "_metadata.w;" << std::endl;

        /* Buffer-backed 2D Array and 3D texture types are not natively supported so texture size
         * is passed in as uniform metadata for 3D to 2D coordinate remapping. */
        if (ELEM(this->texture_samplers[i].type,
                 ImageType::AtomicUint2DArray,
                 ImageType::AtomicUint3D,
                 ImageType::AtomicInt2DArray,
                 ImageType::AtomicInt3D))
        {
          out << "\t" << get_shader_stage_instance_name(shader_stage) << "."
              << this->texture_samplers[i].name << ".atomic.texture_size = ushort3(uniforms->"
              << this->texture_samplers[i].name << "_metadata.xyz);" << std::endl;
        }
      }
    }
  }
  out << std::endl;
  return out.str();
}

void MSLGeneratorInterface::resolve_input_attribute_locations()
{
  /* Determine used-attribute-location mask. */
  uint32_t used_locations = 0;
  for (const MSLVertexInputAttribute &attr : vertex_input_attributes) {
    if (attr.layout_location >= 0) {
      /* Matrix and array types span multiple location slots. */
      uint32_t location_element_count = get_matrix_location_count(attr.type);
      for (uint32_t i = 1; i <= location_element_count; i++) {
        /* Ensure our location hasn't already been used. */
        uint32_t location_mask = (i << attr.layout_location);
        BLI_assert((used_locations & location_mask) == 0);
        used_locations = used_locations | location_mask;
      }
    }
  }

  /* Assign unused location slots to other attributes. */
  for (MSLVertexInputAttribute &attr : vertex_input_attributes) {
    if (attr.layout_location == -1) {
      /* Determine number of locations required. */
      uint32_t required_attr_slot_count = get_matrix_location_count(attr.type);

      /* Determine free location.
       * Starting from 1 is slightly less efficient, however,
       * given multi-sized attributes, an earlier slot may remain free.
       * given GPU_VERT_ATTR_MAX_LEN is small, this wont matter. */
      for (int loc = 0; loc < GPU_VERT_ATTR_MAX_LEN - (required_attr_slot_count - 1); loc++) {

        uint32_t location_mask = (1 << loc);
        /* Generate sliding mask using location and required number of slots,
         * to ensure contiguous slots are free.
         * slot mask will be a number containing N binary 1's, where N is the
         * number of attributes needed.
         * e.g. N=4 -> 1111. */
        uint32_t location_slot_mask = (1 << required_attr_slot_count) - 1;
        uint32_t sliding_location_slot_mask = location_slot_mask << location_mask;
        if ((used_locations & sliding_location_slot_mask) == 0) {
          /* Assign location and update mask. */
          attr.layout_location = loc;
          used_locations = used_locations | location_slot_mask;
          continue;
        }
      }

      /* Error if could not assign attribute. */
      MTL_LOG_ERROR("Could not assign attribute location to attribute %s for shader %s",
                    attr.name.c_str(),
                    this->parent_shader_.name_get().c_str());
    }
  }
}

void MSLGeneratorInterface::resolve_fragment_output_locations()
{
  int running_location_ind = 0;

  /* This code works under the assumption that either all layout_locations are set,
   * or none are. */
  for (int i = 0; i < this->fragment_outputs.size(); i++) {
    BLI_assert_msg(
        ((running_location_ind > 0) ? (this->fragment_outputs[i].layout_location == -1) : true),
        "Error: Mismatched input attributes, some with location specified, some without");
    if (this->fragment_outputs[i].layout_location == -1) {
      this->fragment_outputs[i].layout_location = running_location_ind;
      running_location_ind++;
    }
  }
}

std::string MSLTextureResource::get_msl_texture_type_str() const
{
  bool supports_native_atomics = MTLBackend::get_capabilities().supports_texture_atomics;
  /* Add Types as needed. */
  switch (this->type) {
    case ImageType::Float1D: {
      return "texture1d";
    }
    case ImageType::Float2D: {
      return "texture2d";
    }
    case ImageType::Float3D: {
      return "texture3d";
    }
    case ImageType::FloatCube: {
      return "texturecube";
    }
    case ImageType::Float1DArray: {
      return "texture1d_array";
    }
    case ImageType::Float2DArray: {
      return "texture2d_array";
    }
    case ImageType::FloatCubeArray: {
      return "texturecube_array";
    }
    case ImageType::FloatBuffer: {
      return "texture_buffer";
    }
    case ImageType::Depth2D: {
      return "depth2d";
    }
    case ImageType::Shadow2D: {
      return "depth2d";
    }
    case ImageType::Depth2DArray: {
      return "depth2d_array";
    }
    case ImageType::Shadow2DArray: {
      return "depth2d_array";
    }
    case ImageType::DepthCube: {
      return "depthcube";
    }
    case ImageType::ShadowCube: {
      return "depthcube";
    }
    case ImageType::DepthCubeArray: {
      return "depthcube_array";
    }
    case ImageType::ShadowCubeArray: {
      return "depthcube_array";
    }
    case ImageType::Int1D: {
      return "texture1d";
    }
    case ImageType::Int2D: {
      return "texture2d";
    }
    case ImageType::Int3D: {
      return "texture3d";
    }
    case ImageType::IntCube: {
      return "texturecube";
    }
    case ImageType::Int1DArray: {
      return "texture1d_array";
    }
    case ImageType::Int2DArray: {
      return "texture2d_array";
    }
    case ImageType::IntCubeArray: {
      return "texturecube_array";
    }
    case ImageType::IntBuffer: {
      return "texture_buffer";
    }
    case ImageType::Uint1D: {
      return "texture1d";
    }
    case ImageType::Uint2D: {
      return "texture2d";
    }
    case ImageType::Uint3D: {
      return "texture3d";
    }
    case ImageType::UintCube: {
      return "texturecube";
    }
    case ImageType::Uint1DArray: {
      return "texture1d_array";
    }
    case ImageType::Uint2DArray: {
      return "texture2d_array";
    }
    case ImageType::UintCubeArray: {
      return "texturecube_array";
    }
    case ImageType::UintBuffer: {
      return "texture_buffer";
    }
    /* If texture atomics are natively supported, we use the native texture type, otherwise all
     * other formats are implemented via texture2d. */
    case ImageType::AtomicInt2D:
    case ImageType::AtomicUint2D: {
      return "texture2d";
    }
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicUint2DArray: {
      if (supports_native_atomics) {
        return "texture2d_array";
      }
      else {
        return "texture2d";
      }
    }
    case ImageType::AtomicInt3D:
    case ImageType::AtomicUint3D: {
      if (supports_native_atomics) {
        return "texture3d";
      }
      else {
        return "texture2d";
      }
    }

    default: {
      /* Unrecognized type. */
      BLI_assert_unreachable();
      return "ERROR";
    }
  };
}

std::string MSLTextureResource::get_msl_wrapper_type_str() const
{
  bool supports_native_atomics = MTLBackend::get_capabilities().supports_texture_atomics;
  /* Add Types as needed. */
  switch (this->type) {
    case ImageType::Float1D: {
      return "_mtl_sampler_1d";
    }
    case ImageType::Float2D: {
      return "_mtl_sampler_2d";
    }
    case ImageType::Float3D: {
      return "_mtl_sampler_3d";
    }
    case ImageType::FloatCube: {
      return "_mtl_sampler_cube";
    }
    case ImageType::Float1DArray: {
      return "_mtl_sampler_1d_array";
    }
    case ImageType::Float2DArray: {
      return "_mtl_sampler_2d_array";
    }
    case ImageType::FloatCubeArray: {
      return "_mtl_sampler_cube_array";
    }
    case ImageType::FloatBuffer: {
      return "_mtl_sampler_buffer";
    }
    case ImageType::Depth2D: {
      return "_mtl_sampler_depth_2d";
    }
    case ImageType::Shadow2D: {
      return "_mtl_sampler_depth_2d";
    }
    case ImageType::Depth2DArray: {
      return "_mtl_sampler_depth_2d_array";
    }
    case ImageType::Shadow2DArray: {
      return "_mtl_sampler_depth_2d_array";
    }
    case ImageType::DepthCube: {
      return "_mtl_sampler_depth_cube";
    }
    case ImageType::ShadowCube: {
      return "_mtl_sampler_depth_cube";
    }
    case ImageType::DepthCubeArray: {
      return "_mtl_sampler_depth_cube_array";
    }
    case ImageType::ShadowCubeArray: {
      return "_mtl_sampler_depth_cube_array";
    }
    case ImageType::Int1D: {
      return "_mtl_sampler_1d";
    }
    case ImageType::Int2D: {
      return "_mtl_sampler_2d";
    }
    case ImageType::Int3D: {
      return "_mtl_sampler_3d";
    }
    case ImageType::IntCube: {
      return "_mtl_sampler_cube";
    }
    case ImageType::Int1DArray: {
      return "_mtl_sampler_1d_array";
    }
    case ImageType::Int2DArray: {
      return "_mtl_sampler_2d_array";
    }
    case ImageType::IntCubeArray: {
      return "_mtl_sampler_cube_array";
    }
    case ImageType::IntBuffer: {
      return "_mtl_sampler_buffer";
    }
    case ImageType::Uint1D: {
      return "_mtl_sampler_1d";
    }
    case ImageType::Uint2D: {
      return "_mtl_sampler_2d";
    }
    case ImageType::Uint3D: {
      return "_mtl_sampler_3d";
    }
    case ImageType::UintCube: {
      return "_mtl_sampler_cube";
    }
    case ImageType::Uint1DArray: {
      return "_mtl_sampler_1d_array";
    }
    case ImageType::Uint2DArray: {
      return "_mtl_sampler_2d_array";
    }
    case ImageType::UintCubeArray: {
      return "_mtl_sampler_cube_array";
    }
    case ImageType::UintBuffer: {
      return "_mtl_sampler_buffer";
    }
    /* If native texture atomics are unsupported, map types to fallback atomic structures which
     * contain a buffer pointer and metadata members for size and alignment. */
    case ImageType::AtomicInt2D:
    case ImageType::AtomicUint2D: {
      if (supports_native_atomics) {
        return "_mtl_sampler_2d";
      }
      else {
        return "_mtl_sampler_2d_atomic";
      }
    }
    case ImageType::AtomicInt3D:
    case ImageType::AtomicUint3D: {
      if (supports_native_atomics) {
        return "_mtl_sampler_3d";
      }
      else {
        return "_mtl_sampler_3d_atomic";
      }
    }
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicUint2DArray: {
      if (supports_native_atomics) {
        return "_mtl_sampler_2d_array";
      }
      else {
        return "_mtl_sampler_2d_array_atomic";
      }
    }
    default: {
      /* Unrecognized type. */
      BLI_assert_unreachable();
      return "ERROR";
    }
  };
}

std::string MSLTextureResource::get_msl_return_type_str() const
{
  /* Add Types as needed */
  switch (this->type) {
    /* Floating point return. */
    case ImageType::Float1D:
    case ImageType::Float2D:
    case ImageType::Float3D:
    case ImageType::FloatCube:
    case ImageType::Float1DArray:
    case ImageType::Float2DArray:
    case ImageType::FloatCubeArray:
    case ImageType::FloatBuffer:
    case ImageType::Depth2D:
    case ImageType::Shadow2D:
    case ImageType::Depth2DArray:
    case ImageType::Shadow2DArray:
    case ImageType::DepthCube:
    case ImageType::ShadowCube:
    case ImageType::DepthCubeArray:
    case ImageType::ShadowCubeArray: {
      return "float";
    }
    /* Integer return. */
    case ImageType::Int1D:
    case ImageType::Int2D:
    case ImageType::Int3D:
    case ImageType::IntCube:
    case ImageType::Int1DArray:
    case ImageType::Int2DArray:
    case ImageType::IntCubeArray:
    case ImageType::IntBuffer:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt2DArray:
    case ImageType::AtomicInt3D: {
      return "int";
    }

    /* Unsigned Integer return. */
    case ImageType::Uint1D:
    case ImageType::Uint2D:
    case ImageType::Uint3D:
    case ImageType::UintCube:
    case ImageType::Uint1DArray:
    case ImageType::Uint2DArray:
    case ImageType::UintCubeArray:
    case ImageType::UintBuffer:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint2DArray:
    case ImageType::AtomicUint3D: {
      return "uint32_t";
    }

    default: {
      /* Unrecognized type. */
      BLI_assert_unreachable();
      return "ERROR";
    }
  };
}

GPUTextureType MSLTextureResource::get_texture_binding_type() const
{
  /* Add Types as needed */
  switch (this->type) {
    case ImageType::Float1D: {
      return GPU_TEXTURE_1D;
    }
    case ImageType::Float2D: {
      return GPU_TEXTURE_2D;
    }
    case ImageType::Float3D: {
      return GPU_TEXTURE_3D;
    }
    case ImageType::FloatCube: {
      return GPU_TEXTURE_CUBE;
    }
    case ImageType::Float1DArray: {
      return GPU_TEXTURE_1D_ARRAY;
    }
    case ImageType::Float2DArray: {
      return GPU_TEXTURE_2D_ARRAY;
    }
    case ImageType::FloatCubeArray: {
      return GPU_TEXTURE_CUBE_ARRAY;
    }
    case ImageType::FloatBuffer: {
      return GPU_TEXTURE_BUFFER;
    }
    case ImageType::Depth2D: {
      return GPU_TEXTURE_2D;
    }
    case ImageType::Shadow2D: {
      return GPU_TEXTURE_2D;
    }
    case ImageType::Depth2DArray: {
      return GPU_TEXTURE_2D_ARRAY;
    }
    case ImageType::Shadow2DArray: {
      return GPU_TEXTURE_2D_ARRAY;
    }
    case ImageType::DepthCube: {
      return GPU_TEXTURE_CUBE;
    }
    case ImageType::ShadowCube: {
      return GPU_TEXTURE_CUBE;
    }
    case ImageType::DepthCubeArray: {
      return GPU_TEXTURE_CUBE_ARRAY;
    }
    case ImageType::ShadowCubeArray: {
      return GPU_TEXTURE_CUBE_ARRAY;
    }
    case ImageType::Int1D: {
      return GPU_TEXTURE_1D;
    }
    case ImageType::Int2D: {
      return GPU_TEXTURE_2D;
    }
    case ImageType::Int3D: {
      return GPU_TEXTURE_3D;
    }
    case ImageType::IntCube: {
      return GPU_TEXTURE_CUBE;
    }
    case ImageType::Int1DArray: {
      return GPU_TEXTURE_1D_ARRAY;
    }
    case ImageType::Int2DArray: {
      return GPU_TEXTURE_2D_ARRAY;
    }
    case ImageType::IntCubeArray: {
      return GPU_TEXTURE_CUBE_ARRAY;
    }
    case ImageType::IntBuffer: {
      return GPU_TEXTURE_BUFFER;
    }
    case ImageType::Uint1D: {
      return GPU_TEXTURE_1D;
    }
    case ImageType::Uint2D:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicInt2D: {
      return GPU_TEXTURE_2D;
    }
    case ImageType::Uint3D:
    case ImageType::AtomicUint3D:
    case ImageType::AtomicInt3D: {
      return GPU_TEXTURE_3D;
    }
    case ImageType::UintCube: {
      return GPU_TEXTURE_CUBE;
    }
    case ImageType::Uint1DArray: {
      return GPU_TEXTURE_1D_ARRAY;
    }
    case ImageType::Uint2DArray:
    case ImageType::AtomicUint2DArray:
    case ImageType::AtomicInt2DArray: {
      return GPU_TEXTURE_2D_ARRAY;
    }
    case ImageType::UintCubeArray: {
      return GPU_TEXTURE_CUBE_ARRAY;
    }
    case ImageType::UintBuffer: {
      return GPU_TEXTURE_BUFFER;
    }
    default: {
      BLI_assert_unreachable();
      return GPU_TEXTURE_2D;
    }
  };
}

GPUSamplerFormat MSLTextureResource::get_sampler_format() const
{
  switch (this->type) {
    case ImageType::FloatBuffer:
    case ImageType::Float1D:
    case ImageType::Float1DArray:
    case ImageType::Float2D:
    case ImageType::Float2DArray:
    case ImageType::Float3D:
    case ImageType::FloatCube:
    case ImageType::FloatCubeArray:
      return GPU_SAMPLER_TYPE_FLOAT;
    case ImageType::IntBuffer:
    case ImageType::Int1D:
    case ImageType::Int1DArray:
    case ImageType::Int2D:
    case ImageType::Int2DArray:
    case ImageType::Int3D:
    case ImageType::IntCube:
    case ImageType::IntCubeArray:
    case ImageType::AtomicInt2D:
    case ImageType::AtomicInt3D:
    case ImageType::AtomicInt2DArray:
      return GPU_SAMPLER_TYPE_INT;
    case ImageType::UintBuffer:
    case ImageType::Uint1D:
    case ImageType::Uint1DArray:
    case ImageType::Uint2D:
    case ImageType::Uint2DArray:
    case ImageType::Uint3D:
    case ImageType::UintCube:
    case ImageType::UintCubeArray:
    case ImageType::AtomicUint2D:
    case ImageType::AtomicUint3D:
    case ImageType::AtomicUint2DArray:
      return GPU_SAMPLER_TYPE_UINT;
    case ImageType::Shadow2D:
    case ImageType::Shadow2DArray:
    case ImageType::ShadowCube:
    case ImageType::ShadowCubeArray:
    case ImageType::Depth2D:
    case ImageType::Depth2DArray:
    case ImageType::DepthCube:
    case ImageType::DepthCubeArray:
      return GPU_SAMPLER_TYPE_DEPTH;
    default:
      BLI_assert_unreachable();
  }
  return GPU_SAMPLER_TYPE_FLOAT;
}

/** \} */

}  // namespace blender::gpu
