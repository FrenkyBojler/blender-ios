/* SPDX-FileCopyrightText: 2021-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Generic DBUS access used by the Wayland backend.
 */

#include "GHOST_SystemWaylandDBUS.hh"

#ifdef WITH_GHOST_WAYLAND_DBUS

#  ifdef WITH_GHOST_WAYLAND_DBUS_DYNLOAD
#    include "dbus_dynload.h"
#    include "dbus_dynload_API.h"
#  else
#    include <dbus/dbus.h>
#  endif

namespace {

/* Logical timeout for the portal call: how long we'll wait for a reply before
 * giving up. */
constexpr int dbus_call_timeout_ms = 2000;
/* Polling slice for #dbus_connection_read_write_dispatch. Caps how long
 * shutdown waits for an in-flight call to wake; small enough that it's not
 * user-perceptible, large enough to keep wakeup overhead negligible. */
constexpr int dbus_call_poll_ms = 50;

/**
 * Call `org.freedesktop.portal.Settings.Read(ns, key)` and return the reply.
 *
 * Uses the async DBUS API and polls #dbus_connection_read_write_dispatch in
 * #dbus_call_poll_ms slices, checking #GWL_DBus::worker_should_exit
 * between slices. This means a shutdown request preempts an in-flight call
 * within ~#dbus_call_poll_ms instead of having to wait for the full
 * #dbus_call_timeout_ms. Returns the reply (caller must `dbus_message_unref()`)
 * on success, or `nullptr` on send failure, shutdown, connection loss, or
 * portal error reply.
 */
DBusMessage *portal_settings_read(GWL_DBus *dbus,
                                  DBusConnection *connection,
                                  const char *ns,
                                  const char *key)
{
  DBusMessage *message = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.Settings",
                                                      "Read");
  /* `dbus_message_new_method_call` only returns NULL on memory-allocation
   * failure, so this should almost never happen. */
  if (!message) {
    return nullptr;
  }

  const dbus_bool_t appended = dbus_message_append_args(
      message, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);

  if (!appended) {
    dbus_message_unref(message);
    return nullptr;
  }

  DBusPendingCall *pending = nullptr;
  const dbus_bool_t sent = dbus_connection_send_with_reply(
      connection, message, &pending, dbus_call_timeout_ms);

  dbus_message_unref(message);

  /* `sent == FALSE` is OOM; `pending == NULL` means the connection is
   * disconnected (the docs guarantee one of these implies the other). */
  if (!sent || !pending) {
    return nullptr;
  }

  while (!dbus_pending_call_get_completed(pending)) {
    if (dbus->worker_should_exit.load(std::memory_order_relaxed)) {
      dbus_pending_call_cancel(pending);
      dbus_pending_call_unref(pending);
      return nullptr;
    }
    /* Returns FALSE when the connection is disconnected. The pending call
     * never completes in that case, so bail. */
    if (!dbus_connection_read_write_dispatch(connection, dbus_call_poll_ms)) {
      dbus_pending_call_cancel(pending);
      dbus_pending_call_unref(pending);
      return nullptr;
    }
  }

  DBusMessage *reply = dbus_pending_call_steal_reply(pending);
  dbus_pending_call_unref(pending);
  /* On portal-side error (including the timeout case, which the daemon
   * delivers as `org.freedesktop.DBus.Error.NoReply`), the reply is an error
   * message rather than the expected variant. #reply_variant_basic_parse will
   * reject it, so we don't need to special-case it here. */
  return reply;
}

/**
 * Unwrap the doubly-nested variant `v(v(<basic>))` returned by the portal's
 * `Settings.Read` and copy the basic value of `dbus_type` into `r_value`.
 */
bool reply_variant_basic_parse(DBusMessage *reply, const int dbus_type, void *r_value)
{
  DBusMessageIter iter[3];

  if (!dbus_message_iter_init(reply, &iter[0])) {
    return false;
  }
  if (dbus_message_iter_get_arg_type(&iter[0]) != DBUS_TYPE_VARIANT) {
    return false;
  }

  dbus_message_iter_recurse(&iter[0], &iter[1]);
  if (dbus_message_iter_get_arg_type(&iter[1]) != DBUS_TYPE_VARIANT) {
    return false;
  }

  dbus_message_iter_recurse(&iter[1], &iter[2]);
  if (dbus_message_iter_get_arg_type(&iter[2]) != dbus_type) {
    return false;
  }

  dbus_message_iter_get_basic(&iter[2], r_value);
  return true;
}

/**
 * Run the cursor-size portal Read. Returns true and writes `*r_size` on
 * success, false otherwise.
 */
bool cursor_size_query(GWL_DBus *dbus, int *r_size)
{
  DBusError error;
  dbus_error_init(&error);

  DBusConnection *connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
  if (dbus_error_is_set(&error)) {
    dbus_error_free(&error);
    return false;
  }
  if (connection == nullptr) {
    return false;
  }

  DBusMessage *reply = portal_settings_read(
      dbus, connection, "org.gnome.desktop.interface", "cursor-size");
  if (!reply) {
    dbus_connection_unref(connection);
    return false;
  }

  const bool ok = reply_variant_basic_parse(reply, DBUS_TYPE_INT32, r_size);

  dbus_message_unref(reply);
  dbus_connection_unref(connection);
  return ok;
}

