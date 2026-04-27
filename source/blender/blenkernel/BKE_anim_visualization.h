/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

namespace blender {

struct BlendDataReader;
struct BlendWriter;
struct Object;
struct ReportList;
struct Scene;
struct bAnimVizSettings;
struct bMotionPath;
struct bPoseChannel;
struct wmWindowManager;

/* ---------------------------------------------------- */
/* Animation Visualization */

struct MotionPathRuntime {
  /* Pointer to the window manager holding the job that is calculating the points in the
   * background. Set while the job is running. */
  wmWindowManager *wm = nullptr;
  Scene *job_owner = nullptr;
  /**
   * Has to be called on each `bMotionPath` that is evaluated before WM_jobs_start is called.
   * This is used to correctly kill the job for that motionpath.
   */
  void register_async_job(wmWindowManager *wm, Scene *job_owner);
  void deregister_async_job();
};

/**
 * Initialize the default settings for animation visualization.
 */
void animviz_settings_init(struct bAnimVizSettings *avs);

/**
 * Make a copy of motion-path data, so that viewing with copy on write works.
 */
struct bMotionPath *animviz_copy_motionpath(const struct bMotionPath *mpath_src);

/**
 * Stops the thread evaluating the motion paths and blocks the main thread until it has
 * stopped.
 */
void animviz_stop_motionpath_job_ex(wmWindowManager *wm, Scene *job_owner);

/**
 * Stops the thread calculating the motion path and blocks the main thread until it has
 * stopped. This has to be called before freeing any data that the evaluating thread depends on.
 *
 * \note The job may calculate data for other motion paths as well. Since the whole job will be
 * cancelled other motion paths may be affected.
 *
 * \see MotionPathRuntime.register_async_job
 */
void animviz_stop_motionpath_job(bMotionPath *motion_path);

/**
 * Free the given motion path instance and its data.
 * \note this frees the motion path given!
 */
void animviz_free_motionpath(struct bMotionPath *mpath);

/**
 * Setup motion paths for the given data.
 * \note Only used when explicitly calculating paths on bones which may/may not be consider already
 *
 * \param scene: Current scene (for frame ranges, etc.)
 * \param ob: Object to add paths for (must be provided)
 * \param pchan: Pose-channel to add paths for
 * (optional; if not provided, object-paths are assumed).
 */
struct bMotionPath *animviz_verify_motionpaths(struct ReportList *reports,
                                               struct Scene *scene,
                                               struct Object *ob,
                                               struct bPoseChannel *pchan);

void animviz_motionpath_blend_write(struct BlendWriter *writer, struct bMotionPath *mpath);
void animviz_motionpath_blend_read_data(struct BlendDataReader *reader, struct bMotionPath *mpath);

}  // namespace blender
