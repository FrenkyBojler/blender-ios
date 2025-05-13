/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "oiio/openimageio_support.hh"

#include "BLI_fileops.hh"

#include "IMB_filetype.hh"
#include "IMB_imbuf_types.hh"

OIIO_NAMESPACE_USING
using namespace blender::imbuf;

static const uchar jpeg_default_quality = 75;

bool imb_is_a_jpeg(const uchar *mem, const size_t size)
{
  return imb_oiio_check(mem, size, "jpeg");
}

ImBuf *imb_load_jpeg(const uchar *buffer, size_t size, int flags, ImFileColorSpace &r_colorspace)
{
  ImageSpec config, spec;

  /* Read JPEG comments as attributes. */
  config.attribute("jpeg:com_attributes", 0);

  ReadContext ctx{buffer, size, "jpeg", IMB_FTYPE_JPG, flags};

  ImBuf *ibuf = imb_oiio_read(ctx, config, r_colorspace, spec, "Blender:");
  if (ibuf) {
    ibuf->foptions.quality = jpeg_default_quality;
    std::string compression = spec.get_string_attribute("Compression:jpeg", "");
    if (!compression.empty()) {
      int compression_level = strtol(compression.c_str(), nullptr, 10);
      if (compression_level > 0 && compression_level <= 100) {
        ibuf->foptions.quality = compression_level;
      }
    }
  }

  return ibuf;
}

bool imb_savejpeg(ImBuf *ibuf, const char *filepath, int flags)
{
  const int file_channels = ibuf->planes >> 3;
  const TypeDesc data_format = TypeDesc::UINT8;

  WriteContext ctx = imb_create_write_context("jpeg", ibuf, flags, false);
  ImageSpec file_spec = imb_create_write_spec(ctx, file_channels, data_format, "Blender:");

  int quality = ibuf->foptions.quality;
  printf("%d\n", quality);
  if (quality <= 0) {
    quality = jpeg_default_quality;
  }
  quality = std::min(quality, 100);
  file_spec.attribute("Compression", "jpeg:" + std::to_string(quality));

  /* Write additional attributes (from metadata) as JPEG comments. */
  file_spec.attribute("jpeg:com_attributes", 1);

  return imb_oiio_write(ctx, filepath, file_spec);
}

/* Defines for JPEG Header markers and segment size. */
#define JPEG_MARKER_MSB (0xFF)
#define JPEG_MARKER_SOI (0xD8)
#define JPEG_MARKER_APP0 (0xE1)
#define JPEG_MARKER_APP1 (0xE1)
#define JPEG_MARKER_APPF (0xEF)
#define JPEG_MARKER_COMM (0xFE)

/* Look for APP1 marker.
 * If found, return the data length (infile is at the start of the data).
 * If not found, return zero (infile is at an arbitrary position in the file).
 */
static int jpeg_find_app1(FILE *infile)
{
  /* Ensure that this is a JPEG file (must start with SOI marker). */
  if (fgetc(infile) != JPEG_MARKER_MSB || fgetc(infile) != JPEG_MARKER_SOI) {
    return 0;
  }

  while (!feof(infile)) {
    /* Ensure we're at the start of a marker. */
    if (fgetc(infile) != JPEG_MARKER_MSB) {
      return 0;
    }
    char marker = fgetc(infile);
    if (marker == JPEG_MARKER_APP1) {
      /* Found the APP1 marker. */
      char high = fgetc(infile);
      char low = fgetc(infile);
      return int(high) * 256 + int(low);
    }
    else if ((marker >= JPEG_MARKER_APP0 && marker <= JPEG_MARKER_APPF) ||
             (marker == JPEG_MARKER_COMM))
    {
      /* Skip other APPx markers or comments. */
      char high = fgetc(infile);
      char low = fgetc(infile);
      fseek(infile, int(high) * 256 + int(low), SEEK_CUR);
    }
    else {
      /* Encountered other marker, give up. */
      return 0;
    }
  }
  return 0;
}

ImBuf *imb_load_filepath_thumbnail_jpeg(const char *filepath,
                                        const int flags,
                                        const size_t /*max_thumb_size*/,
                                        ImFileColorSpace &r_colorspace,
                                        size_t * /*r_width*/,
                                        size_t * /*r_height*/)
{
  ImBuf *ibuf = nullptr;
  FILE *infile = BLI_fopen(filepath, "rb");
  if (infile == nullptr) {
    fprintf(stderr, "can't open %s\n", filepath);
    return nullptr;
  }

  /* Check if the file contains an APP1 marker. */
  int app1_length = jpeg_find_app1(infile);
  if (app1_length == 0) {
    fclose(infile);
    return nullptr;
  }

  /* Load the APP1 marker into memory. */
  uchar *app1 = MEM_calloc_arrayN<uchar>(app1_length, "thumbbuffer");
  if (fread(app1, app1_length, 1, infile) == 1) {
    /* Instead of parsing EXIF here, just look for an SOI marker, which we assume
     * belongs to a packed JPEG-formatted thumbnail. */
    for (int i = 0; i < app1_length - 1; i++) {
      if (app1[i] == JPEG_MARKER_MSB && app1[i + 1] == JPEG_MARKER_SOI) {
        ibuf = imb_load_jpeg(app1 + i, app1_length - i, flags, r_colorspace);
        break;
      }
    }
  }

  MEM_SAFE_FREE(app1);
  fclose(infile);
  return ibuf;
}
