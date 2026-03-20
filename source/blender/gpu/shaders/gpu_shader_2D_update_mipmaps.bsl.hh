/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#pragma create_info

#include "gpu_shader_compat.hh"
#include "gpu_shader_create_info.hh"

namespace builtin::mipmaps {

/* TODO: These encoders/decoders are a slightly off from our color management lib. Might fix some
 * specific issues, which we need to test. */
// Convert float (0-1) sRGB red/green/blue component value to linear.
float linear_from_srgb_component(float srgb)
{
  return srgb <= 0.04045 ? srgb * (25 / 323.) : pow((200 * srgb + 11) * (1 / 211.), 2.4);
}

// Convert linear red/green/blue component to float (0-1) sRGB
float srgb_component_from_linear(float linear)
{
  return linear <= 0.0031308 ? (323 / 25.) * linear : 1.055 * pow(linear, 1 / 2.4) - 0.055;
}

float4 srgb_unpack(uint srgb_packed)
{
  float4 srgba = unpackUnorm4x8(srgb_packed);
  float4 linear_color;
  linear_color.r = linear_from_srgb_component(srgba.r);
  linear_color.g = linear_from_srgb_component(srgba.g);
  linear_color.b = linear_from_srgb_component(srgba.b);
  linear_color.a = srgba.a;
  return linear_color;
}

uint srgb_pack(float4 linear_color)
{
  float4 srgba;
  srgba.r = srgb_component_from_linear(linear_color.r);
  srgba.g = srgb_component_from_linear(linear_color.g);
  srgba.b = srgb_component_from_linear(linear_color.b);
  srgba.a = linear_color.a;
  return packUnorm4x8(srgba);
}

/**
 * General-case shader for generating 1 or 2 levels of the mip pyramid.
 * When generating 1 level, each workgroup handles up to 128 samples of the
 * output mip level. When generating 2 levels, each workgroup handles
 * a 8x8 tile of the last (2nd) output mip level, generating up to
 * 17x17 samples of the intermediate (1st) output mip level along the way.
 *
 * Dispatch with y, z = 1
 */
#define LOCAL_SIZE_X 128
#define TILE_SIZE 8
#define MAX_SHARED_SAMPLES (TILE_SIZE + TILE_SIZE + 1)
#define INPUT_LEVEL 0

enum TextureFormat : uint32_t {
  UNORM_8_8_8_8,
  SFLOAT_16,
};

// template <typename SharedStorage> store_shared_sample()

struct SharedSRGB {
  /**
   * When generating 2 levels, the results of generating the intermediate *level(first level
   * generated) are cached here; this is the input tile *needed to generate the 8x8 tile of the
   * second level generated.
   */
  [[shared]] uint intermediate_level[MAX_SHARED_SAMPLES][MAX_SHARED_SAMPLES];

  void store_sample(int2 dst_coord, float4 color)
  {
    intermediate_level[dst_coord.y][dst_coord.x] = srgb_pack(color);
  }

  float4 load_sample(int2 src_coord)
  {
    return srgb_unpack(intermediate_level[src_coord.y][src_coord.x]);
  }
};

struct SharedFloat {
  /**
   * When generating 2 levels, the results of generating the intermediate *level(first level
   * generated) are cached here; this is the input tile *needed to generate the 8x8 tile of the
   * second level generated.
   */
  [[shared]] float intermediate_level[MAX_SHARED_SAMPLES][MAX_SHARED_SAMPLES];

  void store_sample(int2 dst_coord, float4 color)
  {
    intermediate_level[dst_coord.y][dst_coord.x] = color.x;
  }

