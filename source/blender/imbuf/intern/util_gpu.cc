/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "BLI_array.hh"
#include "BLI_bit_span.hh"
#include "BLI_bit_vector.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_mutex.hh"
#include "BLI_rect.hh"
#include "BLI_time.hh"
#include "BLI_utildefines.hh"

#include "MEM_guardedalloc.h"

#include <mutex>

#include "CLG_log.h"

#include "GPU_capabilities.hh"
#include "GPU_texture.hh"

#include "IMB_colormanagement.hh"
#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"
#include "IMB_partial_update.hh"

namespace blender {

static CLG_LogRef LOG = {"image.gpu"};

/* gpu ibuf utils */

static bool imb_is_grayscale_texture_format_compatible(const ImBuf *ibuf)
{
  if (ibuf->color_mode != ImColorMode::BW) {
    return false;
  }

  if (ibuf->byte_data() && !ibuf->float_data()) {

    if (IMB_colormanagement_space_is_scene_linear_srgb(ibuf->byte_buffer.colorspace) ||
        IMB_colormanagement_space_is_scene_linear(ibuf->byte_buffer.colorspace))
    {
      /* Grey-scale byte buffers with these color transforms utilize float buffers under the hood
       * and can therefore be optimized. */
      return true;
    }
    /* TODO: Support gray-scale byte buffers.
     * The challenge is that Blender always stores byte images as RGBA. */
    return false;
  }

  /* Only #IMBuf's with color-space that do not modify the chrominance of the texture data relative
   * to the scene color space can be uploaded as single channel textures. */
  if (IMB_colormanagement_space_is_data(ibuf->float_buffer.colorspace) ||
      IMB_colormanagement_space_is_scene_linear_srgb(ibuf->float_buffer.colorspace) ||
      IMB_colormanagement_space_is_scene_linear(ibuf->float_buffer.colorspace))
  {
    return true;
  }
  return false;
}

static void imb_gpu_get_format(const ImBuf *ibuf,
                               bool high_bitdepth,
                               bool use_grayscale,
                               gpu::TextureFormat *r_texture_format)
{
  const bool float_rect = (ibuf->float_data() != nullptr);
  const bool is_grayscale = use_grayscale && imb_is_grayscale_texture_format_compatible(ibuf);

  if (float_rect) {
    /* Float. */
    const bool use_high_bitdepth = (!(ibuf->foptions.flag & OPENEXR_HALF) && high_bitdepth);
    *r_texture_format = is_grayscale ?
                            (use_high_bitdepth ? gpu::TextureFormat::SFLOAT_32 :
                                                 gpu::TextureFormat::SFLOAT_16) :
                            (use_high_bitdepth ? gpu::TextureFormat::SFLOAT_32_32_32_32 :
                                                 gpu::TextureFormat::SFLOAT_16_16_16_16);
  }
  else {
    if (IMB_colormanagement_space_is_data(ibuf->byte_buffer.colorspace) ||
        IMB_colormanagement_space_is_scene_linear(ibuf->byte_buffer.colorspace))
    {
      /* Non-color data or scene linear, just store buffer as is. */
      *r_texture_format = (is_grayscale) ? gpu::TextureFormat::UNORM_8 :
                                           gpu::TextureFormat::UNORM_8_8_8_8;
    }
    else if (IMB_colormanagement_space_is_scene_linear_srgb(ibuf->byte_buffer.colorspace)) {
      /* scene linear + sRGB, store as byte texture that the GPU can decode directly. */
      *r_texture_format = (is_grayscale) ? gpu::TextureFormat::SFLOAT_16 :
                                           gpu::TextureFormat::SRGBA_8_8_8_8;
    }
    else {
      /* Other colorspace, store as half float texture to avoid precision loss. */
      *r_texture_format = (is_grayscale) ? gpu::TextureFormat::SFLOAT_16 :
                                           gpu::TextureFormat::SFLOAT_16_16_16_16;
    }
  }
}

static const char *imb_gpu_get_swizzle(const ImBuf *ibuf)
{
  return imb_is_grayscale_texture_format_compatible(ibuf) ? "rrra" : "rgba";
}

/* Return false if no suitable format was found. */
bool IMB_gpu_get_compressed_format(const ImBuf *ibuf, gpu::TextureFormat *r_texture_format)
{
  if (ibuf->ftype != IMB_FTYPE_DDS) {
    return false;
  }

  /* Compressed DDS files can really only express sRGB or data/linear. */
  const bool use_srgb = (!IMB_colormanagement_space_is_data(ibuf->byte_buffer.colorspace) &&
                         !IMB_colormanagement_space_is_scene_linear(ibuf->byte_buffer.colorspace));
  if (ibuf->foptions.flag & DDS_COMPRESSED_DXT1) {
    *r_texture_format = use_srgb ? gpu::TextureFormat::SRGB_DXT1 : gpu::TextureFormat::SNORM_DXT1;
    return true;
  }
  if (ibuf->foptions.flag & DDS_COMPRESSED_DXT3) {
    *r_texture_format = use_srgb ? gpu::TextureFormat::SRGB_DXT3 : gpu::TextureFormat::SNORM_DXT3;
    return true;
  }
  if (ibuf->foptions.flag & DDS_COMPRESSED_DXT5) {
    *r_texture_format = use_srgb ? gpu::TextureFormat::SRGB_DXT5 : gpu::TextureFormat::SNORM_DXT5;
    return true;
  }
  return false;
}

/* Extract the first channel of an RGBA buffer into a single channel, for uploading grayscale. */
template<typename T>
static void imb_gpu_extract_first_channel(
    const T *src, const int channels, const int stride, const int w, const int h, T *r_gray)
{
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      r_gray[size_t(row) * w + col] = src[(size_t(row) * stride + col) * channels];
    }
  }
}

