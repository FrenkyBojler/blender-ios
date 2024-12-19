/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

struct ImageFormatData;
struct RenderData;

/** Global initialization of movie support. */
void IMB_movie_init();

/** Global de-initialization of movie support. */
void IMB_movie_exit();

/**
 * Test if the file is a video file (known format, has a video stream and
 * supported video codec). Note that this can be pretty expensive: it is
 * not just a file extension check, it will literally try to decode
 * file headers and find whether it is a video file with some supported
 * codec.
 */
bool IMB_is_movie_file(const char *filepath);

/** Checks whether given ffmpeg video AVCodecID supports alpha channel (RGBA). */
bool IMB_movie_codec_supports_alpha(int av_codec_id);

/** Checks whether given ffmpeg video AVCodecID supports CRF (i.e. "quality level")
 * setting. For codecs that do not support constant quality, only target bitrate
 * can be specified. */
bool IMB_movie_codec_supports_crf(int av_codec_id);

/**
 * Which pixel bit depths are supported by a given ffmpeg video AVCodecID.
 * Returns bitmask of `R_IMF_CHAN_DEPTH_` flags.
 */
int IMB_movie_codec_valid_bit_depths(int av_codec_id);

/**
 * Given desired output image format type, sets up required ffmpeg
 * related settings in render data.
 */
void IMB_movie_validate_output_settings(RenderData *rd, const ImageFormatData *imf);
