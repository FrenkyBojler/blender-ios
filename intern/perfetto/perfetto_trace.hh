/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_perfetto
 *
 * Lightweight wrapper around the Perfetto SDK.
 *
 * Because the perfetto SDK amalgamated header is very large (~200k lines) we do not want to
 * include it in every translation unit. This header exposes a minimal API that can be included
 * anywhere, while the actual SDK is only included in perfetto_trace.cc.
 */

#pragma once

#include <cstdint>

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Lifecycle
 * \{ */

/**
 * Initialize the Perfetto tracing backend and start an in-process tracing session.
 * The trace is written to a file named "blender.perfetto-trace" in the current working directory.
 * Must be called once before any trace events are emitted.
 */
void perfetto_init();

/**
 * Stop the tracing session, flush all pending events, and write the trace file to disk.
 * Must be called once at application shutdown.
 */
void perfetto_shutdown();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scope tracing
 *
 * These functions are the low-level building blocks used by the BLI_profile macros.
 * Prefer using the RAII `PerfettoScope` guard or the macros in BLI_profile.hh instead.
 * \{ */

/**
 * Emit a "begin" slice event on the current thread.
 *
 * \param category: A null-terminated string identifying the trace category
 *                  (must remain valid for the duration of the call).
 * \param name:     A null-terminated string for the slice name
 *                  (must remain valid until the matching `perfetto_scope_end` call).
 */
void perfetto_scope_begin(const char *category, const char *name);

/**
 * Emit an "end" slice event on the current thread, closing the most recently opened slice
 * in \a category.
 *
 * \param category: Must match the category passed to the corresponding `perfetto_scope_begin`.
 */
void perfetto_scope_end(const char *category);

/**
 * Attach an arbitrary text annotation to the innermost open slice on the current thread.
 * No-op if no slice is currently open.
 *
 * \param key:   Annotation key (null-terminated, must remain valid for the duration of the call).
 * \param value: Annotation value (null-terminated, must remain valid for the duration of the
 * call).
 */
void perfetto_scope_add_annotation(const char *key, const char *value);

/**
 * Attach a 64-bit integer annotation to the innermost open slice on the current thread.
 *
 * \param key:   Annotation key (null-terminated, must remain valid for the duration of the call).
 * \param value: Integer value to attach.
 */
void perfetto_scope_add_value(const char *key, uint64_t value);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame markers
 * \{ */

/**
 * Emit a global frame marker (equivalent to Tracy's `FrameMark`).
 * Marks the end of a rendered frame in the trace viewer.
 */
void perfetto_frame_mark();

/**
 * Emit the start of a named frame sequence.
 *
 * \param name: Frame sequence name (null-terminated, must remain valid for the duration of the
 *              call).
 */
void perfetto_frame_mark_start(const char *name);

/**
 * Emit the end of a named frame sequence.
 *
 * \param name: Must match the name passed to `perfetto_frame_mark_start`.
 */
void perfetto_frame_mark_end(const char *name);

/** \} */

/* -------------------------------------------------------------------- */
/** \name RAII scope guard
 * \{ */

/**
 * RAII guard that calls `perfetto_scope_begin` on construction and `perfetto_scope_end` on
 * destruction. Intended to be used via the `BLI_profile_scope` / `BLI_profile_scope_var` macros.
 */
class PerfettoScope {
 public:
  PerfettoScope(const char *category, const char *name) : category_(category)
  {
    perfetto_scope_begin(category_, name);
  }

  ~PerfettoScope()
  {
    perfetto_scope_end(category_);
  }

  /* Non-copyable, non-movable. */
  PerfettoScope(const PerfettoScope &) = delete;
  PerfettoScope &operator=(const PerfettoScope &) = delete;
  PerfettoScope(PerfettoScope &&) = delete;
  PerfettoScope &operator=(PerfettoScope &&) = delete;

 private:
  const char *category_;
};

/** \} */

}  // namespace blender
