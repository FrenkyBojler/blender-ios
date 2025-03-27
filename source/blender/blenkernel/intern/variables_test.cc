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

TEST(blender_variables, path_apply_variables)
{
  VariableMap variables;
  {
    variables.add_string("hi", "hello");
    variables.add_string("bye", "goodbye");
    variables.add_string("long", "This string is exactly 32 bytes.");
    variables.add_integer("the_answer", 42);
    variables.add_integer("prime", 7);
    variables.add_float("pi", 3.14159);
    variables.add_float("e", 2.71828);
    variables.add_float("ntsc", 30.0 / 1.001);
  }

  /* Simple case, testing all variables.
   *
   * TODO: the floats always print with 6 decimal digits. Investigate. */
  {
    char path[FILE_MAX] = "${hi}_${bye}_${the_answer}_${prime}_${pi}_${e}_${ntsc}";
    BKE_path_apply_variables(path, variables);
    EXPECT_EQ(blender::StringRef(path), "hello_goodbye_42_7_3.141590_2.718280_29.970030");
  }

  /* Integer formatting. */
  {
    char path[FILE_MAX] = "${the_answer:1}_${the_answer:2}_${the_answer:4}";
    BKE_path_apply_variables(path, variables);
    EXPECT_EQ(blender::StringRef(path), "42_42_0042");
  }

  /* Float formatting.
   *
   * TODO: the floats print with a maximum of 6 decimal digits. Investigate. */
  {
    char path[FILE_MAX] = "${pi:.4}_${e:.3}_${ntsc:.20}";
    BKE_path_apply_variables(path, variables);
    EXPECT_EQ(blender::StringRef(path), "3.1416_2.718_29.970030");
  }

  /* Missing variable. Substitution should continue on, simply ignoring the
   * missing variable. */
  {
    char path[FILE_MAX] = "${hi}_${missing}_${bye}";
    BKE_path_apply_variables(path, variables);
    EXPECT_EQ(blender::StringRef(path), "hello_${missing}_goodbye");
  }

  /* Malformed syntax: unclosed variable. */
  {
    char path[FILE_MAX] = "${hi_${hi}_${bye}";
    BKE_path_apply_variables(path, variables);
    EXPECT_EQ(blender::StringRef(path), "${hi_hello_goodbye");
  }

  /* Test what happens when the path would expand to a string that's longer than
   * `FILE_MAX`.
   *
   * We don't care so much about any kind of "correctness" here, we just want to
   * ensure that it still results in a valid null-terminated string that fits in
   * `FILE_MAX` bytes.
   *
   * NOTE: this test will have to be updated if `FILE_MAX` is ever changed. */
  {
    char path[FILE_MAX] =
        "_${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${"
        "long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}"
        "${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${"
        "long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}"
        "${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${"
        "long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}"
        "${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${"
        "long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}"
        "${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${"
        "long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}"
        "${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${long}${"
        "long}${long}${long}${long}${long}${long}${long}${long}${long}";
    const char result[FILE_MAX] =
        "_This string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly "
        "32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 "
        "bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 "
        "bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 "
        "bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 byte";
    BKE_path_apply_variables(path, variables);
    EXPECT_EQ(blender::StringRef(path), blender::StringRef(result));
  }
}

}  // namespace blender::bke::tests
