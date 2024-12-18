/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * Functions for writing movie files.
 * \ingroup imbuf
 */

#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"

#include "BLI_utildefines.h"

#include "BKE_report.hh"

#ifdef WITH_FFMPEG
#  include "writeffmpeg.hh"
#endif

#include "movie/IMB_writemovie.hh"

static bool is_imtype_ffmpeg(const char imtype)
{
  return ELEM(imtype,
              R_IMF_IMTYPE_AVIRAW,
              R_IMF_IMTYPE_AVIJPEG,
              R_IMF_IMTYPE_FFMPEG,
              R_IMF_IMTYPE_H264,
              R_IMF_IMTYPE_XVID,
              R_IMF_IMTYPE_THEORA,
              R_IMF_IMTYPE_AV1);
}

ImbMovieWriter *IMB_movie_write_begin(const char imtype,
                                      const Scene *scene,
                                      RenderData *rd,
                                      int rectx,
                                      int recty,
                                      ReportList *reports,
                                      bool preview,
                                      const char *suffix)
{
  if (!is_imtype_ffmpeg(imtype)) {
    return nullptr;
  }

  ImbMovieWriter *writer = nullptr;
#ifdef WITH_FFMPEG
  writer = ffmpeg_movie_open(scene, rd, rectx, recty, reports, preview, suffix);
#endif
  return writer;
}

bool IMB_movie_write_append(ImbMovieWriter *writer,
                            RenderData *rd,
                            int start_frame,
                            int frame,
                            const ImBuf *image,
                            const char *suffix,
                            ReportList *reports)
{
  if (writer == nullptr) {
    return false;
  }

#ifdef WITH_FFMPEG
  bool ok = ffmpeg_movie_append(writer, rd, start_frame, frame, image, suffix, reports);
  return ok;
#else
  return false;
#endif
}

void IMB_movie_write_end(ImbMovieWriter *writer)
{
#ifdef WITH_FFMPEG
  if (writer) {
    ffmpeg_movie_close(writer);
  }
#endif
}

void IMB_movie_filepath_get(char filepath[/*FILE_MAX*/ 1024],
                            const RenderData *rd,
                            bool preview,
                            const char *suffix)
{
#ifdef WITH_FFMPEG
  if (is_imtype_ffmpeg(rd->im_format.imtype)) {
    ffmpeg_get_filepath(filepath, rd, preview, suffix);
    return;
  }
#endif
  filepath[0] = '\0';
}
