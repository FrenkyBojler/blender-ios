/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include <cstddef>
#include <cstring>

#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "CLG_log.h"

#include <ktx.h>

namespace blender {

static CLG_LogRef LOG = {"image.ktx"};

/* -------------------------------------------------------------------- */
/** \name KTX2 Detection and Validation
 * \{ */

static constexpr unsigned char ktx2_magic[12] = {
    0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

bool imb_is_a_ktx(const unsigned char *mem, size_t size)
{
  /* Guard against short buffers before checking the magic header. */
  if (mem == nullptr || size < sizeof(ktx2_magic)) {
    return false;
  }
  return std::memcmp(mem, ktx2_magic, sizeof(ktx2_magic)) == 0;
}

static bool ktx2_is_supported_2d(const ktxTexture2 *tex)
{
  /* Blender currently imports only 2D, single-layer, single-face textures. */
  if (tex->numDimensions != 2) {
    return false;
  }
  if (tex->baseWidth == 0 || tex->baseHeight == 0) {
    return false;
  }
  if (tex->baseDepth != 1 || tex->numLayers != 1 || tex->numFaces != 1) {
    return false;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name KTX2 Loader
 * \{ */

ImBuf *imb_load_ktx(const unsigned char *mem,
                    size_t size,
                    int flags,
                    ImFileColorSpace &r_colorspace)
{
  /* Fast reject for non-KTX data. */
  if (!imb_is_a_ktx(mem, size)) {
    return nullptr;
  }

  ktxTexture2 *tex = nullptr;
  /* Skip image data loading for `IB_test`, but still parse the container. */
  const ktxTextureCreateFlags create_flags = (flags & IB_test) ?
                                                 KTX_TEXTURE_CREATE_NO_FLAGS :
                                                 KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT;

  const KTX_error_code ktx_err = ktxTexture2_CreateFromMemory(
      mem, size, create_flags | KTX_TEXTURE_CREATE_CHECK_GLTF_BASISU_BIT, &tex);
  if (ktx_err != KTX_SUCCESS || tex == nullptr) {
    CLOG_ERROR(&LOG, "KTX2: failed to parse texture (%d)", int(ktx_err));
    return nullptr;
  }

  /* Enforce a layout that ImBuf can represent without additional packing. */
  if (!ktx2_is_supported_2d(tex)) {
    CLOG_ERROR(&LOG, "KTX2: unsupported texture layout");
    ktxTexture2_Destroy(tex);
    return nullptr;
  }

  const int width = int(tex->baseWidth);
  const int height = int(tex->baseHeight);

  if (flags & IB_test) {
    /* Return a dummy buffer with correct dimensions for format probing. */
    ImBuf *ibuf = IMB_allocImBuf(width, height, 32, 0);
    ktxTexture2_Destroy(tex);
    return ibuf;
  }

  if (ktxTexture2_NeedsTranscoding(tex)) {
    /* KTX2 files containing BasisU need to be transcoded into RGBA8. */
    const ktx_error_code_e transcode_err = ktxTexture2_TranscodeBasis(tex, KTX_TTF_RGBA32, 0);
    if (transcode_err != KTX_SUCCESS) {
      CLOG_ERROR(&LOG, "KTX2: transcode failed (%d)", int(transcode_err));
      ktxTexture2_Destroy(tex);
      return nullptr;
    }
  }
  else if (tex->isCompressed) {
    /* Compressed but not BasisU: no supported CPU decode path here. */
    CLOG_ERROR(&LOG, "KTX2: compressed texture without BasisU transcoding");
    ktxTexture2_Destroy(tex);
    return nullptr;
  }
  else {
    /* Expect uncompressed RGBA8 to map directly into ImBuf byte storage. */
    ktx_uint32_t num_components = 0;
    ktx_uint32_t component_size = 0;
    ktxTexture2_GetComponentInfo(tex, &num_components, &component_size);
    if (num_components != 4 || component_size != 1) {
      CLOG_ERROR(&LOG, "KTX2: unsupported component layout");
      ktxTexture2_Destroy(tex);
      return nullptr;
    }
  }

  /* Resolve image data and the first mip/face/layer offset. */
  const ktx_uint8_t *data = ktxTexture_GetData(ktxTexture(tex));
  if (data == nullptr) {
    CLOG_ERROR(&LOG, "KTX2: image data missing");
    ktxTexture2_Destroy(tex);
    return nullptr;
  }

  ktx_size_t offset = 0;
  if (ktxTexture_GetImageOffset(ktxTexture(tex), 0, 0, 0, &offset) != KTX_SUCCESS) {
    CLOG_ERROR(&LOG, "KTX2: failed to get image offset");
    ktxTexture2_Destroy(tex);
    return nullptr;
  }

  const ktx_uint32_t row_pitch = ktxTexture_GetRowPitch(ktxTexture(tex), 0);
  const size_t dst_stride = size_t(width) * 4;
  if (row_pitch < dst_stride) {
    CLOG_ERROR(&LOG, "KTX2: invalid row pitch");
    ktxTexture2_Destroy(tex);
    return nullptr;
  }

  ImBuf *ibuf = IMB_allocImBuf(width, height, 32, IB_byte_data);
  if (ibuf == nullptr) {
    ktxTexture2_Destroy(tex);
    return nullptr;
  }

  const ktx_uint8_t *src = data + offset;
  unsigned char *dst = ibuf->byte_buffer.data;

  /* Apply KTX orientation metadata to match Blender's expected layout. */
  const bool flip_y = (tex->orientation.y == KTX_ORIENT_Y_DOWN);
  const bool flip_x = (tex->orientation.x == KTX_ORIENT_X_LEFT);

  /* Copy row by row, flipping as needed to keep a contiguous RGBA8 buffer. */
  for (int y = 0; y < height; y++) {
    const int dst_y = flip_y ? (height - 1 - y) : y;
    const unsigned char *src_row = src + size_t(y) * row_pitch;
    unsigned char *dst_row = dst + size_t(dst_y) * dst_stride;

    if (!flip_x) {
      std::memcpy(dst_row, src_row, dst_stride);
      continue;
    }

    for (int x = 0; x < width; x++) {
      const unsigned char *src_px = src_row + size_t(width - 1 - x) * 4;
      unsigned char *dst_px = dst_row + size_t(x) * 4;
      std::memcpy(dst_px, src_px, 4);
    }
  }

  /* KTX2 loader currently outputs byte-based RGBA, not float HDR data. */
  r_colorspace.is_hdr_float = false;

  ktxTexture2_Destroy(tex);
  return ibuf;
}

/** \} */

}  // namespace blender
