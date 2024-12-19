/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#pragma once

struct ImageFormatData;
struct RenderData;

/**
 * Test if the file is a video file (known format, has a video stream and
 * supported video codec).
 */
bool IMB_isanim(const char *filepath);

void IMB_ffmpeg_init();
void IMB_ffmpeg_exit();

bool IMB_ffmpeg_alpha_channel_is_supported(int av_codec_id);
bool IMB_ffmpeg_codec_supports_crf(int av_codec_id);
void IMB_ffmpeg_image_type_verify(RenderData *rd, const ImageFormatData *imf);

/**
 * Which pixel bit depths are supported by a given video codec.
 * Returns bitmask of `R_IMF_CHAN_DEPTH_` flags.
 */
int IMB_ffmpeg_valid_bit_depths(int av_codec_id);
