/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifndef __KERNEL_GPU__
#  include "util/sanity_checker.h"  // IWYU pragma: export
#endif

CCL_NAMESPACE_BEGIN

#if defined(WITH_CYCLES_DEBUG)
#  define kernel_sanity_check(object) kernel_assert(is_valid(object))

#  define kernel_sanity_check_PDF(pdf) kernel_assert(is_valid_pdf(pdf))

#  define kernel_sanity_check_RND(rnd) kernel_assert(is_valid_rnd(rnd))
#else
#  define kernel_sanity_check(object)
#  define kernel_sanity_check_PDF(pdf)
#  define kernel_sanity_check_RND(pdf)
#endif

CCL_NAMESPACE_END