/* Returns an image buffer containing the data from the source buffer but suitable for uploading to
 * GPU textures. The output should be freed by the caller. If rescale_size is not nullopt, the
 * image will be scaled using a box filter to match the rescale size. If premultiplied_alpha is
 * true and alpha is not packed, alpha is ensured to be premultiplied, otherwise, it is ensured to
 * be straight. If allow_grayscale is true, if the image could be stored as a grayscale image with
 * no data loss, it will be return as so, otherwise, it will be RGBA. */
static ImBuf *get_gpu_texture_data(ImBuf *source_buffer,
                                   const std::optional<int2> rescale_size,
                                   const bool premultiplied_alpha,
                                   const bool allow_grayscale)
{
  /* Start with a copy of the source buffer, this will be processed further if needed below. */
  ImBuf *output_buffer = IMB_allocImBuf(source_buffer->x, source_buffer->y, ImBufFlags::Zero);
  output_buffer->color_mode = source_buffer->color_mode;
  output_buffer->flags = source_buffer->flags;
  output_buffer->float_buffer = source_buffer->float_buffer;
  output_buffer->channels = source_buffer->channels;
  output_buffer->byte_buffer = source_buffer->byte_buffer;

  const bool is_grayscale = allow_grayscale &&
                            imb_is_grayscale_texture_format_compatible(source_buffer);

  /* Ensure that the output buffer is RGBA with the given alpha association. */
  if (output_buffer->float_data()) {
    /* Float images are already in scene linear color space or contain non-color data with
     * premultiplied alpha by convention, so no color space conversion is needed. But we need to
     * convert to RGBA and unpremultiply alpha if needed. */
    if (output_buffer->channels != 4 || !premultiplied_alpha) {
      ImBuf *buffer = IMB_allocImBuf(output_buffer->x,
                                     output_buffer->y,
                                     ImBufFlags::FloatData | ImBufFlags::UninitializedPixels);
      IMB_colormanagement_imbuf_to_float_texture(buffer->float_data_for_write(),
                                                 0,
                                                 0,
                                                 output_buffer->x,
                                                 output_buffer->y,
                                                 output_buffer,
                                                 premultiplied_alpha);
      IMB_freeImBuf(output_buffer);
      output_buffer = buffer;
    }
  }
  else {
    /* Byte images are in original color space from the file with straight alpha, so we need to
     * convert to scene linear color space and premultiply alpha if needed. An exception is images
     * in sRGB color space, see the relevant case below for more information.  */
    if (IMB_colormanagement_space_is_data(source_buffer->byte_buffer.colorspace)) {
      /* Non-color data, used as is. */
    }
    else if (IMB_colormanagement_space_is_scene_linear(source_buffer->byte_buffer.colorspace)) {
      /* Color space is already linear, we just need to premultiply the alpha if needed. */
      if (!is_grayscale && premultiplied_alpha) {
        ImBuf *buffer = IMB_allocImBuf(output_buffer->x,
                                       output_buffer->y,
                                       ImBufFlags::ByteData | ImBufFlags::UninitializedPixels);
        IMB_colormanagement_imbuf_to_byte_texture(buffer->byte_data_for_write(),
                                                  0,
                                                  0,
                                                  output_buffer->x,
                                                  output_buffer->y,
                                                  output_buffer,
                                                  premultiplied_alpha);
        IMB_freeImBuf(output_buffer);
        output_buffer = buffer;
      }
    }
    else if (IMB_colormanagement_space_is_scene_linear_srgb(source_buffer->byte_buffer.colorspace))
    {
      /* Images in scene linear + sRGB color space are a special case since they can be stored in
       * sRGB textures on GPU, where scene linear space conversion will happen during sampling in
       * the shader, however, this is not possible for grayscale images, so we need to do the
       * conversion here, converting to a float image to prevent precision loss.
       *
       * It should be noted that for other color spaces, color space conversion happen before alpha
       * premultiplication, while for sRGB, premultiplication will happen first since color space
       * conversion happen in the shader as mentioned above. This will manifest as differences near
       * alpha edges. But we generally accept this due to the advantages of sRGB textures. If this
       * is problematic, the source image buffer should be linearised first. */
      if (is_grayscale) {
        ImBuf *buffer = IMB_allocImBuf(output_buffer->x,
                                       output_buffer->y,
                                       ImBufFlags::FloatData | ImBufFlags::UninitializedPixels);
        IMB_colormanagement_imbuf_to_float_texture(buffer->float_data_for_write(),
                                                   0,
                                                   0,
                                                   output_buffer->x,
                                                   output_buffer->y,
                                                   output_buffer,
                                                   premultiplied_alpha);
        IMB_freeImBuf(output_buffer);
        output_buffer = buffer;
      }
      else if (premultiplied_alpha) {
        /* We need to premultiply the alpha. */
        ImBuf *buffer = IMB_allocImBuf(output_buffer->x,
                                       output_buffer->y,
                                       ImBufFlags::ByteData | ImBufFlags::UninitializedPixels);
        IMB_colormanagement_imbuf_to_byte_texture(buffer->byte_data_for_write(),
                                                  0,
                                                  0,
                                                  output_buffer->x,
                                                  output_buffer->y,
                                                  output_buffer,
                                                  premultiplied_alpha);
        IMB_freeImBuf(output_buffer);
        output_buffer = buffer;
      }
    }
    else {
      /* Other color-space, convert to linear color space and premultiply alpha if needed.
       * Conversion happen as a float to avoid precision loss. */
      ImBuf *buffer = IMB_allocImBuf(output_buffer->x,
                                     output_buffer->y,
                                     ImBufFlags::FloatData | ImBufFlags::UninitializedPixels);
      IMB_colormanagement_imbuf_to_float_texture(buffer->float_data_for_write(),
                                                 0,
                                                 0,
                                                 output_buffer->x,
                                                 output_buffer->y,
                                                 output_buffer,
                                                 premultiplied_alpha);
      IMB_freeImBuf(output_buffer);
      output_buffer = buffer;
    }
  }

  /* Rescale the image using a box filter if needed. */
  if (rescale_size.has_value()) {
    if (output_buffer->float_data()) {
      ImBuf *buffer = IMB_allocImBuf(rescale_size->x,
                                     rescale_size->y,
                                     ImBufFlags::FloatData | ImBufFlags::UninitializedPixels);
      IMB_scale_box(output_buffer->float_data(),
                    int2(output_buffer->x, output_buffer->y),
                    4,
                    buffer->float_data_for_write(),
                    *rescale_size,
                    true);
      IMB_freeImBuf(output_buffer);
      output_buffer = buffer;
    }
    else {
      ImBuf *buffer = IMB_allocImBuf(rescale_size->x,
                                     rescale_size->y,
                                     ImBufFlags::ByteData | ImBufFlags::UninitializedPixels);
      IMB_scale_box(output_buffer->byte_data(),
                    int2(output_buffer->x, output_buffer->y),
                    4,
                    buffer->byte_data_for_write(),
                    *rescale_size,
                    true);
      IMB_freeImBuf(output_buffer);
      output_buffer = buffer;
    }
  }

  /* So far, the output buffer is RGBA, if it should be grayscale, we need to convert it to a
   * single channel image. */
  if (is_grayscale) {
    const size_t buffer_size = output_buffer->x * output_buffer->y;
    if (output_buffer->float_data()) {
      ImBuf *buffer = IMB_allocImBuf(output_buffer->x, output_buffer->y, ImBufFlags::Zero);
      IMB_alloc_float_pixels(buffer, 1);
      imb_gpu_extract_first_channel(output_buffer->float_data(),
                                    output_buffer->channels,
                                    output_buffer->x,
                                    output_buffer->x,
                                    output_buffer->y,
                                    buffer->float_data_for_write());
      IMB_freeImBuf(output_buffer);
      output_buffer = buffer;
    }
    else {
      ImBuf *buffer = IMB_allocImBuf(output_buffer->x, output_buffer->y, ImBufFlags::Zero);
      buffer->color_mode = ImColorMode::BW;
      buffer->assign_byte_data(MEM_new_array_uninitialized<uint8_t>(buffer_size, __func__));
      imb_gpu_extract_first_channel(output_buffer->byte_data(),
                                    output_buffer->channels,
                                    output_buffer->x,
                                    output_buffer->x,
                                    output_buffer->y,
                                    buffer->byte_data_for_write());
      IMB_freeImBuf(output_buffer);
      output_buffer = buffer;
    }
  }

  return output_buffer;
}

