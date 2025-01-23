#include "BLI_string_ref.hh"
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <sstream>

namespace blender::gpu {

class ProfileReport {
 private:
  std::fstream report;
  bool first_entry = true;

  ProfileReport()
  {
    report.open("profile.json", std::ios::out);
    report << "{\n\"traceEvents\": [\n";
  }

  ~ProfileReport()
  {
    report << "\n]\n}\n";
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
    if (!first_entry) {
      /* Avoid trailing commas. */
      report << ",\n";
    }
    first_entry = false;

    report << fmt::format(
        R"({{"cat":"GPU","pid":1,"tid":{},"ph":"X","ts":{},"dur":{},"name":"{}"}},)"
        "\n",
        1,
        gpu_start,
        gpu_end - gpu_start,
        name.c_str());

    report << fmt::format(
        R"({{"cat":"CPU","pid":2,"tid":{},"ph":"X","ts":{},"dur":{},"name":"{}"}})",
        1,
        cpu_start,
        cpu_end - cpu_start,
        name.c_str());
  }
};

}  // namespace blender::gpu
