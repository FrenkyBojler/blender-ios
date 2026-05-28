/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_perfetto
 *
 * Implementation of the Blender Perfetto tracing wrapper.
 *
 * This is the ONLY translation unit that includes the heavy perfetto SDK amalgamated header.
 * All other code should include perfetto_trace.hh instead.
 *
 * Perfetto "in-process" tracing model used here:
 *   - `perfetto::Tracing::Initialize()` sets up the in-process backend.
 *   - A `perfetto::TracingSession` is created with a `perfetto::TraceConfig` that enables the
 *     "track_event" data source (which backs the TRACE_EVENT_* macros).
 *   - On shutdown the session is stopped, the trace proto is read back, and written to a file.
 *
 * The trace output file can be opened with the Perfetto UI at https://ui.perfetto.dev/
 */

/* The amalgamated perfetto source requires PERFETTO_IMPLEMENTATION to be defined in exactly one
 * translation unit before including perfetto.h. We define it here so that all the SDK
 * implementation symbols are compiled into this object file only. */
#define PERFETTO_IMPLEMENTATION

#include <perfetto.h>

#include "perfetto_trace.hh"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#else
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* -------------------------------------------------------------------- */
/** \name Perfetto category definitions
 *
 * IMPORTANT — keep the following three things in sync whenever categories change:
 *   1. `PERFETTO_DEFINE_CATEGORIES` below (the Perfetto category strings).
 *   2. `BlenderPerfettoCategory` enum below (maps ProfileCategory colour values → strings).
 *   3. `blender::ProfileCategory` enum in `source/blender/blenlib/BLI_profile.hh`.
 *
 * The `BlenderPerfettoCategory` enum uses the same colour values as `ProfileCategory` so that
 * the raw `uint32_t` passed through the API can be compared without any string conversion.
 * \{ */

/* clang-format off */
PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("default").SetDescription("Default profiling category"),
    perfetto::Category("core")   .SetDescription("Core systems"),
    perfetto::Category("draw")   .SetDescription("Drawing / rendering"),
    perfetto::Category("editor") .SetDescription("Editor operations"),
    perfetto::Category("frame")  .SetDescription("Frame markers")
);
/* clang-format on */

/* Required in exactly one .cc file to instantiate the category registry. */
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

/**
 * Mirror of `blender::ProfileCategory` MUST be kept in sync with `blender::ProfileCategory` in
 * `BLI_profile.hh`.
 */