static void imb_gpu_texture_default_init(gpu::Texture *tex, const ImBuf *ibuf)
{
  GPU_texture_swizzle_set(tex, imb_gpu_get_swizzle(ibuf));
  GPU_texture_anisotropic_filter(tex, true);
}

static void imb_gpu_texture_default_init_mipmap(gpu::Texture *tex)
{
  GPU_texture_extend_mode(tex, GPU_SAMPLER_EXTEND_MODE_REPEAT);
  GPU_texture_mipmap_mode(tex, true, true);
}

static void imb_gpu_texture_default_init_array(gpu::Texture *tex)
{
  const char *swizzle = (GPU_texture_component_len(GPU_texture_format(tex)) == 1) ? "rrra" :
                                                                                    "rgba";
  GPU_texture_swizzle_set(tex, swizzle);
  GPU_texture_anisotropic_filter(tex, true);
  GPU_texture_extend_mode(tex, GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER);
  GPU_texture_mipmap_mode(tex, true, true);
}

gpu::Texture *IMB_touch_gpu_texture(const char *name,
                                    ImBuf *ibuf,
                                    int w,
                                    int h,
                                    int layers,
                                    bool use_high_bitdepth,
                                    bool use_grayscale,
                                    bool writable)
{
  gpu::TextureFormat tex_format;
  imb_gpu_get_format(ibuf, use_high_bitdepth, use_grayscale, &tex_format);

  const eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ |
                                 GPU_texture_mipmap_usage(tex_format, writable);

  gpu::Texture *tex;
  if (layers > 0) {
    tex = GPU_texture_create_2d_array(name, w, h, layers, 9999, tex_format, usage, nullptr);
    imb_gpu_texture_default_init_array(tex);
  }
  else {
    tex = GPU_texture_create_2d(name, w, h, 9999, tex_format, usage, nullptr);
    imb_gpu_texture_default_init(tex, ibuf);
  }
  return tex;
}

void IMB_update_gpu_texture_sub(gpu::Texture *tex,
                                ImBuf *ibuf,
                                int x,
                                int y,
                                int z,
                                int w,
                                int h,
                                bool use_grayscale,
                                bool use_premult)
{
  const std::optional<int2> rescale_size = (ibuf->x != w || ibuf->y != h) ?
                                               std::optional<int2>(int2(w, h)) :
                                               std::nullopt;
  ImBuf *data = get_gpu_texture_data(ibuf, rescale_size, use_premult, use_grayscale);

  if (data->float_data()) {
    GPU_texture_update_sub(tex, GPU_DATA_FLOAT, data->float_data(), x, y, z, w, h, 1);
  }
  else {
    GPU_texture_update_sub(tex, GPU_DATA_UBYTE, data->byte_data(), x, y, z, w, h, 1);
  }
  IMB_freeImBuf(data);
}