  float4 load_sample(int2 src_coord)
  {
    return float4(intermediate_level[src_coord.y][src_coord.x]);
  }
};

template<enum TextureFormat format, typename SharedStorage> struct Resources {
  [[compilation_constant]] const int use_shared_srgb_uint;
  [[compilation_constant]] const int use_shared_float;
  [[push_constant]] const int num_levels;
  [[image(0, read, format)]] image2D mip_in;
  [[image(1, write, format)]] image2D mip_out1;
  [[image(2, write, format)]] image2D mip_out2;
  [[resource_table]] srt_t<SharedStorage> shared_storage;

  /**
   * When generating 2 levels, the results of generating the intermediate *level(first level
   * generated) are cached here; this is the input tile *needed to generate the 8x8 tile of the
   * second level generated.
   */
  [[shared, condition(use_shared_srgb_uint)]] uint shared_level_srgb_uint[MAX_SHARED_SAMPLES]
                                                                         [MAX_SHARED_SAMPLES];
  [[shared,
    condition(use_shared_float)]] float shared_level_float[MAX_SHARED_SAMPLES][MAX_SHARED_SAMPLES];

  void store_sample(int2 dst_coord, int dst_level, float4 color)
  {
    if (dst_level == 1) {
      imageStore(mip_out1, dst_coord, color);
    }
    else if (dst_level == 2) {
      imageStore(mip_out2, dst_coord, color);
    }
  }

  void store_shared_sample(int2 dst_coord, float4 color)
  {
    SharedStorage &storage = shared_storage;
    storage.store_sample(dst_coord, color);
  }

  template<typename T> T load_shared_sample(int2 src_coord)
  {
    SharedStorage &storage = shared_storage;
    float4 color = storage.load_sample(src_coord);
    return T(color);
  }

  int2 level_size(int level)
  {
    int2 mip_in_size = imageSize(mip_in);
    int2 mip_size = max((mip_in_size >> level), int2(1));
    return mip_size;
  }
};

template<typename SRT, typename T, bool load_from_shared>
T load_sample([[resource_table]] SRT &srt, int2 src_coord)
{
  float4 color;
  if (load_from_shared) {
    if (srt.use_shared_srgb_uint) [[static_branch]] {
      color = srgb_unpack(srt.shared_level_srgb_uint[src_coord.y][src_coord.x]);
    }
    if (srt.use_shared_float) [[static_branch]] {
      color = float4(srt.shared_level_float[src_coord.y][src_coord.x]);
    }
  }
  else {
    color = imageLoad(srt.mip_in, src_coord);
  }

  return T(color);
}

template struct Resources<UNORM_8_8_8_8, SharedSRGB>;
template struct Resources<SFLOAT_16, SharedFloat>;

template float4 Resources<UNORM_8_8_8_8, SharedSRGB>::load_shared_sample<float4>(int2 src_coord);
template float Resources<SFLOAT_16, SharedFloat>::load_shared_sample<float>(int2 src_coord);

template float4 load_sample<Resources<UNORM_8_8_8_8, SharedSRGB>, float4, true>(
    Resources<UNORM_8_8_8_8, SharedSRGB> &srt, int2 src_coord);
template float4 load_sample<Resources<UNORM_8_8_8_8, SharedSRGB>, float4, false>(
    Resources<UNORM_8_8_8_8, SharedSRGB> &srt, int2 src_coord);
template float4 load_sample<Resources<SFLOAT_16, SharedFloat>, float4, true>(
    Resources<SFLOAT_16, SharedFloat> &srt, int2 src_coord);
template float4 load_sample<Resources<SFLOAT_16, SharedFloat>, float4, false>(
    Resources<SFLOAT_16, SharedFloat> &srt, int2 src_coord);

template<typename T> T pyramid_reduce(float a0, T v0, float a1, T v1, float a2, T v2)
{
  return a0 * v0 + a1 * v1 + a2 * v2;
}
template float4 pyramid_reduce<float4>(
    float a0, float4 v0, float a1, float4 v1, float a2, float4 v2);

template<typename T> T pyramid_reduce2(T v0, T v1)
{
  return 0.5 * (v0 + v1);
}
template float4 pyramid_reduce2<float4>(float4 v0, float4 v1);

int2 kernel_size_from_input_size(int2 input_size)
{
  return int2(input_size.x == 1 ? 1 : (2 | (input_size.x & 1)),
              input_size.y == 1 ? 1 : (2 | (input_size.y & 1)));
}
/**
 * Handle loading and reducing a rectangle of size kernel_size
 * with the given upper-left coordinate src_coord. Samples read from
 * mip level src_level if !loadFromShared_, sharedLevel_ otherwise.
 *
 * kernel_size must range from 1x1 to 3x3.
 *
 * Once computed, the sample is written to the given coordinate of the
 * specified destination mip level, and returned. The destination
 * image size is needed to compute the kernel weights.
 */
template<typename SRT, typename T, bool load_from_shared>
float4 reduce_store_sample(SRT &srt,
                           int2 src_coord,
                           int src_level,
                           int2 kernel_size,
                           int2 dst_image_size,
                           int2 dst_coord,
                           int dst_level)
{
  float n_ = dst_image_size.y;
  float rcp_ = 1.0f / (2 * n_ + 1);
  float w0_ = rcp_ * (n_ - dst_coord.y);
  float w1_ = rcp_ * n_;
  float w2_ = 1.0f - w0_ - w1_;

  float4 v0_, v1_, v2_, h0_, h1_, h2_, out_;

  // Reduce vertically up to 3 times (depending on kernel horizontal size)
  switch (kernel_size.x) {
    case 3:
      switch (kernel_size.y) {
        case 3:
          v2_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(2, 2));
          ATTR_FALLTHROUGH;
        case 2:
          v1_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(2, 1));
          ATTR_FALLTHROUGH;
        case 1:
          v0_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(2, 0));
          break;
      }
      switch (kernel_size.y) {
        case 3:
          h2_ = pyramid_reduce(w0_, v0_, w1_, v1_, w2_, v2_);
          break;
        case 2:
          h2_ = pyramid_reduce2(v0_, v1_);
          break;
        case 1:
          h2_ = v0_;
          break;
      }
      ATTR_FALLTHROUGH;
    case 2:
      switch (kernel_size.y) {
        case 3:
          v2_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(1, 2));
          ATTR_FALLTHROUGH;
        case 2:
          v1_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(1, 1));
          ATTR_FALLTHROUGH;
        case 1:
          v0_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(1, 0));
          break;
      }
      switch (kernel_size.y) {
        case 3:
          h1_ = pyramid_reduce(w0_, v0_, w1_, v1_, w2_, v2_);
          break;
        case 2:
          h1_ = pyramid_reduce2(v0_, v1_);
          break;
        case 1:
          h1_ = v0_;
          break;
      }
      ATTR_FALLTHROUGH;
    case 1:
      switch (kernel_size.y) {
        case 3:
          v2_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(0, 2));
          ATTR_FALLTHROUGH;
        case 2:
          v1_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(0, 1));
          ATTR_FALLTHROUGH;
        case 1:
          v0_ = load_sample<SRT, T, load_from_shared>(srt, src_coord + int2(0, 0));
          break;
      }
      switch (kernel_size.y) {
        case 3:
          h0_ = pyramid_reduce(w0_, v0_, w1_, v1_, w2_, v2_);
          break;
        case 2:
          h0_ = pyramid_reduce2(v0_, v1_);
          break;
        case 1:
          h0_ = v0_;
          break;
      }
  }

  // Reduce up to 3 samples horizontally.
  switch (kernel_size.x) {
    case 3:
      n_ = dst_image_size.x;
      rcp_ = 1.0f / (2 * n_ + 1);
      w0_ = rcp_ * (n_ - dst_coord.x);
      w1_ = rcp_ * n_;
      w2_ = 1.0f - w0_ - w1_;
      out_ = pyramid_reduce(w0_, h0_, w1_, h1_, w2_, h2_);
      break;
    case 2:
      out_ = pyramid_reduce2(h0_, h1_);
      break;
    case 1:
      out_ = h0_;
  }

  // Write out sample.
  srt.store_sample(dst_coord, dst_level, out_);
  return out_;
}
template float4 reduce_store_sample<Resources<UNORM_8_8_8_8, SharedFloat>, float4, true>(
    Resources<UNORM_8_8_8_8, SharedFloat> &srt,
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);
template float4 reduce_store_sample<Resources<UNORM_8_8_8_8, SharedSRGB>, float4, false>(
    Resources<UNORM_8_8_8_8, SharedSRGB> &srt,
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);
template float4 reduce_store_sample<Resources<SFLOAT_16, SharedFloat>, float4, true>(
    Resources<SFLOAT_16, SharedFloat> &srt,
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);
template float4 reduce_store_sample<Resources<SFLOAT_16, SharedFloat>, float4, false>(
    Resources<SFLOAT_16, SharedFloat> &srt,
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);

