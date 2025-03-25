/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_variables.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

TEST(blender_variables, VariableMap)
{
  VariableMap map;

  /* With in empty variable map, these should all return false / fail. */
  EXPECT_EQ(false, map.contains("hello"));
  EXPECT_EQ(false, map.remove("hello"));
  EXPECT_EQ(std::nullopt, map.get_string("hello"));
  EXPECT_EQ(std::nullopt, map.get_integer("hello"));
  EXPECT_EQ(std::nullopt, map.get_float("hello"));

  /* Populate the map. */
  EXPECT_EQ(true, map.add_string("hello", "What a wonderful world."));
  EXPECT_EQ(true, map.add_integer("bye", 42));
  EXPECT_EQ(true, map.add_float("what", 3.14159));

  /* Attempting to add variables with those names again should fail, since they
   * already exist now. */
  EXPECT_EQ(false, map.add_string("hello", "Sup."));
  EXPECT_EQ(false, map.add_string("bye", "Sup."));
  EXPECT_EQ(false, map.add_string("what", "Sup."));
  EXPECT_EQ(false, map.add_integer("hello", 2));
  EXPECT_EQ(false, map.add_integer("bye", 2));
  EXPECT_EQ(false, map.add_integer("what", 2));
  EXPECT_EQ(false, map.add_float("hello", 2.71828));
  EXPECT_EQ(false, map.add_float("bye", 2.71828));
  EXPECT_EQ(false, map.add_float("what", 2.71828));

  /* Confirm that the right variables exist. */
  EXPECT_EQ(true, map.contains("hello"));
  EXPECT_EQ(true, map.contains("bye"));
  EXPECT_EQ(true, map.contains("what"));
  EXPECT_EQ(false, map.contains("not here"));

  /* Fetch the variables we added. */
  EXPECT_EQ("What a wonderful world.", map.get_string("hello"));
  EXPECT_EQ(42, map.get_integer("bye"));
  EXPECT_EQ(3.14159, map.get_float("what"));

  /* The same variables shouldn't exist for the other types, despite our attempt
   * to add them earlier. */
  EXPECT_EQ(std::nullopt, map.get_integer("hello"));
  EXPECT_EQ(std::nullopt, map.get_float("hello"));
  EXPECT_EQ(std::nullopt, map.get_string("bye"));
  EXPECT_EQ(std::nullopt, map.get_float("bye"));
  EXPECT_EQ(std::nullopt, map.get_string("what"));
  EXPECT_EQ(std::nullopt, map.get_integer("what"));

  /* Remove the variables. */
  EXPECT_EQ(true, map.remove("hello"));
  EXPECT_EQ(true, map.remove("bye"));
  EXPECT_EQ(true, map.remove("what"));

  /* The variables shouldn't exist anymore. */
  EXPECT_EQ(false, map.contains("hello"));
  EXPECT_EQ(false, map.contains("bye"));
  EXPECT_EQ(false, map.contains("what"));
  EXPECT_EQ(std::nullopt, map.get_string("hello"));
  EXPECT_EQ(std::nullopt, map.get_integer("bye"));
  EXPECT_EQ(std::nullopt, map.get_float("what"));
  EXPECT_EQ(false, map.remove("hello"));
  EXPECT_EQ(false, map.remove("bye"));
  EXPECT_EQ(false, map.remove("what"));
}

}  // namespace blender::bke::tests
