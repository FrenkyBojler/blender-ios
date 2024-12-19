/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * Movie file reading / playback functions.
 */

#pragma once

#include "IMB_imbuf_enums.h"
#include "IMB_movie_enums.hh"

struct ImBuf;
struct MoviePlayback;
struct IndexBuildContext;
struct GSet;

/**
 * Opens a movie file for reading / playback.
 * ib_flags are `IB_` ImBuf bitmask (only IB_animdeinterlace is taken into account).
 * streamindex is for multi-track movie files.
 *
 * Returned MoviePlayback object can be used in other playback related functions.
 * Note that a valid object will be returned even if file does not exist or is
 * not a video file. The actual initialization is delayed until
 * #MOV_decode_frame is called.
 *
 * When done with playback, use #MOV_close to delete it.
 */
MoviePlayback *MOV_open_file(const char *filepath,
                             int ib_flags,
                             int streamindex,
                             char colorspace[IM_MAX_SPACE]);

/**
 * Release memory and other resources associated with movie playback.
 */
void MOV_close(MoviePlayback *anim);

/**
 * Fetches a frame from a movie at given frame position.
 *
 * Internally this will seek within the movie as/if needed. For most movie
 * files, decoding frames sequentially is much more efficient than decoding
 * random frames.
 *
 * If proxy_size is not IMB_PROXY_NONE, a proxy file of given size will
 * be attempted. If it exists, the frame will be decoded from it. If the
 * proxy does not exist, original file will be used.
 *
 * Movies that are <= 8 bits/color channel are returned as byte images;
 * higher bit depth movies are returned as float images. Note that the
 * color space is returned as-is, i.e. a float image might not be in
 * linear space.
 *
 * Returned image can be null if movie file does not exist, is not supported
 * or failed decoding.
 */
ImBuf *MOV_decode_frame(MoviePlayback *anim,
                        int position,
                        IMB_Timecode_Type tc /* = 1 = IMB_TC_RECORD_RUN */,
                        IMB_Proxy_Size preview_size /* = 0 = IMB_PROXY_NONE */);

/**
 * Fetches a frame from a movie used for preview/thumbnails.
 * The frame will be halfway into the file duration.
 * Thumbnail related metadata ("Thumb::Video::*") will be set on the
 * returned image.
 */
ImBuf *MOV_decode_preview_frame(MoviePlayback *anim);

/**
 * Return the length (in frames) of the movie.
 */
int MOV_get_duration_frames(MoviePlayback *anim, IMB_Timecode_Type tc);

/**
 * Return the encoded start offset (in seconds) of the movie.
 */
double MOV_get_start_offset_seconds(const MoviePlayback *anim);

/**
 * Returns the frames per second of the movie, or zero if
 * the information is not available. Note that if you want the
 * most accurate representation, use #MOV_get_fps_num_denom.
 */
float MOV_get_fps(const MoviePlayback *anim);

/**
 * Returns the frames per second of the movie as numerator and
 * denominator. False will be returned if the information is
 * not available.
 */
bool MOV_get_fps_num_denom(const MoviePlayback *anim, short &r_fps_num, float &r_fps_denom);

/**
 * Get movie image width in pixels.
 */
int MOV_get_image_width(const MoviePlayback *anim);

/**
 * Get movie image height in pixels.
 */
int MOV_get_image_height(const MoviePlayback *anim);

/**
 * Returns true if movie playback has been fully initialized
 * and is supported. Note that immediately after #MOV_open_file
 * the playback is not initialized yet.
 */
bool MOV_is_initialized_and_valid(const MoviePlayback *anim);

/**
 * Gets filename (without the folder) part of the movie.
 */
void MOV_get_filename(const MoviePlayback *anim, char *filename, int filename_maxncpy);

/**
 * Sets multi-view suffix to be used when building proxies for this movie.
 */
void MOV_set_multiview_suffix(MoviePlayback *anim, const char *suffix);

/**
 * Close any internally opened proxies of this movie.
 */
void MOV_close_proxies(MoviePlayback *anim);

/**
 * Defaults to BL_proxy within the directory of the animation.
 */
void IMB_anim_set_index_dir(MoviePlayback *anim, const char *dir);

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