/**
 * Compute and write out (to the 1st mip level generated) the samples
 * at coordinates
 *     initDst_coord,
 *     initDst_coord + step_, ...
 *     initDst_coord + (iterations_-1) * step_
 * and cache them at in the sharedLevel_ tile at coordinates
 *     initSharedCoord_,
 *     initSharedCoord_ + step_, ...
 *     initSharedCoord_ + (iterations_-1) * step_
 * If boundsCheck_ is true, skip coordinates that are out of bounds.
 */
template<typename SRT>
void intermediateLevelLoop_(SRT &srt,
                            int2 initDst_coord,
                            int2 initSharedCoord_,
                            int2 step_,
                            int iterations_,
                            bool boundsCheck_)
{
  int2 dst_coord = initDst_coord;
  int2 sharedCoord_ = initSharedCoord_;
  int src_level = INPUT_LEVEL;
  int dst_level = src_level + 1;
  int2 srcImageSize_ = srt.level_size(src_level);
  int2 dst_image_size = srt.level_size(dst_level);
  int2 kernel_size = kernel_size_from_input_size(srcImageSize_);

  for (int i_ = 0; i_ < iterations_; ++i_) {
    int2 src_coord = dst_coord * 2;

    // Optional bounds check.
    if (boundsCheck_) {
      if (uint(dst_coord.x) >= uint(dst_image_size.x)) {
        continue;
      }
      if (uint(dst_coord.y) >= uint(dst_image_size.y)) {
        continue;
      }
    }

    float4 sample_ = reduce_store_sample<SRT, float4, false>(
        srt, src_coord, src_level, kernel_size, dst_image_size, dst_coord, dst_level);

    // Above function handles writing to the actual output; manually
    // cache into shared memory here.
    srt.store_shared_sample(sharedCoord_, sample_);
    dst_coord += step_;
    sharedCoord_ += step_;
  }
}