gpu::Texture *IMB_create_gpu_texture(const char *name,
                                     ImBuf *ibuf,
                                     const GPUTextureCreateFlags flags)
{
  ibuf->gpu.lastused = BLI_time_now_seconds_i();

  const bool use_mipmap = flag_is_set(flags, GPUTextureCreateFlags::EnableMipmaps);

  gpu::Texture *tex = nullptr;
  int size[2] = {ibuf->x, ibuf->y};
  if (flag_is_set(flags, GPUTextureCreateFlags::LimitSize)) {
    size[0] = GPU_texture_size_with_limit(ibuf->x);
    size[1] = GPU_texture_size_with_limit(ibuf->y);
  }
  bool do_rescale = (ibuf->x != size[0]) || (ibuf->y != size[1]);

  /* Correct the smaller size to maintain the original aspect ratio of the image. */
  if (do_rescale && ibuf->x != ibuf->y) {
    if (size[0] > size[1]) {
      size[1] = int(ibuf->y * (float(size[0]) / ibuf->x));
    }
    else {
      size[0] = int(ibuf->x * (float(size[1]) / ibuf->y));
    }
  }

  /* Compressed textures can't be written to, so upload uncompressed when e.g. painting. */
  if (ibuf->ftype == IMB_FTYPE_DDS && !flag_is_set(flags, GPUTextureCreateFlags::Writable)) {
    gpu::TextureFormat compressed_format;
    if (!IMB_gpu_get_compressed_format(ibuf, &compressed_format)) {
      CLOG_WARN(&LOG,
                "DDS image '%s' is not in a supported GPU compression format",
                ibuf->filepath.c_str());
    }
    else if (do_rescale) {
      CLOG_WARN(
          &LOG, "DDS image '%s' can't use compressed due to size limit", ibuf->filepath.c_str());
    }
    else if (!is_power_of_2_i(ibuf->x) || !is_power_of_2_i(ibuf->y)) {
      /* We require POT DXT/S3TC texture sizes not because something in there
       * intrinsically needs it, but because we flip them upside down at
       * load time, and that (when mipmaps are involved) is only possible
       * with POT height. */
      CLOG_WARN(&LOG,
                "DDS image '%s' can't use compressed due to non power of two size",
                ibuf->filepath.c_str());
    }
    else {

      int mip_count = 0;
      uint8_t *compressed_data = imb_load_dds_compressed_data(
          ibuf->filepath.c_str(), ibuf->x, ibuf->y, mip_count);
      if (compressed_data != nullptr) {
        tex = GPU_texture_create_compressed_2d(name,
                                               ibuf->x,
                                               ibuf->y,
                                               use_mipmap ? mip_count : 1,
                                               compressed_format,
                                               GPU_TEXTURE_USAGE_GENERAL,
                                               compressed_data);
        MEM_delete(compressed_data);
        if (tex != nullptr) {
          return tex;
        }
        CLOG_WARN(&LOG,
                  "DDS image '%s' failed to create compressed GPU texture",
                  ibuf->filepath.c_str());
      }
      else {
        CLOG_WARN(&LOG, "DDS image '%s' failed to load data from file", ibuf->filepath.c_str());
      }
    }
    /* Fallback to uncompressed texture. */
    CLOG_WARN(&LOG, "DDS image '%s' falling back to uncompressed", ibuf->filepath.c_str());
  }

  gpu::TextureFormat tex_format;
  imb_gpu_get_format(
      ibuf, flag_is_set(flags, GPUTextureCreateFlags::HighBitDepth), true, &tex_format);

  /* Create Texture. Specify read usage to allow both shader and host reads, the latter is needed
   * by the GPU compositor. Mipmaps need additional usage flags. */
  eGPUTextureUsage usage = GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_HOST_READ;
  if (use_mipmap) {
    usage |= GPU_texture_mipmap_usage(tex_format,
                                      flag_is_set(flags, GPUTextureCreateFlags::Writable));
  }
  tex = GPU_texture_create_2d(
      name, UNPACK2(size), use_mipmap ? 9999 : 1, tex_format, usage, nullptr);
  if (tex == nullptr) {
    size[0] = max_ii(1, size[0] / 2);
    size[1] = max_ii(1, size[1] / 2);
    tex = GPU_texture_create_2d(
        name, UNPACK2(size), use_mipmap ? 9999 : 1, tex_format, usage, nullptr);
    do_rescale = true;
  }
  BLI_assert(tex != nullptr);
  const std::optional<int2> rescale_size = do_rescale ? std::optional<int2>(int2(size)) :
                                                        std::nullopt;
  ImBuf *data = get_gpu_texture_data(
      ibuf, rescale_size, flag_is_set(flags, GPUTextureCreateFlags::Premultiplied), true);
  if (data->float_data()) {
    GPU_texture_update(tex, GPU_DATA_FLOAT, data->float_data());
  }
  else {
    GPU_texture_update(tex, GPU_DATA_UBYTE, data->byte_data());
  }
  IMB_freeImBuf(data);

  imb_gpu_texture_default_init(tex, ibuf);

  return tex;
}

/* Number of modified chunks to track for mipmap. */
static blender::int2 imb_gpu_mipmap_modified_chunks_size(gpu::Texture *tex)
{
  return blender::int2(
      divide_ceil_u(GPU_texture_width(tex), GPU_TEXTURE_MIPMAP_UPDATE_CHUNK_SIZE),
      divide_ceil_u(GPU_texture_height(tex), GPU_TEXTURE_MIPMAP_UPDATE_CHUNK_SIZE));
}

