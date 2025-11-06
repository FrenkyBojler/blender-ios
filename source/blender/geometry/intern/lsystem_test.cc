/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "lsystem.hh"

namespace blender::geometry::lsystem::tests {

static void expect_generation_eq(const LSystem &lsystem,
                                 const int generation,
                                 const StringRef expected)
{
  ResourceScope scope;
  Turtle root_turtle;
  const std::optional<Vector<Symbol>> symbols = lsystem.compute_nth_generation(
      scope, root_turtle, generation);
  if (!symbols) {
    FAIL() << fmt::format("Failed to generate lsystem result. Expected: {}", expected);
    return;
  }

  const std::string symbols_str = lsystem.symbols_to_string(*symbols);
  EXPECT_EQ(symbols_str, expected);
}

TEST(lsystem, DoubleF)
{
  LSystem lsystem;
  lsystem.set_axiom("F");
  lsystem.add_rule("F=FF");

  expect_generation_eq(lsystem, 0, "F");
  expect_generation_eq(lsystem, 1, "FF");
  expect_generation_eq(lsystem, 2, "FFFF");
}

TEST(lsystem, FPlusA)
{
  LSystem lsystem;
  lsystem.set_axiom("F+A");
  lsystem.add_rule("A=F+A");

  expect_generation_eq(lsystem, 0, "F+A");
  expect_generation_eq(lsystem, 1, "F+F+A");
  expect_generation_eq(lsystem, 2, "F+F+F+A");
  expect_generation_eq(lsystem, 3, "F+F+F+F+A");
}

TEST(lsystem, FFFA)
{
  LSystem lsystem;
  lsystem.set_axiom("FFFA");
  lsystem.add_rule("A=\" [&FFFA] //// [&FFFA] //// [&FFFA]");

  expect_generation_eq(lsystem, 0, "FFFA");
  expect_generation_eq(lsystem, 1, "FFF\"[&FFFA]////[&FFFA]////[&FFFA]");
  expect_generation_eq(lsystem,
                       2,
                       "FFF\"[&FFF\"[&FFFA]////[&FFFA]////[&FFFA]]////[&FFF\"[&FFFA]////[&FFFA]///"
                       "/[&FFFA]]////[&FFF\"[&FFFA]////[&FFFA]////[&FFFA]]");
}

}  // namespace blender::geometry::lsystem::tests
