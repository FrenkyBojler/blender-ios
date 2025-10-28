/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <mutex>

#include "BLI_map.hh"
#include "BLI_sys_types.h"

namespace blender::timeit {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Nanoseconds = std::chrono::nanoseconds;

void print_duration(Nanoseconds duration);

template <typename U, typename V>
class LockedMap: public blender::Map<U, V> {
 private:
  std::mutex mutex_;

 public:
  LockedMap() = default;

  V lookup_or_add(const U &key, const V &value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return blender::Map<U, V>::lookup_or_add(key, value);
  }
  V lookup(const U &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    V value = blender::Map<U, V>::lookup(key);
    return value;
  }
  bool add_overwrite(const U &key, const V &value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return blender::Map<U, V>::add_overwrite(key, value);
  }
};

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

struct TimerMapEntry {
  int64_t total_count;
  blender::timeit::Nanoseconds total_time;
  blender::timeit::Nanoseconds min_time;
  std::optional<int64_t> window_size_;
  TimePoint last_print;
  Nanoseconds rolling_average_ = Nanoseconds();
};

using TimerMap = LockedMap<std::string, TimerMapEntry>;


class ScopedTimerAveraged {
 private:
  std::string name_;
  TimePoint start_;
  TimerMap &map_;
  std::optional<int64_t> window_size_;

 public:
  ScopedTimerAveraged(std::string name,
                      TimerMap& map,
                      std::optional<int64_t> window_size = std::nullopt)
      : name_(std::move(name)),
      map_(map),
      window_size_(window_size)
  {
    start_ = Clock::now();
  }

  ~ScopedTimerAveraged();
};

}  // namespace blender::timeit

#define SCOPED_TIMER(name) timeit::ScopedTimer scoped_timer(name)

/**
 * ## is normally evaluated before __LINE__,
 * and we have to go through these contortions to use __LINE__ in variable names.
 */
#define TIMER_CONCAT2(X, Y) X##Y
#define TIMER_CONCAT(X, Y) TIMER_CONCAT2(X, Y)

/**
 * Print the average and minimum runtime of the timer's scope.
 * \warning This uses static variables, so it is not thread-safe.
 */
#define SCOPED_TIMER_AVERAGED(name) \
  static blender::timeit::TimerMap TIMER_CONCAT(timer_map_, __LINE__); \
  blender::timeit::ScopedTimerAveraged TIMER_CONCAT(scoped_timer, __LINE__)(name, \
      TIMER_CONCAT(timer_map_, __LINE__));

/**
 * Print the rolling average and minimum runtime of the timer's scope.
 * \warning This uses static variables, so it is not thread-safe.
 */

#define SCOPED_TIMER_ROLLING_AVERAGED(name, window_size) \
  static blender::timeit::TimerMap TIMER_CONCAT(timer_map_, __LINE__); \
  blender::timeit::ScopedTimerAveraged TIMER_CONCAT(scoped_timer, __LINE__)(name, \
      TIMER_CONCAT(timer_map_, __LINE__), window_size);