/* For scaled images or UDIM atlases, set the modified chunk bits. */
static void imb_gpu_mipmap_modified_chunks_mark(blender::MutableBitSpan modified_chunks,
                                                blender::int2 size,
                                                const rcti &bounds)
{
  constexpr int chunk_size = GPU_TEXTURE_MIPMAP_UPDATE_CHUNK_SIZE;
  if (BLI_rcti_is_empty(&bounds)) {
    return;
  }
  const int tx_begin = std::max(bounds.xmin, 0) / chunk_size;
  const int ty_begin = std::max(bounds.ymin, 0) / chunk_size;
  const int tx_end = std::min((bounds.xmax - 1) / chunk_size, size.x - 1);
  const int ty_end = std::min((bounds.ymax - 1) / chunk_size, size.y - 1);
  for (int ty = ty_begin; ty <= ty_end; ty++) {
    for (int tx = tx_begin; tx <= tx_end; tx++) {
      modified_chunks[int64_t(ty) * size.x + tx].set();
    }
  }
}

static ImBuf *update_do_scale(const uchar *rect,
                              const float *rect_float,
                              int *x,
                              int *y,
                              int *w,
                              int *h,
                              int limit_w,
                              int limit_h,
                              int full_w,
                              int full_h)
{
  /* Partial update with scaling. */
  float xratio = limit_w / float(full_w);
  float yratio = limit_h / float(full_h);

  int part_w = *w, part_h = *h;

  /* Find sub coordinates in scaled image. Take ceiling because we will be
   * losing 1 pixel due to rounding errors in x,y. */
  *x *= xratio;
  *y *= yratio;
  *w = int(ceil(xratio * (*w)));
  *h = int(ceil(yratio * (*h)));

  /* ...but take back if we are over the limit! */
  if (*x + *w > limit_w) {
    (*w)--;
  }
  if (*y + *h > limit_h) {
    (*h)--;
  }

  /* Scale pixels. */
  ImBuf *ibuf = IMB_allocFromBuffer(rect, rect_float, part_w, part_h, 4);
  IMB_scale(ibuf, *w, *h, IMBScaleFilter::Box, false);

  return ibuf;
}

static void gpu_texture_update_scaled(gpu::Texture *tex,
                                      const uchar *rect,
                                      const float *rect_float,
                                      int full_w,
                                      int full_h,
                                      int x,
                                      int y,
                                      int layer,
                                      const int2 tile_offset,
                                      const int2 tile_size,
                                      int w,
                                      int h,
                                      const bool is_grayscale,
                                      rcti &r_bounds)
{
  ImBuf *ibuf;
  if (layer > -1) {
    ibuf = update_do_scale(
        rect, rect_float, &x, &y, &w, &h, tile_size.x, tile_size.y, full_w, full_h);

    /* Shift to account for tile packing. */
    x += tile_offset.x;
    y += tile_offset.y;
  }
  else {
    /* Partial update with scaling. */
    int limit_w = GPU_texture_width(tex);
    int limit_h = GPU_texture_height(tex);

    ibuf = update_do_scale(rect, rect_float, &x, &y, &w, &h, limit_w, limit_h, full_w, full_h);
  }

  if (is_grayscale) {
    if (ibuf->float_data()) {
      Array<float> gray(int64_t(w) * int64_t(h));
      imb_gpu_extract_first_channel(
          ibuf->float_data(), ibuf->channels, ibuf->x, w, h, gray.data());
      GPU_texture_update_sub(tex, GPU_DATA_FLOAT, gray.data(), x, y, math::max(layer, 0), w, h, 1);
    }
    else {
      Array<uchar> gray(int64_t(w) * int64_t(h));
      imb_gpu_extract_first_channel(ibuf->byte_data(), ibuf->channels, ibuf->x, w, h, gray.data());
      GPU_texture_update_sub(tex, GPU_DATA_UBYTE, gray.data(), x, y, math::max(layer, 0), w, h, 1);
    }
  }
  else {
    const void *data = ibuf->float_data() ? static_cast<const void *>(ibuf->float_data()) :
                                            static_cast<const void *>(ibuf->byte_data());
    eGPUDataFormat data_format = ibuf->float_data() ? GPU_DATA_FLOAT : GPU_DATA_UBYTE;

    GPU_texture_update_sub(tex, data_format, data, x, y, math::max(layer, 0), w, h, 1);
  }

  BLI_rcti_init(&r_bounds, x, x + w, y, y + h);

  IMB_freeImBuf(ibuf);
}

static void gpu_texture_update_unscaled(gpu::Texture *tex,
                                        uchar *rect,
                                        float *rect_float,
                                        int x,
                                        int y,
                                        int layer,
                                        const int2 tile_offset,
                                        int w,
                                        int h,
                                        int tex_stride,
                                        int tex_offset,
                                        int channels,
                                        const bool is_grayscale,
                                        rcti &r_bounds)
{
  if (layer > -1) {
    /* Shift to account for tile packing. */
    x += tile_offset.x;
    y += tile_offset.y;
  }

  if (is_grayscale) {
    if (rect_float) {
      Array<float> gray(int64_t(w) * int64_t(h));
      imb_gpu_extract_first_channel(
          rect_float + tex_offset, channels, tex_stride, w, h, gray.data());
      GPU_texture_update_sub(tex, GPU_DATA_FLOAT, gray.data(), x, y, math::max(layer, 0), w, h, 1);
    }
    else {
      Array<uchar> gray(int64_t(w) * int64_t(h));
      imb_gpu_extract_first_channel(rect + tex_offset, channels, tex_stride, w, h, gray.data());
      GPU_texture_update_sub(tex, GPU_DATA_UBYTE, gray.data(), x, y, math::max(layer, 0), w, h, 1);
    }
    return;
  }

  void *data = (rect_float) ? static_cast<void *>(rect_float + tex_offset) :
                              static_cast<void *>(rect + tex_offset);
  eGPUDataFormat data_format = (rect_float) ? GPU_DATA_FLOAT : GPU_DATA_UBYTE;

  /* Partial update without scaling. Stride and offset are used to copy only a
   * subset of a possible larger buffer than what we are updating. */

  GPU_texture_update_sub(tex, data_format, data, x, y, math::max(layer, 0), w, h, 1, tex_stride);

  BLI_rcti_init(&r_bounds, x, x + w, y, y + h);
}

