/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup DNA
 *
 * Structs for use by the 'Sequencer' (Video Editor)
 *
 * Note on terminology
 * - #Strip: video/effect/audio data you can select and manipulate in the sequencer.
 * - #Strip.machine: Strange name for the channel.
 * - #StripData: The data referenced by the #Strip
 * - Meta Strip (STRIP_TYPE_META): Support for nesting Sequences.
 */

#pragma once

#include "DNA_color_types.h"
#include "DNA_defs.h"
#include "DNA_listBase.h"
#include "DNA_session_uid_types.h" /* for #SessionUID */
#include "DNA_vec_types.h"         /* for #rctf */

struct Ipo;
struct MovieClip;
struct Scene;
struct StripLookup;
struct VFont;
struct bSound;

#ifdef __cplusplus
namespace blender::seq {
struct MediaPresence;
struct ThumbnailCache;
struct TextVarsRuntime;
}  // namespace blender::seq
using MediaPresence = blender::seq::MediaPresence;
using ThumbnailCache = blender::seq::ThumbnailCache;
using TextVarsRuntime = blender::seq::TextVarsRuntime;
#else
typedef struct MediaPresence MediaPresence;
typedef struct ThumbnailCache ThumbnailCache;
typedef struct TextVarsRuntime TextVarsRuntime;
#endif

/* -------------------------------------------------------------------- */
/** \name Strip & Editing Structs
 * \{ */

/* strlens; 256= FILE_MAXFILE, 768= FILE_MAXDIR */

typedef struct StripAnim {
  struct StripAnim *next = nullptr, *prev = nullptr;
  struct MovieReader *anim = nullptr;
} StripAnim;

typedef struct StripElem {
  /** File name concatenated onto #StripData::dirpath. */
  char filename[256] = "";
  /** Ignore when zeroed. */
  int orig_width = 0, orig_height = 0;
  float orig_fps = 0;
} StripElem;

typedef struct StripCrop {
  int top = 0;
  int bottom = 0;
  int left = 0;
  int right = 0;
} StripCrop;

typedef struct StripTransform {
  float xofs = 0;
  float yofs = 0;
  float scale_x = 0;
  float scale_y = 0;
  float rotation = 0;
  /** 0-1 range, use SEQ_image_transform_origin_offset_pixelspace_get to convert to pixel space. */
  float origin[2] = {};
  int filter = 0;
} StripTransform;

typedef struct StripColorBalance {
  int method = 0;
  float lift[3] = {};
  float gamma[3] = {};
  float gain[3] = {};
  float slope[3] = {};
  float offset[3] = {};
  float power[3] = {};
  int flag = 0;
  char _pad[4] = {};
  // float exposure = 0;
  // float saturation = 0;
} StripColorBalance;

typedef struct StripProxy {
  /** Custom directory for index and proxy files (defaults to "BL_proxy"). */
  char dirpath[768] = "";
  /** Custom file. */
  char filename[256] = "";
  struct MovieReader *anim = nullptr; /* custom proxy anim file */

  short tc = 0; /* time code in use */

  short quality = 0;          /* proxy build quality */
  short build_size_flags = 0; /* size flags (see below) of all proxies */
                              /* to build */
  short build_tc_flags = 0;   /* time code flags (see below) of all tc indices */
                              /* to build */
  short build_flags = 0;
  char storage = 0;
  char _pad[5] = {};
} StripProxy;

typedef struct StripData {
  struct StripData *next = nullptr, *prev = nullptr;
  int us = 0, done = 0;
  int startstill = 0, endstill = 0;
  /**
   * Only used as an array in IMAGE sequences(!),
   * and as a 1-element array in MOVIE sequences,
   * NULL for all other strip-types.
   */
  StripElem *stripdata = nullptr;
  char dirpath[768] = "";
  StripProxy *proxy = nullptr;
  StripCrop *crop = nullptr;
  StripTransform *transform = nullptr;
  StripColorBalance *color_balance DNA_DEPRECATED = nullptr;

  /* color management */
  ColorManagedColorspaceSettings colorspace_settings;
} StripData;

