/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "DNA_scene_types.h"
#include "DNA_space_types.h" /* FILE_MAX */

namespace blender {

struct Image;
struct ImageUser;
struct Main;
struct RenderResult;
struct ReportList;
struct Scene;

/* -------------------------------------------------------------------- */
/** \name Background Image Save Pool
 *
 * Task pool for saving render images asynchronously. Used by both
 * the render pipeline and image save operators.
 * \{ */

/**
 * Optionally pre-initialize the background image save task pool.
 * The pool is initialized lazily on first use, so calling this is optional.
 * Use this for eager initialization at startup if desired.
 */
void BKE_image_save_pool_init();

/** Wait for pending background image saves to complete. */
void BKE_image_save_pool_wait();

/** Wait for pending background saves and free the task pool. Call at application shutdown. */
void BKE_image_save_pool_exit();

/** \} */

/* Image datablock saving. */

struct ImageSaveOptions {
  /** Context within which image is saved. */
  Main *bmain = nullptr;
  /** Scene for color management and format settings. */
  Scene *scene = nullptr;

  /** Image format settings (owned, use BKE_image_format_copy for deep copy). */
  ImageFormatData im_format;
  /** Absolute file path for output. */
  char filepath[FILE_MAX] = "";

  /** Whether to save relative path. */
  bool relative = false;
  /** Whether to save a copy (don't update image filepath). */
  bool save_copy = false;
  /** Whether saving as render output (affects color management). */
  bool save_as_render = false;
  /** Whether to update image filepath after save. */
  bool do_newpath = false;

  /** Original image type, for restoring when type changes back. */
  int orig_imtype = 0;
  /** Original colorspace name. */
  char orig_colorspace[/*MAX_COLORSPACE_NAME*/ 64] = "";

  /** Previous save_as_render value for auto updates in UI. */
  bool prev_save_as_render = false;
  /** Previous image type for auto updates in UI. */
  int prev_imtype = 0;
};

bool BKE_image_save_options_init(ImageSaveOptions *opts,
                                 Main *bmain,
                                 Scene *scene,
                                 Image *ima,
                                 ImageUser *iuser,
                                 bool guess_path,
                                 bool save_as_render);
void BKE_image_save_options_update(ImageSaveOptions *opts, const Image *image);
void BKE_image_save_options_free(ImageSaveOptions *opts);

bool BKE_image_save(
    ReportList *reports, Main *bmain, Image *ima, ImageUser *iuser, const ImageSaveOptions *opts);

/* -------------------------------------------------------------------- */
/** \name Background Image Saving
 * \{ */

/**
 * Queue a background save for a render result.
 * Falls back to synchronous save if background saving is not possible.
 *
 * \param rr: Render result to save.
 * \param scene: Scene for image format settings.
 * \param stamp: Whether to add stamp metadata.
 * \param filepath: Output file path.
 * \return true if the background task was successfully queued, false if the caller should
 *         fall back to synchronous saving (e.g., queue is full or format is unsupported).
 */
bool BKE_image_save_background_render(RenderResult *rr,
                                      const Scene *scene,
                                      bool stamp,
                                      const char *filepath);

/**
 * Queue a background save for an Image datablock.
 * Falls back to synchronous save if background saving is not possible.
 *
 * \param ima: Image to save.
 * \param iuser: Image user for acquiring the buffer.
 * \param opts: Save options including filepath and format.
 * \return true if the background task was successfully queued, false if the caller should
 *         fall back to synchronous saving (e.g., queue is full or format is unsupported).
 */
bool BKE_image_save_background(Image *ima, ImageUser *iuser, const ImageSaveOptions *opts);

/** \} */

/* Render saving.
 *
 * Note on naming: BKE_image_render_write_* functions operate on RenderResult data
 * and write directly to files. They differ from BKE_image_save_* which operate on
 * Image datablocks and handle additional state management. The "render_write" naming
 * emphasizes the direct-to-disk nature without Image datablock state changes. */

/**
 * Save single or multi-layer OpenEXR files from the render result.
 * Optionally saves only a specific view or layer.
 */
bool BKE_image_render_write_exr(ReportList *reports,
                                const RenderResult *rr,
                                const char *filepath,
                                const ImageFormatData *imf,
                                bool save_as_render,
                                const char *view,
                                int layer);

/**
 * \param filepath_basis: May be used as-is, or used as a basis for multi-view images.
 * \param format: The image format to use for saving, if null, the scene format will be used.
 */
bool BKE_image_render_write(ReportList *reports,
                            RenderResult *rr,
                            const Scene *scene,
                            bool stamp,
                            const char *filepath_basis,
                            const ImageFormatData *format = nullptr,
                            bool save_as_render = true);

}  // namespace blender
