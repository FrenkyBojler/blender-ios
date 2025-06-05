/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * \brief Platform independent time functions.
 */

#pragma once

#include <chrono>

#ifndef WIN32
#include <thread>
#endif

/**
 * Return an indication of time, expressed as seconds since some fixed point.
 * Successive calls are guaranteed to generate values greater than or equal to the last call.
 */
extern double BLI_time_now_seconds(void);

/** `int` version of #BLI_time_now_seconds. */
extern long int BLI_time_now_seconds_i(void);

/**
 * Platform-independent sleep function.
 * \param ms: Number of milliseconds to sleep
 */
void BLI_time_sleep_ms(int ms);

#ifdef WIN32
void _BLI_WIN32_time_sleep_duration_nanoseconds(const std::chrono::nanoseconds &sleep_period_ns);
#endif

/**
 * Platform-independent high-resolution sleep function.
 * \param sleep_period: Duration to sleep
 */
template<class Rep, class Period>
inline void BLI_time_sleep_duration(const std::chrono::duration<Rep, Period> &sleep_period)
{
#ifdef WIN32
  _BLI_WIN32_time_sleep_duration_nanoseconds(
      std::chrono::duration_cast<std::chrono::nanoseconds>(sleep_period));
#else
  std::this_thread::sleep_for(sleep_period);
#endif
}
