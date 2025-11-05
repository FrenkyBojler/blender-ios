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
  const Vector<Symbol> symbols = lsystem.compute_nth_generation(generation);
  const std::string symbols_str = lsystem.symbols_to_string(symbols);
  EXPECT_EQ(symbols_str, expected);
}

TEST(lsystem, ApplyRules)
{
  LSystemBuilder builder;
  builder.set_axiom("F");
  builder.add_rule("F=FF");
  const LSystem lsystem = builder.build();

  expect_generation_eq(lsystem, 0, "F");
  expect_generation_eq(lsystem, 1, "FF");
  expect_generation_eq(lsystem, 2, "FFFF");
}

}  // namespace blender::geometry::lsystem::tests
