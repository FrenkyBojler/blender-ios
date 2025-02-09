/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "BLI_utildefines.h"

/**
 * Auto-Keying mode.
 * #Scene.autokey_mode
 */
typedef enum eAutokey_Mode {
  /* AUTOKEY_ON is a bit-flag. */
  AUTOKEY_ON = 1,

  /**
   * AUTOKEY_ON + 2**n...  (i.e. AUTOKEY_MODE_NORMAL = AUTOKEY_ON + 2)
   * to preserve setting, even when auto-key turned off.
   */
  AUTOKEY_MODE_NORMAL = 3,
  AUTOKEY_MODE_EDITKEYS = 5,
} eAutokey_Mode;

/* Stereo Flags. */
#define STEREO_RIGHT_NAME "right"
#define STEREO_LEFT_NAME "left"
#define STEREO_RIGHT_SUFFIX "_R"
#define STEREO_LEFT_SUFFIX "_L"

/** #View3D::stereo3d_camera / #View3D::multiview_eye / #ImageUser::multiview_eye */
typedef enum eStereoViews {
  STEREO_LEFT_ID = 0,
  STEREO_RIGHT_ID = 1,
  STEREO_3D_ID = 2,
  STEREO_MONO_ID = 3,
} eStereoViews;

/** #SceneRenderLayer::passflag */
typedef enum eScenePassType {
  SCE_PASS_COMBINED = (1 << 0),
  SCE_PASS_Z = (1 << 1),
  SCE_PASS_UNUSED_1 = (1 << 2), /* RGBA */
  SCE_PASS_UNUSED_2 = (1 << 3), /* DIFFUSE */
  SCE_PASS_UNUSED_3 = (1 << 4), /* SPEC */
  SCE_PASS_SHADOW = (1 << 5),
  SCE_PASS_AO = (1 << 6),
  SCE_PASS_POSITION = (1 << 7),
  SCE_PASS_NORMAL = (1 << 8),
  SCE_PASS_VECTOR = (1 << 9),
  SCE_PASS_UNUSED_5 = (1 << 10), /* REFRACT */
  SCE_PASS_INDEXOB = (1 << 11),
  SCE_PASS_UV = (1 << 12),
  SCE_PASS_UNUSED_6 = (1 << 13), /* INDIRECT */
  SCE_PASS_MIST = (1 << 14),
  SCE_PASS_UNUSED_7 = (1 << 15), /* RAYHITS */
  SCE_PASS_EMIT = (1 << 16),
  SCE_PASS_ENVIRONMENT = (1 << 17),
  SCE_PASS_INDEXMA = (1 << 18),
  SCE_PASS_DIFFUSE_DIRECT = (1 << 19),
  SCE_PASS_DIFFUSE_INDIRECT = (1 << 20),
  SCE_PASS_DIFFUSE_COLOR = (1 << 21),
  SCE_PASS_GLOSSY_DIRECT = (1 << 22),
  SCE_PASS_GLOSSY_INDIRECT = (1 << 23),
  SCE_PASS_GLOSSY_COLOR = (1 << 24),
  SCE_PASS_TRANSM_DIRECT = (1 << 25),
  SCE_PASS_TRANSM_INDIRECT = (1 << 26),
  SCE_PASS_TRANSM_COLOR = (1 << 27),
  SCE_PASS_SUBSURFACE_DIRECT = (1 << 28),
  SCE_PASS_SUBSURFACE_INDIRECT = (1 << 29),
  SCE_PASS_SUBSURFACE_COLOR = (1 << 30),
  SCE_PASS_ROUGHNESS = (1u << 31u),
} eScenePassType;

#define RE_PASSNAME_DEPRECATED "Deprecated"

