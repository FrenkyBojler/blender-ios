/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include <limits.h>

#include "DNA_ID.h"
#include "DNA_color_types.h" /* for color management */
#include "DNA_defs.h"

struct GPUTexture;
struct MovieReader;
struct MovieCache;
struct PackedFile;
struct RenderResult;
struct Scene;

/* **************** IMAGE ********************* */

/** #Image.flag */
enum {
  IMA_HIGH_BITDEPTH = (1 << 0),
  IMA_FLAG_UNUSED_1 = (1 << 1), /* cleared */
#ifdef DNA_DEPRECATED_ALLOW
  IMA_DO_PREMUL = (1 << 2),
#endif
  IMA_FLAG_UNUSED_4 = (1 << 4), /* cleared */
  IMA_NOCOLLECT = (1 << 5),
  IMA_FLAG_UNUSED_6 = (1 << 6), /* cleared */
  IMA_OLD_PREMUL = (1 << 7),
  IMA_FLAG_UNUSED_8 = (1 << 8), /* cleared */
  IMA_USED_FOR_RENDER = (1 << 9),
  /** For image user, but these flags are mixed. */
  IMA_USER_FRAME_IN_RANGE = (1 << 10),
  IMA_VIEW_AS_RENDER = (1 << 11),
  IMA_FLAG_UNUSED_12 = (1 << 12), /* cleared */
  IMA_DEINTERLACE = (1 << 13),
  IMA_USE_VIEWS = (1 << 14),
  IMA_FLAG_UNUSED_15 = (1 << 15), /* cleared */
  IMA_FLAG_UNUSED_16 = (1 << 16), /* cleared */
};

/** #Image.gpuflag */
enum {
  /** All mipmap levels in OpenGL texture set? */
  IMA_GPU_MIPMAP_COMPLETE = (1 << 0),
};

/* Image.source, where the image comes from */
typedef enum eImageSource {
  /* IMA_SRC_CHECK = 0, */ /* UNUSED */
  IMA_SRC_FILE = 1,
  IMA_SRC_SEQUENCE = 2,
  IMA_SRC_MOVIE = 3,
  IMA_SRC_GENERATED = 4,
  IMA_SRC_VIEWER = 5,
  IMA_SRC_TILED = 6,
} eImageSource;

/* Image.type, how to handle or generate the image */
typedef enum eImageType {
  IMA_TYPE_IMAGE = 0,
  IMA_TYPE_MULTILAYER = 1,
  /* generated */
  IMA_TYPE_UV_TEST = 2,
  /* viewers */
  IMA_TYPE_R_RESULT = 4,
  IMA_TYPE_COMPOSITE = 5,
} eImageType;

/** #Image.gen_type */
enum {
  IMA_GENTYPE_BLANK = 0,
  IMA_GENTYPE_GRID = 1,
  IMA_GENTYPE_GRID_COLOR = 2,
};

/** Size of allocated string #RenderResult::text. */
#define IMA_MAX_RENDER_TEXT_SIZE 512

/** #Image.gen_flag */
enum {
  IMA_GEN_FLOAT = (1 << 0),
  IMA_GEN_TILE = (1 << 1),
};

/** #Image.alpha_mode */
enum {
  IMA_ALPHA_STRAIGHT = 0,
  IMA_ALPHA_PREMUL = 1,
  IMA_ALPHA_CHANNEL_PACKED = 2,
  IMA_ALPHA_IGNORE = 3,
};

/* Image gpu runtime defaults */
#define IMAGE_GPU_FRAME_NONE INT_MAX
#define IMAGE_GPU_PASS_NONE SHRT_MAX
#define IMAGE_GPU_LAYER_NONE SHRT_MAX
#define IMAGE_GPU_VIEW_NONE SHRT_MAX

/**
 * ImageUser is in Texture, in Nodes, Background Image, Image Window, ...
 * should be used in conjunction with an ID * to Image.
 */
typedef struct ImageUser {
  /** To retrieve render result. */
  struct Scene *scene = nullptr;

  /** Movies, sequences: current to display. */
  int framenr = 0;
  /** Total amount of frames to use. */
  int frames = 0;
  /** Offset within movie, start frame in global time. */
  int offset = 0, sfra = 0;
  /** Cyclic flag. */
  char cycl = 0;

  /** Multiview current eye - for internal use of drawing routines. */
  char multiview_eye = 0;
  short pass = 0;

  int tile = 0;

  /** Listbase indices, for menu browsing or retrieve buffer. */
  short multi_index = 0, view = 0, layer = 0;
  short flag = 0;
} ImageUser;

typedef struct ImageAnim {
  struct ImageAnim *next = nullptr, *prev = nullptr;
  struct MovieReader *anim = nullptr;
} ImageAnim;

typedef struct ImageView {
  struct ImageView *next = nullptr, *prev = nullptr;
  /** MAX_NAME. */
  char name[64] = "";
  /** 1024 = FILE_MAX. */
  char filepath[1024] = "";
} ImageView;

typedef struct ImagePackedFile {
  struct ImagePackedFile *next = nullptr, *prev = nullptr;
  struct PackedFile *packedfile = nullptr;

  /* Which view and tile this ImagePackedFile represents. Normal images will use 0 and 1001
   * respectively when creating their ImagePackedFile. Must be provided for each packed image. */
  int view = 0;
  int tile_number = 0;
  /** 1024 = FILE_MAX. */
  char filepath[1024] = "";
} ImagePackedFile;

