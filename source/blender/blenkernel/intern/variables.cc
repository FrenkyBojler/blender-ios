/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_scene.hh"
#include "BKE_variables.hh"

bool VariableMap::contains(blender::StringRef name) const
{
  if (this->strings.contains(name)) {
    return true;
  }
  if (this->integers.contains(name)) {
    return true;
  }
  if (this->floats.contains(name)) {
    return true;
  }
  return false;
}

bool VariableMap::remove(blender::StringRef name)
{
  if (this->strings.remove(name)) {
    return true;
  }
  if (this->integers.remove(name)) {
    return true;
  }
  if (this->floats.remove(name)) {
    return true;
  }
  return false;
}

bool VariableMap::add_string(blender::StringRef name, blender::StringRef value)
{
  if (this->contains(name)) {
    return false;
  }
  this->strings.add_new(name, value);
  return true;
}

bool VariableMap::add_integer(blender::StringRef name, int64_t value)
{
  if (this->contains(name)) {
    return false;
  }
  this->integers.add_new(name, value);
  return true;
}

bool VariableMap::add_float(blender::StringRef name, double value)
{
  if (this->contains(name)) {
    return false;
  }
  this->floats.add_new(name, value);
  return true;
}

std::optional<blender::StringRefNull> VariableMap::get_string(blender::StringRef name) const
{
  const std::string *value = this->strings.lookup_ptr(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return blender::StringRefNull(*value);
}

std::optional<int64_t> VariableMap::get_integer(blender::StringRef name) const
{
  const int64_t *value = this->integers.lookup_ptr(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return *value;
}

std::optional<double> VariableMap::get_float(blender::StringRef name) const
{
  const double *value = this->floats.lookup_ptr(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return *value;
}

/* -------------------------------------------------------------------- */

VariableMap BKE_build_blender_variables(const char *blend_file_path,
                                        std::optional<uint64_t> frame_number,
                                        const RenderData *render_data)
{
  VariableMap variables;

  /* Blend file name. */
  if (blend_file_path) {
    const char *file_name = BLI_path_basename(blend_file_path);
    if (file_name[0] != '\0') {
      const char *file_name_end = BLI_path_extension_or_end(file_name);
      if (file_name_end == file_name) {
        /* When the filename has no extension, but starts with a period. */
        variables.add_string("blend_name", blender::StringRef(file_name));
      }
      else {
        /* Normal case. */
        variables.add_string("blend_name", blender::StringRef(file_name, file_name_end));
      }
    }
  }

  /* Frame number. */
  if (frame_number.has_value()) {
    variables.add_integer("frame_number", *frame_number);
  }

  /* Start/end frame, render resolution, and fps. */
  if (render_data) {
    variables.add_integer("frame_start", render_data->sfra);
    variables.add_integer("frame_end", render_data->efra);

    int res_x, res_y;
    BKE_render_resolution(render_data, false, &res_x, &res_y);
    variables.add_integer("resolution_x", res_x);
    variables.add_integer("resolution_y", res_y);

    /* FPS eval code copied from `BKE_cachefile_filepath_get()`.
     *
     * TODO: should probably use one function for this everywhere to ensure that
     * fps is computed consistently, but at the time of writing no such function
     * seems to exist. Every place in the code base just has its own bespoke
     * code, using different precision, etc. */
    const double fps = double(render_data->frs_sec) / double(render_data->frs_sec_base);
    variables.add_float("fps", fps);
  }

  return variables;
}

/* -------------------------------------------------------------------- */

#define FORMAT_BUFFER_SIZE 128

namespace {

enum class FormatSpecifierType {
  /* No format specifier given. Use default formatting. */
  NONE = 0,

  /* The format specifier was a string of just "#" characters. E.g. "####". */
  INTEGER,

  /* The format specifier was a string of "#" characters with a single ".". E.g.
   * "###.##". */
  FLOAT,

  /* The format specifier was invalid due to incorrect syntax. */
  INVALID_SYNTAX,
};

/**
 * Specifies how a variable should be formatted into a string.
 */
struct FormatSpecifier {
  FormatSpecifierType type = FormatSpecifierType::NONE;

  /* For INTEGER and FLOAT formatting types, the number of digits indicated on
   * either side of the decimal point. */
  std::optional<uint8_t> integer_digit_count;
  std::optional<uint8_t> fractional_digit_count;
};

enum class TokenType {
  /* "{variable_name}" or "{variable_name:format_spec}". */
  VARIABLE,

  /* "{{", which is an escaped "{". */
  LEFT_CURLY_BRACE,

  /* "}}", which is an escaped "}". */
  RIGHT_CURLY_BRACE,
};

/**
 * A token that was parsed and should be substituted in the path string.
 */
struct Token {
  TokenType type = TokenType::VARIABLE;

  /* Byte index range (exclusive on the right) of the token in the path string.
   * This is the range that should be replaced during substitution. For example,
   * for variables this the byte range of the entire "{variable_name}" syntax. */
  blender::IndexRange replacement_range;

  /* Reference to the the variable name as written in the path string. Note that
   * this points into the path string, and does not own the value.
   *
   * Only relevant when `type == VARIABLE`. */
  blender::StringRef variable_name;

  /* Indicates how the variable's value should be formatted into a string. This
   * is derived from the format specification (e.g. the "###" in "{blah:###}").
   *
   * Only relevant when `type == VARIABLE`. */
  FormatSpecifier format;
};

}  // namespace

/**
 * Format an integer into a string, according to `format`.
 *
 * Note: if `format` is not valid for integers, the resulting string will be
 * empty.
 *
 * \return length of the produced string.
 */
static int format_int_to_string(const FormatSpecifier &format,
                                int64_t integer_value,
                                char r_output_string[FORMAT_BUFFER_SIZE])
{
  BLI_assert(format.type != FormatSpecifierType::INVALID_SYNTAX);

  r_output_string[0] = '\0';
  int output_length = 0;

  switch (format.type) {
    case FormatSpecifierType::NONE: {
      output_length = sprintf(r_output_string, "%ld", integer_value);
      break;
    }

    case FormatSpecifierType::INTEGER: {
      BLI_assert(format.integer_digit_count.has_value());
      BLI_assert(*format.integer_digit_count > 0);
      output_length = sprintf(
          r_output_string, "%0*ld", *format.integer_digit_count, integer_value);
      break;
    }

    case FormatSpecifierType::FLOAT: {
      /* Formatting an integer as a float: we do *not* defer to the float
       * formatter for this because we could lose precision with very large
       * numbers. Instead we simply print the integer, and then append ".000..."
       * to it. */
      BLI_assert(format.fractional_digit_count.has_value());
      BLI_assert(*format.fractional_digit_count > 0);

      if (format.integer_digit_count.has_value()) {
        BLI_assert(*format.integer_digit_count > 0);
        output_length = sprintf(
            r_output_string, "%0*ld", *format.integer_digit_count, integer_value);
      }
      else {
        output_length = sprintf(r_output_string, "%ld", integer_value);
      }

      r_output_string[output_length] = '.';
      output_length++;

      for (int i = 0; i < *format.fractional_digit_count; i++) {
        r_output_string[output_length] = '0';
        output_length++;
      }

      r_output_string[output_length] = '\0';

      break;
    }

    case FormatSpecifierType::INVALID_SYNTAX: {
      BLI_assert_msg(
          false,
          "Format specifiers with invalid syntax should have been rejected before getting here.");
      break;
    }
  }

  return output_length;
}

/**
 * Format a floating point number into a string, according to `format`.
 *
 * Note: if `format` is not valid for floating point numbers, the resulting
 * string will be empty.
 *
 * \return length of the produced string.
 */
static int format_float_to_string(const FormatSpecifier &format,
                                  double float_value,
                                  char r_output_string[FORMAT_BUFFER_SIZE])
{
  BLI_assert(format.type != FormatSpecifierType::INVALID_SYNTAX);

  r_output_string[0] = '\0';
  int output_length = 0;

  switch (format.type) {
    case FormatSpecifierType::NONE: {
      /* When no format specification is given, we attempt to approximate
       * Python's behavior in the same situation. We can't exactly match via
       * `sprintf()`, but we can get pretty close. The only major thing we can't
       * replicate via `sprintf()` is that in Python whole numbers are printed
       * with a trailing ".0". So we handle that bit manually. */
      output_length = sprintf(r_output_string, "%.16g", float_value);

      /* If the string consists only of digits and a possible negative sign, then
       * we append a ".0" to match Python. */
      if (blender::StringRef(r_output_string).find_first_not_of("-0123456789") ==
          std::string::npos)
      {
        r_output_string[output_length] = '.';
        r_output_string[output_length + 1] = '0';
        r_output_string[output_length + 2] = '\0';
        output_length += 2;
      }
      break;
    }

    case FormatSpecifierType::INTEGER: {
      /* Defer to the integer formatter with a rounded value. */
      return format_int_to_string(format, std::round(float_value), r_output_string);
    }

    case FormatSpecifierType::FLOAT: {
      BLI_assert(format.fractional_digit_count.has_value());
      BLI_assert(*format.fractional_digit_count > 0);

      if (format.integer_digit_count.has_value()) {
        /* Both integer and fractional component lengths are specified. */
        BLI_assert(*format.integer_digit_count > 0);
        output_length = sprintf(r_output_string,
                                "%0*.*f",
                                *format.integer_digit_count + *format.fractional_digit_count + 1,
                                *format.fractional_digit_count,
                                float_value);
      }
      else {
        /* Only fractional component length is specified. */
        output_length = sprintf(
            r_output_string, "%.*f", *format.fractional_digit_count, float_value);
      }

      break;
    }

    case FormatSpecifierType::INVALID_SYNTAX: {
      BLI_assert_msg(
          false,
          "Format specifiers with invalid syntax should have been rejected before getting here.");
      break;
    }
  }

  return output_length;
}

static FormatSpecifier parse_path_variable_format(const blender::StringRef format_specifier)
{
  FormatSpecifier format = {};

  /* A ":" was used, but no format specifier was given, which is invalid. */
  if (format_specifier.is_empty()) {
    format.type = FormatSpecifierType::INVALID_SYNTAX;
    return format;
  }

  /* If it's all digit specifiers, then format as an integer. */
  if (format_specifier.find_first_not_of("#") == std::string::npos) {
    format.integer_digit_count = format_specifier.size();

    format.type = FormatSpecifierType::INTEGER;
    return format;
  }

  /* If it's digit specifiers and a dot, format as a float. */
  const int64_t dot_index = format_specifier.find_first_of('.');
  const int64_t dot_index_last = format_specifier.find_last_of('.');
  const bool found_dot = dot_index != std::string::npos;
  const bool only_one_dot = dot_index == dot_index_last;
  if (format_specifier.find_first_not_of(".#") == std::string::npos && found_dot && only_one_dot) {
    const blender::StringRef left = format_specifier.substr(0, dot_index);
    const blender::StringRef right = format_specifier.substr(dot_index + 1);

    /* We currently require that the fractional digits are specified, so bail if
     * they aren't. */
    if (right.is_empty()) {
      format.type = FormatSpecifierType::INVALID_SYNTAX;
      return format;
    }

    if (!left.is_empty()) {
      format.integer_digit_count = left.size();
    }

    format.fractional_digit_count = right.size();

    format.type = FormatSpecifierType::FLOAT;
    return format;
  }

  format.type = FormatSpecifierType::INVALID_SYNTAX;
  return format;
}

/**
 * Finds and parses the first valid token in `path`.
 *
 * \param path The path string to parse.
 *
 * \param path_allocation_size The total amount of valid memory that `path`
 * points to, in bytes. This is just used as a fail-safe in case path isn't
 * properly null-terminated, to prevent reading off the end of valid memory.
 *
 * \return The parsed token information, or nullopt if no token is found in
 * `path`.
 */
static std::optional<Token> next_token(char *path, const int path_allocation_size)
{
  /* We use the magic number -1 here to indicate that a component hasn't been
   * found yet. When a component is found, the respective variable here is set
   * to the byte offset it was found at. */
  int start = -1;                  /* "{" */
  int format_specifier_split = -1; /* ":" */
  int end = -1;                    /* "}" */

  for (int byte_index = 0; byte_index < path_allocation_size && path[byte_index] != '\0';
       byte_index++)
  {
    /* Check for escaped {. */
    if ((byte_index + 1) < path_allocation_size && path[byte_index] == '{' &&
        path[byte_index + 1] == '{')
    {
      Token variable;
      variable.type = TokenType::LEFT_CURLY_BRACE;
      variable.replacement_range = blender::IndexRange::from_begin_end(byte_index, byte_index + 2);
      return variable;
    }

    /* Check for escaped }. */
    if ((byte_index + 1) < path_allocation_size && path[byte_index] == '}' &&
        path[byte_index + 1] == '}')
    {
      Token variable;
      variable.type = TokenType::RIGHT_CURLY_BRACE;
      variable.replacement_range = blender::IndexRange::from_begin_end(byte_index, byte_index + 2);
      return variable;
    }

    /* Check if we've found a starting "{".
     *
     * Note that if we're already inside a variable reference, this restarts
     * from the new one we've just found. This is okay, since it's not valid to
     * have `{` inside a variable reference, and this just treats such
     * situations as an incomplete (and thus invalid) variable reference. */
    if (path[byte_index] == '{') {
      start = byte_index;
      format_specifier_split = -1;
      continue;
    }

    /* If we haven't found a start, we shouldn't try to parse the other bits
     * yet. */
    if (start == -1) {
      continue;
    }

    /* Check if we've found a format splitter. */
    if (path[byte_index] == ':') {
      if (format_specifier_split == -1) {
        format_specifier_split = byte_index;
      }
      else {
        /* Found a second format specifier split. Invalid! Restart. */
        start = -1;
        format_specifier_split = -1;
      }
      byte_index++;
      continue;
    }

    /* Check if we've found the closing "}". */
    if (path[byte_index] == '}') {
      end = byte_index + 1; /* Exclusive end. */
      break;
    }
  }

  /* No variable reference found. */
  if (start == -1 || end == -1) {
    return std::nullopt;
  }

  /* Parse the variable reference we found. */
  Token variable;
  variable.replacement_range = blender::IndexRange::from_begin_end(start, end);
  if (format_specifier_split == -1) {
    /* No format specifier. */
    variable.variable_name = blender::StringRef(path + start + 1, path + end - 1);
  }
  else {
    /* Found format specifier. */
    variable.variable_name = blender::StringRef(path + start + 1, path + format_specifier_split);
    variable.format = parse_path_variable_format(
        blender::StringRef(path + format_specifier_split + 1, path + end - 1));
  }

  return variable;
}

bool BKE_path_apply_variables(char path[FILE_MAX], const VariableMap &variables)
{
  bool was_modified = false;

  int bytes_processed = 0;
  while (bytes_processed < FILE_MAX && path[bytes_processed] != '\0') {
    const std::optional<Token> token = next_token(path + bytes_processed,
                                                  FILE_MAX - bytes_processed);

    if (!token.has_value()) {
      break;
    }

    /* Skip variables with invalid format specifier syntax. */
    if (token->format.type == FormatSpecifierType::INVALID_SYNTAX) {
      bytes_processed += token->replacement_range.one_after_last();
      continue;
    }

    /* Check for escapes. */
    if (token->type == TokenType::LEFT_CURLY_BRACE) {
      BLI_string_replace_range(path + bytes_processed,
                               FILE_MAX - bytes_processed,
                               token->replacement_range.start(),
                               token->replacement_range.one_after_last(),
                               "{");

      bytes_processed += 1;
      was_modified = true;
      continue;
    }
    if (token->type == TokenType::RIGHT_CURLY_BRACE) {
      BLI_string_replace_range(path + bytes_processed,
                               FILE_MAX - bytes_processed,
                               token->replacement_range.start(),
                               token->replacement_range.one_after_last(),
                               "}");

      bytes_processed += 1;
      was_modified = true;
      continue;
    }

    /* For formatting integer and float variables into strings. */
    char format_buffer[FORMAT_BUFFER_SIZE];

    /* Points to the string that will replace the "{variable}" in `path`. If no
     * corresponding variable is found, or if the format specification is
     * invalid, this is left null to indicate that no replacement should be
     * done. */
    const char *replacement_string = nullptr;

    /* Try to find a matching variable, and construct a string for it. */
    if (std::optional<blender::StringRefNull> string_value = variables.get_string(
            token->variable_name))
    {
      /* String variable found, but we only process it if there's no format
       * specifier: string variables do not support format specifiers. */
      if (token->format.type == FormatSpecifierType::NONE) {
        replacement_string = string_value->c_str();
      }
    }
    else if (std::optional<int64_t> integer_value = variables.get_integer(token->variable_name)) {
      /* Integer variable found. */
      format_int_to_string(token->format, *integer_value, format_buffer);
      replacement_string = format_buffer;
    }
    else if (std::optional<double> float_value = variables.get_float(token->variable_name)) {
      /* Float variable found. */
      format_float_to_string(token->format, *float_value, format_buffer);
      replacement_string = format_buffer;
    }

    /* Perform the replacement if we found a matching variable, otherwise skip. */
    if (replacement_string != nullptr) {
      BLI_string_replace_range(path + bytes_processed,
                               FILE_MAX - bytes_processed,
                               token->replacement_range.start(),
                               token->replacement_range.one_after_last(),
                               replacement_string);

      bytes_processed += token->replacement_range.one_after_last();
      bytes_processed -= token->replacement_range.size();
      bytes_processed += strlen(replacement_string);

      was_modified = true;
    }
    else {
      /* No matching variable, so skip. */
      bytes_processed += token->replacement_range.one_after_last();
    }
  }

  return was_modified;
}