template void intermediateLevelLoop_<Resources<UNORM_8_8_8_8, SharedSRGB>>(
    Resources<UNORM_8_8_8_8, SharedSRGB> &srt,
    int2 initDst_coord,
    int2 initSharedCoord_,
    int2 step_,
    int iterations_,
    bool boundsCheck_);
template void intermediateLevelLoop_<Resources<SFLOAT_16, SharedFloat>>(
    Resources<SFLOAT_16, SharedFloat> &srt,
    int2 initDst_coord,
    int2 initSharedCoord_,
    int2 step_,
    int iterations_,
    bool boundsCheck_);

/**
 *Function for the workgroup that handles filling the intermediate level
 * (caching it in shared memory as well).
 *
 * We need somewhere from 16x16 to 17x17 samples, depending
 * on what the kernel size for the 2nd mip level generation will be.
 *
 * dstTileCoord_ : upper left coordinate of the tile to generate.
 * boundsCheck_  : whether to skip samples that are out-of-bounds.
 */
template<typename SRT>
void fillIntermediateTile_(SRT &srt, uint local_index, int2 dstTileCoord_, bool boundsCheck_)
{
  int2 initThreadOffset_;
  int2 step_;
  int iterations_;

  int2 dst_image_size = srt.level_size(INPUT_LEVEL + 1);
  int2 futureKernel_size = kernel_size_from_input_size(dst_image_size);

  if (futureKernel_size.x == 3) {
    if (futureKernel_size.y == 3) {
      // Fill in 2 17x7 steps and 1 17x3 step (9 idle threads)
      initThreadOffset_ = int2(local_index % 17u, local_index / 17u);
      step_ = int2(0, 7);
      iterations_ = local_index >= 7 * 17 ? 0 : local_index < 3 * 17 ? 3 : 2;
    }
    else  // Future 3x[2,1] kernel
    {
      // Fill in 2 8x16 steps and 1 1x16 step
      initThreadOffset_ = int2(local_index / 16u, local_index % 16u);
      step_ = int2(8, 0);
      iterations_ = local_index < 1 * 16 ? 3 : 2;
    }
  }
  else {
    if (futureKernel_size.y == 3) {
      // Fill in 2 16x8 steps and 1 16x1 step
      initThreadOffset_ = int2(local_index % 16u, local_index / 16u);
      step_ = int2(0, 8);
      iterations_ = local_index < 1 * 16 ? 3 : 2;
    }
    else {
      // Fill in 2 16x8 steps
      initThreadOffset_ = int2(local_index % 16u, local_index / 16u);
      step_ = int2(0, 8);
      iterations_ = 2;
    }
  }

  intermediateLevelLoop_<SRT>(
      srt, dstTileCoord_ + initThreadOffset_, initThreadOffset_, step_, iterations_, boundsCheck_);
}
template void fillIntermediateTile_<Resources<UNORM_8_8_8_8, SharedSRGB>>(
    Resources<UNORM_8_8_8_8, SharedSRGB> &srt,
    uint local_index,
    int2 dstTileCoord_,
    bool boundsCheck_);
