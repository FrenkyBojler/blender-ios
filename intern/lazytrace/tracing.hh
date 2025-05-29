/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache License 2.0 */

#pragma once

#include <string>
#include <unordered_map>
#include <variant>

namespace lazytrace {
using TraceVariant = std::variant<float, double, int, std::string, size_t>;
using TraceArgs = std::unordered_map<std::string, TraceVariant>;
enum class TraceStorageStrategy {
  Disabled,   /* Do not store trace information. */
  Collect,    /* Collect trace information but do not save. */
  DirectSave, /* Collect trace information and write to disk as soon as trace events come in, this
                 is _very_ expensive, and should only be used in situations where a crash in the
                 spplication makes it impossible to write the trace data at a later time. */
  SaveOnExit, /* Collect trace information and write to disk as the application closes. */
  Chunked, /* Collect trace infromation and write to disk every N events, this is trying to strike
              a blance between memory use and disk activity. */
};

/* The default tracename is lazytrace.json */
std::string TraceFileName();
void SetTraceFileName(const std::string &file_name);

/* The default storage strategy is Collect. */
TraceStorageStrategy StorageStrategy();
void SetStorageStrategy(TraceStorageStrategy strategy);

/* The default chunk size is 8192 */
size_t StorageChunkSize();
void SetStorageChunkSize(size_t chunk_size);

/* Convenience helper, this will return false if Disabled is the current storage strategy, true for
 * all others. */
bool TraceEnabled();

/* This will save any events in the collection queue regardless of the storage strategy */
bool Save();

void Init();
void ShutDown();

/* Threads may only be named once, will return true the first time any of these methods
 * are called, false for any further invocations. */
bool NameThread(size_t process_id, size_t thread_id, const std::string &thread_name);
bool NameThread(size_t thread_id, const std::string &thread_name);
bool NameThread(const std::string &thread_name);

std::string ThreadName(size_t process_id, size_t thread_id);
std::string ThreadName(size_t thread_id);
std::string ThreadName();

/* Threads may only be named once, will return true the first time any of these methods
 * are called, false for any further invocations. */
bool NameProcess(size_t process_id, const std::string &process_name);
bool NameProcess(const std::string &process_name);

std::string ProcessName(size_t process_id);
std::string ProcessName();

struct TraceData {
  std::string name; /* The name of the event, as displayed in Trace Viewer */
  std::string cat;  /* The event categories. This is a comma separated list of categories for the
                       event. The categories can be used to hide events in the Trace Viewer UI. */
  char ph;    /* The event type.This is a single character which changes depending on the type of
                 event being output. */
  int64_t ts; /* The tracing clock timestamp of the event. The timestamps are provided at
                 microsecond granularity. */
#if 0
		/* As i'm insure what this does/why i would want this, this field is disabled. */
		int64_t tts: /* Optional.The thread clock timestamp of the event.The timestamps are provided at microsecond granularity.*/
#endif
  size_t pid; /* The process ID for the process that output this event. */
  size_t tid; /* The thread ID for the thread that output this event. */
  TraceArgs
      args; /* Any arguments provided for the event. Some of the event types have required argument
               fields, otherwise, you can put any information you wish in here. The arguments are
               displayed in Trace Viewer when you view an event in the analysis section. */
  std::string id2;
  std::string to_string();
};

TraceData CreateData(const std::string &name, const char ph);
TraceData CreateData(const std::string &name, const char ph, size_t pid, size_t tid);
TraceData CreateGlobalData(const std::string &name, const char ph, size_t id);

void Emit(const TraceData &event);

/* Helper Functions to do common things */
void EmitBegin(const std::string &name);
void EmitEnd(const std::string &name);
void EmitCounter(const std::string &name, const std::string &variable, const TraceVariant &value);

class ScopedTrace {
 private:
  std::string name_;

 public:
  TraceArgs args;
  ScopedTrace(std::string name) : name_(name)
  {
    if (TraceEnabled()) {
      EmitBegin(name);
    }
  };
  ~ScopedTrace()
  {
    if (TraceEnabled()) {
      TraceData evt = CreateData(name_, 'E');
      evt.args = args;
      Emit(evt);
    }
  }
};

#define TRACE_STRINGIFY(x) #x
#define SCOPED_TRACE(name) lazytrace::ScopedTrace scoped_trace(name)
#define SCOPED_TRACE_FUNCTION() lazytrace::ScopedTrace scoped_trace(__func__)
#define SCOPED_TRACE_DATA(key, value) scoped_trace.args[key] = value
#define TRACE_COUNTER(value) lazytrace::EmitCounter(__func__, TRACE_STRINGIFY(value), value)
#undef TRACE_STRINGIFY

}  // namespace lazytrace
