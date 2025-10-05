/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "mtl_push_constant.hh"

namespace blender::gpu {

static size_t padded_size(const shader::ShaderCreateInfo::PushConst &push_constant,
                          size_t &alignment)
{
  int comp = to_component_count(push_constant.type);
  size_t size;
  if (comp == 3) {
    /* Padded size for float3. */
    size = 16;
    alignment = 16;
  }
  else if (comp == 9) {
    /* Padded size for float3x3. */
    size = 16 * 3;
    alignment = 16;
  }
  else {
    size = comp * sizeof(float);
    alignment = size;
  }
  return size * push_constant.array_size_safe();
}

MTLPushConstantBuf::MTLPushConstantBuf(const shader::ShaderCreateInfo &info)
{
  BLI_assert(info.push_constants_.is_empty() == false);
  /* Compute size of backing buffer. */
  size_ = 0;
  for (const shader::ShaderCreateInfo::PushConst &push_constant : info.push_constants_) {
    size_t alignment;
    size_t pc_size = padded_size(push_constant, alignment);
    /* Padding for alignment. */
    size_ += (size_ + alignment) % alignment;
    size_ += pc_size;
  }
  data_ = reinterpret_cast<uint8_t *>(
      MEM_malloc_arrayN_aligned(1, size_, 128, "MTLPushConstantData"));
}

MTLPushConstantBuf::~MTLPushConstantBuf()
{
  MEM_freeN(data_);
}

int MTLPushConstantBuf::append(shader::ShaderCreateInfo::PushConst push_constant)
{
  size_t alignment;
  size_t pc_size = padded_size(push_constant, alignment);
  /* Padding for alignment. */
  offset_ += (offset_ + alignment) % alignment;
  int loc = offset_;
  offset_ += pc_size;
  BLI_assert(offset_ <= size_);
  return loc;
}

}  // namespace blender::gpu
