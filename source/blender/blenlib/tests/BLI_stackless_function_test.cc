/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "BLI_stackless_function.hh"

#include "testing/testing.h"

namespace blender::tests {

TEST(stackless_function, StacklessCall)
{
  int value = 5;
  stackless([&]() {
    value += 10;
  });
  EXPECT_EQ(value, 15);
}

TEST(stackless_function, StacklessCallWithReturn)
{
  const std::string value = stackless([&]() {
    return std::string("1234") + " 1234";
  });
  EXPECT_EQ(value, "1234 1234");
}

}  // namespace blender::tests
