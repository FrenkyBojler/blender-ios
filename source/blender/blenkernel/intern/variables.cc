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
     * TODO: it might make sense to make a function for this to ensure that all
     * uses of these render variables produce a consistent fps? */
    const double fps = double(render_data->frs_sec) / double(render_data->frs_sec_base);
    variables.add_float("fps", fps);
  }

  return variables;
}

/* -------------------------------------------------------------------- */

/* Anonymous namespace to make the types local to this file. */
namespace {

enum class FormatSpecifierType {
  /* No format specifier given. */
  NONE = 0,

  /* The format specifier was invalid, due to e.g. incorrect syntax. */
  INVALID,

  INTEGER,
  FLOAT,
};

struct FormatSpecifier {
  FormatSpecifierType type = FormatSpecifierType::NONE;

  std::optional<uint8_t> fixed_integer_digits;
  std::optional<uint8_t> fixed_fractional_digits;
};

enum class TokenType {
  VARIABLE,

  /* An escaped { */
  LEFT_CURLY_BRACE,

  /* An escaped } */
  RIGHT_CURLY_BRACE,
};

/**
 * A token that was parsed and should be substituted in the path string.
 */
struct Token {
  TokenType type = TokenType::VARIABLE;

  /* Byte index range (exclusive on the right) in the path string that should be
   * replaced with the variable value. This is the full range of the `{blah}`
   * syntax that was parsed. */
  blender::IndexRange replacement_range;

  /* Reference to the the variable name as written in the path string. Note that
   * this references the path string, and does not own the value.
   *
   * Only relevant when `type == VARIABLE`. */
  blender::StringRef variable_name;

  /* Indicates how the variable's value should be formatted into a string. This
   * is derived from the format string after the `:` in e.g. `${blah:5}`.
   *
   * Only relevant when `type == VARIABLE`. */
  FormatSpecifier format;
};

}  // namespace

/**
 * \return length of the produced string.
 */
static int format_int_to_string(const FormatSpecifier &format,
                                char *output_string,
                                int64_t integer_value)
{
  int output_length = 0;

  if (format.fixed_integer_digits.has_value()) {
    output_length = sprintf(output_string, "%0*ld", *format.fixed_integer_digits, integer_value);
  }
  else {
    output_length = sprintf(output_string, "%ld", integer_value);
  }

  return output_length;
}

/**
 * \return length of the produced string.
 */
static int format_float_to_string(const FormatSpecifier &format,
                                  char *output_string,
                                  double float_value)
{
  /* If an integer format was specified, defer to the integer formatter with a
   * rounded value. */
  if (format.type == FormatSpecifierType::INTEGER) {
    const int int_length = format_int_to_string(format, output_string, std::round(float_value));
    return int_length;
  }

  int output_length = 0;
  if (format.fixed_integer_digits.has_value() && format.fixed_fractional_digits.has_value()) {
    /* Both integer and fractional component lengths are specified. */
    output_length = sprintf(output_string,
                            "%0*.*f",
                            *format.fixed_integer_digits + *format.fixed_fractional_digits + 1,
                            *format.fixed_fractional_digits,
                            float_value);
  }
  else if (format.fixed_integer_digits.has_value()) {
    /* Only integer component length is specified.
     *
     * We currently don't support this as it's not clear exactly what should
     * happen. We can revisit this in the future when people have an actual use
     * case. */
  }
  else if (format.fixed_fractional_digits.has_value()) {
    /* Only fractional component length is specified. */
    output_length = sprintf(output_string, "%.*f", *format.fixed_fractional_digits, float_value);
  }
  else {
    /* No format specification is given.
     *
     * When no format specification is given, we attempt to approximate Python's
     * behavior when no format specification is given. We can't exactly match
     * via `sprintf()`, but we can get pretty close. The only major difference
     * that we can't replicate is that in `sprintf()` whole numbers are printed
     * without a trailing ".0", whereas in Python they are. So we handle that
     * bit manually. */
    output_length = sprintf(output_string, "%.16g", float_value);

    /* If the string consists only of digits and a possible negative sign, then
     * we append a ".0" to match Python. */
    if (blender::StringRef(output_string).find_first_not_of("-0123456789") == std::string::npos) {
      output_string[output_length] = '.';
      output_string[output_length + 1] = '0';
      output_string[output_length + 2] = '\0';
      output_length += 2;
    }
  }

  return output_length;
}

static FormatSpecifier parse_path_variable_format(const blender::StringRef format_specifier)
{
  FormatSpecifier format = {};

  /* A ":" was used, but no format specifier was given, which is invalid. */
  if (format_specifier.is_empty()) {
    format.type = FormatSpecifierType::INVALID;
    return format;
  }

  /* If it's all digit specifiers, then format as an integer. */
  if (format_specifier.find_first_not_of("#") == std::string::npos) {
    format.fixed_integer_digits = format_specifier.size();

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
      format.type = FormatSpecifierType::INVALID;
      return format;
    }

    if (!left.is_empty()) {
      format.fixed_integer_digits = left.size();
    }

    format.fixed_fractional_digits = right.size();

    format.type = FormatSpecifierType::FLOAT;
    return format;
  }

  format.type = FormatSpecifierType::INVALID;
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
  /* We use magic number -1 to indicate that the component hasn't been found
   * yet. Otherwise they are the byte offset at which the component was found. */
  int start = -1;
  int format_specifier_split = -1;
  int end = -1;

  /* Just a simple loop over the bytes of the path. */
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
    if (token->format.type == FormatSpecifierType::INVALID) {
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

    /* For computing strings for integer and float variables. */
    char string_buffer[128];

    /* Will point to the string to substitute the variable with in `path`. If no
     * corresponding variable is found, is left null. */
    const char *replacement_string = nullptr;

    /* Try to find a matching variable, and construct a string for it. */
    if (std::optional<blender::StringRefNull> string_value = variables.get_string(
            token->variable_name))
    {
      /* String variable found, but only process if there's no format specifier.
       * String variables do not support format specifiers. */
      if (token->format.type == FormatSpecifierType::NONE) {
        replacement_string = string_value->c_str();
      }
    }
    else if (std::optional<int64_t> integer_value = variables.get_integer(token->variable_name)) {
      /* Integer variable found. */
      format_int_to_string(token->format, string_buffer, *integer_value);
      replacement_string = string_buffer;
    }
    else if (std::optional<double> float_value = variables.get_float(token->variable_name)) {
      /* Float variable found. */
      format_float_to_string(token->format, string_buffer, *float_value);
      replacement_string = string_buffer;
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
