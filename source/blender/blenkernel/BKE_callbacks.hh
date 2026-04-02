/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

namespace blender {

struct Depsgraph;
struct ID;
struct Main;
struct PointerRNA;

/**
 * Callbacks for One Off Actions
 * =============================
 *
 * - `{ACTION}` use in cases where only a single callback is required,
 *   `VERSION_UPDATE` and `RENDER_STATS` for example.
 *
 * \note avoid single callbacks if there is a chance `PRE/POST` are useful to differentiate
 * since renaming callbacks may break Python scripts.
 *
 * Callbacks for Common Actions
 * ============================
 *
 * - `{ACTION}_PRE` run before the action.
 * - `{ACTION}_POST` run after the action.
 *
 * Optional Additional Callbacks
 * -----------------------------
 *
 * - `{ACTION}_INIT` when the handler may manipulate the context used to run the action.
 *
 *   Examples where `INIT` functions may be useful are:
 *
 *   - When rendering, an `INIT` function may change the camera or render settings,
 *     things which a `PRE` function can't support as this information has already been used.
 *   - When saving an `INIT` function could temporarily change the preferences.
 *
 * - `{ACTION}_POST_FAIL` should be included if the action may fail.
 *
 *   Use this so a call to the `PRE` callback always has a matching call to `POST` or `POST_FAIL`.
 *
 * \note in most cases only `PRE/POST` are required.
 *
 * Callbacks for Background/Modal Tasks
 * ====================================
 *
 * - `{ACTION}_INIT`
 * - `{ACTION}_COMPLETE` when a background job has finished.
 * - `{ACTION}_CANCEL` When a background job is canceled partway through.
 *
 *   While cancellation may be caused by any number of reasons, common causes may include:
 *
 *   - Explicit user cancellation.
 *   - Exiting Blender.
 *   - Failure to acquire resources (such as disk-full, out of memory ... etc).
 *
 * \note `PRE/POST` handlers may be used along side modal task handlers
 * as is the case for rendering, where rendering an animation uses modal task handlers,
 * rendering a single frame has `PRE/POST` handlers.
 *
 * Python Access
 * =============
 *
 * All callbacks here must be exposed via the Python module `bpy.app.handlers`,
 * see `bpy_app_handlers.cc`.
 */
enum eCbEvent {
  BkeCbEvtFrameChangePre,
  BkeCbEvtFrameChangePost,
  BkeCbEvtRenderPre,
  BkeCbEvtRenderPost,
  BkeCbEvtRenderWrite,
  BkeCbEvtRenderStats,
  BkeCbEvtRenderInit,
  BkeCbEvtRenderComplete,
  BkeCbEvtRenderCancel,
  BkeCbEvtLoadPre,
  BkeCbEvtLoadPost,
  BkeCbEvtLoadPostFail,
  BkeCbEvtSavePre,
  BkeCbEvtSavePost,
  BkeCbEvtSavePostFail,
  BkeCbEvtUndoPre,
  BkeCbEvtUndoPost,
  BkeCbEvtRedoPre,
  BkeCbEvtRedoPost,
  BkeCbEvtDepsgraphUpdatePre,
  BkeCbEvtDepsgraphUpdatePost,
  BkeCbEvtVersionUpdate,
  BkeCbEvtLoadFactoryUserdefPost,
  BkeCbEvtLoadFactoryStartupPost,
  BkeCbEvtXrSessionStartPre,
  BkeCbEvtAnnotationPre,
  BkeCbEvtAnnotationPost,
  BkeCbEvtObjectBakePre,
  BkeCbEvtObjectBakeComplete,
  BkeCbEvtObjectBakeCancel,
  BkeCbEvtCompositePre,
  BkeCbEvtCompositePost,
  BkeCbEvtCompositeCancel,
  BkeCbEvtAnimationPlaybackPre,
  BkeCbEvtAnimationPlaybackPost,
  BkeCbEvtTranslationUpdatePost,
  BkeCbEvtExtensionReposUpdatePre,
  BkeCbEvtExtensionReposUpdatePost,
  BkeCbEvtExtensionReposSync,
  BkeCbEvtExtensionReposFilesClear,
  BkeCbEvtBlendimportPre,
  BkeCbEvtBlendimportPost,
  BkeCbEvtExitPre,
  BkeCbEvtTot,
};

struct bCallbackFuncStore {
  bCallbackFuncStore *next, *prev;
  void (*func)(Main *, PointerRNA **, int pointers_num, void *arg);
  void *arg;
  short alloc;
};

void BKE_callback_exec(Main *bmain, PointerRNA **pointers, int pointers_num, eCbEvent evt);
void BKE_callback_exec_null(Main *bmain, eCbEvent evt);
void BKE_callback_exec_id(Main *bmain, ID *id, eCbEvent evt);
void BKE_callback_exec_id_depsgraph(Main *bmain, ID *id, Depsgraph *depsgraph, eCbEvent evt);
void BKE_callback_exec_boolean(Main *bmain, bool value, eCbEvent evt);
void BKE_callback_exec_string(Main *bmain, const char *str, eCbEvent evt);
void BKE_callback_add(bCallbackFuncStore *funcstore, eCbEvent evt);
void BKE_callback_remove(bCallbackFuncStore *funcstore, eCbEvent evt);

void BKE_callback_global_init();
/**
 * Call on application exit.
 */
void BKE_callback_global_finalize();

}  // namespace blender
