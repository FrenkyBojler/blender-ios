/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_gtest_base.hh"
#include "BKE_node_socket_value2.hh"

#include "FN_field.hh"
#include "FN_field_evaluation.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class SocketValueVariantTest : public BlenderGTestBase {};

TEST_F(SocketValueVariantTest, SimpleInt)
{
  SocketValueVariant2 s;
  {
    int &x = s.ensure_type<int>();
    x = 5;
  }
  {
    const int *x = s.get_if<int>();
    EXPECT_NE(x, nullptr);
    EXPECT_EQ(*x, 5);
  }
}

TEST_F(SocketValueVariantTest, IntToFloat)
{
  SocketValueVariant2 s;
  {
    float &x = s.ensure_type<float>();
    x = 5.3f;
  }
  {
    const int *x = s.get_if<int>();
    EXPECT_EQ(x, nullptr);
  }
  {
    int &x = s.ensure_type<int>();
    EXPECT_EQ(x, 5);
  }
}

TEST_F(SocketValueVariantTest, IntToIntField)
{
  SocketValueVariant2 s;
  s.ensure_type<int>() = 23;
  const fn::Field<int> &f = s.ensure_type<fn::Field<int>>();
  EXPECT_FALSE(f.depends_on_input());
  EXPECT_EQ(fn::evaluate_constant_field(f), 23);
}

}  // namespace blender::bke::tests
