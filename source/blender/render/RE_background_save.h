/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup render
 *
 * Unified background image save system for both viewport and final rendering.
 *
 * This module provides asynchronous image saving capabilities that are used by:
 * - Viewport rendering (render_opengl.cc)
 * - Final rendering (pipeline.cc)
 *
 * Key design principles:
 * - All scene-dependent data is extracted BEFORE queueing (no scene pointer in background tasks)
 * - Full RenderResult deep copy for thread safety
 * - Automatic memory-based heuristics for deciding sync vs async saves
 * - Serial execution for movies (frame ordering), parallel for images
 *
 * Note: Movie formats are intentionally excluded from this API. Viewport/movie output relies on
 * ordered, serialized writers and remains handled by the existing movie pipeline (e.g.
 * render_opengl.cc). Movies require ordered frame writes for codec stream integrity, which is
 * fundamentally incompatible with parallel background saves.
 */

#pragma once

#include <cstddef>

#include "RE_pipeline.h"

/* Forward declarations in blender namespace. */
namespace blender {
struct Object;
struct Scene;
}  // namespace blender

/* -------------------------------------------------------------------- */
/** \name Initialization and Shutdown
 * \{ */

/**
 * Initialize the background save system.
 * Called at startup or lazily on first use.
 * Safe to call multiple times.
 */
void RE_background_save_init();

/**
 * Shutdown and cleanup the background save system.
 * Waits for all pending saves to complete before returning.
 * Called at application exit.
 */
void RE_background_save_exit();

/**
 * Wait for all pending background saves to complete.
 * Use this at the end of animation rendering to ensure all frames are written.
 */
void RE_background_save_wait();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Queuing
 * \{ */

/**
 * Queue a background save for a render result.
 *
 * This function extracts ALL scene-dependent data immediately:
 * - Stamp metadata (frame number, note, camera, render time, memory, etc.)
 * - Image format settings (deep copy)
 * - Deep copy of RenderResult
 *
 * The background task has NO scene pointer - all data is self-contained.
 *
 * Complex formats (multilayer EXR, multiview, movies) are rejected - the caller
 * should fall back to synchronous saving in these cases.
 *
 * \param rr: Render result (will be deep copied).
 * \param scene: Scene for data extraction (NOT stored in task).
 * \param camera: Camera for stamp data (NOT stored in task).
 * \param filepath: Output file path.
 * \return true if successfully queued, false if caller should use sync save.
 */
bool RE_background_save_render(blender::RenderResult *rr,
                               const blender::Scene *scene,
                               const blender::Object *camera,
                               const char *filepath);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Heuristics
 * \{ */

/**
 * Check if background save should be used based on memory heuristics.
 *
 * Returns false (suggesting sync save) when:
 * - Peak render memory exceeds 50% of system memory
 * - Less than 500MB available memory
 * - Background queue is heavily loaded (>= MAX_SCHEDULED_FRAMES)
 *
 * \param peak_memory_mb: Peak memory usage from current render in MB.
 * \return true if background save is recommended, false for sync save.
 */
bool RE_background_save_should_use(size_t peak_memory_mb);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Error Reporting
 * \{ */

/**
 * Get the number of background saves that failed since last clear.
 * Check this after RE_background_save_wait() to detect errors.
 */
int RE_background_save_get_failed_count();

/**
 * Clear the failed save counter.
 * Call this after reporting errors to the user.
 */
void RE_background_save_clear_failed_count();

/**
 * Drain completed background saves and return count.
 * Call this periodically during animation render.
 * Note: Callbacks may fire out-of-frame-order since async saves complete in arbitrary order.
 *
 * \param out_frames: Array to receive completed frame numbers (can be NULL).
 * \param max_frames: Maximum frames to return.
 * \return Number of completed saves drained.
 */
int RE_background_save_drain_completed(int *out_frames, int max_frames);

/**
 * Push a frame number to the completion queue.
 * Used for sync saves so they also fire RENDER_WRITE via drain.
 */
void RE_background_save_push_completed(int frame);

/**
 * Check if there are pending background saves.
 * Used to determine if drain job should continue.
 */
bool RE_background_save_has_pending();

/** \} */
