/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#pragma create_info

#include "gpu_shader_compat.hh"
#include "gpu_shader_create_info.hh"

namespace builtin::mipmaps {

/* Conversion functions. */
template<typename DstType, typename SrcType>
void convert(DstType &dst_value, const SrcType src_value)
{
}

template<> void convert<float4, float>(float4 &dst_value, const float src_value)
{
  dst_value.x = src_value;
  dst_value.y = 0.0;
  dst_value.z = 0.0;
  dst_value.w = 0.0;
}
template<> void convert<float, float4>(float &dst_value, const float4 src_value)
{
  dst_value = src_value.x;
}
template<> void convert<float4, float4>(float4 &dst_value, const float4 src_value)
{
  dst_value = src_value;
}

/* TODO: These encoders/decoders are a slightly off from our own implementation. Might fix some
 * specific issues, which we need to test. */
/**  Convert float (0-1) sRGB red/green/blue component value to linear. */
float linear_from_srgb_component(float srgb)
{
  return srgb <= 0.04045 ? srgb * (25 / 323.) : pow((200 * srgb + 11) * (1 / 211.), 2.4);
}

/** Convert linear red/green/blue component to float (0-1) sRGB */
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

/** Shared storage that can store intermediate results in an SRGB encoded uint. */
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

/** Shared storage that can store intermediate results in a single float. */
struct SharedFloat {
  /**
   * When generating 2 levels, the results of generating the intermediate *level(first level
   * generated) are cached here; this is the input tile *needed to generate the 8x8 tile of the
   * second level generated.
   */
  [[shared]] float intermediate_level[MAX_SHARED_SAMPLES][MAX_SHARED_SAMPLES];

  void store_sample(int2 dst_coord, float color)
  {
    intermediate_level[dst_coord.y][dst_coord.x] = color;
  }

