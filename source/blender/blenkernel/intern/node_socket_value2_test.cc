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

TEST_F(SocketValueVariantTest, IntToFloatField)
{
  SocketValueVariant2 s;
  s.ensure_type<int>() = 23;
  const fn::Field<float> &f = s.ensure_type<fn::Field<float>>();
  EXPECT_FALSE(f.depends_on_input());
  EXPECT_EQ(fn::evaluate_constant_field(f), 23.0f);
}

TEST_F(SocketValueVariantTest, IntToGField)
{
  SocketValueVariant2 s;
  s.ensure_type<int>() = 23;
  const fn::GField &f = s.ensure_type<fn::GField>();
  EXPECT_FALSE(f.depends_on_input());
  EXPECT_TRUE(f.cpp_type().is<int>());
  EXPECT_EQ(fn::evaluate_constant_field(f.typed<int>()), 23);
}

TEST_F(SocketValueVariantTest, ConstantIntFieldToInt)
{
  SocketValueVariant2 s;
  s.ensure_type<fn::Field<int>>() = fn::Field<int>(42);
  const int &v = s.ensure_type<int>();
  EXPECT_EQ(v, 42);
}

TEST_F(SocketValueVariantTest, ConstantIntFieldToFloat)
{
  SocketValueVariant2 s;
  s.ensure_type<fn::Field<float>>() = fn::Field<float>(42.0f);
  const int &v = s.ensure_type<int>();
  EXPECT_EQ(v, 42);
}

TEST_F(SocketValueVariantTest, ConstIntFieldToFloatField)
{
  SocketValueVariant2 s;
  s.ensure_type<fn::Field<int>>() = fn::Field<int>(42);
  const fn::Field<float> &f = s.ensure_type<fn::Field<float>>();
  EXPECT_FALSE(f.depends_on_input());
  EXPECT_EQ(fn::evaluate_constant_field(f), 42.0f);
}

TEST_F(SocketValueVariantTest, IndexFieldToFloatField)
{
  SocketValueVariant2 s;
  s.ensure_type<fn::Field<int>>() = fn::IndexFieldInput::get_field();
  const fn::Field<float> &f = s.ensure_type<fn::Field<float>>();
  EXPECT_TRUE(f.depends_on_input());

  fn::FieldContext context;
  fn::FieldEvaluator evaluator{context, 5};
  evaluator.add(f);
  evaluator.evaluate();
  const VArray<float> values = evaluator.get_evaluated<float>(0);
  EXPECT_EQ(values.size(), 5);
  EXPECT_EQ(values[0], 0.0f);
  EXPECT_EQ(values[1], 1.0f);
  EXPECT_EQ(values[2], 2.0f);
  EXPECT_EQ(values[3], 3.0f);
  EXPECT_EQ(values[4], 4.0f);
}

}  // namespace blender::bke::tests
