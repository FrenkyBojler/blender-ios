/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include "BLI_string_ref.hh"
#include "BLI_timeit.hh"

#include <algorithm>
#include <iostream>

#include <fmt/format.h>

namespace blender::timeit {

static void format_duration(Nanoseconds duration, fmt::memory_buffer &buf)
{
  using namespace std::chrono;
  if (duration < microseconds(100)) {
    fmt::format_to(fmt::appender(buf), FMT_STRING("{} ns"), duration.count());
  }
  else if (duration < seconds(5)) {
    fmt::format_to(fmt::appender(buf), FMT_STRING("{:.2f} ms"), duration.count() / 1.0e6);
  }
  else if (duration > seconds(90)) {
    /* Long durations: print seconds, and also H:m:s */
    const auto dur_hours = duration_cast<hours>(duration);
    const auto dur_mins = duration_cast<minutes>(duration - dur_hours);
    const auto dur_sec = duration_cast<seconds>(duration - dur_hours - dur_mins);
    fmt::format_to(fmt::appender(buf),
                   FMT_STRING("{:.1f} s ({}H:{}m:{}s)"),
                   duration.count() / 1.0e9,
                   dur_hours.count(),
                   dur_mins.count(),
                   dur_sec.count());
  }
  else {
    fmt::format_to(fmt::appender(buf), FMT_STRING("{:.1f} s"), duration.count() / 1.0e9);
  }
}

void print_duration(Nanoseconds duration)
{
  fmt::memory_buffer buf;
  format_duration(duration, buf);
  std::cout << StringRef(buf.data(), buf.size());
}

ScopedTimer::~ScopedTimer()
{
  const TimePoint end = Clock::now();
  const Nanoseconds duration = end - start_;

  fmt::memory_buffer buf;
  fmt::format_to(fmt::appender(buf), FMT_STRING("Timer '{}' took "), name_);
  format_duration(duration, buf);
  buf.append(StringRef("\n"));
  std::cout << StringRef(buf.data(), buf.size());
}

ScopedTimerAveraged::~ScopedTimerAveraged()
{
  const TimePoint end = Clock::now();
  const Nanoseconds duration = end - start_;

  TimerMapEntry state{-1, blender::timeit::Nanoseconds::zero(), blender::timeit::Nanoseconds::max(), window_size_, start_ - std::chrono::seconds(1)};
  state = map_.lookup_or_add(name_, state);

  if (state.total_count < 0) {
    state.total_count = 0;
    map_.add_overwrite(name_, state);
    // First hit; discard the measurement and return.
    return;
  }

  state.total_count++;
  state.total_time += duration;
  state.min_time = std::min(duration, state.min_time);

  if (!state.window_size_ || state.total_count < state.window_size_) {
    state.rolling_average_ = state.total_time / state.total_count;
  }
  else {
    state.rolling_average_ = (state.rolling_average_ * (state.window_size_.value() - 1) / state.window_size_.value()) +
                       (duration / state.window_size_.value());
  }

  // Each timer instance only prints at most once per second.
  if ((end - state.last_print) < std::chrono::seconds(1)) {
    map_.add_overwrite(name_, state);
    return;
  }

  fmt::memory_buffer buf;
  fmt::format_to(fmt::appender(buf), FMT_STRING("Timer '{}': (Average: "), name_);
  format_duration(state.rolling_average_, buf);
  if (window_size_) {
    fmt::format_to(
        fmt::appender(buf), " of last {} events", std::min(state.window_size_.value(), state.total_count));
  }
  buf.append(StringRef(", Min: "));
  format_duration(state.min_time, buf);
  buf.append(StringRef(", Last: "));
  format_duration(duration, buf);
  fmt::format_to(fmt::appender(buf), ", Samples: {})\n", state.total_count);
  std::cout << StringRef(buf.data(), buf.size());

  state.last_print = end;
  map_.add_overwrite(name_, state);
}

}  // namespace blender::timeit
