/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_cpp_type.hh"
#include "FN_field.hh"
#include "FN_field_recognize.hh"

namespace blender::fn::tests {

TEST(field, PolynomConst)
{
  EXPECT_TRUE(Polynom<int>::from_const(555).is_const());
  EXPECT_EQ(Polynom<int>::from_const(555).size(), 1);

  EXPECT_EQ(Polynom<int>::from_degree_variable(1, 1).is_const(), false);
  EXPECT_EQ(Polynom<int>::from_degree_variable(1, 1).size(), 2);
}

TEST(field, PolynomValue)
{
  EXPECT_EQ(Polynom<int>::from_const(555).value_at(0), 555);
  EXPECT_EQ(Polynom<int>::from_const(555).value_at(std::numeric_limits<int>::min()), 555);
  EXPECT_EQ(Polynom<int>::from_const(555).value_at(std::numeric_limits<int>::max()), 555);

  EXPECT_EQ(Polynom<int>::from_degree_variable(1, 2).value_at(-10), -20);
  EXPECT_EQ(Polynom<int>::from_degree_variable(1, 2).value_at(10), 20);

  EXPECT_EQ(Polynom<int>::from_degree_variable(2, 1).value_at(-10), 100);
  EXPECT_EQ(Polynom<int>::from_degree_variable(2, 1).value_at(10), 100);

  EXPECT_EQ(Polynom<int>::from_degree_variable(3, 1).value_at(-10), -1000);
  EXPECT_EQ(Polynom<int>::from_degree_variable(3, 1).value_at(10), 1000);
}

TEST(field, PolynomLine)
{
  EXPECT_TRUE(Polynom<int>::from_degree_variable(1, 1).is_unit_line());
  EXPECT_EQ(Polynom<int>::from_degree_variable(1, 1).as_unit_line(), std::make_pair(0, false));
  EXPECT_EQ(Polynom<int>::from_degree_variable(1, -1).as_unit_line(), std::make_pair(0, true));

  EXPECT_EQ(
      (Polynom<int>::from_const(3) + Polynom<int>::from_degree_variable(1, 1)).as_unit_line(),
      std::make_pair(3, false));
  EXPECT_EQ(
      (Polynom<int>::from_const(3) + Polynom<int>::from_degree_variable(1, -1)).as_unit_line(),
      std::make_pair(3, true));

  EXPECT_FALSE(Polynom<int>::from_degree_variable(0, 1).is_unit_line());
  EXPECT_FALSE(Polynom<int>::from_degree_variable(2, 1).is_unit_line());
  EXPECT_FALSE(Polynom<int>::from_degree_variable(3, 1).is_unit_line());
}

TEST(field, PolynomValueOps1)
{
  const Polynom<int> single = Polynom<int>::from_const(33);
  const Polynom<int> line = Polynom<int>::from_degree_variable(1, -44);
  const Polynom<int> parabola = Polynom<int>::from_degree_variable(2, 93);
  const Polynom<int> cubic = Polynom<int>::from_degree_variable(3, 103);

  EXPECT_EQ((-single).value_at(-55), -single.value_at(-55));
  EXPECT_EQ((-line).value_at(-55), -line.value_at(-55));
  EXPECT_EQ((-parabola).value_at(-55), -parabola.value_at(-55));
  EXPECT_EQ((-cubic).value_at(-55), -cubic.value_at(-55));

  EXPECT_EQ((single + single).value_at(-55), single.value_at(-55) + single.value_at(-55));
  EXPECT_EQ((line + line).value_at(-55), line.value_at(-55) + line.value_at(-55));
  EXPECT_EQ((parabola + parabola).value_at(-55), parabola.value_at(-55) + parabola.value_at(-55));
  EXPECT_EQ((cubic + cubic).value_at(-55), cubic.value_at(-55) + cubic.value_at(-55));

  EXPECT_EQ((single - single).value_at(-55), single.value_at(-55) - single.value_at(-55));
  EXPECT_EQ((line - line).value_at(-55), line.value_at(-55) - line.value_at(-55));
  EXPECT_EQ((parabola - parabola).value_at(-55), parabola.value_at(-55) - parabola.value_at(-55));
  EXPECT_EQ((cubic - cubic).value_at(-55), cubic.value_at(-55) - cubic.value_at(-55));

  EXPECT_EQ((single + line).value_at(-55), single.value_at(-55) + line.value_at(-55));
  EXPECT_EQ((single + parabola).value_at(-55), single.value_at(-55) + parabola.value_at(-55));
  EXPECT_EQ((single + cubic).value_at(-55), single.value_at(-55) + cubic.value_at(-55));

  EXPECT_EQ((single - line).value_at(-55), single.value_at(-55) - line.value_at(-55));
  EXPECT_EQ((single - parabola).value_at(-55), single.value_at(-55) - parabola.value_at(-55));
  EXPECT_EQ((single - cubic).value_at(-55), single.value_at(-55) - cubic.value_at(-55));

  EXPECT_EQ((single + line).value_at(-55), single.value_at(-55) + line.value_at(-55));
  EXPECT_EQ((parabola + line).value_at(-55), parabola.value_at(-55) + line.value_at(-55));
  EXPECT_EQ((cubic + line).value_at(-55), cubic.value_at(-55) + line.value_at(-55));

  EXPECT_EQ((single * line).value_at(-55), single.value_at(-55) * line.value_at(-55));
  EXPECT_EQ((parabola * line).value_at(-55), parabola.value_at(-55) * line.value_at(-55));
  EXPECT_EQ((cubic * line).value_at(-55), cubic.value_at(-55) * line.value_at(-55));
}

TEST(field, PolynomValueOps2)
{
  const Polynom<int> single = Polynom<int>::from_const(33);
  const Polynom<int> line = Polynom<int>::from_degree_variable(1, -44);
  const Polynom<int> parabola = Polynom<int>::from_degree_variable(2, 93);
  const Polynom<int> cubic = Polynom<int>::from_degree_variable(3, 103);

  const Polynom<int> first = single * cubic + parabola - line;
  const Polynom<int> second = single * parabola - line;
  const Polynom<int> third = line - cubic;

  EXPECT_EQ((-first).value_at(-55), -first.value_at(-55));
  EXPECT_EQ((-second).value_at(-55), -second.value_at(-55));
  EXPECT_EQ((-third).value_at(-55), -third.value_at(-55));

  EXPECT_EQ((first + second).value_at(-55), first.value_at(-55) + second.value_at(-55));
  EXPECT_EQ((first + third).value_at(-55), first.value_at(-55) + third.value_at(-55));

  EXPECT_EQ((first - second).value_at(-55), first.value_at(-55) - second.value_at(-55));
  EXPECT_EQ((first - third).value_at(-55), first.value_at(-55) - third.value_at(-55));

  EXPECT_EQ((first * second).value_at(-55), first.value_at(-55) * second.value_at(-55));
  EXPECT_EQ((first * third).value_at(-55), first.value_at(-55) * third.value_at(-55));
}

}  // namespace blender::fn::tests
