/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* Always include that so that `BLI_mutex.hh` can be used as replacement to including <mutex>.
 * Otherwise it might be confusing if both are included explicitly in a file. Also making the
 * difference between compiling with and without TBB smaller. */
#include <mutex>  // IWYU pragma: export

#ifdef WITH_TBB
#  include <tbb/mutex.h>
#endif

namespace blender {

#ifdef WITH_TBB
/**
 * This mutex has a size of just 1 byte (compared to e.g. the 40 bytes of std::mutex when using
 * GCC). It also has good performance characteristics in the non-contended and contended case. It's
 * not a fair mutex though, so if the mutex is always contended, it is possible that some thread
 * will never lock the mutex.
 */
using Mutex = tbb::mutex;
static_assert(sizeof(Mutex) == 1);
#else
using Mutex = std::mutex;
#endif

}  // namespace blender
