/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_path_templates.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

// static void debug_print_error(const TemplateError &error)
// {
//   const char *type;
//   switch (error.type) {
//     case TemplateErrorType::UNESCAPED_CURLY_BRACE:
//       type = "UNESCAPED_CURLY_BRACE";
//       break;
//     case TemplateErrorType::VARIABLE_SYNTAX:
//       type = "VARIABLE_SYNTAX";
//       break;
//     case TemplateErrorType::FORMAT_SPECIFIER:
//       type = "FORMAT_SPECIFIER";
//       break;
//     case TemplateErrorType::UNKNOWN_VARIABLE:
//       type = "UNKNOWN_VARIABLE";
//       break;
//   }
//   printf("(%s, (%ld, %ld))", type, error.byte_range.start(), error.byte_range.size());
// }

// static void debug_print_errors(Span<TemplateError> errors)
// {
//   printf("[");
//   for (const TemplateError &error : errors) {
//     debug_print_error(error);
//     printf(", ");
//   }
//   printf("]\n");
// }

TEST(path_templates, TemplateVariableMap)
{
  TemplateVariableMap map;

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

TEST(path_templates, path_apply_variables)
{
  TemplateVariableMap variables;
  {
    variables.add_string("hi", "hello");
    variables.add_string("bye", "goodbye");
    variables.add_string("long", "This string is exactly 32 bytes.");
    variables.add_integer("the_answer", 42);
    variables.add_integer("prime", 7);
    variables.add_integer("i_negative", -7);
    variables.add_float("pi", 3.14159265358979323846);
    variables.add_float("e", 2.71828182845904523536);
    variables.add_float("ntsc", 30.0 / 1.001);
    variables.add_float("two", 2.0);
    variables.add_float("f_negative", -3.14159265358979323846);
    variables.add_float("huge", 200000000000000000000000000000000.0);
    variables.add_float("tiny", 0.000000000000000000000000000000002);
  }

  /* Simple case, testing all variables. */
  {
    char path[FILE_MAX] =
        "{hi}_{bye}_{the_answer}_{prime}_{i_negative}_{pi}_{e}_{ntsc}_{two}_{f_negative}_{huge}_{"
        "tiny}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path),
              "hello_goodbye_42_7_-7_3.141592653589793_2.718281828459045_29.97002997002997_2.0_-3."
              "141592653589793_2e+32_2e-33");
  }

  /* Integer formatting. */
  {
    char path[FILE_MAX] = "{the_answer:#}_{the_answer:##}_{the_answer:####}_{i_negative:####}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path), "42_42_0042_-007");
  }

  /* Integer formatting as float. */
  {
    char path[FILE_MAX] =
        "{the_answer:.###}_{the_answer:#.##}_{the_answer:###.##}_{i_negative:###.####}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path), "42.000_42.00_042.00_-07.0000");
  }

  /* Float formatting: specify fractional digits only. */
  {
    char path[FILE_MAX] =
        "{pi:.####}_{e:.###}_{ntsc:.########}_{two:.##}_{f_negative:.##}_{huge:.##}_{tiny:.##}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path),
              "3.1416_2.718_29.97002997_2.00_-3.14_200000000000000010732324408786944.00_0.00");
  }

  /* Float formatting: specify both integer and fractional digits. */
  {
    char path[FILE_MAX] =
        "{pi:##.####}_{e:####.###}_{ntsc:#.########}_{two:###.##}_{f_negative:###.##}_{huge:###.##"
        "}_{tiny:###.##}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(
        blender::StringRef(path),
        "03.1416_0002.718_29.97002997_002.00_-03.14_200000000000000010732324408786944.00_000.00");
  }

  /* Float formatting: format as integer. */
  {
    char path[FILE_MAX] = "{pi:##}_{e:####}_{ntsc:#}_{two:###}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path), "03_0003_30_002");
  }

  /* Escaping. "{{" and "}}" are the escape codes for literal "{" and "}". */
  {
    char path[FILE_MAX] = "{hi}_{{hi}}_{{{bye}}}_{bye}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path), "hello_{hi}_{goodbye}_goodbye");
  }

  /* Error: string variables do not support format specifiers. */
  {
    char path[FILE_MAX] = "{hi:##}_{bye:#}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(0, 7)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(8, 7)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path), "{hi:##}_{bye:#}");
  }

  /* Error: float formatting: specifying integer digits only (but still wanting
   * it printed as a float) is currently not supported. */
  {
    char path[FILE_MAX] =
        "{pi:##.}_{e:####.}_{ntsc:#.}_{two:###.}_{f_negative:###.}_{huge:###.}_{tiny:###.}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(0, 8)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(9, 9)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(19, 9)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(29, 10)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(40, 17)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(58, 11)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(70, 11)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path),
              "{pi:##.}_{e:####.}_{ntsc:#.}_{two:###.}_{f_negative:###.}_{huge:###.}_{tiny:###.}");
  }

  /* Error: missing variable. */
  {
    char path[FILE_MAX] = "{hi}_{missing}_{bye}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::UNKNOWN_VARIABLE, IndexRange(5, 9)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path), "{hi}_{missing}_{bye}");
  }

  /* Error: incomplete variable expression. */
  {
    char path[FILE_MAX] = "foo{hi";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::VARIABLE_SYNTAX, IndexRange(3, 3)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path), "foo{hi");
  }

  /* Error: invalid format specifiers. */
  {
    char path[FILE_MAX] = "{prime:}_{prime:.}_{prime:#.#.#}_{prime:sup}_{prime::sup}_{prime}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(0, 8)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(9, 9)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(19, 13)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(33, 11)},
        {TemplateErrorType::FORMAT_SPECIFIER, IndexRange(45, 12)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path),
              "{prime:}_{prime:.}_{prime:#.#.#}_{prime:sup}_{prime::sup}_{prime}");
  }

  /* Error: unclosed variable. */
  {
    char path[FILE_MAX] = "{hi_{hi}_{bye}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::VARIABLE_SYNTAX, IndexRange(0, 4)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path), "{hi_{hi}_{bye}");
  }

  /* Error: escaped braces inside variable. */
  {
    char path[FILE_MAX] = "{hi_{{hi}}_{bye}";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);
    const Vector<TemplateError> expected_errors = {
        {TemplateErrorType::VARIABLE_SYNTAX, IndexRange(0, 4)},
    };

    EXPECT_EQ(errors, expected_errors);
    EXPECT_EQ(blender::StringRef(path), "{hi_{{hi}}_{bye}");
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
        "___{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{"
        "long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}"
        "{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{"
        "long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}"
        "{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{"
        "long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}"
        "{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{"
        "long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}"
        "{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{"
        "long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}"
        "{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{"
        "long}{long}{long}{long}{long}{long}{long}{long}{long}{long}{long}";
    const char result[FILE_MAX] =
        "___This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 "
        "bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 "
        "bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 "
        "bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This string is "
        "exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 bytes.This "
        "string is exactly 32 bytes.This string is exactly 32 bytes.This string is exactly 32 by";
    const Vector<TemplateError> errors = BKE_path_apply_template(path, variables);

    EXPECT_TRUE(errors.is_empty());
    EXPECT_EQ(blender::StringRef(path), blender::StringRef(result));
  }
}

}  // namespace blender::bke::tests