#define RE_PASSNAME_COMBINED "Combined"
#define RE_PASSNAME_Z "Depth"
#define RE_PASSNAME_VECTOR "Vector"
#define RE_PASSNAME_POSITION "Position"
#define RE_PASSNAME_NORMAL "Normal"
#define RE_PASSNAME_UV "UV"
#define RE_PASSNAME_EMIT "Emit"
#define RE_PASSNAME_SHADOW "Shadow"

#define RE_PASSNAME_AO "AO"
#define RE_PASSNAME_ENVIRONMENT "Env"
#define RE_PASSNAME_INDEXOB "IndexOB"
#define RE_PASSNAME_INDEXMA "IndexMA"
#define RE_PASSNAME_MIST "Mist"

#define RE_PASSNAME_DIFFUSE_DIRECT "DiffDir"
#define RE_PASSNAME_DIFFUSE_INDIRECT "DiffInd"
#define RE_PASSNAME_DIFFUSE_COLOR "DiffCol"
#define RE_PASSNAME_GLOSSY_DIRECT "GlossDir"
#define RE_PASSNAME_GLOSSY_INDIRECT "GlossInd"
#define RE_PASSNAME_GLOSSY_COLOR "GlossCol"
#define RE_PASSNAME_TRANSM_DIRECT "TransDir"
#define RE_PASSNAME_TRANSM_INDIRECT "TransInd"
#define RE_PASSNAME_TRANSM_COLOR "TransCol"

#define RE_PASSNAME_SUBSURFACE_DIRECT "SubsurfaceDir"
#define RE_PASSNAME_SUBSURFACE_INDIRECT "SubsurfaceInd"
#define RE_PASSNAME_SUBSURFACE_COLOR "SubsurfaceCol"

#define RE_PASSNAME_FREESTYLE "Freestyle"
#define RE_PASSNAME_BLOOM "BloomCol"
#define RE_PASSNAME_VOLUME_LIGHT "VolumeDir"
#define RE_PASSNAME_TRANSPARENT "Transp"

#define RE_PASSNAME_CRYPTOMATTE_OBJECT "CryptoObject"
#define RE_PASSNAME_CRYPTOMATTE_ASSET "CryptoAsset"
#define RE_PASSNAME_CRYPTOMATTE_MATERIAL "CryptoMaterial"

/** #ToolSettings.vgroupsubset */
typedef enum eVGroupSelect {
  WT_VGROUP_ALL = 0,
  WT_VGROUP_ACTIVE = 1,
  WT_VGROUP_BONE_SELECT = 2,
  WT_VGROUP_BONE_DEFORM = 3,
  WT_VGROUP_BONE_DEFORM_OFF = 4,
} eVGroupSelect;

typedef enum eSeqImageFitMethod {
  SEQ_SCALE_TO_FIT,
  SEQ_SCALE_TO_FILL,
  SEQ_STRETCH_TO_FILL,
  SEQ_USE_ORIGINAL_SIZE,
} eSeqImageFitMethod;

/**
 * #Paint::symmetry_flags
 * (for now just a duplicate of sculpt symmetry flags).
 */
typedef enum ePaintSymmetryFlags {
  PAINT_SYMM_NONE = 0,
  PAINT_SYMM_X = (1 << 0),
  PAINT_SYMM_Y = (1 << 1),
  PAINT_SYMM_Z = (1 << 2),
  PAINT_SYMMETRY_FEATHER = (1 << 3),
  PAINT_TILE_X = (1 << 4),
  PAINT_TILE_Y = (1 << 5),
  PAINT_TILE_Z = (1 << 6),
} ePaintSymmetryFlags;
ENUM_OPERATORS(ePaintSymmetryFlags, PAINT_TILE_Z);
#define PAINT_SYMM_AXIS_ALL (PAINT_SYMM_X | PAINT_SYMM_Y | PAINT_SYMM_Z)

#ifdef __cplusplus
inline ePaintSymmetryFlags operator++(ePaintSymmetryFlags &flags, int)
{
  flags = ePaintSymmetryFlags(char(flags) + 1);
  return flags;
}
#endif