typedef struct RenderSlot {
  struct RenderSlot *next = nullptr, *prev = nullptr;
  /** 64 = MAX_NAME. */
  char name[64] = "";
  struct RenderResult *render = nullptr;
} RenderSlot;

typedef struct ImageTile_Runtime {
  int tilearray_layer = 0;
  int _pad = 0;
  int tilearray_offset[2] = {};
  int tilearray_size[2] = {};
} ImageTile_Runtime;

typedef struct ImageTile {
  struct ImageTile *next = nullptr, *prev = nullptr;

  struct ImageTile_Runtime runtime;

  int tile_number = 0;

  /* for generated images */
  int gen_x = 0, gen_y = 0;
  char gen_type = 0, gen_flag = 0;
  short gen_depth = 0;
  float gen_color[4] = {};

  char label[64] = "";
} ImageTile;

/** #ImageUser::flag */
enum {
  IMA_ANIM_ALWAYS = 1 << 0,
  // IMA_UNUSED_1 = 1 << 1,
  // IMA_UNUSED_2 = 1 << 2,
  IMA_NEED_FRAME_RECALC = 1 << 3,
  IMA_SHOW_STEREO = 1 << 4,
  // IMA_UNUSED_5 = 1 << 5,
};

/* Used to get the correct gpu texture from an Image datablock. */
typedef enum eGPUTextureTarget {
  TEXTARGET_2D = 0,
  TEXTARGET_2D_ARRAY,
  TEXTARGET_TILE_MAPPING,
  TEXTARGET_COUNT,
} eGPUTextureTarget;

/* Defined in BKE_image.hh. */
struct PartialUpdateRegister;
struct PartialUpdateUser;

typedef struct Image_Runtime {
  /* Mutex used to guarantee thread-safe access to the cached ImBuf of the corresponding image ID.
   */
  void *cache_mutex = nullptr;

  /** \brief Register containing partial updates. */
  struct PartialUpdateRegister *partial_update_register = nullptr;
  /** \brief Partial update user for GPUTextures stored inside the Image. */
  struct PartialUpdateUser *partial_update_user = nullptr;

  /* Compositor viewer might be translated, and that translation will be stored in this runtime
   * vector by the compositor so that the editor draw code can draw the image translated. */
  float backdrop_offset[2] = {};
} Image_Runtime;

typedef struct Image {
  ID id;
  struct AnimData *adt = nullptr;
  /**
   * Engines draw data, must be immediately after AnimData. See IdDdtTemplate and
   * DRW_drawdatalist_from_id to understand this requirement.
   */
  DrawDataList drawdata;

  /** File path, 1024 = FILE_MAX. */
  char filepath[1024] = "";

  /** Not written in file. */
  struct MovieCache *cache = nullptr;
  /** Not written in file 3 = TEXTARGET_COUNT, 2 = stereo eyes. */
  struct GPUTexture *gputexture[3][2] = {};

  /* sources from: */
  ListBase anims = {nullptr, nullptr};
  struct RenderResult *rr = nullptr;

  ListBase renderslots = {nullptr, nullptr};
  short render_slot = 0, last_render_slot = 0;

  int flag = 0;
  short source = 0, type = 0;
  int lastframe = 0;

  /* GPU texture flag. */
  int gpuframenr = IMAGE_GPU_FRAME_NONE;
  short gpuflag = 0;
  short gpu_pass = IMAGE_GPU_PASS_NONE;
  short gpu_layer = IMAGE_GPU_LAYER_NONE;
  short gpu_view = IMAGE_GPU_VIEW_NONE;

  /* Number of iterations to perform when extracting mask for uv seam fixing. */
  short seam_margin = 8;

  char _pad2[2] = {};

  /** Deprecated. */
  struct PackedFile *packedfile DNA_DEPRECATED = nullptr;
  struct ListBase packedfiles = {nullptr, nullptr};
  struct PreviewImage *preview = nullptr;

  int lastused = 0;

  /* for generated images */
  int gen_x DNA_DEPRECATED = 1024, gen_y DNA_DEPRECATED = 1024;
  char gen_type DNA_DEPRECATED = IMA_GENTYPE_GRID, gen_flag DNA_DEPRECATED = 0;
  short gen_depth DNA_DEPRECATED = 0;
  float gen_color[4] DNA_DEPRECATED = {};

  /* display aspect - for UV editing images resized for faster openGL display */
  float aspx = 1.0, aspy = 1.0;

  /* color management */
  ColorManagedColorspaceSettings colorspace_settings;
  char alpha_mode = 0;

  char _pad = 0;

  /* Multiview */
  /** For viewer node stereoscopy. */
  char eye = 0;
  char views_format = 0;

  /* ImageTile list for UDIMs. */
  int active_tile_index = 0;
  ListBase tiles = {nullptr, nullptr};

  /** ImageView. */
  ListBase views = {nullptr, nullptr};
  struct Stereo3dFormat *stereo3d_format = nullptr;

  Image_Runtime runtime;
} Image;
