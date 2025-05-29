/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache License 2.0 */

/* LazyDodo's tracing library base on the documentation in

https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview?tab=t.0

*/

#include "tracing.hh"
#include "tracing_internal.hh"
#include <filesystem>
#include <fstream>
#include <inttypes.h>
#include <iostream>
#include <mutex>
#include <sstream>

static lazytrace::internal::Data _data;
static lazytrace::internal::Settings _settings;
static std::mutex _lazylock;

std::string lazytrace::TraceFileName()
{
  return _settings.file_name;
}

void lazytrace::SetTraceFileName(const std::string &file_name)
{
  _settings.file_name = file_name;
}

lazytrace::TraceStorageStrategy lazytrace::StorageStrategy()
{
  return _settings.strategy;
}

void lazytrace::SetStorageStrategy(TraceStorageStrategy strategy)
{
  _settings.strategy = strategy;
  if (_settings.strategy == TraceStorageStrategy::DirectSave) {
    Save();
  }
}

size_t lazytrace::StorageChunkSize()
{
  return _settings.chunk_size;
}

void lazytrace::SetStorageChunkSize(size_t chunk_size)
{
  _settings.chunk_size = chunk_size;
}

bool lazytrace::TraceEnabled()
{
  return StorageStrategy() != lazytrace::TraceStorageStrategy::Disabled;
}

bool lazytrace::Save()
{
  if (!TraceEnabled())
    return false;
  std::scoped_lock lock(_lazylock);
  std::fstream _out_file;
  if (!std::filesystem::exists(_settings.file_name)) {
    _settings.first_save = true;
  }
  if (_settings.first_save) {
    _out_file.open(_settings.file_name, std::fstream::out);
  }
  else {
    /* Chop off the trailing ']' */
    std::filesystem::resize_file(_settings.file_name,
                                 std::filesystem::file_size(_settings.file_name) - 1);
    _out_file.open(_settings.file_name, std::fstream::in | std::fstream::out | std::fstream::app);
  }

  if (!_out_file.is_open())
    return false;

  if (_settings.first_save) {
    _out_file << "[\n";
  }

  for (auto &evt : _data.events_) {
    _out_file << evt.to_string() << ",\n";
  }
  _out_file << "]";
  _out_file.close();
  _settings.first_save = false;
  return true;
}

void lazytrace::ShutDown()
{
  switch (StorageStrategy()) {
    case TraceStorageStrategy::Disabled:
    case TraceStorageStrategy::Collect:
    case TraceStorageStrategy::DirectSave:
      break;
    case TraceStorageStrategy::SaveOnExit:
    case TraceStorageStrategy::Chunked:
      Save();
      break;
  }
}

bool lazytrace::NameThread(size_t process_id, size_t thread_id, const std::string &thread_name)
{
  bool Result = !_data.KnownThread(process_id, thread_id);
  if (Result) {
    _data.SetThreadName(process_id, thread_id, thread_name);
    TraceData evt = CreateData("thread_name", 'M');
    evt.pid = process_id;
    evt.tid = thread_id;
    evt.args["name"] = thread_name;
    Emit(evt);
  }
  return Result;
}

bool lazytrace::NameThread(size_t thread_id, const std::string &thread_name)
{
  return NameThread(lazytrace::internal::CurrentProcessID(), thread_id, thread_name);
}

bool lazytrace::NameThread(const std::string &thread_name)
{
  return NameThread(lazytrace::internal::CurrentProcessID(),
                    lazytrace::internal::CurrentThreadID(),
                    thread_name);
}

std::string lazytrace::ThreadName(size_t process_id, size_t thread_id)
{
  return _data.ThreadName(process_id, thread_id);
}

std::string lazytrace::ThreadName(size_t thread_id)
{
  return ThreadName(lazytrace::internal::CurrentProcessID(), thread_id);
}

std::string lazytrace::ThreadName()
{
  return ThreadName(lazytrace::internal::CurrentProcessID(),
                    lazytrace::internal::CurrentThreadID());
}

bool lazytrace::NameProcess(size_t process_id, const std::string &process_name)
{
  bool Result = !_data.KnownProcess(process_id);
  if (Result) {
    _data.SetProcessName(process_id, process_name);
    TraceData evt = CreateData("process_name", 'M');
    evt.pid = process_id;
    evt.args["name"] = process_name;
    Emit(evt);
  }
  return Result;
}