template void fillIntermediateTile_<Resources<SFLOAT_16, SharedFloat>>(
    Resources<SFLOAT_16, SharedFloat> &srt,
    uint local_index,
    int2 dstTileCoord_,
    bool boundsCheck_);

/**
 * Function for the workgroup that handles filling the last level tile
 * (2nd level after the original input level), using as input the
 * tile in shared memory.
 *
 * dstTileCoord_ : upper left coordinate of the tile to generate.
 * boundsCheck_  : whether to skip samples that are out-of-bounds.
 */
template<typename SRT>
void fillLastTile_(SRT &srt, uint local_index, int2 dstTileCoord_, bool boundsCheck_)
{

  if (local_index < 8 * 8) {
    int2 threadOffset_ = int2(local_index % 8u, local_index / 8u);
    int src_level = INPUT_LEVEL + 1;
    int dst_level = INPUT_LEVEL + 2;
    int2 srcImageSize_ = srt.level_size(src_level);
    int2 dst_image_size = srt.level_size(dst_level);

    int2 srcSharedCoord_ = threadOffset_ * 2;
    int2 kernel_size = kernel_size_from_input_size(srcImageSize_);
    int2 dst_coord = threadOffset_ + dstTileCoord_;

    bool inBounds_ = true;
    if (boundsCheck_) {
      inBounds_ = (uint(dst_coord.x) < uint(dst_image_size.x)) &&
                  (uint(dst_coord.y) < uint(dst_image_size.y));
    }
    if (inBounds_) {
      reduce_store_sample<SRT, float4, true>(
          srt, srcSharedCoord_, 0, kernel_size, dst_image_size, dst_coord, dst_level);
    }
  }
}
template void fillLastTile_<Resources<UNORM_8_8_8_8, SharedSRGB>>(
    Resources<UNORM_8_8_8_8, SharedSRGB> &srt,
    uint local_index,
    int2 dstTileCoord_,
    bool boundsCheck_);
template void fillLastTile_<Resources<SFLOAT_16, SharedFloat>>(
    Resources<SFLOAT_16, SharedFloat> &srt,
    uint local_index,
    int2 dstTileCoord_,
    bool boundsCheck_);

