/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * All XR functionality is accessed through a #GHOST_XrContext handle.
 * The lifetime of this context also determines the lifetime of the OpenXR instance, which is the
 * representation of the OpenXR runtime connection within the application.
 */

#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"
#include "BKE_workspace.hh"

#include "DNA_scene_types.h"
#include "DNA_windowmanager_types.h"
#include "DNA_workspace_types.h"

#include "BLI_listbase.hh"
#include "BLI_string.hh"

#include "ED_screen.hh"
#include "UI_interface_c.hh"

#include "GHOST_IXrContext.hh"
#include "GHOST_Types.hh"
#include "GHOST_Xr-api.hh"

#include "GPU_context.hh"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"

#include "wm_window.hh"
#include "wm_xr_intern.hh"

namespace blender {

struct wmXrErrorHandlerData {
  wmWindowManager *wm;
};

static wmWindow *wm_xr_session_virtual_window_create(bContext *C, wmWindowManager *wm)
{
  wmWindow *root_win = CTX_wm_window(C);
  if (root_win == nullptr) {
    return nullptr;
  }

  Main *bmain = CTX_data_main(C);
  wmWindow *xr_win = wm_window_new(bmain, wm, nullptr, false);
  xr_win->scene = root_win->scene;
  xr_win->posx = root_win->posx;
  xr_win->posy = root_win->posy;
  xr_win->sizex = root_win->sizex;
  xr_win->sizey = root_win->sizey;
  xr_win->windowstate = root_win->windowstate;
  xr_win->active = root_win->active;
  BLI_strncpy(xr_win->view_layer_name, root_win->view_layer_name, sizeof(xr_win->view_layer_name));

  WorkSpace *workspace = WM_window_get_active_workspace(root_win);
  WorkSpaceLayout *layout = WM_window_get_active_layout(root_win);
  if (workspace != nullptr) {
    BKE_workspace_active_set(xr_win->workspace_hook, workspace);
    if (layout != nullptr) {
      BKE_workspace_active_layout_set(xr_win->workspace_hook, xr_win->winid, workspace, layout);
    }
  }

  xr_win->runtime->eventstate = MEM_new<wmEvent>("xr virtual window eventstate");
  if (root_win->runtime->eventstate != nullptr) {
    *xr_win->runtime->eventstate = *root_win->runtime->eventstate;
  }
  else {
    *xr_win->runtime->eventstate = wmEvent{};
  }

  return xr_win;
}

static bScreen *wm_xr_session_virtual_screen_create(wmWindow *xr_win,
                                                    ScrArea *xr_operator_area,
                                                    WorkSpaceLayout **r_layout)
{
  bScreen *screen = MEM_new<bScreen>(__func__);
  screen->temp = true;
  screen->winid = xr_win->winid;
  screen->do_draw = true;
  screen->do_refresh = true;
  screen->redraws_flag = TIME_ALL_3D_WIN | TIME_ALL_ANIM_WIN;
  if (xr_operator_area != nullptr) {
    BLI_addtail(&screen->areabase, xr_operator_area);
  }

  WorkSpaceLayout *layout = MEM_new<WorkSpaceLayout>(__func__);
  layout->screen = screen;
  BLI_strncpy(layout->name, "XR", sizeof(layout->name));
  *r_layout = layout;
  return screen;
}

/* -------------------------------------------------------------------- */

static void wm_xr_error_handler(const GHOST_XrError *error)
{
  wmXrErrorHandlerData *handler_data = static_cast<wmXrErrorHandlerData *>(error->customdata);
  wmWindowManager *wm = handler_data->wm;
  wmWindow *xr_root_win = nullptr;
  if (wm->xr.runtime != nullptr) {
    xr_root_win = wm->xr.runtime->desktop_root_win ? wm->xr.runtime->desktop_root_win :
                                                     CTX_wm_window(wm->xr.runtime->b_context);
  }

  BKE_reports_clear(&wm->runtime->reports);
  WM_global_report(RPT_ERROR, error->user_message);
  /* Internally rely on the first WM window as a fallback when `xr_root_win` is nullptr. */
  WM_report_banner_show(wm, xr_root_win);

  if (wm->xr.runtime) {
    /* Just play safe and destroy the entire runtime data, including context. */
    wm_xr_runtime_data_free(&wm->xr.runtime);
  }
}

bool wm_xr_init(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (wm->xr.runtime && wm->xr.runtime->ghost_context) {
    return true;
  }
  static wmXrErrorHandlerData error_customdata;

  /* Set up error handling. */
  error_customdata.wm = wm;
  GHOST_XrErrorHandler(wm_xr_error_handler, &error_customdata);

  {
    Vector<GHOST_TXrGraphicsBinding> gpu_bindings_candidates;
    switch (GPU_backend_get_type()) {
#ifdef WITH_OPENGL_BACKEND
      case GPU_BACKEND_OPENGL:
        gpu_bindings_candidates.append(GHOST_kXrGraphicsOpenGL);
#  ifdef WIN32
        gpu_bindings_candidates.append(GHOST_kXrGraphicsOpenGLD3D11);
#  endif
        break;
#endif

#ifdef WITH_VULKAN_BACKEND
      case GPU_BACKEND_VULKAN:
        gpu_bindings_candidates.append(GHOST_kXrGraphicsVulkan);
#  ifdef WIN32
        gpu_bindings_candidates.append(GHOST_kXrGraphicsVulkanD3D11);
#  endif
        break;
#endif

#ifdef WITH_METAL_BACKEND
      case GPU_BACKEND_METAL:
        gpu_bindings_candidates.append(GHOST_kXrGraphicsMetal);
        break;
#endif

      default:
        break;
    }

    GHOST_XrContextCreateInfo create_info{
        /*gpu_binding_candidates*/ gpu_bindings_candidates.data(),
        /*gpu_binding_candidates_count*/ uint32_t(gpu_bindings_candidates.size()),
    };
    if (G.debug & G_DEBUG_XR) {
      create_info.context_flag |= GHOST_kXrContextDebug;
    }
    if (G.debug & G_DEBUG_XR_TIME) {
      create_info.context_flag |= GHOST_kXrContextDebugTime;
    }
#ifdef WIN32
    if (GPU_type_matches(GPU_DEVICE_NVIDIA, GPU_OS_WIN, GPU_DRIVER_ANY)) {
      create_info.context_flag |= GHOST_kXrContextGpuNVIDIA;
    }
#endif

    GHOST_IXrContext *ghost_context;
    if (!(ghost_context = GHOST_XrContextCreate(&create_info))) {
      return false;
    }

    /* Set up context callbacks. */
    GHOST_XrGraphicsContextBindFuncs(ghost_context,
                                     wm_xr_session_gpu_binding_context_create,
                                     wm_xr_session_gpu_binding_context_destroy);
    GHOST_XrDrawViewFunc(ghost_context, wm_xr_draw_view);
    GHOST_XrPassthroughEnabledFunc(ghost_context, wm_xr_passthrough_enabled);
    GHOST_XrDisablePassthroughFunc(ghost_context, wm_xr_disable_passthrough);

    if (!wm->xr.runtime) {
      wm->xr.runtime = wm_xr_runtime_data_create();
      wm->xr.runtime->ghost_context = ghost_context;

      /* Create a minimal XR-specific context. */
      wm->xr.runtime->b_context = CTX_create();
      wm->xr.runtime->desktop_root_win = CTX_wm_window(C);
      wm->xr.runtime->xr_window = wm_xr_session_virtual_window_create(C, wm);
      if (wm->xr.runtime->xr_window == nullptr) {
        CTX_free(wm->xr.runtime->b_context);
        MEM_SAFE_DELETE(wm->xr.runtime);
        GHOST_XrContextDestroy(ghost_context);
        return false;
      }
      /* Base Main and WM pointers. */
      CTX_wm_manager_set(wm->xr.runtime->b_context, CTX_wm_manager(C));
      CTX_data_main_set(wm->xr.runtime->b_context, CTX_data_main(C));

      /* Create the XR offscreen area (independent of any bScreen). */
      wm->xr.runtime->xr_operator_area = ED_area_offscreen_create(wm->xr.runtime->xr_window,
                                                                  SPACE_VIEW3D);
      if (wm->xr.runtime->xr_operator_area != nullptr) {
        wm->xr.runtime->xr_screen = wm_xr_session_virtual_screen_create(
            wm->xr.runtime->xr_window,
            wm->xr.runtime->xr_operator_area,
            &wm->xr.runtime->xr_layout);
      }
      if (wm->xr.runtime->xr_operator_area == nullptr || wm->xr.runtime->xr_screen == nullptr ||
          wm->xr.runtime->xr_layout == nullptr)
      {
        if (wm->xr.runtime->xr_layout != nullptr) {
          MEM_delete(wm->xr.runtime->xr_layout);
          wm->xr.runtime->xr_layout = nullptr;
        }
        if (wm->xr.runtime->xr_screen != nullptr) {
          MEM_delete(wm->xr.runtime->xr_screen);
          wm->xr.runtime->xr_screen = nullptr;
        }
        if (wm->xr.runtime->xr_operator_area != nullptr) {
          ED_area_offscreen_free(wm, wm->xr.runtime->xr_window, wm->xr.runtime->xr_operator_area);
        }
        wm->xr.runtime->xr_operator_area = nullptr;
        BLI_remlink(&wm->windows, wm->xr.runtime->xr_window);
        wm_window_free(wm->xr.runtime->b_context, wm, wm->xr.runtime->xr_window);
        wm->xr.runtime->xr_window = nullptr;
        CTX_free(wm->xr.runtime->b_context);
        MEM_SAFE_DELETE(wm->xr.runtime);
        GHOST_XrContextDestroy(ghost_context);
        return false;
      }
      if (WorkSpace *workspace = WM_window_get_active_workspace(wm->xr.runtime->desktop_root_win))
      {
        BKE_workspace_active_set(wm->xr.runtime->xr_window->workspace_hook, workspace);
      }
      wm->xr.runtime->xr_window->workspace_hook->act_layout = wm->xr.runtime->xr_layout;
      if (wm->xr.runtime->xr_operator_area != nullptr) {
        wm->xr.runtime->xr_operator_region = BKE_area_find_region_type(
            wm->xr.runtime->xr_operator_area, RGN_TYPE_WINDOW);
      }
      WM_xr_session_context_ensure(&wm->xr, wm);
    }
  }
  BLI_assert(wm->xr.runtime && wm->xr.runtime->ghost_context && wm->xr.runtime->b_context);

  return true;
}

void wm_xr_exit(wmWindowManager *wm)
{
  if (wm->xr.runtime != nullptr) {
    wm_xr_runtime_data_free(&wm->xr.runtime);
  }

  /* See #wm_xr_data_free for logic that frees window-manager XR data
   * that may exist even when built without XR. */
}

bool wm_xr_events_handle(wmWindowManager *wm)
{
  if (wm->xr.runtime && wm->xr.runtime->ghost_context) {
    GHOST_XrEventsHandle(wm->xr.runtime->ghost_context);

    /* Process OpenXR action events. */
    if (WM_xr_session_is_ready(&wm->xr)) {
      wm_xr_session_actions_update(wm);
    }

    /* #wm_window_events_process() uses the return value to determine if it can put the main thread
     * to sleep for some milliseconds. We never want that to happen while the VR session runs on
     * the main thread. So always return true. */
    return true;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name XR Runtime Data
 * \{ */

wmXrRuntimeData *wm_xr_runtime_data_create()
{
  wmXrRuntimeData *runtime = MEM_new_zeroed<wmXrRuntimeData>(__func__);
  return runtime;
}

void wm_xr_runtime_data_free(wmXrRuntimeData **runtime)
{
  /* This function may be called recursively via the #GHOST_XrContextDestroy session exit callback.
   * Guard against double-free by nulling pointers after freeing. */

  /* Destroy context if still alive. */
  if ((*runtime)->ghost_context != nullptr) {
    GHOST_IXrContext *ghost_context = (*runtime)->ghost_context;
    /* Set to nullptr before calling XrContextDestroy to prevent recursive calls. */
    (*runtime)->ghost_context = nullptr;

    GHOST_XrContextDestroy(ghost_context);
  }

  /* Free remaining runtime data. */
  if (*runtime != nullptr) {
    ScrArea *xr_operator_area = (*runtime)->xr_operator_area;
    bScreen *xr_screen = (*runtime)->xr_screen;
    WorkSpaceLayout *xr_layout = (*runtime)->xr_layout;
    BLI_assert(xr_operator_area);

    wmWindowManager *wm = static_cast<wmWindowManager *>(G_MAIN->wm.first);
    wmWindow *xr_win = (*runtime)->xr_window ?
                           (*runtime)->xr_window :
                           wm_xr_desktop_root_window_or_fallback_get(wm, (*runtime));
    bContext *xr_context = (*runtime)->b_context;

    CTX_wm_window_set(xr_context, xr_win);
    CTX_wm_area_set(xr_context, xr_operator_area);
    for (ARegion *region = static_cast<ARegion *>(xr_operator_area->regionbase.first);
         region != nullptr;
         region = region->next)
    {
      CTX_wm_region_set(xr_context, region);
      WM_event_remove_handlers(xr_context, &region->runtime->handlers);
      ui::UI_region_free_active_but_all(xr_context, region);
      ED_region_panels_exit_active_state(xr_context, region);
      ui::blocklist_free(xr_context, region);
      BKE_area_region_panels_free(&region->panels);
    }
    CTX_wm_region_set(xr_context, nullptr);

    if (wmXrSurfaceData *surface_data = WM_xr_surface_data_get()) {
      for (wmXrUiRegion *panel = static_cast<wmXrUiRegion *>(surface_data->ui_regions.first); panel != nullptr;
           panel = panel->next)
      {
        if (panel->ui_region_host_area == nullptr) {
          continue;
        }
        CTX_wm_area_set(xr_context, panel->ui_region_host_area);
        for (ARegion *region = static_cast<ARegion *>(panel->ui_region_host_area->regionbase.first);
             region != nullptr;
             region = region->next)
        {
          CTX_wm_region_set(xr_context, region);
          WM_event_remove_handlers(xr_context, &region->runtime->handlers);
          ui::UI_region_free_active_but_all(xr_context, region);
          ED_region_panels_exit_active_state(xr_context, region);
          ui::blocklist_free(xr_context, region);
          BKE_area_region_panels_free(&region->panels);
        }
        CTX_wm_region_set(xr_context, nullptr);
        WM_event_remove_handlers_by_area(&xr_win->runtime->handlers, panel->ui_region_host_area);
        if (xr_screen != nullptr) {
          BLI_remlink(&xr_screen->areabase, panel->ui_region_host_area);
        }
        ED_area_offscreen_free(wm, xr_win, panel->ui_region_host_area);
        panel->ui_region_host_area = nullptr;
        panel->ui_region_host_region = nullptr;
        panel->ui_region_host_win = nullptr;
      }
      CTX_wm_area_set(xr_context, nullptr);
    }

    WM_event_remove_handlers_by_area(&xr_win->runtime->handlers, xr_operator_area);
    if (xr_screen != nullptr) {
      CTX_wm_screen_set(xr_context, xr_screen);
      WM_tooltip_clear(xr_context, xr_win);
      BLI_remlink(&xr_screen->areabase, xr_operator_area);
    }
    ED_area_offscreen_free(wm, xr_win, xr_operator_area);
    (*runtime)->xr_operator_area = nullptr;

    if ((*runtime)->xr_window != nullptr) {
      BLI_remlink(&wm->windows, (*runtime)->xr_window);
      wm_window_free(xr_context, wm, (*runtime)->xr_window);
      (*runtime)->xr_window = nullptr;
    }
    if (xr_layout != nullptr) {
      MEM_delete(xr_layout);
      (*runtime)->xr_layout = nullptr;
    }
    if (xr_screen != nullptr) {
      MEM_delete(xr_screen);
      (*runtime)->xr_screen = nullptr;
    }

    CTX_free((*runtime)->b_context);

    wm_xr_session_data_free(&(*runtime)->session_state);
    WM_xr_actionmaps_clear(*runtime);

    MEM_SAFE_DELETE(*runtime);
    *runtime = nullptr;
  }
}

/** \} */ /* XR Runtime Data. */

}  // namespace blender
