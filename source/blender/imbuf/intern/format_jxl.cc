/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "oiio/openimageio_support.hh"

#include "IMB_filetype.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

namespace blender {

const char *imb_file_extensions_jxl[] = {".jxl", nullptr};

OIIO_NAMESPACE_USING
using namespace imbuf;

bool imb_is_a_jxl(const unsigned char *mem, size_t size)
{
  return imb_oiio_check(mem, size, "jxl");
}

ImBuf *imb_load_jxl(const unsigned char *mem,
                    size_t size,
                    int flags,
                    ImFileColorSpace &r_colorspace)
{
  ImageSpec config, spec;
  ReadContext ctx{mem, size, "jxl", IMB_FTYPE_JXL, flags};
  return imb_oiio_read(ctx, config, r_colorspace, spec);
}

bool imb_save_jxl(ImBuf *ibuf, const char *filepath, int flags)
{
  const int file_channels = ibuf->planes >> 3;
  TypeDesc data_format = TypeDesc::UINT8;
  int bits_per_sample = 8;

  if (ibuf->foptions.flag & JXL_10BIT) {
    data_format = TypeDesc::UINT16;
    bits_per_sample = 10;
  }
  else if (ibuf->foptions.flag & JXL_12BIT) {
    data_format = TypeDesc::UINT16;
    bits_per_sample = 12;
  }
  else if (ibuf->foptions.flag & JXL_16BIT) {
    data_format = TypeDesc::UINT16;
    bits_per_sample = 16;
  }
  else if (ibuf->foptions.flag & JXL_32BIT) {
    data_format = TypeDesc::FLOAT;
    bits_per_sample = 32;
  }

  WriteContext ctx = imb_create_write_context("jxl", ibuf, flags);
  if (!ctx.out) {
    return false;
  }

  ImageSpec file_spec = imb_create_write_spec(ctx, file_channels, data_format);

  file_spec.attribute("CompressionQuality", (int)ibuf->foptions.quality);
  file_spec.attribute("oiio:BitsPerSample", bits_per_sample);

  return imb_oiio_write(ctx, filepath, file_spec);
}

}  // namespace blender
