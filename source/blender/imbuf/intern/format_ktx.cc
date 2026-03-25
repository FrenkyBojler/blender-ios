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

ImBuf *imb_load_ktx(const unsigned char *mem,
                    size_t size,
                    int flags,
                    ImFileColorSpace &r_colorspace)
{
  ktxTexture2 *texture = nullptr;
  KTX_error_code result = ktxTexture2_CreateFromMemory(
      mem, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture);
  if (result != KTX_SUCCESS) {
    CLOG_ERROR(&LOG, "Failed to load KTX2 texture: %s", ktxErrorString(result));
    return nullptr;
  }

  if (texture->numDimensions != 2) {
    CLOG_ERROR(&LOG, "KTX2: only 2D textures are supported (got %uD)", texture->numDimensions);
    ktxTexture_Destroy(ktxTexture(texture));
    return nullptr;
  }

  const int width = int(texture->baseWidth);
  const int height = int(texture->baseHeight);

  /* Save VkFormat before transcoding — it may change after TranscodeBasis. */
  const uint32_t vk_format = texture->vkFormat;
  const bool is_srgb = (vk_format == VK_FORMAT_R8G8B8_SRGB ||
                        vk_format == VK_FORMAT_R8G8B8A8_SRGB);

  /* Transcode Basis Universal to RGBA32 (uncompressed, 4 bytes/pixel). */
  if (ktxTexture_NeedsTranscoding(ktxTexture(texture))) {
    result = ktxTexture2_TranscodeBasis(texture, KTX_TTF_RGBA32, 0);
    if (result != KTX_SUCCESS) {
      CLOG_ERROR(&LOG, "Failed to transcode KTX2 texture: %s", ktxErrorString(result));
      ktxTexture_Destroy(ktxTexture(texture));
      return nullptr;
    }
  }

  ImBuf *ibuf = IMB_allocImBuf(width, height, 32, IB_byte_data);
  if (ibuf == nullptr) {
    CLOG_ERROR(&LOG, "Failed to allocate ImBuf (%dx%d)", width, height);
    ktxTexture_Destroy(ktxTexture(texture));
    return nullptr;
  }

  if (flags & IB_test) {
    /* Metadata-only load — dimensions are set, no pixel copy needed. */
    ktxTexture_Destroy(ktxTexture(texture));
    return ibuf;
  }

  /* Copy level 0 pixels into the ImBuf byte buffer. */
  ktx_size_t offset = 0;
  ktxTexture_GetImageOffset(ktxTexture(texture), 0, 0, 0, &offset);
  memcpy(ibuf->byte_buffer.data, texture->pData + offset, size_t(width) * height * 4);

  /* KTXorientation=rd means top-left origin — flip to Blender's bottom-left convention. */
  ktx_uint32_t orientation_len = 0;
  void *orientation_val = nullptr;
  if (ktxHashList_FindValue(&texture->kvDataHead,
                            KTX_ORIENTATION_KEY,
                            &orientation_len,
                            &orientation_val) == KTX_SUCCESS)
  {
    if (orientation_val && strncmp(static_cast<const char *>(orientation_val), "rd", 2) == 0) {
      IMB_flipy(ibuf);
    }
  }

  /* Set colorspace: sRGB formats are standard color, UNORM formats are linear data. */
  if (!is_srgb) {
    ibuf->colormanage_flag |= IMB_COLORMANAGE_IS_DATA;
  }

  ibuf->ftype = IMB_FTYPE_KTX;
  ibuf->planes = 32;

  r_colorspace.is_hdr_float = false;

  ktxTexture_Destroy(ktxTexture(texture));
  return ibuf;
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

  /* Strict dimension check: KHR_texture_basisu requires multiples of 4 for glTF compliance. */
  if ((ibuf->foptions.flag & KTX2_STRICT_DIM) &&
      ((ibuf->x % 4 != 0) || (ibuf->y % 4 != 0)))
  {
    CLOG_ERROR(&LOG,
               "KTX2: dimensions must be multiples of 4 for glTF compliance (%dx%d)",
               ibuf->x,
               ibuf->y);
    return false;
  }

  /* Determine channel count from planes (24 = RGB, 32 = RGBA). */
  const int channels = ibuf->planes >> 3;
  const bool is_data = (ibuf->colormanage_flag & IMB_COLORMANAGE_IS_DATA) != 0;

  /* TODO: R and RG channel formats (VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM) are not
   * implemented. In the glTF pipeline, single-channel data (occlusion, roughness, metallic)
   * is always packed into RGB textures (ORM) by the exporter, so R/RG are not needed. */

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

  /* glTF compliance: flip vertically (Blender = bottom-left, KTX2/glTF = top-left "rd"). */
  const bool gltf_compat = (ibuf->foptions.flag & KTX2_ORIENTATION_RD) != 0;
  std::vector<uint8_t> flipped;
  if (gltf_compat) {
    const int row_size = ibuf->x * channels;
    flipped.resize(ktx_size_t(ibuf->x) * ktx_size_t(ibuf->y) * channels);
    for (int y = 0; y < ibuf->y; y++) {
      memcpy(flipped.data() + y * row_size,
             pixel_data + (ibuf->y - 1 - y) * row_size,
             row_size);
    }
    pixel_data = flipped.data();
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

  /* Write KTXorientation=rd for glTF compliance (top-left origin). */
  if (gltf_compat) {
    ktxHashList_AddKVPair(&texture->kvDataHead, KTX_ORIENTATION_KEY, sizeof("rd"), "rd");
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
