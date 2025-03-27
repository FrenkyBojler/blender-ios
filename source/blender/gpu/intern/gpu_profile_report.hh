/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace blender::gpu {

class ProfileReport {
 private:
  std::fstream _report;
  std::mutex _mutex;
  Map<size_t, int> _thread_ids;

  ProfileReport()
  {
    _report.open("profile.json", std::ios::out);
    _report << R"([{"name":"process_name","ph":"M","pid":1,"args":{"name":"GPU"}})"
               ",\n";
    _report << R"({"name":"process_name","ph":"M","pid":2,"args":{"name":"CPU"}})"
               ",\n";
    _report << R"({"name":"process_name","ph":"M","pid":3,"args":{"name":"VkDevice"}})";
  }

  ~ProfileReport()
  {
    _report << "\n]\n";
    _report.close();
  }

 public:
  static ProfileReport &get()
  {
    static ProfileReport singleton;
    return singleton;
  }

  void add_timing(StringRefNull object_name,
                  int handle,
                  StringRefNull sub_name,
                  int pid,
                  uint64_t start,
                  uint64_t end)
  {
    std::scoped_lock lock(_mutex);

    _report << fmt::format(
        ",\n"
        R"({{"name":"{}[{}]::{}","ph":"X","ts":{},"dur":{},"pid":{},"tid":{}}})",
        object_name.c_str(),
        handle,
        sub_name.c_str(),
        start / uint64_t(1000),
        (end - start) / uint64_t(1000),
        pid,
        1);
  }

  void add_group(StringRefNull name,
                 uint64_t gpu_start,
                 uint64_t gpu_end,
                 uint64_t cpu_start,
                 uint64_t cpu_end)
  {
    std::scoped_lock lock(_mutex);

    size_t thread_hash = std::hash<std::thread::id>()(std::this_thread::get_id());
    int thread_id = _thread_ids.lookup_or_add(thread_hash, _thread_ids.size());

    _report << fmt::format(
        ",\n"
        R"({{"name":"{}","ph":"X","ts":{},"dur":{},"pid":1,"tid":{}}})",
        name.c_str(),
        gpu_start / uint64_t(1000),
        (gpu_end - gpu_start) / uint64_t(1000),
        thread_id);

    _report << fmt::format(
        ",\n"
        R"({{"name":"{}","ph":"X","ts":{},"dur":{},"pid":2,"tid":{}}})",
        name.c_str(),
        cpu_start / uint64_t(1000),
        (cpu_end - cpu_start) / uint64_t(1000),
        thread_id);
  }
};

}  // namespace blender::gpu