/**
 * Run the cursor-theme portal Read. Returns true and writes the theme name
 * into `*r_theme` on success, false otherwise.
 */
bool cursor_theme_query(GWL_DBus *dbus, std::string *r_theme)
{
  DBusError error;
  dbus_error_init(&error);

  DBusConnection *connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
  if (dbus_error_is_set(&error)) {
    dbus_error_free(&error);
    return false;
  }
  if (connection == nullptr) {
    return false;
  }

  DBusMessage *reply = portal_settings_read(
      dbus, connection, "org.gnome.desktop.interface", "cursor-theme");
  if (!reply) {
    dbus_connection_unref(connection);
    return false;
  }

  /* `dbus_message_iter_get_basic` for strings writes a `const char *` whose
   * buffer is owned by the reply message. Copy out before unref. */
  const char *theme_str = nullptr;
  const bool ok = reply_variant_basic_parse(reply, DBUS_TYPE_STRING, &theme_str);
  if (ok && theme_str) {
    *r_theme = theme_str;
  }

  dbus_message_unref(reply);
  dbus_connection_unref(connection);
  return ok && theme_str != nullptr;
}

void dbus_worker_thread_fn(GWL_DBus *dbus)
{
  for (;;) {
    {
      std::unique_lock lock(dbus->worker_mutex);
      dbus->worker_cond.wait(lock, [dbus] {
        return dbus->worker_should_exit.load(std::memory_order_relaxed) ||
               dbus->cursor_size_pending.load(std::memory_order_relaxed) ||
               dbus->cursor_theme_pending.load(std::memory_order_relaxed);
      });
      if (dbus->worker_should_exit.load(std::memory_order_relaxed)) {
        return;
      }
    }

    bool any_published = false;

    if (dbus->cursor_size_pending.exchange(false, std::memory_order_acquire)) {
      int size = 0;
      if (cursor_size_query(dbus, &size) && size > 0) {
        std::lock_guard lock(dbus->results_mutex);
        dbus->results.cursor_size = size;
        dbus->results.cursor_size_is_set = true;
        any_published = true;
      }
    }

    if (dbus->cursor_theme_pending.exchange(false, std::memory_order_acquire)) {
      std::string theme;
      if (cursor_theme_query(dbus, &theme) && !theme.empty()) {
        std::lock_guard lock(dbus->results_mutex);
        dbus->results.cursor_theme = std::move(theme);
        dbus->results.cursor_theme_is_set = true;
        any_published = true;
      }
    }

    if (any_published) {
      /* Single hot-path gate flipped after all results for this iteration are
       * in place. Release synchronizes with the consumer's acquire-load. */
      dbus->results_ready.store(true, std::memory_order_release);
    }
  }
}

}  // namespace

GWL_DBus *ghost_wl_dbus_create()
{
#  ifdef WITH_GHOST_WAYLAND_DBUS_DYNLOAD
  /* Lazily resolve libdbus once. If it's not present, every call from then on
   * just returns null and the rest of the system silently falls back. */
  static std::once_flag dynload_once;
  static bool dynload_ok = false;
  std::call_once(dynload_once, []() { dynload_ok = dbus_dynload_init(false); });
  if (!dynload_ok) {
    return nullptr;
  }
#  endif

  /* libdbus has been thread-safe by default since 1.7 (2013); no init call needed. */
  GWL_DBus *dbus = new GWL_DBus();
  dbus->worker = std::thread(dbus_worker_thread_fn, dbus);
  return dbus;
}

void ghost_wl_dbus_destroy(GWL_DBus *dbus)
{
  if (!dbus) {
    return;
  }
  {
    std::lock_guard lock(dbus->worker_mutex);
    dbus->worker_should_exit.store(true, std::memory_order_relaxed);
  }
  dbus->worker_cond.notify_all();
  dbus->worker.join();
  delete dbus;
}

void ghost_wl_dbus_cursor_size_request(GWL_DBus *dbus)
{
  if (!dbus) {
    return;
  }
  {
    std::lock_guard lock(dbus->worker_mutex);
    dbus->cursor_size_pending.store(true, std::memory_order_release);
  }
  dbus->worker_cond.notify_one();
}

void ghost_wl_dbus_cursor_theme_request(GWL_DBus *dbus)
{
  if (!dbus) {
    return;
  }
  {
    std::lock_guard lock(dbus->worker_mutex);
    dbus->cursor_theme_pending.store(true, std::memory_order_release);
  }
  dbus->worker_cond.notify_one();
}

void ghost_wl_dbus_results_take(GWL_DBus *dbus, GWL_DBusResults *r_results)
{
  std::lock_guard lock(dbus->results_mutex);
  *r_results = std::move(dbus->results);
  dbus->results = {};
}

#else /* !WITH_GHOST_WAYLAND_DBUS */

GWL_DBus *ghost_wl_dbus_create()
{
  return nullptr;
}

void ghost_wl_dbus_destroy(GWL_DBus * /*dbus*/) {}

void ghost_wl_dbus_cursor_size_request(GWL_DBus * /*dbus*/) {}
void ghost_wl_dbus_cursor_theme_request(GWL_DBus * /*dbus*/) {}

void ghost_wl_dbus_results_take(GWL_DBus * /*dbus*/, GWL_DBusResults * /*r_results*/) {}

#endif /* !WITH_GHOST_WAYLAND_DBUS */
