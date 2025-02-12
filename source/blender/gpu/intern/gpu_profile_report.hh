/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace blender::gpu {

class ProfileReport {
 private:
  std::fstream _report;
  std::mutex _mutex;

  ProfileReport()
  {
    _report.open("profile.json", std::ios::out);
    _report << R"([{"name":"thread_name","ph":"M","pid":1,"tid":1,"args":{"name":"GPU"}})"
               ",\n";
    _report << R"({"name":"thread_name","ph":"M","pid":1,"tid":2,"args":{"name":"CPU"}})";
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

  void add_group(StringRefNull name,
                 uint64_t gpu_start,
                 uint64_t gpu_end,
                 uint64_t cpu_start,
                 uint64_t cpu_end)
  {
    std::scoped_lock lock(_mutex);

    _report << fmt::format(
        ",\n"
        R"({{"name":"{}","ph":"X","ts":{},"dur":{},"pid":1,"tid":1}})",
        name.c_str(),
        gpu_start / uint64_t(1000),
        (gpu_end - gpu_start) / uint64_t(1000));

    _report << fmt::format(
        ",\n"
        R"({{"name":"{}","ph":"X","ts":{},"dur":{},"pid":1,"tid":2}})",
        name.c_str(),
        cpu_start / uint64_t(1000),
        (cpu_end - cpu_start) / uint64_t(1000));
  }
};

}  // namespace blender::gpu
