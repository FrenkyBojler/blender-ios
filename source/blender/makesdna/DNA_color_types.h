/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_defs.h"
#include "DNA_vec_types.h"

/* general defines for kernel functions */
#define CM_RESOL 32
#define CM_TABLE 256
#define CM_TABLEDIV (1.0f / 256.0f)

#define CM_TOT 4

#define GPU_SKY_WIDTH 512
#define GPU_SKY_HEIGHT 128

typedef struct CurveMapPoint {
  float x, y;
  /** Shorty for result lookup. */
  short flag, shorty;
} CurveMapPoint;

/** #CurveMapPoint.flag */
enum {
  CUMA_SELECT = (1 << 0),
  CUMA_HANDLE_VECTOR = (1 << 1),
  CUMA_HANDLE_AUTO_ANIM = (1 << 2),
  /** Temporary tag for point deletion. */
  CUMA_REMOVE = (1 << 3),
};

typedef struct CurveMap {
  short totpoint;
  short flag DNA_DEPRECATED;

  /** Quick multiply value for reading table. */
  float range;
  /** The x-axis range for the table. */
  float mintable, maxtable;
  /** For extrapolated curves, the direction vector. */
  float ext_in[2], ext_out[2];
  /** Actual curve. */
  CurveMapPoint *curve;
  /** Display and evaluate table. */
  CurveMapPoint *table;

  /** For RGB curves, pre-multiplied table. */
  CurveMapPoint *premultable;
  /** For RGB curves, pre-multiplied extrapolation vector. */
  float premul_ext_in[2];
  float premul_ext_out[2];
  short default_handle_type;
  char _pad[6];
} CurveMap;

typedef struct CurveMapping {
  /** Cur; for buttons, to show active curve. */
  int flag, cur;
  int preset;
  int changed_timestamp;

  /** Current rect, clip rect (is default rect too). */
  rctf curr, clipr;

  /** Max 4 builtin curves per mapping struct now. */
  CurveMap cm[4];
  /** Black/white point (black[0] abused for current frame). */
  float black[3], white[3];
  /** Black/white point multiply value, for speed. */
  float bwmul[3];

  /** Sample values, if flag set it draws line and intersection. */
  float sample[3];

  short tone;
  char _pad[6];
} CurveMapping;

/** #CurveMapping.flag */
typedef enum eCurveMappingFlags {
  CUMA_DO_CLIP = (1 << 0),
  CUMA_PREMULLED = (1 << 1),
  CUMA_DRAW_CFRA = (1 << 2),
  CUMA_DRAW_SAMPLE = (1 << 3),

  /** The curve is extended by extrapolation. When not set the curve is extended horizontally. */
  CUMA_EXTEND_EXTRAPOLATE = (1 << 4),
  CUMA_USE_WRAPPING = (1 << 5),
} eCurveMappingFlags;

/** #CurveMapping.preset */
typedef enum eCurveMappingPreset {
  CURVE_PRESET_LINE = 0,
  CURVE_PRESET_SHARP = 1,
  CURVE_PRESET_SMOOTH = 2,
  CURVE_PRESET_MAX = 3,
  CURVE_PRESET_MID8 = 4,
  CURVE_PRESET_ROUND = 5,
  CURVE_PRESET_ROOT = 6,
  CURVE_PRESET_GAUSS = 7,
  CURVE_PRESET_BELL = 8,
  CURVE_PRESET_CONSTANT_MEDIAN = 9,
} eCurveMappingPreset;

/** #CurveMapping.tone */
typedef enum eCurveMappingTone {
  CURVE_TONE_STANDARD = 0,
  CURVE_TONE_FILMLIKE = 2,
} eCurveMappingTone;

/** #Histogram.mode */
enum {
  HISTO_MODE_LUMA = 0,
  HISTO_MODE_RGB = 1,
  HISTO_MODE_R = 2,
  HISTO_MODE_G = 3,
  HISTO_MODE_B = 4,
  HISTO_MODE_ALPHA = 5,
};

enum {
  HISTO_FLAG_LINE = (1 << 0),
  HISTO_FLAG_SAMPLELINE = (1 << 1),
};

