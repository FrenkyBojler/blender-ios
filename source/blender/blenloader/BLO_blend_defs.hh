/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup blenloader
 * \brief defines for blend-file codes.
 */

/* INTEGER CODES */
#ifdef __BIG_ENDIAN__
/* Big Endian */
#  define BLEND_MAKE_ID(a, b, c, d) ((int)(a) << 24 | (int)(b) << 16 | (c) << 8 | (d))
#else
/* Little Endian */
#  define BLEND_MAKE_ID(a, b, c, d) ((int)(d) << 24 | (int)(c) << 16 | (b) << 8 | (a))
#endif

/**
 * Codes used for #BHead.code.
 *
 * These coexist with ID codes such as #ID_OB, #ID_SCE ... etc.
 */
enum {
  /**
   * Arbitrary allocated memory
   * (typically owned by #ID's, will be freed when there are no users).
   */
  BLO_CODE_DATA = BLEND_MAKE_ID('D', 'A', 'T', 'A'),
  /**
   * Used for #Global struct.
   */
  BLO_CODE_GLOB = BLEND_MAKE_ID('G', 'L', 'O', 'B'),
  /**
   * Used for storing the encoded SDNA string
   * (decoded into an #SDNA on load).
   */
  BLO_CODE_DNA1 = BLEND_MAKE_ID('D', 'N', 'A', '1'),
  /**
   * Used to store thumbnail previews, written between #REND and #GLOB blocks,
   * (ignored for regular file reading).
   */
  BLO_CODE_TEST = BLEND_MAKE_ID('T', 'E', 'S', 'T'),
  /**
   * Used for #RenderInfo, basic Scene and frame range info,
   * can be easily read by other applications without writing a full blend file parser.
   */
  BLO_CODE_REND = BLEND_MAKE_ID('R', 'E', 'N', 'D'),
  /**
   * Used for #UserDef, (user-preferences data).
   * (written to #BLENDER_STARTUP_FILE & #BLENDER_USERPREF_FILE).
   */
  BLO_CODE_USER = BLEND_MAKE_ID('U', 'S', 'E', 'R'),
  /**
   * Terminate reading (no data).
   */
  BLO_CODE_ENDB = BLEND_MAKE_ID('E', 'N', 'D', 'B'),
};

#define BLEN_THUMB_MEMSIZE_FILE(_x, _y) (sizeof(int) * (2 + (size_t)(_x) * (size_t)(_y)))

/**
 * A low level blend file version number. Also see #decode_blender_header for how the first few
 * bytes of a .blend file are structured.
 *
 * 0: Uses #BHead4 or #SmallBHead8 for block headers depending on a .blend file header byte.
 * 1: Uses #LargeBHead8 for block headers.
 */
#define BLEND_FILE_VERSION_FORMAT_0 0
#define BLEND_FILE_VERSION_FORMAT_1 1

/**
 * Only "modern" systems support writing files with #LargeBHead8 headers. Other systems are
 * deprecated. The reduces the amount of variation we have to deal with when reading .blend files.
 */
#define SYSTEM_SUPPORTS_WRITING_FILE_VERSION_1 (ENDIAN_ORDER == L_ENDIAN && sizeof(void *) == 8)
