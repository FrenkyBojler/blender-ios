/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup imbuf
 */

/**
 * Time-code files contain timestamps (PTS, DTS) and packet seek position.
 * These values are obtained by decoding each frame in movie stream. Time-code types define how
 * these map to frame index in Blender. This is used when seeking in movie stream. Note, that
 * meaning of terms time-code and record run here has little connection to their actual meaning.
 */
enum IMB_Timecode_Type {
  /** Don't use time-code files at all. Use FFmpeg API to seek to PTS calculated on the fly. */
  IMB_TC_NONE = 0,
  /**
   * TC entries (and therefore frames in movie stream) are mapped to frame index, such that
   * timestamp in Blender matches timestamp in the movie stream. This assumes, that time starts at
   * 0 in both cases.
   *
   * Simplified formula is `frame_index = movie_stream_timestamp * FPS`.
   */
  IMB_TC_RECORD_RUN = 1,
  /**
   * Each TC entry (and therefore frame in movie stream) is mapped to new frame index in Blender.
   *
   * For example: FFmpeg may say, that a frame should be displayed for 0.5 seconds, but this option
   * ignores that and only displays it in one particular frame index in Blender.
   */
  IMB_TC_RECORD_RUN_NO_GAPS = 8,
  IMB_TC_NUM_TYPES = 2,
};
