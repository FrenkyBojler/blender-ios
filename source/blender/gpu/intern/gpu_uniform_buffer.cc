/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "MEM_guardedalloc.h"
#include <cstring>

#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_string.h"

#include "BKE_global.hh"

#include "gpu_backend.hh"
#include "gpu_node_graph.hh"

#include "GPU_capabilities.hh"
#include "GPU_context.hh"
#include "GPU_material.hh"

#include "GPU_uniform_buffer.hh"
#include "gpu_context_private.hh"
#include "gpu_uniform_buffer_private.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Creation & Deletion
 * \{ */

namespace gpu {

UniformBuf::UniformBuf(size_t size, const char *name)
{
  /* Make sure that UBO is padded to size of vec4 */
  BLI_assert((size % 16) == 0);

  size_in_bytes_ = size;

  STRNCPY(name_, name);
}

UniformBuf::~UniformBuf()
{
  MEM_SAFE_DELETE_VOID(data_);
}

}  // namespace gpu

/** \} */

/* -------------------------------------------------------------------- */
/** \name Uniform buffer from GPUInput list
 * \{ */

/**
 * We need to pad some data types (vec3) on the C side
 * To match the GPU expected memory block alignment.
 */
static GPUType get_padded_gpu_type(LinkData *link)
{
  GPUInput *input = static_cast<GPUInput *>(link->data);
  GPUType gputype = input->type;
  /* Metal cannot pack floats after vec3. */
  if (GPU_backend_get_type() == GPU_BACKEND_METAL) {
    return (gputype == GPU_VEC3) ? GPU_VEC4 : gputype;
  }
  /* Unless the vec3 is followed by a float we need to treat it as a vec4. */
  if (gputype == GPU_VEC3 && (link->next != nullptr) &&
      ((static_cast<GPUInput *>(link->next->data))->type != GPU_FLOAT))
  {
    gputype = GPU_VEC4;
  }
  return gputype;
}

/**
 * Returns 1 if the first item should be after second item.
 * We make sure the vec4 uniforms come first.
 */
static int inputs_cmp(const void *a, const void *b)
{
  const LinkData *link_a = static_cast<const LinkData *>(a),
                 *link_b = static_cast<const LinkData *>(b);
  const GPUInput *input_a = static_cast<const GPUInput *>(link_a->data);
  const GPUInput *input_b = static_cast<const GPUInput *>(link_b->data);
  return gpu_type_element_count(input_a->type) < gpu_type_element_count(input_b->type) ? 1 : 0;
}

/* First link of each type in the sorted list. */
struct UBOFirstLinks {
  LinkData *link_mat4;
  LinkData *link_vec4;
  LinkData *link_vec3;
  LinkData *link_vec2;
  LinkData *link_float;
};

static LinkData **ubo_first_link_ptr(UBOFirstLinks &first_links, const GPUType type)
{
  switch (type) {
    case GPU_NONE:
      break;
    case GPU_FLOAT:
      return &first_links.link_float;
    case GPU_VEC2:
      return &first_links.link_vec2;
    case GPU_VEC3:
      return &first_links.link_vec3;
    case GPU_VEC4:
      return &first_links.link_vec4;
    case GPU_MAT3:
      break;
    case GPU_MAT4:
      return &first_links.link_mat4;
    case GPU_TEX1D_ARRAY:
    case GPU_TEX2D:
    case GPU_TEX2D_ARRAY:
    case GPU_TEX3D:
    case GPU_CLOSURE:
    case GPU_ATTR:
      break;
  }

  BLI_assert_unreachable();
  return nullptr;
}

/**
 * Make sure we respect the expected alignment of UBOs.
 * mat4, vec4, pad vec3 as vec4, then vec2, then floats.
 */
static void buffer_from_list_inputs_sort(ListBaseT<LinkData> *inputs)
{
  /* Order them as mat4, vec4, vec3, vec2, float. */
  BLI_listbase_sort(inputs, inputs_cmp);

  /* Metal cannot pack floats after vec3. */
  if (GPU_backend_get_type() == GPU_BACKEND_METAL) {
    return;
  }

  UBOFirstLinks first_links{};
  GPUType cur_type = GPU_NONE;

  for (LinkData &link : *inputs) {
    GPUInput *input = static_cast<GPUInput *>(link.data);

    if (input->type == GPU_MAT3) {
      /* Alignment for mat3 is not handled currently, so not supported */
      BLI_assert_msg(0, "mat3 not supported in UBO");
      continue;
    }

    LinkData **first_link_ptr = ubo_first_link_ptr(first_links, input->type);
    if (first_link_ptr == nullptr) {
      BLI_assert_msg(0, "GPU type not supported in UBO");
      continue;
    }

    if (input->type == cur_type) {
      continue;
    }

    *first_link_ptr = &link;
    cur_type = input->type;
  }

  /* If there is no GPU_VEC3 there is no need for alignment. */
  if (first_links.link_vec3 == nullptr) {
    return;
  }

  LinkData *link = first_links.link_vec3;
  while (link != nullptr && (static_cast<GPUInput *>(link->data))->type == GPU_VEC3) {
    LinkData *link_next = link->next;

    /* If GPU_VEC3 is followed by nothing or a GPU_FLOAT, no need for alignment. */
    if ((link_next == nullptr) || (static_cast<GPUInput *>(link_next->data))->type == GPU_FLOAT) {
      break;
    }

    /* If there is a float, move it next to current vec3. */
    if (first_links.link_float != nullptr) {
      LinkData *float_input = first_links.link_float;
      first_links.link_float = float_input->next;

      BLI_remlink(inputs, float_input);
      BLI_insertlinkafter(inputs, link, float_input);
    }

    link = link_next;
  }
}

static inline size_t buffer_size_from_list(ListBaseT<LinkData> *inputs)
{
  size_t buffer_size = 0;
  for (LinkData &link : *inputs) {
    const GPUType gputype = get_padded_gpu_type(&link);
    buffer_size += gpu_type_element_count(gputype) * sizeof(float);
  }
  /* Round up to size of vec4. (Opengl Requirement) */
  size_t alignment = sizeof(float[4]);
  buffer_size = divide_ceil_u(buffer_size, alignment) * alignment;

  return buffer_size;
}

static inline void buffer_fill_from_list(void *data, ListBaseT<LinkData> *inputs)
{
  /* Now that we know the total ubo size we can start populating it. */
  float *offset = static_cast<float *>(data);
  for (LinkData &link : *inputs) {
    GPUInput *input = static_cast<GPUInput *>(link.data);
    memcpy(offset, input->vec, gpu_type_element_count(input->type) * sizeof(float));
    offset += gpu_type_element_count(get_padded_gpu_type(&link));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name C-API
 * \{ */

using namespace blender::gpu;

gpu::UniformBuf *GPU_uniformbuf_create_ex(size_t size, const void *data, const char *name)
{
  UniformBuf *ubo = GPUBackend::get()->uniformbuf_alloc(size, name);
  /* Direct init. */
  if (data != nullptr) {
    ubo->update(data);
  }
  else if (G.debug & G_DEBUG_GPU) {
    /* Fill the buffer with poison values.
     * (NaN for floats, -1 for `int` and "max value" for `uint`). */
    Vector<uchar> uninitialized_data(size, 0xFF);
    ubo->update(uninitialized_data.data());
  }
  return ubo;
}

gpu::UniformBuf *GPU_uniformbuf_create_from_list(ListBaseT<LinkData> *inputs, const char *name)
{
  /* There is no point on creating an UBO if there is no arguments. */
  if (inputs->is_empty()) {
    return nullptr;
  }

  buffer_from_list_inputs_sort(inputs);
  size_t buffer_size = buffer_size_from_list(inputs);
  void *data = MEM_new_uninitialized(buffer_size, __func__);
  buffer_fill_from_list(data, inputs);

  UniformBuf *ubo = nullptr;
  if (buffer_size <= GPU_max_uniform_buffer_size()) {
    ubo = GPUBackend::get()->uniformbuf_alloc(buffer_size, name);
    /* Defer data upload. */
    ubo->attach_data(data);
  }
  return ubo;
}

void GPU_uniformbuf_free(gpu::UniformBuf *ubo)
{
  delete ubo;
}

void GPU_uniformbuf_update(gpu::UniformBuf *ubo, const void *data)
{
  ubo->update(data);
}

void GPU_uniformbuf_bind(gpu::UniformBuf *ubo, int slot)
{
  ubo->bind(slot);
}

void GPU_uniformbuf_bind_as_ssbo(gpu::UniformBuf *ubo, int slot)
{
  ubo->bind_as_ssbo(slot);
}

void GPU_uniformbuf_unbind(gpu::UniformBuf *ubo)
{
  ubo->unbind();
}

void GPU_uniformbuf_debug_unbind_all()
{
  Context::get()->debug_unbind_all_ubo();
}

void GPU_uniformbuf_clear_to_zero(gpu::UniformBuf *ubo)
{
  ubo->clear_to_zero();
}

/** \} */

}  // namespace blender
