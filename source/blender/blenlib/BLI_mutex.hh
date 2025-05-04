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
 * This mutex has pretty much the same interface as std::mutex which some limitations. Therefore,
 * it can be used with std::lock_guard and the like. It should be used by default in Blender unless
 * there are some special needs that really require a different mutex. For example, if the mutex
 * needs to be used with std::condition_variable, then std::mutex has to be used.
 *
 * The mutex provided by TBB has these properties:
 * - It's only 1 byte large, compared to e.g. 40 bytes when using the std::mutex of GCC.
 * - It's as fast as a spin-lock in the non-contended case, i.e. when no other thread is trying to
 * lock the mutex at the same time.
 * - In the contended case, it spins a couple of times but then blocks to avoid draining system
 *   resources by spinning for a long time.
 * - It is *not* a fair mutex, i.e. it's not guaranteed that a thread will ever be able to lock the
 *   mutex when there are always more than one threads that try to lock it. In the majority of
 *   cases, using a fair mutex just causes extra overhead without any benefit. std::mutex is not
 *   guaranteed to be fair either.
 */
using Mutex = tbb::mutex;
static_assert(sizeof(Mutex) == 1);

#else

/** Use std::mutex as fallback when compiling without TBB. */
using Mutex = std::mutex;

#endif

}  // namespace blender
