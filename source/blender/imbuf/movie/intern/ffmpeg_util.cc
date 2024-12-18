/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "ffmpeg_util.hh"
#include "swscale.hh"

#ifdef WITH_FFMPEG

#  include "BLI_path_utils.hh"
#  include "BLI_string.h"

#  include "BKE_global.hh"

extern "C" {
#  include "ffmpeg_compat.h"
#  include <libavcodec/avcodec.h>
#  include <libavdevice/avdevice.h>
#  include <libavformat/avformat.h>
#  include <libavutil/log.h>
}

static char ffmpeg_last_error_buffer[1024];

/* BLI_vsnprintf in ffmpeg_log_callback() causes invalid warning */
#  ifdef __GNUC__
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wmissing-format-attribute"
#  endif

static void ffmpeg_log_callback(void *ptr, int level, const char *format, va_list arg)
{
  if (ELEM(level, AV_LOG_FATAL, AV_LOG_ERROR)) {
    size_t n;
    va_list args_cpy;

    va_copy(args_cpy, arg);
    n = VSNPRINTF(ffmpeg_last_error_buffer, format, args_cpy);
    va_end(args_cpy);

    /* strip trailing \n */
    ffmpeg_last_error_buffer[n - 1] = '\0';
  }

  if (G.debug & G_DEBUG_FFMPEG) {
    /* call default logger to print all message to console */
    av_log_default_callback(ptr, level, format, arg);
  }
}

#  ifdef __GNUC__
#    pragma GCC diagnostic pop
#  endif

const char *ffmpeg_last_error()
{
  return ffmpeg_last_error_buffer;
}

void IMB_ffmpeg_init()
{
  avdevice_register_all();

  ffmpeg_last_error_buffer[0] = '\0';

  if (G.debug & G_DEBUG_FFMPEG) {
    av_log_set_level(AV_LOG_DEBUG);
  }

  /* set separate callback which could store last error to report to UI */
  av_log_set_callback(ffmpeg_log_callback);
}

void IMB_ffmpeg_exit()
{
  ffmpeg_sws_exit();
}

static int isffmpeg(const char *filepath)
{
  AVFormatContext *pFormatCtx = nullptr;
  uint i;
  int videoStream;
  const AVCodec *pCodec;

  if (BLI_path_extension_check_n(filepath,
                                 ".swf",
                                 ".jpg",
                                 ".jp2",
                                 ".j2c",
                                 ".png",
                                 ".dds",
                                 ".tga",
                                 ".bmp",
                                 ".tif",
                                 ".exr",
                                 ".cin",
                                 ".wav",
                                 nullptr))
  {
    return 0;
  }

  if (avformat_open_input(&pFormatCtx, filepath, nullptr, nullptr) != 0) {
    return 0;
  }

  if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
    avformat_close_input(&pFormatCtx);
    return 0;
  }

  /* Find the first video stream */
  videoStream = -1;
  for (i = 0; i < pFormatCtx->nb_streams; i++) {
    if (pFormatCtx->streams[i] && pFormatCtx->streams[i]->codecpar &&
        (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
    {
      videoStream = i;
      break;
    }
  }

  if (videoStream == -1) {
    avformat_close_input(&pFormatCtx);
    return 0;
  }

  AVCodecParameters *codec_par = pFormatCtx->streams[videoStream]->codecpar;

  /* Find the decoder for the video stream */
  pCodec = avcodec_find_decoder(codec_par->codec_id);
  if (pCodec == nullptr) {
    avformat_close_input(&pFormatCtx);
    return 0;
  }

  avformat_close_input(&pFormatCtx);

  return 1;
}
#endif /* WITH_FFMPEG */

bool IMB_isanim(const char *filepath)
{
  BLI_assert(!BLI_path_is_rel(filepath));

#ifdef WITH_FFMPEG
  if (isffmpeg(filepath)) {
    return true;
  }
#endif

  return false;
}

int IMB_ffmpeg_valid_bit_depths(int av_codec_id)
{
  int bit_depths = R_IMF_CHAN_DEPTH_8;
  /* Note: update properties_output.py `use_bpp` when changing this function. */
  if (ELEM(av_codec_id, AV_CODEC_ID_H264, AV_CODEC_ID_H265, AV_CODEC_ID_AV1)) {
    bit_depths |= R_IMF_CHAN_DEPTH_10;
  }
  if (ELEM(av_codec_id, AV_CODEC_ID_H265, AV_CODEC_ID_AV1)) {
    bit_depths |= R_IMF_CHAN_DEPTH_12;
  }
  return bit_depths;
}
