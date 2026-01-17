/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 */

#pragma once

#include "BKE_image_save.hh"

struct bContext;
struct Image;
struct ImageUser;
struct RenderResult;
struct ReportList;
struct Scene;

namespace blender::ed::space_image {

/** Initialize the background image save task pool. Called lazily on first use. */
void image_save_pool_init();

/** Wait for pending saves and free the task pool. Call at shutdown. */
void image_save_pool_exit();

/**
 * Attempt to save an image in a background thread using BLI_task.
 * Works in both interactive and background render mode.
 *
 * \param C: Context (unused, kept for API consistency).
 * \param ima: Image to save.
 * \param iuser: Image user for acquiring the buffer.
 * \param opts: Save options including filepath and format.
 * \return true if the background task was successfully queued, false if the caller should
 *         fall back to synchronous saving (e.g., queue is full or format is unsupported).
 */
bool image_save_background(bContext *C,
                           Image *ima,
                           ImageUser *iuser,
                           const ImageSaveOptions *opts);

/**
 * Attempt to save a render result in a background thread using BLI_task.
 * Works in both interactive and background render mode.
 *
 * \param reports: Report list for errors.
 * \param rr: Render result to save.
 * \param scene: Scene for image format settings.
 * \param stamp: Whether to add stamp metadata.
 * \param filepath: Output file path.
 * \return true if the background task was successfully queued, false if the caller should
 *         fall back to synchronous saving (e.g., queue is full or format is unsupported).
 */
bool image_save_background_render(ReportList *reports,
                                  RenderResult *rr,
                                  const Scene *scene,
                                  bool stamp,
                                  const char *filepath);

}  // namespace blender::ed::space_image