bool lazytrace::NameProcess(const std::string &process_name)
{
  return NameProcess(lazytrace::internal::CurrentProcessID(), process_name);
}

std::string lazytrace::ProcessName(size_t process_id)
{
  return _data.ProcessName(process_id);
}

std::string lazytrace::ProcessName()
{
  return ProcessName(lazytrace::internal::CurrentProcessID());
}

void lazytrace::Emit(const TraceData &event)
{
  if (TraceEnabled()) {
    _lazylock.lock();
    _data.events_.push_back(event);
    _lazylock.unlock();
    if ((StorageStrategy() == TraceStorageStrategy::DirectSave) ||
        (StorageStrategy() == TraceStorageStrategy::Chunked &&
         _data.events_.size() >= _settings.chunk_size))
    {
      Save();
      _lazylock.lock();
      _data.events_.clear();
      _lazylock.unlock();
    }
  }
}

lazytrace::TraceData lazytrace::CreateData(const std::string &name, const char ph)
{
  return CreateData(
      name, ph, lazytrace::internal::CurrentProcessID(), lazytrace::internal::CurrentThreadID());
}

lazytrace::TraceData lazytrace::CreateData(const std::string &name,
                                           const char ph,
                                           size_t pid,
                                           size_t tid)
{
  lazytrace::TraceData result;
  result.name = name;
  result.ph = ph;
  result.tid = tid;
  result.pid = pid;
  result.ts = lazytrace::internal::CurrentClock();
  return result;
}
std::string json_escape_string(const std::string &value)
{
  // per rfc4627.txt
  std::stringstream result;
  for (auto k : value) {
    switch (k) {
      case 0x22:  // quotation mark
      case 0x5c:  // reverse solidus
      case 0x2f:  // solidus
      case 0x08:  // backspace
      case 0x0c:  // form feed
      case 0x0a:  // line feed
      case 0x0d:  // carriage return
      case 0x09:  // tab
        result << "\\";
        result << k;
        break;
      default:
        result << k;
    }
  }
  return result.str();
}

lazytrace::TraceData lazytrace::CreateGlobalData(const std::string &name, const char ph, size_t id)
{
  lazytrace::TraceData result = CreateData(name, ph);
  result.id2 = "{\"global\": \"" + std::to_string(id) + "\"}";
  return result;
}

std::string variant_to_string(const lazytrace::TraceVariant &v)
{
  return std::visit(
      [](const auto &x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return "\"" + json_escape_string(x) + "\"";
        }
        else {
          return std::to_string(x);
        }
      },
      v);
}

static size_t tracedata_to_json(
    lazytrace::TraceData &data, char *buffer, size_t size, const char *args, const char *id2)
{
  return snprintf(buffer,
                  size,
                  "{\"name\":\"%s\", \"cat\": \"%s\", \"ph\":\"%c\",\"ts\":%" PRId64
                  ", \"pid\":%zu,\"tid\":%zu %s %s}",
                  json_escape_string(data.name).c_str(),
                  json_escape_string(data.cat).c_str(),
                  data.ph,
                  data.ts,
                  data.pid,
                  data.tid,
                  id2,
                  args);
}

std::string lazytrace::TraceData::to_string()
{
  /* Would have liked <format> but that is c++ 20, don't want to take on a dependency on fmt */
  /* so this will do for now. */
  std::string buf;
  std::string args_str = "";
  if (args.size()) {
    args_str = ", \"args\": {";
    for (auto kv : args) {
      args_str += "\"" + json_escape_string(kv.first) + "\": " + variant_to_string(kv.second) +
                  ", ";
    }
    args_str += "}";
  }
  std::string id2_str = "";
  if (id2 != "") {
    id2_str = ",\"id2\":" + id2;
  }
  size_t len = tracedata_to_json(*this, nullptr, 0, args_str.c_str(), id2_str.c_str());
  buf.resize(len + 1);  // add zero termintor
  tracedata_to_json(*this, buf.data(), buf.size(), args_str.c_str(), id2_str.c_str());
  return buf;
}

void lazytrace::EmitBegin(const std::string &name)
{
  Emit(CreateData(name, 'B'));
}

void lazytrace::EmitEnd(const std::string &name)
{
  Emit(CreateData(name, 'E'));
}

void lazytrace::EmitCounter(const std::string &name,
                            const std::string &variable,
                            const TraceVariant &value)
{
  lazytrace::TraceData evt = CreateData(name, 'C');
  evt.args[variable] = value;
  Emit(evt);
}