template<typename SRT, typename T>
void update_mipmaps(const uint3 global_id,
                    const uint3 group_id,
                    const uint local_index,
                    [[resource_table]] SRT &srt)
{
  int inputLevel_ = INPUT_LEVEL;

  if (srt.num_levels == 1u) {
    int2 kernel_size = kernel_size_from_input_size(srt.level_size(inputLevel_));
    int2 dst_image_size = srt.level_size(inputLevel_ + 1);
    int2 dst_coord = int2(int(global_id.x) % dst_image_size.x,
                          int(global_id.x) / dst_image_size.x);
    int2 src_coord = dst_coord * 2;

    if (dst_coord.y < dst_image_size.y) {
      reduce_store_sample<SRT, float4, false>(
          srt, src_coord, inputLevel_, kernel_size, dst_image_size, dst_coord, inputLevel_ + 1);
    }
  }
  else  // Handling two levels.
  {
    // Assign a 8x8 tile of mip level inputLevel_ + 2 to this workgroup.
    int level2_ = inputLevel_ + 2;
    int2 level2Size_ = srt.level_size(level2_);
    int2 tileCount_;
    tileCount_.x = int(uint(level2Size_.x + 7) / 8u);
    tileCount_.y = int(uint(level2Size_.y + 7) / 8u);
    int2 tileIdx_ = int2(group_id.x % uint(tileCount_.x), group_id.x / uint(tileCount_.x));

    // Determine if bounds checking is needed; this is only the case
    // for tiles at the right or bottom fringe that might be cut off
    // by the image border. Note that later, I use if statements rather
    // than passing boundsCheck_ directly to convince the compiler
    // to inline everything.
    bool boundsCheck_ = tileIdx_.x >= tileCount_.x - 1 || tileIdx_.y >= tileCount_.y - 1;

    if (boundsCheck_) {
      // Compute the tile in level inputLevel_ + 1 that's needed to
      // compute the above 8x8 tile.
      fillIntermediateTile_<SRT>(srt, local_index, tileIdx_ * 2 * int2(8, 8), true);
      barrier();

      // Compute the inputLevel_ + 2 tile of size 8x8, loading
      // inupts from shared memory.
      fillLastTile_<SRT>(srt, local_index, tileIdx_ * int2(8, 8), true);
    }
    else {
      // Same with no bounds checking.
      fillIntermediateTile_<SRT>(srt, local_index, tileIdx_ * 2 * int2(8, 8), false);
      barrier();
      fillLastTile_<SRT>(srt, local_index, tileIdx_ * int2(8, 8), false);
    }
  }
}
template void update_mipmaps<Resources<UNORM_8_8_8_8, SharedSRGB>, float4>(
    [[global_invocation_id]] const uint3 global_id,
    [[work_group_id]] const uint3 group_id,
    [[local_invocation_index]] const uint local_index,
    [[resource_table]] Resources<UNORM_8_8_8_8, SharedSRGB> &srt);
template void update_mipmaps<Resources<SFLOAT_16, SharedFloat>, float>(
    [[global_invocation_id]] const uint3 global_id,
    [[work_group_id]] const uint3 group_id,
    [[local_invocation_index]] const uint local_index,
    [[resource_table]] Resources<SFLOAT_16, SharedFloat> &srt);

/**
 * \brief Update mipmaps entry compute shader entry point for multi component images.
 */
[[local_size(LOCAL_SIZE_X)]] [[compute]]
void update_mipmaps_float4([[global_invocation_id]] const uint3 global_id,
                           [[work_group_id]] const uint3 group_id,
                           [[local_invocation_index]] const uint local_index,
                           [[resource_table]] Resources<UNORM_8_8_8_8, SharedSRGB> &srt)
{
  update_mipmaps<Resources<UNORM_8_8_8_8, SharedSRGB>, float4>(
      global_id, group_id, local_index, srt);
}

/**
 * \brief Update mipmaps entry compute shader entry point for single component images.
 */
[[local_size(LOCAL_SIZE_X)]] [[compute]]
void update_mipmaps_float([[global_invocation_id]] const uint3 global_id,
                          [[work_group_id]] const uint3 group_id,
                          [[local_invocation_index]] const uint local_index,
                          [[resource_table]] Resources<SFLOAT_16, SharedFloat> &srt)
{
  update_mipmaps<Resources<SFLOAT_16, SharedFloat>, float>(global_id, group_id, local_index, srt);
}

}  // namespace builtin::mipmaps

PipelineCompute gpu_shader_2D_update_mipmaps_unorm_8_8_8_8(
    builtin::mipmaps::update_mipmaps_float4,
    builtin::mipmaps::Resources<builtin::mipmaps::UNORM_8_8_8_8, builtin::mipmaps::SharedSRGB>{
        .use_shared_srgb_uint = true, .use_shared_float = false});

PipelineCompute gpu_shader_2D_update_mipmaps_sfloat_16(
    builtin::mipmaps::update_mipmaps_float,
    builtin::mipmaps::Resources<builtin::mipmaps::SFLOAT_16, builtin::mipmaps::SharedFloat>{
        .use_shared_srgb_uint = false, .use_shared_float = true});
