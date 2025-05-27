/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 */

#pragma once

#include "GHOST_Types.h"
#include "GPU_platform_backend_enum.h"

struct wmWindow;
struct wmWindowManager;
struct wmXrData;

using wmXrSessionExitFn = void (*)(const wmXrData *xr_data);

/* `wm_xr.cc` */

bool wm_xr_init(wmWindowManager *wm, eGPUBackendType gpu_backend);
void wm_xr_exit(wmWindowManager *wm);
void wm_xr_session_toggle(wmWindowManager *wm,
                          wmWindow *session_root_win,
                          wmXrSessionExitFn session_exit_fn);
bool wm_xr_events_handle(wmWindowManager *wm);

/**
 * Get the GHOST_XrContextHandle of the active XR context. Store the active XR context handle in
 * r_xr_context.
 *
 * Returns true when r_xr_context is filled with the active XR context. Returns false when no
 * context is present.
 */
bool wm_xr_context_handle_get(const wmWindowManager *wm, GHOST_XrContextHandle *r_xr_context);

/* `wm_xr_operators.cc` */

void wm_xr_operatortypes_register();