enum class BlenderPerfettoCategory : uint32_t {
  Default = 0x000001,
  Core = 0x0088FE,
  Draw = 0x00C49F,
  Editor = 0xFFBB28,
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Internal state
 * \{ */

namespace {

/** Guards access to `g_tracing_session` and `g_live_fd`. */
std::mutex g_session_mutex;

/** Active tracing session; null when tracing is not running. */
std::unique_ptr<perfetto::TracingSession> g_tracing_session;

/** Path of the trace output file. */
constexpr const char *TRACE_OUTPUT_FILE = "blender.perfetto-trace";

/**
 * File descriptor used in live mode (`BLENDER_PERFETTO_LIVE=1`).
 * Perfetto writes directly to this fd as events are emitted, so a trace is
 * recoverable even if Blender crashes before `perfetto_shutdown()` is called.
 * -1 when not in live mode.
 */
int g_live_fd = -1;

/* -------------------------------------------------------------------- */
/** \name Category dispatch helpers
 *
 * TRACE_EVENT_BEGIN/END require a compile-time constant category string on MSVC.
 * Since our category set is fixed and known at compile time, we dispatch to the
 * correct static call based on the uint32_t category value.
 * \{ */

void trace_event_begin_for_category(uint32_t category, const char *name)
{
  switch (static_cast<BlenderPerfettoCategory>(category)) {
    case BlenderPerfettoCategory::Core:
      TRACE_EVENT_BEGIN("core", perfetto::DynamicString(name));
      break;
    case BlenderPerfettoCategory::Draw:
      TRACE_EVENT_BEGIN("draw", perfetto::DynamicString(name));
      break;
    case BlenderPerfettoCategory::Editor:
      TRACE_EVENT_BEGIN("editor", perfetto::DynamicString(name));
      break;
    case BlenderPerfettoCategory::Default:
    default:
      TRACE_EVENT_BEGIN("default", perfetto::DynamicString(name));
      break;
  }
}

void trace_event_end_for_category(uint32_t category)
{
  switch (static_cast<BlenderPerfettoCategory>(category)) {
    case BlenderPerfettoCategory::Core:
      TRACE_EVENT_END("core");
      break;
    case BlenderPerfettoCategory::Draw:
      TRACE_EVENT_END("draw");
      break;
    case BlenderPerfettoCategory::Editor:
      TRACE_EVENT_END("editor");
      break;
    case BlenderPerfettoCategory::Default:
    default:
      TRACE_EVENT_END("default");
      break;
  }
}

/** \} */

}  // namespace

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API implementation
 * \{ */

namespace blender {

void perfetto_init()
{
  std::lock_guard<std::mutex> lock(g_session_mutex);

  if (g_tracing_session) {
    /* Already initialised – nothing to do. */
    return;
  }

  /* Initialise the Perfetto library with the in-process backend.
   * This must be called once per process before any tracing session is started. */
  perfetto::TracingInitArgs init_args;
  init_args.backends = perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(init_args);

  /* Register the track-event data source so that TRACE_EVENT_* macros work. */
  perfetto::TrackEvent::Register();

  /* Build a trace configuration that enables the "track_event" data source. */
  perfetto::TraceConfig cfg;

  auto *ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");

  /* Check whether the user wants live (crash-safe) tracing.
   *
   * LIVE MODE (`BLENDER_PERFETTO_LIVE=1`):
   *   The trace proto is written directly to the output file as events are emitted.
   *   A partial trace is recoverable even if Blender crashes before shutdown.
   *   Slightly higher I/O overhead per event.
   *
   * BUFFERED MODE (default):
   *   Events are held in a 64 MB ring buffer in memory. When the buffer is full, the
   *   OLDEST events are silently dropped to make room for new ones — there is no
   *   automatic flush to disk as the buffer fills. Only the most recent ~64 MB of
   *   events survive to be written on clean exit via `ReadTraceBlocking()`.
   *   If a session produces more data than the buffer can hold, increase `set_size_kb`
   *   below. This mode loses all data on crash. */
  const char *live_env = std::getenv("BLENDER_PERFETTO_LIVE");
  const bool live_mode = (live_env != nullptr && live_env[0] == '1');

  if (live_mode) {
    /* Open the output file and hand the fd to Perfetto.
     * The SDK will write the trace proto incrementally as events arrive. */
#if defined(_WIN32)
    g_live_fd = _open(
        TRACE_OUTPUT_FILE, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    g_live_fd = open(TRACE_OUTPUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (g_live_fd == -1) {
      /* Fall back to buffered mode if the file cannot be opened. */
    }
    else {
      /* No ring buffer needed — data goes straight to disk. */
      cfg.add_buffers()->set_size_kb(4 * 1024); /* Small scratch buffer. */
      g_tracing_session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
      g_tracing_session->Setup(cfg, g_live_fd);
      g_tracing_session->StartBlocking();
      return;
    }
  }

  /* Buffered (default) mode: hold events in a 64 MB ring buffer and write on shutdown. */
  cfg.add_buffers()->set_size_kb(64 * 1024);
  g_tracing_session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
  g_tracing_session->Setup(cfg);
  g_tracing_session->StartBlocking();
}

void perfetto_shutdown()
{
  std::unique_ptr<perfetto::TracingSession> session;
  int live_fd = -1;

  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    if (!g_tracing_session) {
      return;
    }
    session = std::move(g_tracing_session);
    live_fd = g_live_fd;
    g_live_fd = -1;
  }

  /* Flush any pending events and stop the session synchronously. */
  perfetto::TrackEvent::Flush();
  session->StopBlocking();

  if (live_fd != -1) {
    /* Live mode: Perfetto has already written everything to the file.
     * Just close the file descriptor. */
#if defined(_WIN32)
    _close(live_fd);
#else
    close(live_fd);
#endif
  }
  else {
    /* Buffered mode: read the serialised trace proto and write it to disk now. */
    std::vector<char> trace_data = session->ReadTraceBlocking();

    std::ofstream output(TRACE_OUTPUT_FILE, std::ios::out | std::ios::binary | std::ios::trunc);
    if (output.is_open()) {
      output.write(trace_data.data(), static_cast<std::streamsize>(trace_data.size()));
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Scope tracing
 * \{ */

void perfetto_scope_begin(uint32_t category, const char *name)
{
  /* Dispatch to the correct static category. TRACE_EVENT_BEGIN requires a compile-time constant
   * string on MSVC, so we cannot pass a runtime value directly. The uint32_t is the raw value
   * of blender::ProfileCategory — see the dispatch helpers above. */
  trace_event_begin_for_category(category, name);
}

void perfetto_scope_end(uint32_t category)
{
  trace_event_end_for_category(category);
}

void perfetto_scope_add_annotation(const char * /*key*/, const char * /*value*/)
{
  /* Perfetto does not support attaching annotations to an already-open slice from outside the
   * TRACE_EVENT_BEGIN lambda. Annotations must be provided at the time the slice is opened.
   * This function is intentionally a no-op; use `perfetto_scope_begin` with a lambda-based
   * overload if annotations are needed (future work). */
}

void perfetto_scope_add_value(const char * /*key*/, uint64_t /*value*/)
{
  /* Same limitation as perfetto_scope_add_annotation — no-op. */
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame markers
 * \{ */

void perfetto_frame_mark()
{
  /* Emit an instant event on the global track to mark a frame boundary.
   * Perfetto does not have a direct equivalent of Tracy's FrameMark, so we use an instant event
   * on a dedicated global track. */
  TRACE_EVENT_INSTANT("frame", "FrameMark", perfetto::Track::Global(0));
}

void perfetto_frame_mark_start(const char *name)
{
  TRACE_EVENT_BEGIN("frame", perfetto::DynamicString(name), perfetto::Track::Global(0));
}

void perfetto_frame_mark_end(const char * /*name*/)
{
  TRACE_EVENT_END("frame", perfetto::Track::Global(0));
}

/** \} */

}  // namespace blender