typedef enum eSeqRetimingKeyFlag {
  SEQ_SPEED_TRANSITION_IN = (1 << 0),
  SEQ_SPEED_TRANSITION_OUT = (1 << 1),
  SEQ_FREEZE_FRAME_IN = (1 << 2),
  SEQ_FREEZE_FRAME_OUT = (1 << 3),
  SEQ_KEY_SELECTED = (1 << 4),
} eSeqRetimingKeyFlag;

typedef struct SeqRetimingKey {
  double strip_frame_index = 0;
  int flag = 0; /* eSeqRetimingKeyFlag */
  int _pad0 = 0;
  float retiming_factor = 0; /* Value between 0-1 mapped to original content range. */

  char _pad1[4] = {};
  double original_strip_frame_index = 0; /* Used for transition keys only. */
  float original_retiming_factor = 0;    /* Used for transition keys only. */
  char _pad2[4] = {};
} SeqRetimingKey;

typedef struct StripRuntime {
  SessionUID session_uid;
} StripRuntime;

/**
 * The sequence structure is the basic struct used by any strip.
 * each of the strips uses a different sequence structure.
 *
 * \warning The first part identical to ID (for use in ipo's)
 * the comment above is historic, probably we can drop the ID compatibility,
 * but take care making this change.
 */
typedef struct Strip {
  struct Strip *next = nullptr, *prev = nullptr;
  void *_pad = nullptr;
  /** Needed (to be like ipo), else it will raise libdata warnings, this should never be used. */
  void *lib = nullptr;
  /** STRIP_NAME_MAXSTR - name, set by default and needs to be unique, for RNA paths. */
  char name[64] = "";

  /** Flags bitmap (see below) and the type of sequence. */
  int flag = 0, type = 0;
  /** The length of the contents of this strip - before handles are applied. */
  int len = 0;
  /**
   * Start frame of contents of strip in absolute frame coordinates.
   * For meta-strips start of first strip startdisp.
   */
  float start = 0;
  /**
   * Frames after the first frame where display starts,
   * frames before the last frame where display ends.
   */
  float startofs = 0, endofs = 0;
  /**
   * Frames that use the first frame before data begins,
   * frames that use the last frame after data ends.
   */
  float startstill = 0, endstill = 0;
  /** Machine: the strip channel */
  int machine = 0;
  /** Starting and ending points of the effect strip. Undefined for other strip types. */
  int startdisp = 0, enddisp = 0;
  float sat = 0;
  float mul = 0;

  /** Stream-index for movie or sound files with several streams. */
  short streamindex = 0;
  short _pad1 = 0;
  /** For multi-camera source selection. */
  int multicam_source = 0;
  /** MOVIECLIP render flags. */
  int clip_flag = 0;

  StripData *data = nullptr;

  /** Old animation system, deprecated for 2.5. */
  struct Ipo *ipo DNA_DEPRECATED = nullptr;

  /** these ID vars should never be NULL but can be when linked libraries fail to load,
   * so check on access */
  struct Scene *scene = nullptr;
  /** Override scene camera. */
  struct Object *scene_camera = nullptr;
  /** For MOVIECLIP strips. */
  struct MovieClip *clip = nullptr;
  /** For MASK strips. */
  struct Mask *mask = nullptr;
  /** For MOVIE strips. */
  ListBase anims = {nullptr, nullptr};

  float effect_fader = 0;
  /* DEPRECATED, only used for versioning. */
  float speed_fader = 0;

  /* pointers for effects: */
  struct Strip *seq1 = nullptr, *seq2 = nullptr;

  /* This strange padding is needed due to how `seqbasep` de-serialization is
   * done right now in #scene_blend_read_data. */
  void *_pad7 = nullptr;
  int _pad8[2] = {};

  /** List of strips for meta-strips. */
  ListBase seqbase = {nullptr, nullptr};
  ListBase channels = {nullptr, nullptr}; /* SeqTimelineChannel */

  /* List of strip connections (one-way, not bidirectional). */
  ListBase connections = {nullptr, nullptr}; /* StripConnection */

  /** The linked "bSound" object. */
  struct bSound *sound = nullptr;
  /** Handle to #AUD_SequenceEntry. */
  void *scene_sound = nullptr;
  float volume = 0;

  /** Pitch (-0.1..10), pan -2..2. */
  float pitch DNA_DEPRECATED = 0, pan = 0;
  float strobe = 0;

  float sound_offset = 0;
  char _pad4[4] = {};

  /** Struct pointer for effect settings. */
  void *effectdata = nullptr;

  /** Only use part of animation file. */
  int anim_startofs = 0;
  /** Is subtle different to startofs / endofs. */
  int anim_endofs = 0;

  int blend_mode = 0;
  float blend_opacity = 0;

  /* Tag color showed if `SEQ_TIMELINE_SHOW_STRIP_COLOR_TAG` is set. */
  int8_t color_tag = 0;

  char alpha_mode = 0;
  char _pad2[2] = {};

  int cache_flag = 0;

  /* is sfra needed anymore? - it looks like its only used in one place */
  /** Starting frame according to the timeline of the scene. */
  int sfra = 0;

  /* Multiview */
  char views_format = 0;
  char _pad3[3] = {};
  struct Stereo3dFormat *stereo3d_format = nullptr;

  struct IDProperty *prop = nullptr;

  /* modifiers */
  ListBase modifiers = {nullptr, nullptr};

  /* Playback rate of strip content in frames per second. */
  float media_playback_rate = 0;
  float speed_factor = 0;

  struct SeqRetimingKey *retiming_keys = nullptr;
  void *_pad5 = nullptr;
  int retiming_keys_num = 0;
  char _pad6[4] = {};

  StripRuntime runtime;
} Strip;

