/* SPDX-FileCopyrightText: 2021-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Generic DBUS access used by the Wayland backend.
 *
 * The work runs on a dedicated background thread so the main thread never
 * blocks on the session bus. Callers fire-and-forget via `*_request` and
 * later check `results_ready` and call `ghost_wl_dbus_results_take` to pick
 * up whatever the worker has produced since the last call. The main thread
 * is responsible for applying the values.
 *
 * The portal settings we query change very rarely (typically once per
 * session, at startup), so the consumer side is optimized for the common
 * case where nothing has changed: a single relaxed atomic load gates all
 * mutex/string/result work.
 *
 * When `WITH_GHOST_WAYLAND_DBUS` is disabled (or the bus is unreachable)
 * `ghost_wl_dbus_create` returns null. All other functions accept a null
 * pointer and behave as no-ops.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

/**
 * Snapshot of the worker's output since the last drain. Each `*_is_set` flag
 * indicates whether the corresponding value is fresh and should be applied.
 */
struct GWL_DBusResults {
  bool cursor_size_is_set = false;
  int cursor_size = 0;
  bool cursor_theme_is_set = false;
  std::string cursor_theme;
};

struct GWL_DBus {
  std::thread worker;
  std::mutex worker_mutex;
  std::condition_variable worker_cond;
  std::atomic<bool> worker_should_exit{false};

  /* Main thread -> worker: per-action request flags. Cheap, only flipped at
   * startup (and any future re-request), not on the hot path. */
  std::atomic<bool> cursor_size_pending{false};
  std::atomic<bool> cursor_theme_pending{false};

  /* Worker -> main: single hot-path gate. The worker sets this to true after
   * publishing one or more results to `results` (under `results_mutex`).
   * The event loop checks it on every tick; the false branch is the common
   * case and bails without touching the mutex. */
  std::atomic<bool> results_ready{false};

  /* All worker output. Always accessed under `results_mutex`. */
  std::mutex results_mutex;
  GWL_DBusResults results;
};

GWL_DBus *ghost_wl_dbus_create();
void ghost_wl_dbus_destroy(GWL_DBus *dbus);

void ghost_wl_dbus_cursor_size_request(GWL_DBus *dbus);
void ghost_wl_dbus_cursor_theme_request(GWL_DBus *dbus);

/**
 * Move the worker's published results into `*r_results` and reset the storage.
 * The caller MUST have observed `results_ready == true` and exchanged it
 * to false before calling - this function does NOT re-check the atomic.
 */
void ghost_wl_dbus_results_take(GWL_DBus *dbus, GWL_DBusResults *r_results);
