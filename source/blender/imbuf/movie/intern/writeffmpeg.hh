/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup imbuf
 */

#ifdef WITH_FFMPEG

struct ImageFormatData;
struct ImbMovieWriter;
struct ImBuf;
struct RenderData;
struct ReportList;
struct Scene;

ImbMovieWriter *ffmpeg_movie_open(const Scene *scene,
                                  RenderData *rd,
                                  int rectx,
                                  int recty,
                                  ReportList *reports,
                                  bool preview,
                                  const char *suffix);

void ffmpeg_movie_close(ImbMovieWriter *context);

bool ffmpeg_movie_append(ImbMovieWriter *context,
                         RenderData *rd,
                         int start_frame,
                         int frame,
                         const ImBuf *image,
                         const char *suffix,
                         ReportList *reports);

void ffmpeg_get_filepath(char filepath[/*FILE_MAX*/ 1024],
                         const RenderData *rd,
                         bool preview,
                         const char *suffix);

#endif
