/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

TEST(socket_value_variant2, SimpleInt)
{
  SocketValueVariant2 s;
  {
    int &x = s.ensure_type<int>();
    x = 5;
  }
  const int *x = s.get_if<int>();
  EXPECT_NE(x, nullptr);
  EXPECT_EQ(*x, 5);
}

}  // namespace blender::bke::tests
