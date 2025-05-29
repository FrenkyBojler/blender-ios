/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache License 2.0 */

#pragma once

#include "tracing.hh"
#include <cstddef>
#include <map>
#include <thread>
#include <vector>

namespace lazytrace::internal {
size_t CurrentThreadID();
size_t CurrentProcessID();
size_t CurrentClock();

class Settings {
 public:
  std::string file_name = "lazy_trace.json";
  lazytrace::TraceStorageStrategy strategy = lazytrace::TraceStorageStrategy::Collect;
  size_t chunk_size = 4096;
  bool first_save = true;
};

class Data {
 public:
  std::map<std::pair<size_t, size_t>, std::string> threadNames_;
  std::map<size_t, std::string> processNames_;
  std::vector<lazytrace::TraceData> events_;
  bool KnownProcess(size_t process_id)
  {
    return processNames_.count(process_id) != 0;
  }

  bool KnownThread(size_t process_id, size_t thread_id)
  {
    std::pair<size_t, size_t> key = std::pair(process_id, thread_id);
    return threadNames_.count(key) != 0;
  }

  std::string ThreadName(size_t process_id, size_t thread_id)
  {
    if (KnownThread(process_id, thread_id)) {
      return threadNames_[std::pair(process_id, thread_id)];
    }
    return "Unknown";
  }

  std::string ProcessName(size_t process_id)
  {
    if (KnownProcess(process_id)) {
      return processNames_[process_id];
    }
    return "Unknown";
  }

  void SetThreadName(size_t process_id, size_t thread_id, const std::string &name)
  {
    threadNames_[std::pair(process_id, thread_id)] = name;
  };

  void SetProcessName(size_t process_id, const std::string &name)
  {
    processNames_[process_id] = name;
  };
};

}  // namespace lazytrace::internal
