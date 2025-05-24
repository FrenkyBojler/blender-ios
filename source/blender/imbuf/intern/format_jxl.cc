/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "oiio/openimageio_support.hh"

#include "IMB_filetype.hh"

#include "IMB_imbuf_types.hh"

OIIO_NAMESPACE_USING
using namespace blender::imbuf;

bool imb_is_a_jxl(const unsigned char *mem, size_t size)
{
  return imb_oiio_check(mem, size, "jxl");
}

ImBuf *imb_load_jxl(const unsigned char *mem, size_t size, int flags, ImFileColorSpace &r_colorspace)
{
  ImageSpec config, spec;

  ReadContext ctx{mem, size, "jxl", IMB_FTYPE_JXL, flags};

  return imb_oiio_read(ctx, config, r_colorspace, spec);
}

bool imb_save_jxl(ImBuf *ibuf, const char *filepath, int flags)
{
  const int file_channels = ibuf->planes >> 3;
  const TypeDesc data_format = TypeDesc::FLOAT;

  WriteContext ctx = imb_create_write_context("jxl", ibuf, flags);
  ImageSpec file_spec = imb_create_write_spec(ctx, file_channels, data_format);

  return imb_oiio_write(ctx, filepath, file_spec);
}