static void imb_gpu_texture_update_region(gpu::Texture *tex,
                                          ImBuf *ibuf,
                                          const bool store_premultiplied,
                                          int x,
                                          int y,
                                          int w,
                                          int h,
                                          const int layer,
                                          const int2 tile_offset,
                                          const int2 tile_size,
                                          rcti &r_bounds)
{
  bool scaled;
  if (layer >= 0) {
    scaled = (ibuf->x != tile_size.x) || (ibuf->y != tile_size.y);
  }
  else {
    scaled = (GPU_texture_width(tex) != ibuf->x) || (GPU_texture_height(tex) != ibuf->y);
  }

  if (scaled) {
    /* Extra padding to account for bleed from neighboring pixels. */
    const int padding = 4;
    const int xmax = min_ii(x + w + padding, ibuf->x);
    const int ymax = min_ii(y + h + padding, ibuf->y);
    x = max_ii(x - padding, 0);
    y = max_ii(y - padding, 0);
    w = xmax - x;
    h = ymax - y;
  }

  const bool is_grayscale = GPU_texture_component_len(GPU_texture_format(tex)) == 1;

  /* Get texture data pointers. */
  float *rect_float = ibuf->float_data_for_write();
  uchar *rect = ibuf->byte_data_for_write();
  int tex_stride = ibuf->x;
  int tex_offset = ibuf->channels * (y * ibuf->x + x);
  int src_channels = ibuf->channels;

  if (rect_float) {
    /* Float image is already in scene linear colorspace or non-color data by
     * convention, no colorspace conversion needed. But we do require 4 channels
     * currently. */
    if (src_channels != 4 || scaled || !store_premultiplied) {
      rect_float = MEM_new_array_uninitialized<float>(4 * size_t(w) * size_t(h), __func__);
      if (rect_float == nullptr) {
        return;
      }

      tex_stride = w;
      tex_offset = 0;
      src_channels = 4;

      IMB_colormanagement_imbuf_to_float_texture(
          rect_float, x, y, w, h, ibuf, store_premultiplied);
    }
  }
  else {
    /* Byte image is in original colorspace from the file, and may need conversion. */
    if (IMB_colormanagement_space_is_data(ibuf->byte_buffer.colorspace) && !scaled) {
      /* Not scaled Non-color data, just store buffer as is. */
    }
    else if ((IMB_colormanagement_space_is_scene_linear_srgb(ibuf->byte_buffer.colorspace) &&
              !is_grayscale) ||
             IMB_colormanagement_space_is_scene_linear(ibuf->byte_buffer.colorspace) ||
             IMB_colormanagement_space_is_data(ibuf->byte_buffer.colorspace))
    {
      /* scene linear + sRGB transfer function or scene linear or scaled down non-color data,
       * store as byte texture that the GPU can decode directly. Grayscale sRGB is an exception
       * that uses a float texture (see #imb_gpu_get_format), so it takes the float path below. */
      rect = MEM_new_array_uninitialized<uchar>(4 * size_t(w) * size_t(h), __func__);
      if (rect == nullptr) {
        return;
      }

      tex_stride = w;
      tex_offset = 0;
      src_channels = 4;

      /* Convert to scene linear with sRGB compression, and premultiplied for
       * correct texture interpolation. */
      IMB_colormanagement_imbuf_to_byte_texture(rect, x, y, w, h, ibuf, store_premultiplied);
    }
    else {
      /* Other colorspace, store as float texture to avoid precision loss. */
      rect_float = MEM_new_array_uninitialized<float>(4 * size_t(w) * size_t(h), __func__);
      if (rect_float == nullptr) {
        return;
      }

      tex_stride = w;
      tex_offset = 0;
      src_channels = 4;

      IMB_colormanagement_imbuf_to_float_texture(
          rect_float, x, y, w, h, ibuf, store_premultiplied);
    }
  }

  if (scaled) {
    gpu_texture_update_scaled(tex,
                              rect,
                              rect_float,
                              ibuf->x,
                              ibuf->y,
                              x,
                              y,
                              layer,
                              tile_offset,
                              tile_size,
                              w,
                              h,
                              is_grayscale,
                              r_bounds);
  }
  else {
    gpu_texture_update_unscaled(tex,
                                rect,
                                rect_float,
                                x,
                                y,
                                layer,
                                tile_offset,
                                w,
                                h,
                                tex_stride,
                                tex_offset,
                                src_channels,
                                is_grayscale,
                                r_bounds);
  }

  /* Free buffers if needed. */
  if (rect && rect != ibuf->byte_data()) {
    MEM_delete(rect);
  }
  if (rect_float && rect_float != ibuf->float_data()) {
    MEM_delete(rect_float);
  }
}