typedef struct Histogram {
  int channels;
  int x_resolution;
  float data_luma[256];
  float data_r[256];
  float data_g[256];
  float data_b[256];
  float data_a[256];
  float xmax, ymax;
  short mode;
  short flag;
  int height;

  /** Sample line only (image coords: source -> destination). */
  float co[2][2];
} Histogram;

/* Multiplier to map YUV U,V range (+-0.436, +-0.615) to +-0.5 on both axes. */
#define SCOPES_VEC_U_SCALE float(0.5f / 0.436f)
#define SCOPES_VEC_V_SCALE float(0.5f / 0.615f)

typedef struct Scopes {
  int ok;
  int sample_full;
  int sample_lines;
  int wavefrm_mode;
  int vecscope_mode;
  int wavefrm_height;
  int vecscope_height;
  int waveform_tot;
  float accuracy;
  float wavefrm_alpha;
  float wavefrm_yfac;
  float vecscope_alpha;
  float minmax[3][2];
  struct Histogram hist;
  float *waveform_1;
  float *waveform_2;
  float *waveform_3;
  float *vecscope;
  float *vecscope_rgb;
} Scopes;

/** #Scopes.wavefrm_mode */
enum {
  SCOPES_WAVEFRM_LUMA = 0,
  SCOPES_WAVEFRM_RGB_PARADE = 1,
  SCOPES_WAVEFRM_YCC_601 = 2,
  SCOPES_WAVEFRM_YCC_709 = 3,
  SCOPES_WAVEFRM_YCC_JPEG = 4,
  SCOPES_WAVEFRM_RGB = 5,
};

/** #Scopes.vecscope_mode */
enum {
  SCOPES_VECSCOPE_RGB = 0,
  SCOPES_VECSCOPE_LUMA = 1,
};

typedef struct ColorManagedViewSettings {
  int flag;
  char _pad[4];
  /** Look which is being applied when displaying buffer on the screen
   * (prior to view transform). */
  char look[64];
  /** View transform which is being applied when displaying buffer on the screen. */
  char view_transform[64];
  /** F-stop exposure. */
  float exposure;
  /** Post-display gamma transform. */
  float gamma;
  /** White balance parameters. */
  float temperature;
  float tint;
  /** Pre-display RGB curves transform. */
  struct CurveMapping *curve_mapping;
  void *_pad2;
} ColorManagedViewSettings;

typedef struct ColorManagedDisplaySettings {
  char display_device[64];
} ColorManagedDisplaySettings;

typedef struct ColorManagedColorspaceSettings {
  /** MAX_COLORSPACE_NAME. */
  char name[64];
} ColorManagedColorspaceSettings;

/* -------------------------------------------------------------------- */
/** \name Multi-View
 * \{ */

/** View (Multi-view). */
typedef struct SceneRenderView {
  struct SceneRenderView *next, *prev;

  /** MAX_NAME. */
  char name[64];
  /** MAX_NAME. */
  char suffix[64];

  int viewflag;
  char _pad2[4];

} SceneRenderView;

/** #SceneRenderView::viewflag */
enum {
  SCE_VIEW_DISABLE = 1 << 0,
};

/** #RenderData::views_format */
enum {
  SCE_VIEWS_FORMAT_STEREO_3D = 0,
  SCE_VIEWS_FORMAT_MULTIVIEW = 1,
};

/** #ImageFormatData::views_format (also used for #Strip::views_format). */
enum {
  R_IMF_VIEWS_INDIVIDUAL = 0,
  R_IMF_VIEWS_STEREO_3D = 1,
  R_IMF_VIEWS_MULTIVIEW = 2,
};

typedef struct Stereo3dFormat {
  short flag;
  /** Encoding mode. */
  char display_mode;
  /** Anaglyph scheme for the user display. */
  char anaglyph_type;
  /** Interlace type for the user display. */
  char interlace_type;
  char _pad[3];
} Stereo3dFormat;

/** #Stereo3dFormat::display_mode */
typedef enum eStereoDisplayMode {
  S3D_DISPLAY_ANAGLYPH = 0,
  S3D_DISPLAY_INTERLACE = 1,
  S3D_DISPLAY_PAGEFLIP = 2,
  S3D_DISPLAY_SIDEBYSIDE = 3,
  S3D_DISPLAY_TOPBOTTOM = 4,
} eStereoDisplayMode;

