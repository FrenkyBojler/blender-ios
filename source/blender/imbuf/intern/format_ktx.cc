/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

#include "BLI_utildefines.h"

#include "MEM_guardedalloc.h"

#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "CLG_log.h"

#include "ktx.h"

namespace blender {

/* KTX2 magic identifier: 12 bytes */
static const unsigned char ktx2_magic[] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

static CLG_LogRef LOG = {"image.ktx"};

const char *imb_file_extensions_ktx[] = {".ktx2", nullptr};

/* VkFormat values used here (from Vulkan spec).
 * Not including vulkan headers to avoid dependency. */
#define VK_FORMAT_R8G8B8_UNORM 23
#define VK_FORMAT_R8G8B8_SRGB 29
#define VK_FORMAT_R8G8B8A8_UNORM 37
#define VK_FORMAT_R8G8B8A8_SRGB 43

bool imb_is_a_ktx(const unsigned char *mem, size_t size)
{
  if (size < sizeof(ktx2_magic)) {
    return false;
  }
  return memcmp(mem, ktx2_magic, sizeof(ktx2_magic)) == 0;
}

ImBuf *imb_load_ktx(const unsigned char * /*mem*/,
                    size_t /*size*/,
                    int /*flags*/,
                    ImFileColorSpace & /*r_colorspace*/)
{
  /* TODO: implement KTX loading */
  return nullptr;
}

bool imb_save_ktx(ImBuf *ibuf, const char *filepath, int /*flags*/)
{
  /* Ensure a byte buffer is available, transcoding from float if needed. */
  if (ibuf->byte_buffer.data == nullptr) {
    if (ibuf->float_buffer.data == nullptr) {
      CLOG_ERROR(&LOG, "No image data to save");
      return false;
    }
    IMB_byte_from_float(ibuf);
    if (ibuf->byte_buffer.data == nullptr) {
      CLOG_ERROR(&LOG, "Failed to convert float buffer to byte buffer");
      return false;
    }
  }

  /* Determine channel count from planes (24 = RGB, 32 = RGBA). */
  const int channels = ibuf->planes >> 3;
  const bool is_data = (ibuf->colormanage_flag & IMB_COLORMANAGE_IS_DATA) != 0;

  /* Select VkFormat based on channel count and colorspace.
   * Blender byte_buffer is always RGBA, so RGB requires packing. */
  uint32_t vk_format;
  if (channels == 3) {
    vk_format = is_data ? VK_FORMAT_R8G8B8_UNORM : VK_FORMAT_R8G8B8_SRGB;
  }
  else {
    vk_format = is_data ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB;
  }

  /* Pack RGB if needed: Blender always stores 4 bytes per pixel, strip alpha. */
  std::vector<uint8_t> packed_rgb;
  const uint8_t *pixel_data = ibuf->byte_buffer.data;
  if (channels == 3) {
    const int pixel_count = ibuf->x * ibuf->y;
    packed_rgb.resize(pixel_count * 3);
    const uint8_t *src = ibuf->byte_buffer.data;
    uint8_t *dst = packed_rgb.data();
    for (int i = 0; i < pixel_count; i++, src += 4, dst += 3) {
      dst[0] = src[0];
      dst[1] = src[1];
      dst[2] = src[2];
    }
    pixel_data = packed_rgb.data();
  }

  ktxTextureCreateInfo create_info = {};
  create_info.vkFormat = vk_format;
  create_info.baseWidth = uint32_t(ibuf->x);
  create_info.baseHeight = uint32_t(ibuf->y);
  create_info.baseDepth = 1;
  create_info.numDimensions = 2;
  create_info.numLevels = 1;
  create_info.numLayers = 1;
  create_info.numFaces = 1;
  create_info.isArray = KTX_FALSE;
  create_info.generateMipmaps = KTX_FALSE;

  ktxTexture2 *texture = nullptr;
  KTX_error_code result = ktxTexture2_Create(&create_info,
                                             KTX_TEXTURE_CREATE_ALLOC_STORAGE,
                                             &texture);
  if (result != KTX_SUCCESS) {
    CLOG_ERROR(&LOG, "Failed to create KTX2 texture: %s", ktxErrorString(result));
    return false;
  }

  const ktx_size_t image_size = ktx_size_t(ibuf->x) * ktx_size_t(ibuf->y) * channels;
  result = ktxTexture_SetImageFromMemory(ktxTexture(texture),
                                        /*level*/ 0,
                                        /*layer*/ 0,
                                        /*faceSlice*/ 0,
                                        pixel_data,
                                        image_size);
  if (result != KTX_SUCCESS) {
    CLOG_ERROR(&LOG, "Failed to set KTX2 image data: %s", ktxErrorString(result));
    ktxTexture_Destroy(ktxTexture(texture));
    return false;
  }

  const bool use_uastc = (ibuf->foptions.flag & KTX2_UASTC) != 0;

  ktxBasisParams params = {};
  params.structSize = sizeof(params);
  params.threadCount = uint32_t(std::max(1u, std::thread::hardware_concurrency()));

  if (use_uastc) {
    /* UASTC: higher quality than ETC1S, larger output.
     * Map foptions.quality [0, 100] to UASTC levels [0, 4]. */
    params.uastc = KTX_TRUE;
    const int level = int(float(ibuf->foptions.quality) / 100.0f *
                              float(KTX_PACK_UASTC_MAX_LEVEL) +
                          0.5f);
    params.uastcFlags = ktx_pack_uastc_flags(
        std::clamp(level, 0, int(KTX_PACK_UASTC_MAX_LEVEL)));
  }
  else {
    /* ETC1S: smaller output, lower quality than UASTC.
     * Map foptions.quality [0, 100] to qualityLevel [1, 255]. */
    params.compressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL;
    params.qualityLevel = uint32_t(
        std::clamp(int(float(ibuf->foptions.quality) / 100.0f * 254.0f + 1.5f), 1, 255));
  }

  result = ktxTexture2_CompressBasisEx(texture, &params);
  if (result != KTX_SUCCESS) {
    CLOG_ERROR(&LOG, "Failed to compress KTX2 texture: %s", ktxErrorString(result));
    ktxTexture_Destroy(ktxTexture(texture));
    return false;
  }

  if (use_uastc && ibuf->foptions.compress > 0) {
    /* Apply Zstandard supercompression on top of UASTC.
     * foptions.compress is a percentage [0, 100]: 0 disables Zstd,
     * 1-100 maps to Zstd levels [1, 22]. */
    const ktx_uint32_t zstd_level = ktx_uint32_t(
        std::clamp(int(float(ibuf->foptions.compress) / 100.0f * 22.0f + 0.5f), 1, 22));
    result = ktxTexture2_DeflateZstd(texture, zstd_level);
    if (result != KTX_SUCCESS) {
      CLOG_ERROR(&LOG, "Failed to apply Zstandard compression: %s", ktxErrorString(result));
      ktxTexture_Destroy(ktxTexture(texture));
      return false;
    }
  }

  result = ktxTexture_WriteToNamedFile(ktxTexture(texture), filepath);
  ktxTexture_Destroy(ktxTexture(texture));

  if (result != KTX_SUCCESS) {
    CLOG_ERROR(&LOG, "Failed to write KTX2 file '%s': %s", filepath, ktxErrorString(result));
    return false;
  }

  return true;
}

}  // namespace blender
