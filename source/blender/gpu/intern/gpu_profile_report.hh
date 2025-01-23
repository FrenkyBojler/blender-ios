#include "BLI_string_ref.hh"
#include <iostream>
#include <sstream>

namespace blender::gpu {

class ProfileReport {
 private:
  std::stringstream report;

 public:
  void begin(float gpu_total_time, float cpu_total_time)
  {
    report << "\n";
    report << " Group                          | GPU  | CPU  | Latency\n";
    report << "--------------------------------|------|------|--------\n";
    report << " Total                          | ";
    report << std::to_string(gpu_total_time).substr(0, 4) << " | ";
    report << std::to_string(cpu_total_time).substr(0, 4) << " | \n";
  }
  void add_group(StringRefNull name,
                 int stack_depth,
                 uint64_t gpu_start,
                 uint64_t gpu_end,
                 uint64_t cpu_start,
                 uint64_t cpu_end)
  {
    float gpu_time = (gpu_end - gpu_start) / 1000000.0;
    float cpu_time = (cpu_end - cpu_start) / 1000000.0;
    float latency = (gpu_start - cpu_start) / 1000000.0;

    report << std::string(stack_depth, '.');
    report << " " << name << std::string(std::max(0, 30 - stack_depth - int(name.size())), ' ')
           << " | ";
    report << std::to_string(gpu_time).substr(0, 4) << " | ";
    report << std::to_string(cpu_time).substr(0, 4) << " | ";
    report << std::to_string(latency).substr(0, 4) << "\n";
  }
  void end()
  {
    std::cout << report.str();
  }
};

}  // namespace blender::gpu
