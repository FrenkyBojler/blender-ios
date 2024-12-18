/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "movie/IMB_movie_enums.hh"

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

void IMB_ffmpeg_init()
{
#ifdef WITH_FFMPEG
  avdevice_register_all();

  ffmpeg_last_error_buffer[0] = '\0';

  if (G.debug & G_DEBUG_FFMPEG) {
    av_log_set_level(AV_LOG_DEBUG);
  }

  /* set separate callback which could store last error to report to UI */
  av_log_set_callback(ffmpeg_log_callback);
#endif
}

void IMB_ffmpeg_exit()
{
#ifdef WITH_FFMPEG
  ffmpeg_sws_exit();
#endif
}

int IMB_ffmpeg_valid_bit_depths(int av_codec_id)
{
  int bit_depths = R_IMF_CHAN_DEPTH_8;
#ifdef WITH_FFMPEG
  /* Note: update properties_output.py `use_bpp` when changing this function. */
  if (ELEM(av_codec_id, AV_CODEC_ID_H264, AV_CODEC_ID_H265, AV_CODEC_ID_AV1)) {
    bit_depths |= R_IMF_CHAN_DEPTH_10;
  }
  if (ELEM(av_codec_id, AV_CODEC_ID_H265, AV_CODEC_ID_AV1)) {
    bit_depths |= R_IMF_CHAN_DEPTH_12;
  }
#endif
  return bit_depths;
}

#ifdef WITH_FFMPEG
static void ffmpeg_preset_set(RenderData *rd, int preset)
{
  bool is_ntsc = (rd->frs_sec != 25);

  switch (preset) {
    case FFMPEG_PRESET_H264:
      rd->ffcodecdata.type = FFMPEG_AVI;
      rd->ffcodecdata.codec = AV_CODEC_ID_H264;
      rd->ffcodecdata.video_bitrate = 6000;
      rd->ffcodecdata.gop_size = is_ntsc ? 18 : 15;
      rd->ffcodecdata.rc_max_rate = 9000;
      rd->ffcodecdata.rc_min_rate = 0;
      rd->ffcodecdata.rc_buffer_size = 224 * 8;
      rd->ffcodecdata.mux_packet_size = 2048;
      rd->ffcodecdata.mux_rate = 10080000;
      break;

    case FFMPEG_PRESET_THEORA:
    case FFMPEG_PRESET_XVID:
      if (preset == FFMPEG_PRESET_XVID) {
        rd->ffcodecdata.type = FFMPEG_AVI;
        rd->ffcodecdata.codec = AV_CODEC_ID_MPEG4;
      }
      else if (preset == FFMPEG_PRESET_THEORA) {
        rd->ffcodecdata.type = FFMPEG_OGG; /* XXX broken */
        rd->ffcodecdata.codec = AV_CODEC_ID_THEORA;
      }

      rd->ffcodecdata.video_bitrate = 6000;
      rd->ffcodecdata.gop_size = is_ntsc ? 18 : 15;
      rd->ffcodecdata.rc_max_rate = 9000;
      rd->ffcodecdata.rc_min_rate = 0;
      rd->ffcodecdata.rc_buffer_size = 224 * 8;
      rd->ffcodecdata.mux_packet_size = 2048;
      rd->ffcodecdata.mux_rate = 10080000;
      break;

    case FFMPEG_PRESET_AV1:
      rd->ffcodecdata.type = FFMPEG_AV1;
      rd->ffcodecdata.codec = AV_CODEC_ID_AV1;
      rd->ffcodecdata.video_bitrate = 6000;
      rd->ffcodecdata.gop_size = is_ntsc ? 18 : 15;
      rd->ffcodecdata.rc_max_rate = 9000;
      rd->ffcodecdata.rc_min_rate = 0;
      rd->ffcodecdata.rc_buffer_size = 224 * 8;
      rd->ffcodecdata.mux_packet_size = 2048;
      rd->ffcodecdata.mux_rate = 10080000;
      break;
  }
}
#endif

void IMB_ffmpeg_image_type_verify(RenderData *rd, const ImageFormatData *imf)
{
#ifdef WITH_FFMPEG
  int audio = 0;

  if (imf->imtype == R_IMF_IMTYPE_FFMPEG) {
    if (rd->ffcodecdata.type <= 0 || rd->ffcodecdata.codec <= 0 ||
        rd->ffcodecdata.audio_codec <= 0 || rd->ffcodecdata.video_bitrate <= 1)
    {
      ffmpeg_preset_set(rd, FFMPEG_PRESET_H264);
      rd->ffcodecdata.constant_rate_factor = FFM_CRF_MEDIUM;
      rd->ffcodecdata.ffmpeg_preset = FFM_PRESET_GOOD;
      rd->ffcodecdata.type = FFMPEG_MKV;
    }
    if (rd->ffcodecdata.type == FFMPEG_OGG) {
      rd->ffcodecdata.type = FFMPEG_MPEG2;
    }

    audio = 1;
  }
  else if (imf->imtype == R_IMF_IMTYPE_H264) {
    if (rd->ffcodecdata.codec != AV_CODEC_ID_H264) {
      ffmpeg_preset_set(rd, FFMPEG_PRESET_H264);
      audio = 1;
    }
  }
  else if (imf->imtype == R_IMF_IMTYPE_XVID) {
    if (rd->ffcodecdata.codec != AV_CODEC_ID_MPEG4) {
      ffmpeg_preset_set(rd, FFMPEG_PRESET_XVID);
      audio = 1;
    }
  }
  else if (imf->imtype == R_IMF_IMTYPE_THEORA) {
    if (rd->ffcodecdata.codec != AV_CODEC_ID_THEORA) {
      ffmpeg_preset_set(rd, FFMPEG_PRESET_THEORA);
      audio = 1;
    }
  }
  else if (imf->imtype == R_IMF_IMTYPE_AV1) {
    if (rd->ffcodecdata.codec != AV_CODEC_ID_AV1) {
      ffmpeg_preset_set(rd, FFMPEG_PRESET_AV1);
      audio = 1;
    }
  }

  if (audio && rd->ffcodecdata.audio_codec < 0) {
    rd->ffcodecdata.audio_codec = AV_CODEC_ID_NONE;
    rd->ffcodecdata.audio_bitrate = 128;
  }
#endif
}

bool IMB_ffmpeg_alpha_channel_is_supported(int av_codec_id)
{
#if WITH_FFMPEG
  return ELEM(av_codec_id,
              AV_CODEC_ID_FFV1,
              AV_CODEC_ID_QTRLE,
              AV_CODEC_ID_PNG,
              AV_CODEC_ID_VP9,
              AV_CODEC_ID_HUFFYUV);
#else
  return false;
#endif
}

bool IMB_ffmpeg_codec_supports_crf(int av_codec_id)
{
#if WITH_FFMPEG
  return ELEM(av_codec_id,
              AV_CODEC_ID_H264,
              AV_CODEC_ID_H265,
              AV_CODEC_ID_MPEG4,
              AV_CODEC_ID_VP9,
              AV_CODEC_ID_AV1);
#else
  return false;
#endif
}
