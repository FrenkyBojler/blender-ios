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
  std::fstream report;

  ProfileReport()
  {
    report.open("profile.json", std::ios::out);
    report << R"([{"name":"thread_name","ph":"M","pid":1,"tid":1,"args":{"name":"GPU"}})"
              ",\n";
    report << R"({"name":"thread_name","ph":"M","pid":1,"tid":2,"args":{"name":"CPU"}})";
  }

  ~ProfileReport()
  {
    report << "\n]\n";
    report.close();
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
    report << fmt::format(
        ",\n"
        R"({{"name":"{}","ph":"X","ts":{},"dur":{},"pid":1,"tid":1}})",
        name.c_str(),
        gpu_start / uint64_t(1000),
        (gpu_end - gpu_start) / uint64_t(1000));

    report << fmt::format(
        ",\n"
        R"({{"name":"{}","ph":"X","ts":{},"dur":{},"pid":1,"tid":2}})",
        name.c_str(),
        cpu_start / uint64_t(1000),
        (cpu_end - cpu_start) / uint64_t(1000));
  }
};

}  // namespace blender::gpu
