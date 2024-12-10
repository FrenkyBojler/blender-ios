/* SPDX-FileCopyrightText: 2011-2024 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "util/math.h"
#include "util/math_fast.h"

CCL_NAMESPACE_BEGIN

TEST(math, fast_sinf)
{
  /* Test a whole lot of various values across the range. */
  constexpr int N = 1000 + 1;
  for (int i = 0; i < N; ++i) {
    const float delta = 4 * M_PI_F / (N - 1);
    const float phi = -2 * M_PI_F + i * delta;
    EXPECT_NEAR(fast_sinf(phi), sinf(phi), 1e-4f);
  }

  /* Test exact arg values, which might not be covered above due to precision issues. */
  EXPECT_NEAR(fast_sinf(1.57085085f), 1.0f, 1e-4f);
  EXPECT_NEAR(fast_sinf(-1.57085085f), -1.0f, 1e-4f);
}

CCL_NAMESPACE_END
