/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

TEST(socket_value_variant2, Test)
{
  SocketValueVariant2 s;
  s.ensure_type<int>();
}

}  // namespace blender::bke::tests