typedef struct MetaStack {
  struct MetaStack *next = nullptr, *prev = nullptr;
  ListBase *oldbasep = nullptr;
  ListBase *old_channels = nullptr;
  Strip *parseq = nullptr;
  /* the startdisp/enddisp when entering the meta */
  int disp_range[2] = {};
} MetaStack;

typedef struct SeqTimelineChannel {
  struct SeqTimelineChannel *next = nullptr, *prev = nullptr;
  char name[64] = "";
  int index = 0;
  int flag = 0;
} SeqTimelineChannel;

typedef struct StripConnection {
  struct StripConnection *next = nullptr, *prev = nullptr;
  Strip *strip_ref = nullptr;
} StripConnection;

typedef struct EditingRuntime {
  struct StripLookup *strip_lookup = nullptr;
  MediaPresence *media_presence = nullptr;
  ThumbnailCache *thumbnail_cache = nullptr;
  void *_pad = nullptr;
} EditingRuntime;

typedef struct Editing {
  /** Pointer to the current list of seq's being edited (can be within a meta strip). */
  ListBase *seqbasep = nullptr;
  ListBase *displayed_channels = nullptr;
  void *_pad0 = nullptr;
  /** Pointer to the top-most seq's. */
  ListBase seqbase = {nullptr, nullptr};
  ListBase metastack = {nullptr, nullptr};
  ListBase channels = {nullptr, nullptr}; /* SeqTimelineChannel */

  /* Context vars, used to be static */
  Strip *act_seq = nullptr;
  /** 1024 = FILE_MAX. */
  char act_imagedir[1024] = "";
  /** 1024 = FILE_MAX. */
  char act_sounddir[1024] = "";
  /** 1024 = FILE_MAX. */
  char proxy_dir[1024] = "";

  int proxy_storage = 0;

  int overlay_frame_ofs = 0, overlay_frame_abs = 0;
  int overlay_frame_flag = 0;
  rctf overlay_frame_rect;

  int show_missing_media_flag = 0;
  int _pad1 = 0;

  struct SeqCache *cache = nullptr;

  /* Cache control */
  float recycle_max_cost = 0; /* UNUSED only for versioning. */
  int cache_flag = 0;

  struct PrefetchJob *prefetch_job = nullptr;

  /* Must be initialized only by seq_cache_create() */
  int64_t disk_cache_timestamp = 0;

  EditingRuntime runtime;
} Editing;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Effect Variable Structs
 * \{ */

typedef struct WipeVars {
  float edgeWidth = 0, angle = 0;
  short forward = 0, wipetype = 0;
} WipeVars;

typedef struct GlowVars {
  /** Minimum intensity to trigger a glow. */
  float fMini = 0;
  float fClamp = 0;
  /** Amount to multiply glow intensity. */
  float fBoost = 0;
  /** Radius of glow blurring. */
  float dDist = 0;
  int dQuality = 0;
  /** SHOW/HIDE glow buffer. */
  int bNoComp = 0;
} GlowVars;

typedef struct TransformVars {
  float ScalexIni = 0;
  float ScaleyIni = 0;
  float xIni = 0;
  float yIni = 0;
  float rotIni = 0;
  int percent = 0;
  int interpolation = 0;
  /** Preserve aspect/ratio when scaling. */
  int uniform_scale = 0;
} TransformVars;

typedef struct SolidColorVars {
  float col[3] = {};
  char _pad[4] = {};
} SolidColorVars;

typedef struct SpeedControlVars {
  float *frameMap = nullptr;
  /* DEPRECATED, only used for versioning. */
  float globalSpeed = 0;
  int flags = 0;

  int speed_control_type = 0;

  float speed_fader = 0;
  float speed_fader_length = 0;
  float speed_fader_frame_number = 0;
} SpeedControlVars;

/** #SpeedControlVars.speed_control_type */
enum {
  SEQ_SPEED_STRETCH = 0,
  SEQ_SPEED_MULTIPLY = 1,
  SEQ_SPEED_LENGTH = 2,
  SEQ_SPEED_FRAME_NUMBER = 3,
};

typedef struct GaussianBlurVars {
  float size_x = 0;
  float size_y = 0;
} GaussianBlurVars;

typedef struct TextVars {
  char text[512] = "";
  struct VFont *text_font = nullptr;
  int text_blf_id = 0;
  float text_size = 0;
  float color[4] = {}, shadow_color[4] = {}, box_color[4] = {}, outline_color[4] = {};
  float loc[2] = {};
  float wrap_width = 0;
  float box_margin = 0;
  float box_roundness = 0;
  float shadow_angle = 0;
  float shadow_offset = 0;
  float shadow_blur = 0;
  float outline_width = 0;
  char flag = 0;
  char align = 0;
  char _pad[2] = {};

  /** Offsets in characters (unicode code-points) for #TextVars::text. */
  int cursor_offset = 0;
  int selection_start_offset = 0;
  int selection_end_offset = 0;

  char align_y DNA_DEPRECATED = 0 /* Only used for versioning. */;
  char anchor_x = 0, anchor_y = 0;
  char _pad1 = 0;
  TextVarsRuntime *runtime = nullptr;
} TextVars;

/** #TextVars.flag */
enum {
  SEQ_TEXT_SHADOW = (1 << 0),
  SEQ_TEXT_BOX = (1 << 1),
  SEQ_TEXT_BOLD = (1 << 2),
  SEQ_TEXT_ITALIC = (1 << 3),
  SEQ_TEXT_OUTLINE = (1 << 4),
};

/** #TextVars.align */
enum {
  SEQ_TEXT_ALIGN_X_LEFT = 0,
  SEQ_TEXT_ALIGN_X_CENTER = 1,
  SEQ_TEXT_ALIGN_X_RIGHT = 2,
};

/** #TextVars.align_y */
enum {
  SEQ_TEXT_ALIGN_Y_TOP = 0,
  SEQ_TEXT_ALIGN_Y_CENTER = 1,
  SEQ_TEXT_ALIGN_Y_BOTTOM = 2,
};

#define STRIP_FONT_NOT_LOADED -2

typedef struct ColorMixVars {
  /** Value from STRIP_TYPE_XXX enumeration. */
  int blend_effect = 0;
  /** Blend factor [0.0f, 1.0f]. */
  float factor = 0;
} ColorMixVars;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Strip Modifiers
 * \{ */

typedef struct SequenceModifierData {
  struct SequenceModifierData *next = nullptr, *prev = nullptr;
  int type = 0, flag = 0;
  /** MAX_NAME. */
  char name[64] = "";

  /* mask input, either sequence or mask ID */
  int mask_input_type = 0;
  int mask_time = 0;

  struct Strip *mask_sequence = nullptr;
  struct Mask *mask_id = nullptr;
} SequenceModifierData;

typedef struct ColorBalanceModifierData {
  SequenceModifierData modifier;

  StripColorBalance color_balance;
  float color_multiply = 0;
} ColorBalanceModifierData;

enum {
  SEQ_COLOR_BALANCE_METHOD_LIFTGAMMAGAIN = 0,
  SEQ_COLOR_BALANCE_METHOD_SLOPEOFFSETPOWER = 1,
};

typedef struct CurvesModifierData {
  SequenceModifierData modifier;

  struct CurveMapping curve_mapping;
} CurvesModifierData;

typedef struct HueCorrectModifierData {
  SequenceModifierData modifier;

  struct CurveMapping curve_mapping;
} HueCorrectModifierData;

typedef struct BrightContrastModifierData {
  SequenceModifierData modifier;

  float bright = 0;
  float contrast = 0;
} BrightContrastModifierData;

typedef struct SequencerMaskModifierData {
  SequenceModifierData modifier;
} SequencerMaskModifierData;

typedef struct WhiteBalanceModifierData {
  SequenceModifierData modifier;

  float white_value[3] = {};
  char _pad[4] = {};
} WhiteBalanceModifierData;

typedef struct SequencerTonemapModifierData {
  SequenceModifierData modifier;

  float key = 0, offset = 0, gamma = 0;
  float intensity = 0, contrast = 0, adaptation = 0, correction = 0;
  int type = 0;
} SequencerTonemapModifierData;

enum {
  SEQ_TONEMAP_RH_SIMPLE = 0,
  SEQ_TONEMAP_RD_PHOTORECEPTOR = 1,
};

/** \} */

/** \name Sound Modifiers
 * \{ */

typedef struct EQCurveMappingData {
  struct EQCurveMappingData *next = nullptr, *prev = nullptr;
  struct CurveMapping curve_mapping;
} EQCurveMappingData;

typedef struct SoundEqualizerModifierData {
  SequenceModifierData modifier;
  /* EQCurveMappingData */
  ListBase graphics = {nullptr, nullptr};
} SoundEqualizerModifierData;
/** \} */

/* -------------------------------------------------------------------- */
/** \name Flags & Types
 * \{ */

/** #Editor::overlay_frame_flag */
enum {
  SEQ_EDIT_OVERLAY_FRAME_SHOW = 1,
  SEQ_EDIT_OVERLAY_FRAME_ABS = 2,
};

/** #Editing::show_missing_media_flag */
enum {
  SEQ_EDIT_SHOW_MISSING_MEDIA = 1 << 0,
};

#define STRIP_OFSBOTTOM 0.05f
#define STRIP_OFSTOP 0.95f

/** #Editor::proxy_storage */
enum {
  /** Store proxies in project directory. */
  SEQ_EDIT_PROXY_DIR_STORAGE = 1,
};

/** #SpeedControlVars::flags */
enum {
  SEQ_SPEED_UNUSED_2 = 1 << 0, /* cleared */
  SEQ_SPEED_UNUSED_1 = 1 << 1, /* cleared */
  SEQ_SPEED_UNUSED_3 = 1 << 2, /* cleared */
  SEQ_SPEED_USE_INTERPOLATION = 1 << 3,
};

#define STRIP_NAME_MAXSTR 64

/* From: `DNA_object_types.h`, see it's doc-string there. */
#define SELECT 1

/** #Strip.flag */
enum {
  /* `SELECT = (1 << 0)` */
  SEQ_LEFTSEL = (1 << 1),
  SEQ_RIGHTSEL = (1 << 2),
  SEQ_OVERLAP = (1 << 3),
  SEQ_FILTERY = (1 << 4),
  SEQ_MUTE = (1 << 5),
  SEQ_FLAG_TEXT_EDITING_ACTIVE = (1 << 6),
  SEQ_REVERSE_FRAMES = (1 << 7),
  SEQ_IPO_FRAME_LOCKED = (1 << 8),
  SEQ_EFFECT_NOT_LOADED = (1 << 9),
  SEQ_FLAG_DELETE = (1 << 10),
  SEQ_FLIPX = (1 << 11),
  SEQ_FLIPY = (1 << 12),
  SEQ_MAKE_FLOAT = (1 << 13),
  SEQ_LOCK = (1 << 14),
  SEQ_USE_PROXY = (1 << 15),
  SEQ_IGNORE_CHANNEL_LOCK = (1 << 16),
  SEQ_AUTO_PLAYBACK_RATE = (1 << 17),
  SEQ_SINGLE_FRAME_CONTENT = (1 << 18),
  SEQ_SHOW_RETIMING = (1 << 19),
  SEQ_MULTIPLY_ALPHA = (1 << 21),

  SEQ_USE_EFFECT_DEFAULT_FADE = (1 << 22),
  SEQ_USE_LINEAR_MODIFIERS = (1 << 23),

  /* flags for whether those properties are animated or not */
  SEQ_AUDIO_VOLUME_ANIMATED = (1 << 24),
  SEQ_AUDIO_PITCH_ANIMATED = (1 << 25),
  SEQ_AUDIO_PAN_ANIMATED = (1 << 26),
  SEQ_AUDIO_DRAW_WAVEFORM = (1 << 27),

  /* don't include Annotations in OpenGL previews of Scene strips */
  SEQ_SCENE_NO_ANNOTATION = (1 << 28),
  SEQ_USE_VIEWS = (1 << 29),

  /* Access scene strips directly (like a meta-strip). */
  SEQ_SCENE_STRIPS = (1 << 30),

  SEQ_INVALID_EFFECT = (1u << 31),
};

/** #StripProxy.storage */
enum {
  SEQ_STORAGE_PROXY_CUSTOM_FILE = (1 << 1), /* store proxy in custom directory */
  SEQ_STORAGE_PROXY_CUSTOM_DIR = (1 << 2),  /* store proxy in custom file */
};

/* convenience define for all selection flags */
#define STRIP_ALLSEL (SELECT + SEQ_LEFTSEL + SEQ_RIGHTSEL)

/* Deprecated, don't use a flag anymore. */
// #define STRIP_ACTIVE 1048576

enum {
  SEQ_COLOR_BALANCE_INVERSE_GAIN = 1 << 0,
  SEQ_COLOR_BALANCE_INVERSE_GAMMA = 1 << 1,
  SEQ_COLOR_BALANCE_INVERSE_LIFT = 1 << 2,
  SEQ_COLOR_BALANCE_INVERSE_SLOPE = 1 << 3,
  SEQ_COLOR_BALANCE_INVERSE_OFFSET = 1 << 4,
  SEQ_COLOR_BALANCE_INVERSE_POWER = 1 << 5,
};

/**
 * \warning has to be same as `IMB_imbuf.hh`: `IMB_PROXY_*` and `IMB_TC_*`.
 */
enum {
  SEQ_PROXY_IMAGE_SIZE_25 = 1 << 0,
  SEQ_PROXY_IMAGE_SIZE_50 = 1 << 1,
  SEQ_PROXY_IMAGE_SIZE_75 = 1 << 2,
  SEQ_PROXY_IMAGE_SIZE_100 = 1 << 3,
};

/**
 * \warning has to be same as `IMB_imbuf.hh`: `IMB_TC_*`.
 */
enum {
  SEQ_PROXY_TC_NONE = 0,
  SEQ_PROXY_TC_RECORD_RUN = 1 << 0,
  SEQ_PROXY_TC_RECORD_RUN_NO_GAPS = 1 << 1,
};

/** SeqProxy.build_flags */
enum {
  SEQ_PROXY_SKIP_EXISTING = 1,
};

/** #Strip.alpha_mode */
enum {
  SEQ_ALPHA_STRAIGHT = 0,
  SEQ_ALPHA_PREMUL = 1,
};

/**
 * #Strip.type
 *
 * \warning #STRIP_TYPE_EFFECT BIT is used to determine if this is an effect strip!
 */
typedef enum StripType {
  STRIP_TYPE_IMAGE = 0,
  STRIP_TYPE_META = 1,
  STRIP_TYPE_SCENE = 2,
  STRIP_TYPE_MOVIE = 3,
  STRIP_TYPE_SOUND_RAM = 4,
  STRIP_TYPE_SOUND_HD = 5, /* DEPRECATED */
  STRIP_TYPE_MOVIECLIP = 6,
  STRIP_TYPE_MASK = 7,

  STRIP_TYPE_EFFECT = 8,
  STRIP_TYPE_CROSS = 8,
  STRIP_TYPE_ADD = 9,
  STRIP_TYPE_SUB = 10,
  STRIP_TYPE_ALPHAOVER = 11,
  STRIP_TYPE_ALPHAUNDER = 12,
  STRIP_TYPE_GAMCROSS = 13,
  STRIP_TYPE_MUL = 14,
  STRIP_TYPE_OVERDROP_REMOVED =
      15, /* Removed (behavior was the same as alpha-over), only used when reading old files. */
  /* STRIP_TYPE_PLUGIN      = 24, */ /* Deprecated */
  STRIP_TYPE_WIPE = 25,
  STRIP_TYPE_GLOW = 26,
  STRIP_TYPE_TRANSFORM = 27,
  STRIP_TYPE_COLOR = 28,
  STRIP_TYPE_SPEED = 29,
  STRIP_TYPE_MULTICAM = 30,
  STRIP_TYPE_ADJUSTMENT = 31,
  STRIP_TYPE_GAUSSIAN_BLUR = 40,
  STRIP_TYPE_TEXT = 41,
  STRIP_TYPE_COLORMIX = 42,

  /* Blend modes */
  STRIP_TYPE_SCREEN = 43,
  STRIP_TYPE_LIGHTEN = 44,
  STRIP_TYPE_DODGE = 45,
  STRIP_TYPE_DARKEN = 46,
  STRIP_TYPE_COLOR_BURN = 47,
  STRIP_TYPE_LINEAR_BURN = 48,
  STRIP_TYPE_OVERLAY = 49,
  STRIP_TYPE_HARD_LIGHT = 50,
  STRIP_TYPE_SOFT_LIGHT = 51,
  STRIP_TYPE_PIN_LIGHT = 52,
  STRIP_TYPE_LIN_LIGHT = 53,
  STRIP_TYPE_VIVID_LIGHT = 54,
  STRIP_TYPE_HUE = 55,
  STRIP_TYPE_SATURATION = 56,
  STRIP_TYPE_VALUE = 57,
  STRIP_TYPE_BLEND_COLOR = 58,
  STRIP_TYPE_DIFFERENCE = 59,
  STRIP_TYPE_EXCLUSION = 60,

  STRIP_TYPE_MAX = 60,
} StripType;

enum {
  SEQ_MOVIECLIP_RENDER_UNDISTORTED = 1 << 0,
  SEQ_MOVIECLIP_RENDER_STABILIZED = 1 << 1,
};

enum {
  SEQ_BLEND_REPLACE = 0,
};
/* all other BLEND_MODEs are simple STRIP_TYPE_EFFECT ids and therefore identical
 * to the table above. (Only those effects that handle _exactly_ two inputs,
 * otherwise, you can't really blend, right :) !)
 */

#define STRIP_HAS_PATH(_seq) \
  (ELEM((_seq)->type, \
        STRIP_TYPE_MOVIE, \
        STRIP_TYPE_IMAGE, \
        STRIP_TYPE_SOUND_RAM, \
        STRIP_TYPE_SOUND_HD))

/* modifiers */

/** #SequenceModifierData.type */
enum {
  seqModifierType_ColorBalance = 1,
  seqModifierType_Curves = 2,
  seqModifierType_HueCorrect = 3,
  seqModifierType_BrightContrast = 4,
  seqModifierType_Mask = 5,
  seqModifierType_WhiteBalance = 6,
  seqModifierType_Tonemap = 7,
  seqModifierType_SoundEqualizer = 8,
  /* Keep last. */
  NUM_SEQUENCE_MODIFIER_TYPES,
};

/** #SequenceModifierData.flag */
enum {
  SEQUENCE_MODIFIER_MUTE = (1 << 0),
  SEQUENCE_MODIFIER_EXPANDED = (1 << 1),
};

enum {
  SEQUENCE_MASK_INPUT_STRIP = 0,
  SEQUENCE_MASK_INPUT_ID = 1,
};

enum {
  /* Mask animation will be remapped relative to the strip start frame. */
  SEQUENCE_MASK_TIME_RELATIVE = 0,
  /* Global (scene) frame number will be used to access the mask. */
  SEQUENCE_MASK_TIME_ABSOLUTE = 1,
};

/**
 * #Strip.cache_flag
 * - #SEQ_CACHE_STORE_RAW
 * - #SEQ_CACHE_STORE_PREPROCESSED
 * - #SEQ_CACHE_STORE_COMPOSITE
 * - #FINAL_OUT is ignored
 *
 * #Editing.cache_flag
 * all entries
 */
enum {
  SEQ_CACHE_STORE_RAW = (1 << 0),
  SEQ_CACHE_STORE_PREPROCESSED = (1 << 1),
  SEQ_CACHE_STORE_COMPOSITE = (1 << 2),
  SEQ_CACHE_STORE_FINAL_OUT = (1 << 3),

  /* For lookup purposes */
  SEQ_CACHE_ALL_TYPES = SEQ_CACHE_STORE_RAW | SEQ_CACHE_STORE_PREPROCESSED |
                        SEQ_CACHE_STORE_COMPOSITE | SEQ_CACHE_STORE_FINAL_OUT,

  SEQ_CACHE_OVERRIDE = (1 << 4),

  SEQ_CACHE_UNUSED_5 = (1 << 5),
  SEQ_CACHE_UNUSED_6 = (1 << 6),
  SEQ_CACHE_UNUSED_7 = (1 << 7),
  SEQ_CACHE_UNUSED_8 = (1 << 8),
  SEQ_CACHE_UNUSED_9 = (1 << 9),

  SEQ_CACHE_PREFETCH_ENABLE = (1 << 10),
  SEQ_CACHE_DISK_CACHE_ENABLE = (1 << 11),
};

/** #Strip.color_tag. */
typedef enum StripColorTag {
  STRIP_COLOR_NONE = -1,
  STRIP_COLOR_01,
  STRIP_COLOR_02,
  STRIP_COLOR_03,
  STRIP_COLOR_04,
  STRIP_COLOR_05,
  STRIP_COLOR_06,
  STRIP_COLOR_07,
  STRIP_COLOR_08,
  STRIP_COLOR_09,

  STRIP_COLOR_TOT,
} StripColorTag;

/* Strip->StripTransform->filter */
enum {
  SEQ_TRANSFORM_FILTER_AUTO = -1,
  SEQ_TRANSFORM_FILTER_NEAREST = 0,
  SEQ_TRANSFORM_FILTER_BILINEAR = 1,
  SEQ_TRANSFORM_FILTER_BOX = 2,
  SEQ_TRANSFORM_FILTER_CUBIC_BSPLINE = 3,
  SEQ_TRANSFORM_FILTER_CUBIC_MITCHELL = 4,
};

typedef enum eSeqChannelFlag {
  SEQ_CHANNEL_LOCK = (1 << 0),
  SEQ_CHANNEL_MUTE = (1 << 1),
} eSeqChannelFlag;

/** \} */
