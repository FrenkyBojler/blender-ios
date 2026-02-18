/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * SVG vector graphics format support for the purpose of thumbnail-display.
 * While loading these as an #ImBuf is trivial to support, it would expose
 * limitations of NANOSVG and users may end up needing more advanced options
 * specific to loading vector graphics (such as resolution control), see #109567 for details.
 */

#include "IMB_colormanagement.hh"
#include "IMB_filetype.hh"
#include "IMB_imbuf_types.hh"
#include "thorvg.h"

namespace blender {

ImBuf *imb_load_filepath_thumbnail_svg(const char *filepath,
                                       const int /*flags*/,
                                       const size_t max_thumb_size,
                                       ImFileColorSpace & /*r_colorspace*/,
                                       size_t *r_width,
                                       size_t *r_height)
{
  /* Create a Picture and parse the SVG into it. */
  tvg::Picture *picture = tvg::Picture::gen();

  if (picture->load(filepath) != tvg::Result::Success) {
    return nullptr;
  }

  float width;
  float height;
  picture->size(&width, &height);

  if (width == 0 || height == 0) {
    return nullptr;
  }

  const float scale = float(max_thumb_size) / std::max(width, height);
  const int dest_w = std::max(int(width * scale), 1);
  const int dest_h = std::max(int(height * scale), 1);
  ImBuf *ibuf = IMB_allocImBuf(dest_w, dest_h, 32, IB_byte_data);

  picture->scale(scale);

  if (ibuf != nullptr) {

    /* Create a canvas that will draw to our bitmap. */
    tvg::SwCanvas *canvas = tvg::SwCanvas::gen();
    uint32_t *bitmap_rgba_uint32 = reinterpret_cast<uint32_t *>(ibuf->byte_buffer.data);
    canvas->target(bitmap_rgba_uint32, dest_w, dest_w, dest_h, tvg::ColorSpace::ABGR8888);

    /* Push the SVG image to the canvas. */
    canvas->push(picture);
    /* Release the paint object. */
    tvg::Paint::rel(picture);

    /* Draw to the bitmap. */
    canvas->draw(true);
    canvas->sync();

    IMB_flipy(ibuf);

    /* Return full size of the image. */
    *r_width = size_t(width);
    *r_height = size_t(height);
  }

  return ibuf;
}

}  // namespace blender