void IMB_gpu_texture_apply_partial_update(gpu::Texture *tex,
                                          ImBuf *ibuf,
                                          const bool store_premultiplied,
                                          const imbuf::partial_update::Changes &changes,
                                          const int layer,
                                          const int2 tile_offset,
                                          const int2 tile_size)
{
  /* For simplicity we require partial update and mipmap chunk sizes to match. */
  static_assert(imbuf::partial_update::CHUNK_SIZE == GPU_TEXTURE_MIPMAP_UPDATE_CHUNK_SIZE);

  /* For scaled images and UDIM atlases, we need to recompute the chunks rather than
   * getting them directly from the changes. */
  const bool compute_chunks = (GPU_texture_width(tex) != ibuf->x) ||
                              (GPU_texture_height(tex) != ibuf->y) ||
                              (layer >= 0 &&
                               (tile_offset != int2(0, 0) || tile_size != int2(ibuf->x, ibuf->y)));
  BitVector<> modified_chunks;
  int2 modified_chunks_size;
  if (compute_chunks) {
    modified_chunks_size = imb_gpu_mipmap_modified_chunks_size(tex);
    modified_chunks.resize(int64_t(modified_chunks_size.x) * modified_chunks_size.y, false);
  }

  /* Update modified regions and gather modified chunks for mipmap update. */
  rcti buffer_rect;
  BLI_rcti_init(&buffer_rect, 0, ibuf->x, 0, ibuf->y);
  for (const rcti &region : changes.modified_regions()) {
    rcti clipped;
    if (!BLI_rcti_isect(&buffer_rect, &region, &clipped)) {
      continue;
    }
    rcti bounds;
    imb_gpu_texture_update_region(tex,
                                  ibuf,
                                  store_premultiplied,
                                  clipped.xmin,
                                  clipped.ymin,
                                  BLI_rcti_size_x(&clipped),
                                  BLI_rcti_size_y(&clipped),
                                  layer,
                                  tile_offset,
                                  tile_size,
                                  bounds);
    if (compute_chunks) {
      imb_gpu_mipmap_modified_chunks_mark(modified_chunks, modified_chunks_size, bounds);
    }
  }

  /* Partial mipmap update. */
  GPU_texture_update_mipmap_chain_partial(tex,
                                          math::max(layer, 0),
                                          compute_chunks ? BitSpan(modified_chunks) :
                                                           BitSpan(changes.modified_chunks));
  GPU_texture_unbind(tex);

  if (ibuf->gpu.texture == tex) {
    ibuf->gpu.flag |= IMB_GPU_MIPMAP_COMPLETE;
  }
}

static void imb_gpu_texture_make_writable(const char *name, ImBuf *ibuf)
{
  gpu::Texture *old_tex = ibuf->gpu.texture;
  BLI_assert(old_tex != nullptr);

  /* Compressed textures can't be written to, so fully recreate them uncompressed. */
  if (GPU_texture_has_compressed_format(old_tex)) {
    if (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr) {
      return;
    }
    GPU_texture_free(old_tex);
    ibuf->gpu.texture = nullptr;
    ibuf->gpu.flag &= IMB_GPU_WRITABLE;
    return;
  }

  /* Recreate texture, with GPU copy to avoid host roundtrip. */
  const int w = GPU_texture_width(old_tex);
  const int h = GPU_texture_height(old_tex);
  const int mip_count = GPU_texture_mip_count(old_tex);
  const gpu::TextureFormat format = GPU_texture_format(old_tex);
  const eGPUTextureUsage usage = GPU_texture_usage(old_tex) |
                                 GPU_texture_mipmap_usage(format, true);
  const bool is_array = GPU_texture_is_array(old_tex);

  gpu::Texture *new_tex =
      is_array ?
          GPU_texture_create_2d_array(
              name, w, h, GPU_texture_layer_count(old_tex), mip_count, format, usage, nullptr) :
          GPU_texture_create_2d(name, w, h, mip_count, format, usage, nullptr);
  if (new_tex == nullptr) {
    return;
  }

  /* GPU pixel copy from old to new texture, including all mipmap levels and layers. */
  GPU_texture_copy_mipmap_chain(new_tex, old_tex);

  /* Initialize matching texture settings. */
  if (is_array) {
    imb_gpu_texture_default_init_array(new_tex);
  }
  else {
    imb_gpu_texture_default_init(new_tex, ibuf);
    imb_gpu_texture_default_init_mipmap(new_tex);
  }

  GPU_texture_original_size_set(
      new_tex, GPU_texture_original_width(old_tex), GPU_texture_original_height(old_tex));

  GPU_texture_free(old_tex);
  ibuf->gpu.texture = new_tex;
}

static void imb_gpu_texture_apply_partial_updates(ImBuf *ibuf, const bool use_premult)
{
  if (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr) {
    return;
  }

  using imbuf::partial_update::Changes;
  IMB_partial_update_flush(ibuf);
  const int64_t new_changeset_id = IMB_partial_update_changeset_id_current();
  const Changes changes = IMB_partial_update_collect(ibuf, ibuf->gpu.partial_update_changeset);
  switch (changes.kind) {
    case Changes::Kind::Full:
    case Changes::Kind::Resized:
      GPU_texture_free(ibuf->gpu.texture);
      ibuf->gpu.texture = nullptr;
      ibuf->gpu.flag &= IMB_GPU_WRITABLE;
      break;
    case Changes::Kind::Partial:
      IMB_gpu_texture_apply_partial_update(
          ibuf->gpu.texture, ibuf, use_premult, changes, -1, int2(0), int2(0));
      ibuf->gpu.flag |= IMB_GPU_MIPMAP_COMPLETE;
      ibuf->gpu.partial_update_changeset = new_changeset_id;
      break;
    case Changes::Kind::None:
      ibuf->gpu.partial_update_changeset = new_changeset_id;
      break;
  }
}

