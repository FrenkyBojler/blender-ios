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
 private:
  std::string name_;
  TimePoint start_;

  std::atomic<int64_t> &total_count_;
  std::atomic<int64_t> &total_time_;
  std::atomic<int64_t> &min_time_;

 public:
  ScopedTimerAveraged(std::string name,
                      std::atomic<int64_t> &total_count,
                      std::atomic<int64_t> &total_time,
                      std::atomic<int64_t> &min_time)
      : name_(std::move(name)),
        total_count_(total_count),
        total_time_(total_time),
        min_time_(min_time)
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
  static std::atomic<int64_t> total_count_ = 0; \
  static std::atomic<int64_t> total_time_ = 0; \
  static std::atomic<int64_t> min_time_ = INT64_MAX; \
  blender::timeit::ScopedTimerAveraged scoped_timer(name, total_count_, total_time_, min_time_)
