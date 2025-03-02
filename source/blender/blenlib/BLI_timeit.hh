/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <chrono>
#include <string>

#include "BLI_sys_types.h"

namespace blender::timeit {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Nanoseconds = std::chrono::nanoseconds;

void print_duration(Nanoseconds duration);

class ScopedTimer {
 private:
  std::string name_;
  TimePoint start_;

 public:
  ScopedTimer(std::string name) : name_(std::move(name))
  {
    start_ = Clock::now();
  }

  ~ScopedTimer();
};

class ScopedTimerAveraged {
 public:
  struct ScopedTimerSamples {
   public:
    int64_t total_count;
    blender::timeit::Nanoseconds total_time;
    blender::timeit::Nanoseconds min_time;

    ScopedTimerSamples()
        : total_count(0), total_time(), min_time(blender::timeit::Nanoseconds::max())
    {
    }
  };

 private:
  std::string name_;
  TimePoint start_;

  ScopedTimerSamples &sample_counter_;

 public:
  ScopedTimerAveraged(std::string name, ScopedTimerSamples &sample_counter)
      : name_(std::move(name)), sample_counter_(sample_counter)
  {
    start_ = Clock::now();
  }

  ~ScopedTimerAveraged();
};

}  // namespace blender::timeit

#define SCOPED_TIMER(name) blender::timeit::ScopedTimer scoped_timer(name)

/**
 * Print the average and minimum runtime of the timer's scope.
 * \warning This uses static variables, so it is not thread-safe.
 */
#define SCOPED_TIMER_AVERAGED(name) \
  static blender::timeit::ScopedTimerAveraged::ScopedTimerSamples sample_counter_; \
  blender::timeit::ScopedTimerAveraged scoped_timer(name, sample_counter_)

/**
 * Same as `SCOPED_TIMER_AVERAGED` above but keeps a separate record for each unique sample name.
 * \warning Records are not shared/visible across compilation units.
 */
#define SCOPED_TIMER_AVERAGED_TABLE(name) \
  static std::unordered_map<std::string, \
                            blender::timeit::ScopedTimerAveraged::ScopedTimerSamples> \
      sample_counter_map; \
  blender::timeit::ScopedTimerAveraged::ScopedTimerSamples &sample = sample_counter_map[name]; \
  blender::timeit::ScopedTimerAveraged scoped_timer(name, sample);
