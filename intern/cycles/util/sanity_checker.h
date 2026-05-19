/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifndef __KERNEL_GPU__
#  include "util/sanity_checker.h"  // IWYU pragma: export
#endif

CCL_NAMESPACE_BEGIN

# if defined(WITH_CYCLES_DEBUG)
# define SANITY_IS_VALID(object) \
    kernel_assert(is_valid(object)) 

# define SANITY_IS_VALID_PDF(pdf) \
    kernel_assert(is_valid_pdf(pdf))

# define SANITY_IS_VALID_RND(rnd) \
    kernel_assert(is_valid_rnd(rnd))
#else 
# define SANITY_IS_VALID(object)
# define SANITY_IS_VALID_PDF(pdf)
# define SANITY_IS_VALID_RND(pdf)
# endif 

CCL_NAMESPACE_END