  float load_sample(int2 src_coord)
  {
    return intermediate_level[src_coord.y][src_coord.x];
  }
};

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

template<enum TextureFormat format, typename SharedStorage, typename InnerType> struct Resources {
  [[push_constant]] const int num_levels;
  [[image(0, read, format)]] image2D mip_in;
  [[image(1, write, format)]] image2D mip_out1;
  [[image(2, write, format)]] image2D mip_out2;
  [[resource_table]] srt_t<SharedStorage> shared_storage;

  /** Store sample result into an output mip image. */
  void store_sample(int2 dst_coord, int dst_level, InnerType color)
  {
    float4 color_out;
    convert<float4, InnerType>(color_out, color);

    if (dst_level == 1) {
      imageStore(mip_out1, dst_coord, color_out);
    }
    else if (dst_level == 2) {
      imageStore(mip_out2, dst_coord, color_out);
    }
  }

  void store_shared_sample(int2 dst_coord, InnerType color)
  {
    SharedStorage &storage = shared_storage;
    storage.store_sample(dst_coord, color);
  }

  InnerType load_sample(int2 src_coord, bool load_from_shared)
  {
    InnerType color;
    if (load_from_shared) {
      SharedStorage &storage = shared_storage;
      color = storage.load_sample(src_coord);
    }
    else {
      float4 loaded_color = imageLoad(mip_in, src_coord);
      convert<InnerType, float4>(color, loaded_color);
    }
    return color;
  }

  int2 level_size(int level)
  {
    int2 mip_in_size = imageSize(mip_in);
    int2 mip_size = max((mip_in_size >> level), int2(1));
    return mip_size;
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
  template<bool load_from_shared>
  InnerType reduce_store_sample(int2 src_coord,
                                int src_level,
                                int2 kernel_size,
                                int2 dst_image_size,
                                int2 dst_coord,
                                int dst_level)
  {
    float num_dst_pixels = dst_image_size.y;
    float rcp = 1.0f / (2 * num_dst_pixels + 1);
    float w0 = rcp * (num_dst_pixels - dst_coord.y);
    float w1 = rcp * num_dst_pixels;
    float w2 = 1.0f - w0 - w1;

    InnerType v0, v1, v2, h0, h1, h2, out_pixel;

    /* Reduce vertically up to 3 times (depending on kernel horizontal size) */
    switch (kernel_size.x) {
      case 3:
        switch (kernel_size.y) {
          case 3:
            v2 = load_sample(src_coord + int2(2, 2), load_from_shared);
            ATTR_FALLTHROUGH;
          case 2:
            v1 = load_sample(src_coord + int2(2, 1), load_from_shared);
            ATTR_FALLTHROUGH;
          case 1:
            v0 = load_sample(src_coord + int2(2, 0), load_from_shared);
            break;
        }
        switch (kernel_size.y) {
          case 3:
            h2 = pyramid_reduce(w0, v0, w1, v1, w2, v2);
            break;
          case 2:
            h2 = pyramid_reduce2(v0, v1);
            break;
          case 1:
            h2 = v0;
            break;
        }
        ATTR_FALLTHROUGH;
      case 2:
        switch (kernel_size.y) {
          case 3:
            v2 = load_sample(src_coord + int2(1, 2), load_from_shared);
            ATTR_FALLTHROUGH;
          case 2:
            v1 = load_sample(src_coord + int2(1, 1), load_from_shared);
            ATTR_FALLTHROUGH;
          case 1:
            v0 = load_sample(src_coord + int2(1, 0), load_from_shared);
            break;
        }
        switch (kernel_size.y) {
          case 3:
            h1 = pyramid_reduce(w0, v0, w1, v1, w2, v2);
            break;
          case 2:
            h1 = pyramid_reduce2(v0, v1);
            break;
          case 1:
            h1 = v0;
            break;
        }
        ATTR_FALLTHROUGH;
      case 1:
        switch (kernel_size.y) {
          case 3:
            v2 = load_sample(src_coord + int2(0, 2), load_from_shared);
            ATTR_FALLTHROUGH;
          case 2:
            v1 = load_sample(src_coord + int2(0, 1), load_from_shared);
            ATTR_FALLTHROUGH;
          case 1:
            v0 = load_sample(src_coord + int2(0, 0), load_from_shared);
            break;
        }
        switch (kernel_size.y) {
          case 3:
            h0 = pyramid_reduce(w0, v0, w1, v1, w2, v2);
            break;
          case 2:
            h0 = pyramid_reduce2(v0, v1);
            break;
          case 1:
            h0 = v0;
            break;
        }
    }

    /* Reduce up to 3 samples horizontally. */
    switch (kernel_size.x) {
      case 3:
        num_dst_pixels = dst_image_size.x;
        rcp = 1.0f / (2 * num_dst_pixels + 1);
        w0 = rcp * (num_dst_pixels - dst_coord.x);
        w1 = rcp * num_dst_pixels;
        w2 = 1.0f - w0 - w1;
        out_pixel = pyramid_reduce(w0, h0, w1, h1, w2, h2);
        break;
      case 2:
        out_pixel = pyramid_reduce2(h0, h1);
        break;
      case 1:
        out_pixel = h0;
    }

    /* Write out sample. */
    store_sample(dst_coord, dst_level, out_pixel);
    return out_pixel;
  }

  /**
   * Compute and write out (to the 1st mip level generated) the samples
   * at coordinates
   *     init_dst_coord,
   *     init_dst_coord + step, ...
   *     init_dst_coord + (iterations-1) * step
   * and cache them at in the sharedLevel_ tile at coordinates
   *     init_shared_coord,
   *     init_shared_coord + step, ...
   *     init_shared_coord + (iterations-1) * step
   * If use_bounds_check is true, skip coordinates that are out of bounds.
   */
  void intermediate_level_loop(int2 init_dst_coord,
                               int2 init_shared_coord,
                               int2 step,
                               int iterations,
                               bool use_bounds_check)
  {
    int2 dst_coord = init_dst_coord;
    int2 shared_coord = init_shared_coord;
    int src_level = INPUT_LEVEL;
    int dst_level = src_level + 1;
    int2 src_image_size = level_size(src_level);
    int2 dst_image_size = level_size(dst_level);
    int2 kernel_size = kernel_size_from_input_size(src_image_size);

    for (int i_ = 0; i_ < iterations; ++i_) {
      int2 src_coord = dst_coord * 2;

      if (use_bounds_check) {
        if (uint(dst_coord.x) >= uint(dst_image_size.x)) {
          continue;
        }
        if (uint(dst_coord.y) >= uint(dst_image_size.y)) {
          continue;
        }
      }

      InnerType sample = reduce_store_sample<false>(
          src_coord, src_level, kernel_size, dst_image_size, dst_coord, dst_level);

      /* `reduce_store_sample` handles writing to the actual output; manually
       * cache into shared memory here. */
      store_shared_sample(shared_coord, sample);
      dst_coord += step;
      shared_coord += step;
    }
  }

  /**
   * Function for the workgroup that handles filling the intermediate level
   * (caching it in shared memory as well).
   *
   * We need somewhere from 16x16 to 17x17 samples, depending
   * on what the kernel size for the 2nd mip level generation will be.
   *
   * dst_tile_coord : upper left coordinate of the tile to generate.
   * use_bounds_check  : whether to skip samples that are out-of-bounds.
   */
  void fill_intermediate_tile(uint local_index, int2 dst_tile_coord, bool use_bounds_check)
  {
    int2 init_thread_offset;
    int2 step;
    int iterations;

    int2 dst_image_size = level_size(INPUT_LEVEL + 1);
    int2 future_kernel_size = kernel_size_from_input_size(dst_image_size);

    if (future_kernel_size.x == 3) {
      if (future_kernel_size.y == 3) {
        /* Fill in 2 17x7 steps and 1 17x3 step (9 idle threads) */
        init_thread_offset = int2(local_index % 17u, local_index / 17u);
        step = int2(0, 7);
        iterations = local_index >= 7 * 17 ? 0 : local_index < 3 * 17 ? 3 : 2;
      }
      else {
        /* Future 3x[2,1] kernel
         * Fill in 2 8x16 steps and 1 1x16 step */
        init_thread_offset = int2(local_index / 16u, local_index % 16u);
        step = int2(8, 0);
        iterations = local_index < 1 * 16 ? 3 : 2;
      }
    }
    else {
      if (future_kernel_size.y == 3) {
        /* Fill in 2 16x8 steps and 1 16x1 step */
        init_thread_offset = int2(local_index % 16u, local_index / 16u);
        step = int2(0, 8);
        iterations = local_index < 1 * 16 ? 3 : 2;
      }
      else {
        /* Fill in 2 16x8 steps */
        init_thread_offset = int2(local_index % 16u, local_index / 16u);
        step = int2(0, 8);
        iterations = 2;
      }
    }

    intermediate_level_loop(dst_tile_coord + init_thread_offset,
                            init_thread_offset,
                            step,
                            iterations,
                            use_bounds_check);
  }

  /**
   * Function for the workgroup that handles filling the last level tile
   * (2nd level after the original input level), using as input the
   * tile in shared memory.
   *
   * dst_tile_coord : upper left coordinate of the tile to generate.
   * use_bounds_check  : whether to skip samples that are out-of-bounds.
   */
  void fill_last_tile(uint local_index, int2 dst_tile_coord, bool use_bounds_check)
  {

    if (local_index < 8 * 8) {
      int2 thread_offset = int2(local_index % 8u, local_index / 8u);
      int src_level = INPUT_LEVEL + 1;
      int dst_level = INPUT_LEVEL + 2;
      int2 src_image_size = level_size(src_level);
      int2 dst_image_size = level_size(dst_level);

      int2 src_shared_coord = thread_offset * 2;
      int2 kernel_size = kernel_size_from_input_size(src_image_size);
      int2 dst_coord = thread_offset + dst_tile_coord;

      bool within_bounds = true;
      if (use_bounds_check) {
        within_bounds = (uint(dst_coord.x) < uint(dst_image_size.x)) &&
                        (uint(dst_coord.y) < uint(dst_image_size.y));
      }
      if (within_bounds) {
        reduce_store_sample<true>(
            src_shared_coord, 0, kernel_size, dst_image_size, dst_coord, dst_level);
      }
    }
  }
};

template struct Resources<UNORM_8_8_8_8, SharedSRGB, float4>;
template struct Resources<SFLOAT_16, SharedFloat, float>;

template float4 Resources<UNORM_8_8_8_8, SharedSRGB, float4>::reduce_store_sample<true>(
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);
template float4 Resources<UNORM_8_8_8_8, SharedSRGB, float4>::reduce_store_sample<false>(
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);
template float Resources<SFLOAT_16, SharedFloat, float>::reduce_store_sample<true>(
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);
template float Resources<SFLOAT_16, SharedFloat, float>::reduce_store_sample<false>(
    int2 src_coord,
    int src_level,
    int2 kernel_size,
    int2 dst_image_size,
    int2 dst_coord,
    int dst_level);

template<typename SRT>
[[local_size(LOCAL_SIZE_X)]] [[compute]]
void update_mipmaps(const uint3 global_id,
                    const uint3 group_id,
                    const uint local_index,
                    [[resource_table]] SRT &srt)
{
  if (srt.num_levels == 1u) {
    int2 kernel_size = kernel_size_from_input_size(srt.level_size(INPUT_LEVEL));
    int2 dst_image_size = srt.level_size(INPUT_LEVEL + 1);
    int2 dst_coord = int2(int(global_id.x) % dst_image_size.x,
                          int(global_id.x) / dst_image_size.x);
    int2 src_coord = dst_coord * 2;

    if (dst_coord.y < dst_image_size.y) {
      srt.template reduce_store_sample<false>(
          src_coord, INPUT_LEVEL, kernel_size, dst_image_size, dst_coord, INPUT_LEVEL + 1);
    }
  }
  else {
    /* Handling two levels.
     * Assign a 8x8 tile of mip level inputLevel_ + 2 to this workgroup. */
    int level2 = INPUT_LEVEL + 2;
    int2 level2_size = srt.level_size(level2);
    int2 tile_count;
    tile_count.x = int(uint(level2_size.x + 7) / 8u);
    tile_count.y = int(uint(level2_size.y + 7) / 8u);
    int2 tile_index = int2(group_id.x % uint(tile_count.x), group_id.x / uint(tile_count.x));

    /* Determine if bounds checking is needed; this is only the case
     * for tiles at the right or bottom fringe that might be cut off
     * by the image border. Note that later, I use if statements rather
     * than passing use_bounds_check directly to convince the compiler
     * to inline everything. */
    bool use_bounds_check = tile_index.x >= tile_count.x - 1 || tile_index.y >= tile_count.y - 1;

    if (use_bounds_check) {
      /* Compute the tile in level inputLevel_ + 1 that's needed to
       * compute the above 8x8 tile. */
      srt.fill_intermediate_tile(local_index, tile_index * 2 * int2(8, 8), true);
      barrier();

      /* Compute the inputLevel_ + 2 tile of size 8x8, loading
       * inputs from shared memory. */
      srt.fill_last_tile(local_index, tile_index * int2(8, 8), true);
    }
    else {
      /* Same but without bounds checking. */
      srt.fill_intermediate_tile(local_index, tile_index * 2 * int2(8, 8), false);
      barrier();
      srt.fill_last_tile(local_index, tile_index * int2(8, 8), false);
    }
  }
}

template void update_mipmaps<Resources<UNORM_8_8_8_8, SharedSRGB, float4>>(
    [[global_invocation_id]] const uint3 global_id,
    [[work_group_id]] const uint3 group_id,
    [[local_invocation_index]] const uint local_index,
    [[resource_table]] Resources<UNORM_8_8_8_8, SharedSRGB, float4> &srt);
template void update_mipmaps<Resources<SFLOAT_16, SharedFloat, float>>(
    [[global_invocation_id]] const uint3 global_id,
    [[work_group_id]] const uint3 group_id,
    [[local_invocation_index]] const uint local_index,
    [[resource_table]] Resources<SFLOAT_16, SharedFloat, float> &srt);

}  // namespace builtin::mipmaps

PipelineCompute gpu_shader_2D_update_mipmaps_unorm_8_8_8_8(
    builtin::mipmaps::update_mipmaps<builtin::mipmaps::Resources<builtin::mipmaps::UNORM_8_8_8_8,
                                                                 builtin::mipmaps::SharedSRGB,
                                                                 float4>>,
    builtin::mipmaps::
        Resources<builtin::mipmaps::UNORM_8_8_8_8, builtin::mipmaps::SharedSRGB, float4>{});

PipelineCompute gpu_shader_2D_update_mipmaps_sfloat_16(
    builtin::mipmaps::update_mipmaps<builtin::mipmaps::Resources<builtin::mipmaps::SFLOAT_16,
                                                                 builtin::mipmaps::SharedFloat,
                                                                 float>>,
    builtin::mipmaps::
        Resources<builtin::mipmaps::SFLOAT_16, builtin::mipmaps::SharedFloat, float>{});