gpu::Texture *IMB_acquire_gpu_texture(const char *name,
                                      ImBuf *ibuf,
                                      bool use_high_bitdepth,
                                      bool use_premult,
                                      bool limit_size,
                                      bool try_only)
{
  if (ibuf == nullptr || (ibuf->byte_data() == nullptr && ibuf->float_data() == nullptr &&
                          ibuf->gpu.texture == nullptr))
  {
    return nullptr;
  }

  std::scoped_lock lock(ibuf->gpu.mutex);
  if (ibuf->gpu.texture != nullptr) {
    /* Ensure the texture is writable when it needs to be. */
    if (ibuf->gpu.flag & IMB_GPU_WRITABLE) {
      gpu::Texture *tex = ibuf->gpu.texture;
      const eGPUTextureUsage mipmap_usage = GPU_texture_mipmap_usage(GPU_texture_format(tex),
                                                                     true);
      if ((GPU_texture_usage(tex) & mipmap_usage) != mipmap_usage) {
        imb_gpu_texture_make_writable(name, ibuf);
      }
    }
    if (ibuf->gpu.texture != nullptr) {
      imb_gpu_texture_apply_partial_updates(ibuf, use_premult);
    }
    if (ibuf->gpu.texture != nullptr) {
      ibuf->gpu.lastused = BLI_time_now_seconds_i();
      GPU_texture_ref(ibuf->gpu.texture);
      return ibuf->gpu.texture;
    }
  }
  if (try_only) {
    return nullptr;
  }

  const int64_t changeset_id = IMB_partial_update_changeset_id_next();

  GPUTextureCreateFlags create_flags = GPUTextureCreateFlags::EnableMipmaps;
  if (use_high_bitdepth) {
    create_flags |= GPUTextureCreateFlags::HighBitDepth;
  }
  if (use_premult) {
    create_flags |= GPUTextureCreateFlags::Premultiplied;
  }
  if (limit_size) {
    create_flags |= GPUTextureCreateFlags::LimitSize;
  }
  if (ibuf->gpu.flag & IMB_GPU_WRITABLE) {
    create_flags |= GPUTextureCreateFlags::Writable;
  }
  gpu::Texture *tex = IMB_create_gpu_texture(name, ibuf, create_flags);
  if (tex == nullptr) {
    ibuf->gpu.flag |= IMB_GPU_LOAD_FAILED;
    ibuf->gpu.lastused = BLI_time_now_seconds_i();
    return nullptr;
  }
  ibuf->gpu.flag &= ~IMB_GPU_LOAD_FAILED;

  GPU_texture_update_mipmap_chain(tex);
  imb_gpu_texture_default_init_mipmap(tex);
  ibuf->gpu.flag |= IMB_GPU_MIPMAP_COMPLETE;

  ibuf->gpu.partial_update_changeset = changeset_id;
  ibuf->gpu.texture = tex;
  ibuf->gpu.lastused = BLI_time_now_seconds_i();
  GPU_texture_ref(tex);
  return tex;
}

gpu::TextureFormat IMB_gpu_get_texture_format(const ImBuf *ibuf,
                                              bool high_bitdepth,
                                              bool use_grayscale)
{
  gpu::TextureFormat gpu_texture_format;
  imb_gpu_get_format(ibuf, high_bitdepth, use_grayscale, &gpu_texture_format);
  return gpu_texture_format;
}

void IMB_free_gpu_textures(ImBuf *ibuf)
{
  if (!ibuf) {
    return;
  }

  std::scoped_lock lock(ibuf->gpu.mutex);
  if (ibuf->gpu.texture) {
    GPU_texture_free(ibuf->gpu.texture);
    ibuf->gpu.texture = nullptr;
  }
  ibuf->gpu.flag &= IMB_GPU_WRITABLE;
}

void IMB_gpu_texture_ensure_writable(ImBuf *ibuf)
{
  /* Only set the flag here, #IMB_acquire_gpu_texture will take it into account. */
  std::scoped_lock lock(ibuf->gpu.mutex);
  ibuf->gpu.flag |= IMB_GPU_WRITABLE;
}

void IMB_assign_gpu_texture(ImBuf *ibuf, gpu::Texture *texture)
{
  if (!ibuf) {
    return;
  }

  std::scoped_lock lock(ibuf->gpu.mutex);
  if (ibuf->gpu.texture) {
    GPU_texture_free(ibuf->gpu.texture);
    ibuf->gpu.texture = nullptr;
  }
  /* If texture was already writable, we keep it that way. */
  ibuf->gpu.flag = (texture && (GPU_texture_usage(texture) & GPU_TEXTURE_USAGE_SHADER_WRITE)) ?
                       IMB_GPU_WRITABLE :
                       ImBufGPUFlag(0);
  ibuf->gpu.partial_update_changeset = IMB_partial_update_changeset_id_current();
  ibuf->gpu.texture = texture;
}

void IMB_gpu_clamp_half_float(ImBuf *image_buffer)
{
  const float half_min = -65504;
  const float half_max = 65504;
  if (!image_buffer->float_data()) {
    return;
  }

  float *rect_float = image_buffer->float_data_for_write();

  int rect_float_len = image_buffer->x * image_buffer->y *
                       (image_buffer->channels == 0 ? 4 : image_buffer->channels);

  for (int i = 0; i < rect_float_len; i++) {
    rect_float[i] = clamp_f(rect_float[i], half_min, half_max);
  }
}

}  // namespace blender
