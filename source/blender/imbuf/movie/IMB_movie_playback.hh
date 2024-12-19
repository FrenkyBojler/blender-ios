/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

#include "IMB_imbuf_enums.h"
#include "IMB_movie_enums.hh"

struct ImBuf;
struct MoviePlayback;
struct IndexBuildContext;
struct GSet;

MoviePlayback *IMB_open_anim(const char *filepath,
                             int ib_flags,
                             int streamindex,
                             char colorspace[IM_MAX_SPACE]);

void IMB_free_anim(MoviePlayback *anim);

/**
 * Defaults to BL_proxy within the directory of the animation.
 */
void IMB_anim_set_index_dir(MoviePlayback *anim, const char *dir);
void IMB_anim_get_filename(MoviePlayback *anim, char *filename, int filename_maxncpy);

int IMB_anim_index_get_frame_index(MoviePlayback *anim, IMB_Timecode_Type tc, int position);

int IMB_anim_proxy_get_existing(MoviePlayback *anim);

/**
 * Prepare context for proxies/time-codes builder
 */
IndexBuildContext *IMB_anim_index_rebuild_context(MoviePlayback *anim,
                                                  IMB_Timecode_Type tcs_in_use,
                                                  int proxy_sizes_in_use,
                                                  int quality,
                                                  const bool overwrite,
                                                  GSet *file_list,
                                                  bool build_only_on_bad_performance);

/**
 * Will rebuild all used indices and proxies at once.
 */
void IMB_anim_index_rebuild(IndexBuildContext *context,
                            bool *stop,
                            bool *do_update,
                            float *progress);

/**
 * Finish rebuilding proxies/time-codes and free temporary contexts used.
 */
void IMB_anim_index_rebuild_finish(IndexBuildContext *context, bool stop);

/**
 * Return the length (in frames) of the given \a anim.
 */
int IMB_anim_get_duration(MoviePlayback *anim, IMB_Timecode_Type tc);

/**
 * Return the encoded start offset (in seconds) of the given \a anim.
 */
double IMB_anim_get_offset(MoviePlayback *anim);

/**
 * Return the fps contained in movie files (function rval is false,
 * and frs_sec and frs_sec_base untouched if none available!)
 */
bool IMB_anim_get_fps(const MoviePlayback *anim,
                      bool no_av_base,
                      short *r_frs_sec,
                      float *r_frs_sec_base);

void IMB_suffix_anim(MoviePlayback *anim, const char *suffix);
void IMB_close_anim(MoviePlayback *anim);
void IMB_close_anim_proxies(MoviePlayback *anim);
bool IMB_anim_can_produce_frames(const MoviePlayback *anim);

int IMB_anim_get_image_width(MoviePlayback *anim);
int IMB_anim_get_image_height(MoviePlayback *anim);

/**
 * Fetches a frame from a movie at given frame position.
 *
 * Movies that are <= 8 bits/color channel are returned as byte images;
 * higher bit depth movies are returned as float images. Note that the
 * color space is returned as-is, i.e. a float image might not be in
 * linear space.
 */
ImBuf *IMB_anim_absolute(MoviePlayback *anim,
                         int position,
                         IMB_Timecode_Type tc /* = 1 = IMB_TC_RECORD_RUN */,
                         IMB_Proxy_Size preview_size /* = 0 = IMB_PROXY_NONE */);

/**
 * fetches a define preview-frame, usually half way into the movie.
 */
ImBuf *IMB_anim_previewframe(MoviePlayback *anim);