/** #Stereo3dFormat::flag */
typedef enum eStereo3dFlag {
  S3D_INTERLACE_SWAP = (1 << 0),
  S3D_SIDEBYSIDE_CROSSEYED = (1 << 1),
  S3D_SQUEEZED_FRAME = (1 << 2),
} eStereo3dFlag;

/** #Stereo3dFormat::anaglyph_type */
typedef enum eStereo3dAnaglyphType {
  S3D_ANAGLYPH_REDCYAN = 0,
  S3D_ANAGLYPH_GREENMAGENTA = 1,
  S3D_ANAGLYPH_YELLOWBLUE = 2,
} eStereo3dAnaglyphType;

/** #Stereo3dFormat::interlace_type */
typedef enum eStereo3dInterlaceType {
  S3D_INTERLACE_ROW = 0,
  S3D_INTERLACE_COLUMN = 1,
  S3D_INTERLACE_CHECKERBOARD = 2,
} eStereo3dInterlaceType;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image Format Data
 * \{ */

/**
 * Generic image format settings,
 * this is used for #NodeImageFile and IMAGE_OT_save_as operator too.
 *
 * NOTE: its a bit strange that even though this is an image format struct
 * the imtype can still be used to select video formats.
 * RNA ensures these enum's are only selectable for render output.
 */
typedef struct ImageFormatData {
  /**
   * R_IMF_IMTYPE_PNG, R_...
   * \note Video types should only ever be set from this structure when used from #RenderData.
   */
  char imtype;
  /**
   * bits per channel, R_IMF_CHAN_DEPTH_8 -> 32,
   * not a flag, only set 1 at a time. */
  char depth;

  /** R_IMF_PLANES_BW, R_IMF_PLANES_RGB, R_IMF_PLANES_RGBA. */
  char planes;
  /** Generic options for all image types, alpha Z-buffer. */
  char flag;

  /** (0 - 100), eg: JPEG quality. */
  char quality;
  /** (0 - 100), eg: PNG compression. */
  char compress;

  /* --- format specific --- */

  /** OpenEXR: R_IMF_EXR_CODEC_* values in low OPENEXR_CODEC_MASK bits. */
  char exr_codec;

  /** CINEON. */
  char cineon_flag;
  short cineon_white, cineon_black;
  float cineon_gamma;

  /** Jpeg2000. */
  char jp2_flag;
  char jp2_codec;

  /** TIFF. */
  char tiff_codec;

  char _pad[4];

  /** Multi-view. */
  char views_format;
  Stereo3dFormat stereo3d_format;

  /* Color management members. */

  char color_management;
  char _pad1[7];
  ColorManagedViewSettings view_settings;
  ColorManagedDisplaySettings display_settings;
  ColorManagedColorspaceSettings linear_colorspace_settings;
} ImageFormatData;

/** #ImageFormatData::imtype */
enum {
  R_IMF_IMTYPE_TARGA = 0,
  R_IMF_IMTYPE_IRIS = 1,
  // R_HAMX = 2,  /* DEPRECATED */
  // R_FTYPE = 3, /* DEPRECATED */
  R_IMF_IMTYPE_JPEG90 = 4,
  // R_MOVIE = 5, /* DEPRECATED */
  R_IMF_IMTYPE_IRIZ = 7,
  R_IMF_IMTYPE_RAWTGA = 14,
  R_IMF_IMTYPE_AVIRAW = 15,
  R_IMF_IMTYPE_AVIJPEG = 16,
  R_IMF_IMTYPE_PNG = 17,
  // R_IMF_IMTYPE_AVICODEC = 18,  /* DEPRECATED */
  // R_IMF_IMTYPE_QUICKTIME = 19, /* DEPRECATED */
  R_IMF_IMTYPE_BMP = 20,
  R_IMF_IMTYPE_RADHDR = 21,
  R_IMF_IMTYPE_TIFF = 22,
  R_IMF_IMTYPE_OPENEXR = 23,
  R_IMF_IMTYPE_FFMPEG = 24,
  // R_IMF_IMTYPE_FRAMESERVER = 25, /* DEPRECATED */
  R_IMF_IMTYPE_CINEON = 26,
  R_IMF_IMTYPE_DPX = 27,
  R_IMF_IMTYPE_MULTILAYER = 28,
  R_IMF_IMTYPE_DDS = 29,
  R_IMF_IMTYPE_JP2 = 30,
  R_IMF_IMTYPE_H264 = 31,
  R_IMF_IMTYPE_XVID = 32,
  R_IMF_IMTYPE_THEORA = 33,
  R_IMF_IMTYPE_PSD = 34,
  R_IMF_IMTYPE_WEBP = 35,
  R_IMF_IMTYPE_AV1 = 36,

  R_IMF_IMTYPE_INVALID = 255,
};

/** #ImageFormatData::flag */
enum {
  // R_IMF_FLAG_ZBUF = 1 << 0, /* DEPRECATED, and cleared. */
  R_IMF_FLAG_PREVIEW_JPG = 1 << 1,
};

/*  */

/**
 * #ImageFormatData::depth
 *
 * Return values from #BKE_imtype_valid_depths, note this is depths per channel.
 */
typedef enum eImageFormatDepth {
  /** 1bits  (unused). */
  R_IMF_CHAN_DEPTH_1 = (1 << 0),
  /** 8bits  (default). */
  R_IMF_CHAN_DEPTH_8 = (1 << 1),
  /** 10bits (uncommon, Cineon/DPX support). */
  R_IMF_CHAN_DEPTH_10 = (1 << 2),
  /** 12bits (uncommon, jp2/DPX support). */
  R_IMF_CHAN_DEPTH_12 = (1 << 3),
  /** 16bits (TIFF, half float EXR). */
  R_IMF_CHAN_DEPTH_16 = (1 << 4),
  /** 24bits (unused). */
  R_IMF_CHAN_DEPTH_24 = (1 << 5),
  /** 32bits (full float EXR). */
  R_IMF_CHAN_DEPTH_32 = (1 << 6),
} eImageFormatDepth;

/** #ImageFormatData::planes */
enum {
  R_IMF_PLANES_RGB = 24,
  R_IMF_PLANES_RGBA = 32,
  R_IMF_PLANES_BW = 8,
};

/** #ImageFormatData::exr_codec */
enum {
  R_IMF_EXR_CODEC_NONE = 0,
  R_IMF_EXR_CODEC_PXR24 = 1,
  R_IMF_EXR_CODEC_ZIP = 2,
  R_IMF_EXR_CODEC_PIZ = 3,
  R_IMF_EXR_CODEC_RLE = 4,
  R_IMF_EXR_CODEC_ZIPS = 5,
  R_IMF_EXR_CODEC_B44 = 6,
  R_IMF_EXR_CODEC_B44A = 7,
  R_IMF_EXR_CODEC_DWAA = 8,
  R_IMF_EXR_CODEC_DWAB = 9,
  R_IMF_EXR_CODEC_MAX = 10,
};

/** #ImageFormatData::jp2_flag */
enum {
  /** When disabled use RGB. */
  R_IMF_JP2_FLAG_YCC = 1 << 0,         /* Was `R_JPEG2K_YCC`. */
  R_IMF_JP2_FLAG_CINE_PRESET = 1 << 1, /* Was `R_JPEG2K_CINE_PRESET`. */
  R_IMF_JP2_FLAG_CINE_48 = 1 << 2,     /* Was `R_JPEG2K_CINE_48FPS`. */
};

/** #ImageFormatData::jp2_codec */
enum {
  R_IMF_JP2_CODEC_JP2 = 0,
  R_IMF_JP2_CODEC_J2K = 1,
};

/** #ImageFormatData::cineon_flag */
enum {
  R_IMF_CINEON_FLAG_LOG = 1 << 0, /* Was `R_CINEON_LOG`. */
};

/** #ImageFormatData::tiff_codec */
enum {
  R_IMF_TIFF_CODEC_DEFLATE = 0,
  R_IMF_TIFF_CODEC_LZW = 1,
  R_IMF_TIFF_CODEC_PACKBITS = 2,
  R_IMF_TIFF_CODEC_NONE = 3,
};

/** \} */

/** #ColorManagedViewSettings.flag */
enum {
  COLORMANAGE_VIEW_USE_CURVES = (1 << 0),
  COLORMANAGE_VIEW_USE_HDR = (1 << 1),
  COLORMANAGE_VIEW_USE_WHITE_BALANCE = (1 << 2),
};
