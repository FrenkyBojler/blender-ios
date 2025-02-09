/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_colorband_types.h"
#include "DNA_defs.h"
#include "DNA_image_types.h" /* ImageUser */

#include "BLI_math_constants.h"

struct AnimData;
struct ColorBand;
struct CurveMapping;
struct Image;
struct Ipo;
struct Object;
struct PreviewImage;
struct Tex;

/* -------------------------------------------------------------------- */
/** \name #TexMapping Types
 * \{ */

/** #TexMapping::flag bit-mask. */
enum {
  TEXMAP_CLIP_MIN = 1 << 0,
  TEXMAP_CLIP_MAX = 1 << 1,
  TEXMAP_UNIT_MATRIX = 1 << 2,
};

/** #TexMapping::type. */
enum {
  TEXMAP_TYPE_POINT = 0,
  TEXMAP_TYPE_TEXTURE = 1,
  TEXMAP_TYPE_VECTOR = 2,
  TEXMAP_TYPE_NORMAL = 3,
};

/** #ColorMapping::flag bit-mask. */
enum {
  COLORMAP_USE_RAMP = 1,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Tex Types
 * \{ */

/** #Tex::type. */
enum {
  TEX_CLOUDS = 1,
  TEX_WOOD = 2,
  TEX_MARBLE = 3,
  TEX_MAGIC = 4,
  TEX_BLEND = 5,
  TEX_STUCCI = 6,
  TEX_NOISE = 7,
  TEX_IMAGE = 8,
  // TEX_PLUGIN = 9,  /* Deprecated */
  // TEX_ENVMAP = 10, /* Deprecated */
  TEX_MUSGRAVE = 11,
  TEX_VORONOI = 12,
  TEX_DISTNOISE = 13,
  // TEX_POINTDENSITY = 14, /* Deprecated */
  // TEX_VOXELDATA = 15,    /* Deprecated */
  // TEX_OCEAN = 16,        /* Deprecated */
};

/** #Tex::stype musgrave. */
enum {
  TEX_MFRACTAL = 0,
  TEX_RIDGEDMF = 1,
  TEX_HYBRIDMF = 2,
  TEX_FBM = 3,
  TEX_HTERRAIN = 4,
};

/** #Tex::noisebasis, #Tex::noisebasis2. */
enum {
  TEX_BLENDER = 0,
  TEX_STDPERLIN = 1,
  TEX_NEWPERLIN = 2,
  TEX_VORONOI_F1 = 3,
  TEX_VORONOI_F2 = 4,
  TEX_VORONOI_F3 = 5,
  TEX_VORONOI_F4 = 6,
  TEX_VORONOI_F2F1 = 7,
  TEX_VORONOI_CRACKLE = 8,
  TEX_CELLNOISE = 14,
};

/** #Tex::vn_distm voronoi distance metrics. */
enum {
  TEX_DISTANCE = 0,
  TEX_DISTANCE_SQUARED = 1,
  TEX_MANHATTAN = 2,
  TEX_CHEBYCHEV = 3,
  TEX_MINKOVSKY_HALF = 4,
  TEX_MINKOVSKY_FOUR = 5,
  TEX_MINKOVSKY = 6,
};

/** #Tex::imaflag bit-mask. */
enum {
  TEX_INTERPOL = 1 << 0,
  TEX_USEALPHA = 1 << 1,
  TEX_MIPMAP = 1 << 2,
  TEX_IMAROT = 1 << 4,
  TEX_CALCALPHA = 1 << 5,
  TEX_NORMALMAP = 1 << 11,
  TEX_GAUSS_MIP = 1 << 12,
  TEX_FILTER_MIN = 1 << 13,
  TEX_DERIVATIVEMAP = 1 << 14,
};

/** #Tex::texfilter type. */
enum {
  TXF_BOX = 0, /* Blender's old texture filtering method. */
  TXF_EWA = 1,
  TXF_FELINE = 2,
  TXF_AREA = 3,
};

/** #Tex::flag bit-mask. */
enum {
  TEX_COLORBAND = 1 << 0,
  TEX_FLIPBLEND = 1 << 1,
  TEX_NEGALPHA = 1 << 2,
  TEX_CHECKER_ODD = 1 << 3,
  TEX_CHECKER_EVEN = 1 << 4,
  TEX_PRV_ALPHA = 1 << 5,
  TEX_PRV_NOR = 1 << 6,
  TEX_REPEAT_XMIR = 1 << 7,
  TEX_REPEAT_YMIR = 1 << 8,
  TEX_DS_EXPAND = 1 << 9,
  TEX_NO_CLAMP = 1 << 10,
};

/** #Tex::extend (starts with 1 because of backward compatibility). */
enum {
  TEX_EXTEND = 1,
  TEX_CLIP = 2,
  TEX_REPEAT = 3,
  TEX_CLIPCUBE = 4,
  TEX_CHECKER = 5,
};

/** #Tex::noisetype type. */
enum {
  TEX_NOISESOFT = 0,
  TEX_NOISEPERL = 1,
};

/** #Tex::noisebasis2 wood waveforms. */
enum {
  TEX_SIN = 0,
  TEX_SAW = 1,
  TEX_TRI = 2,
};

/** #Tex::stype wood types. */
enum {
  TEX_BAND = 0,
  TEX_RING = 1,
  TEX_BANDNOISE = 2,
  TEX_RINGNOISE = 3,
};

/** #Tex::stype cloud types. */
enum {
  TEX_DEFAULT = 0,
  TEX_COLOR = 1,
};

/** #Tex::stype marble types. */
enum {
  TEX_SOFT = 0,
  TEX_SHARP = 1,
  TEX_SHARPER = 2,
};

/** #Tex::stype blend types. */
enum {
  TEX_LIN = 0,
  TEX_QUAD = 1,
  TEX_EASE = 2,
  TEX_DIAG = 3,
  TEX_SPHERE = 4,
  TEX_HALO = 5,
  TEX_RAD = 6,
};

/** #Tex::stype stucci types. */
enum {
  TEX_PLASTIC = 0,
  TEX_WALLIN = 1,
  TEX_WALLOUT = 2,
};

/** #Tex::vn_coltype voronoi color types. */
enum {
  TEX_INTENSITY = 0,
  TEX_COL1 = 1,
  TEX_COL2 = 2,
  TEX_COL3 = 3,
};

/** Return value. */
enum {
  TEX_INT = 0,
  TEX_RGB = 1,
};

/**
 * - #Material::pr_texture
 * - #Light::pr_texture
 * - #World::pr_texture
 * - #FreestyleLineStyle::pr_texture
 */
enum {
  TEX_PR_TEXTURE = 0,
  TEX_PR_OTHER = 1,
  TEX_PR_BOTH = 2,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #TexMapping Types
 * \{ */

/**
 * #TexMapping::projx
 * #TexMapping::projy
 * #TexMapping::projz
 */
enum {
  PROJ_N = 0,
  PROJ_X = 1,
  PROJ_Y = 2,
  PROJ_Z = 3,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #MTex Types
 * \{ */

/** #MTex::mapping. */
enum {
  MTEX_FLAT = 0,
  MTEX_CUBE = 1,
  MTEX_TUBE = 2,
  MTEX_SPHERE = 3,
};

/** #MTex::blendtype. */
enum {
  MTEX_BLEND = 0,
  MTEX_MUL = 1,
  MTEX_ADD = 2,
  MTEX_SUB = 3,
  MTEX_DIV = 4,
  MTEX_DARK = 5,
  MTEX_DIFF = 6,
  MTEX_LIGHT = 7,
  MTEX_SCREEN = 8,
  MTEX_OVERLAY = 9,
  MTEX_BLEND_HUE = 10,
  MTEX_BLEND_SAT = 11,
  MTEX_BLEND_VAL = 12,
  MTEX_BLEND_COLOR = 13,
  MTEX_SOFT_LIGHT = 15,
  MTEX_LIN_LIGHT = 16,
};

/** #MTex::brush_map_mode. */
enum {
  MTEX_MAP_MODE_VIEW = 0,
  MTEX_MAP_MODE_TILED = 1,
  MTEX_MAP_MODE_3D = 2,
  MTEX_MAP_MODE_AREA = 3,
  MTEX_MAP_MODE_RANDOM = 4,
  MTEX_MAP_MODE_STENCIL = 5,
};

/** #MTex::brush_angle_mode. */
enum {
  MTEX_ANGLE_RANDOM = 1,
  MTEX_ANGLE_RAKE = 2,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #PointDensity Types
 * \{ */

/** #PointDensity::source. */
enum {
  TEX_PD_PSYS = 0,
  TEX_PD_OBJECT = 1,
  TEX_PD_FILE = 2,
};

/** #PointDensity::falloff_type. */
enum {
  TEX_PD_FALLOFF_STD = 0,
  TEX_PD_FALLOFF_SMOOTH = 1,
  TEX_PD_FALLOFF_SOFT = 2,
  TEX_PD_FALLOFF_CONSTANT = 3,
  TEX_PD_FALLOFF_ROOT = 4,
  TEX_PD_FALLOFF_PARTICLE_AGE = 5,
  TEX_PD_FALLOFF_PARTICLE_VEL = 6,
};

/** #PointDensity::psys_cache_space. */
enum {
  TEX_PD_OBJECTLOC = 0,
  TEX_PD_OBJECTSPACE = 1,
  TEX_PD_WORLDSPACE = 2,
};

/** #PointDensity::flag. */
enum {
  TEX_PD_TURBULENCE = 1 << 0,
  TEX_PD_FALLOFF_CURVE = 1 << 1,
};

/** #PointDensity::noise_influence. */
enum {
  TEX_PD_NOISE_STATIC = 0,
  // TEX_PD_NOISE_VEL = 1,  /* Deprecated. */
  // TEX_PD_NOISE_AGE = 2,  /* Deprecated. */
  // TEX_PD_NOISE_TIME = 3, /* Deprecated. */
};

/** #PointDensity::color_source. */
enum {
  TEX_PD_COLOR_CONSTANT = 0,
  /* color_source: particles */
  TEX_PD_COLOR_PARTAGE = 1,
  TEX_PD_COLOR_PARTSPEED = 2,
  TEX_PD_COLOR_PARTVEL = 3,
  /* color_source: vertices */
  TEX_PD_COLOR_VERTCOL = 1,
  TEX_PD_COLOR_VERTWEIGHT = 2,
  TEX_PD_COLOR_VERTNOR = 3,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name #MTex
 * \{ */

/** #MTex::texco */
enum {
  TEXCO_ORCO = 1 << 0,
  // TEXCO_REFL = 1 << 1, /* Deprecated. */
  // TEXCO_NORM = 1 << 2, /* Deprecated. */
  TEXCO_GLOB = 1 << 3,
  TEXCO_UV = 1 << 4,
  TEXCO_OBJECT = 1 << 5,
  // TEXCO_LAVECTOR = 1 << 6, /* Deprecated. */
  // TEXCO_VIEW = 1 << 7,     /* Deprecated. */
  // TEXCO_STICKY = 1 << 8,   /* Deprecated. */
  // TEXCO_OSA = 1 << 9,      /* Deprecated. */
  TEXCO_WINDOW = 1 << 10,
  // NEED_UV = 1 << 11,       /* Deprecated. */
  // TEXCO_TANGENT = 1 << 12, /* Deprecated. */
  /** still stored in `vertex->accum`, 1 D. */
  TEXCO_STRAND = 1 << 13,
  /** strand is used for normal materials, particle for halo materials */
  TEXCO_PARTICLE = 1 << 13,
  // TEXCO_STRESS = 1 << 14, /* Deprecated. */
  // TEXCO_SPEED = 1 << 15,  /* Deprecated. */
};

/** #MTex::mapto */
enum {
  MAP_COL = 1 << 0,
  MAP_ALPHA = 1 << 7,
};

typedef struct MTex {
  DNA_DEFINE_CXX_METHODS(MTex)

  short texco = TEXCO_UV, mapto = MAP_COL, blendtype = MTEX_BLEND;
  char _pad2[2] = {};
  struct Object *object = nullptr;
  struct Tex *tex = nullptr;
  /** MAX_CUSTOMDATA_LAYER_NAME. */
  char uvname[68] = "";

  char projx = PROJ_X, projy = PROJ_Y, projz = PROJ_Z, mapping = MTEX_FLAT;
  char brush_map_mode = MTEX_MAP_MODE_VIEW, brush_angle_mode = 0;

  /**
   * Match against the texture node (#TEX_NODE_OUTPUT, #bNode::custom1 value).
   * otherwise zero when unspecified (default).
   */
  short which_output = 0;

  float ofs[3] = {0.0f, 0.0f, 0.0f};
  float size[3] = {1.0f, 1.0f, 1.0f};
  float rot = 0;
  float random_angle = 2.0f * (float)M_PI;

  float r = 1.0, g = 0.0, b = 1.0, k = 1.0;
  float def_var = 1.0;

  /* common */
  float colfac = 1.0;
  float alphafac = 1.0f;

  /* particles */
  float timefac = 1.0f, lengthfac = 1.0f, clumpfac = 1.0f, dampfac = 1.0f;
  float kinkfac = 1.0f, kinkampfac = 1.0f, roughfac = 1.0f, padensfac = 1.0f, gravityfac = 1.0f;
  float lifefac = 1.0f, sizefac = 1.0f, ivelfac = 1.0f, fieldfac = 1.0f;
  float twistfac = 1.0f;
} MTex;

/** \} */

/* -------------------------------------------------------------------- */
/** \name #PointDensity
 * \{ */

typedef struct PointDensity {
  DNA_DEFINE_CXX_METHODS(PointDensity)

  short flag = 0;

  short falloff_type = 0;
  float falloff_softness = 0;
  float radius = 0;
  short source = 0;
  char _pad0[2] = {};

  /** psys_color_source */
  short color_source = 0;
  short ob_color_source = 0;

  int totpoints = 0;

  /** for 'Object' or 'Particle system' type - source object */
  struct Object *object = nullptr;
  /** `index + 1` in ob.particle-system, non-ID pointer not allowed. */
  int psys = 0;
  /** cache points in world-space, object space, ... ? */
  short psys_cache_space = 0;
  /** cache points in world-space, object space, ... ? */
  short ob_cache_space = 0;
  /** vertex attribute layer for color source, MAX_CUSTOMDATA_LAYER_NAME */
  char vertex_attribute_name[68] = "";
  char _pad1[4] = {};

  /** The acceleration tree containing points. */
  void *point_tree = nullptr;
  /** Dynamically allocated extra for extra information, like particle age. */
  float *point_data = nullptr;

  float noise_size = 0;
  short noise_depth = 0;
  short noise_influence = 0;
  short noise_basis = 0;
  char _pad2[6] = {};
  float noise_fac = 0;

  float speed_scale = 0, falloff_speed_scale = 0;
  char _pad3[4] = {};
  /** For time -> color */
  struct ColorBand *coba = nullptr;

  /** Falloff density curve. */
  struct CurveMapping *falloff_curve = nullptr;
} PointDensity;

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Tex
 * \{ */

typedef struct Tex {
  DNA_DEFINE_CXX_METHODS(Tex)

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;
  /**
   * Engines draw data, must be immediately after AnimData. See IdDdtTemplate and
   * DRW_drawdatalist_from_id to understand this requirement.
   */
  DrawDataList drawdata;

  float noisesize = 0.25, turbul = 5.0;
  float bright = 1.0, contrast = 1.0, saturation = 1.0, rfac = 1.0, gfac = 1.0, bfac = 1.0;
  float filtersize = 1.0;
  char _pad2[4] = {};

  /* newnoise: musgrave parameters */
  float mg_H = 1.0, mg_lacunarity = 2.0, mg_octaves = 2.0, mg_offset = 1.0, mg_gain = 1.0;

  /* newnoise: distorted noise amount, musgrave & voronoi output scale */
  float dist_amount = 1.0, ns_outscale = 1.0;

  /* newnoise: voronoi nearest neighbor weights, minkovsky exponent,
   * distance metric & color type */
  float vn_w1 = 1.0;
  float vn_w2 = 0.0;
  float vn_w3 = 0.0;
  float vn_w4 = 0.0;
  float vn_mexp = 2.5;
  short vn_distm = 0, vn_coltype = 0;

  /* noisedepth MUST be <= 30 else we get floating point exceptions */
  short noisedepth = 2, noisetype = 0;

  /* newnoise: noisebasis type for clouds/marble/etc, noisebasis2 only used for distorted noise */
  short noisebasis = 0, noisebasis2 = 0;

  short imaflag = TEX_INTERPOL | TEX_MIPMAP | TEX_USEALPHA, flag = TEX_CHECKER_ODD | TEX_NO_CLAMP;
  short type = TEX_IMAGE, stype = 0;

  float cropxmin = 0.0, cropymin = 0.0, cropxmax = 1.0, cropymax = 1.0;
  int texfilter = TXF_EWA;
  /** Anisotropic filter maximum value, EWA -> max eccentricity, feline -> max probes. */
  int afmax = 8;
  short xrepeat = 1, yrepeat = 1;
  short extend = TEX_REPEAT;

  /* Variables only used for versioning, moved to struct member `iuser`. */
  short _pad0 = 0;
  int len DNA_DEPRECATED = 0;
  int frames DNA_DEPRECATED = 0;
  int offset DNA_DEPRECATED = 0;
  int sfra DNA_DEPRECATED = 1;

  float checkerdist = 0, nabla = 0.025 /* also in do_versions. */;
  char _pad1[4] = {};

  struct ImageUser iuser;

  struct bNodeTree *nodetree = nullptr;
  /* old animation system, deprecated for 2.5 */
  struct Ipo *ipo DNA_DEPRECATED = nullptr;
  struct Image *ima = nullptr;
  struct ColorBand *coba = nullptr;
  struct PreviewImage *preview = nullptr;

  char use_nodes = 0;
  char _pad[7] = {};

} Tex;

/** Used for mapping and texture nodes. */
typedef struct TexMapping {
  float loc[3] = {};
  /** Rotation in radians. */
  float rot[3] = {};
  float size[3] = {};
  int flag = 0;
  char projx = 0, projy = 0, projz = 0, mapping = 0;
  int type = 0;

  float mat[4][4] = {};
  float min[3] = {}, max[3] = {};
  struct Object *ob = nullptr;

} TexMapping;

typedef struct ColorMapping {
  struct ColorBand coba;

  float bright = 0, contrast = 0, saturation = 0;
  int flag = 0;

  float blend_color[3] = {};
  float blend_factor = 0;
  int blend_type = 0;
  char _pad[4] = {};
} ColorMapping;

/** \} */
