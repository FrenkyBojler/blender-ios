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

//-------------------------------------------------------------

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

enum class VariableFormatType {
  NONE = 0,
  INTEGER,
  FLOAT,
};

struct VariableFormat {
  VariableFormatType type;

  std::optional<uint8_t> fixed_integer_digits;
  std::optional<uint8_t> fixed_fractional_digits;
};

static std::optional<VariableFormat> parse_path_variable_format(
    const blender::StringRef format_specifier)
{
  VariableFormat format = {};

  if (format_specifier.is_empty() || format_specifier.size() > 5) {
    return std::nullopt;
  }

  /* If it's all digits. */
  if (format_specifier.find_first_not_of("0123456789") == std::string::npos) {
    format.type = VariableFormatType::INTEGER;
    format.fixed_integer_digits = std::stoi(format_specifier);
    return format;
  }

  /* If it's digits and a dot. */
  const int64_t dot_index = format_specifier.find_first_of('.');
  const int64_t dot_index_last = format_specifier.find_last_of('.');
  const bool found_dot = dot_index != std::string::npos;
  const bool only_one_dot = dot_index == dot_index_last;
  const bool not_just_dot = format_specifier.size() > 1;
  if (format_specifier.find_first_not_of(".0123456789") == std::string::npos && found_dot &&
      only_one_dot && not_just_dot)
  {
    format.type = VariableFormatType::FLOAT;
    const blender::StringRef left = format_specifier.substr(0, dot_index);
    if (!left.is_empty()) {
      format.fixed_integer_digits = std::stoi(left);
    }

    const blender::StringRef right = format_specifier.substr(dot_index + 1);
    if (!right.is_empty()) {
      format.fixed_fractional_digits = std::stoi(right);
    }

    return format;
  }

  return std::nullopt;
}

struct ParsedPathVariable {
  blender::IndexRange replacement_range;
  blender::StringRef name;

  /* Simply the empty string when there is no format specifier. */
  VariableFormat format;
};

static std::optional<ParsedPathVariable> next_path_variable(char *path,
                                                            const int path_allocation_size)
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
    /* Check if we've found a starting "${". */
    if (start == -1) {
      if ((byte_index + 1) < path_allocation_size && path[byte_index] == '$' &&
          path[byte_index + 1] == '{')
      {
        start = byte_index;
        byte_index++; /* To jump past the "{" as well. */
      }
      continue;
    }

    /* "$" or "{" within a variable name is illegal, so we bail.
     *
     * TODO: is this the right thing to do when we encounter this? */
    if (path[byte_index] == '$' || path[byte_index] == '{') {
      return std::nullopt;
    }

    /* Check if we've found a format splitter. */
    if (path[byte_index] == ':') {
      if (format_specifier_split != -1) {
        /* Found a second format specifier split. Invalid! Bail.
         *
         * TODO: is this the right thing to do when we encounter this? */
        return std::nullopt;
      }
      format_specifier_split = byte_index;
      byte_index++;
      continue;
    }

    /* Check if we've found the closing "}". */
    if (path[byte_index] == '}') {
      end = byte_index + 1; /* Exclusive end. */
      break;
    }
  }

  if (start == -1 || end == -1) {
    return std::nullopt;
  }

  /* TODO: syntax checks. */

  ParsedPathVariable variable;
  variable.replacement_range = blender::IndexRange::from_begin_end(start, end);
  if (format_specifier_split == -1) {
    /* No format specifier. */
    variable.name = blender::StringRef(path + start + 2, path + end - 1);
  }
  else {
    /* Found format specifier. */
    variable.name = blender::StringRef(path + start + 2, path + format_specifier_split);

    if (std::optional<VariableFormat> format = parse_path_variable_format(
            blender::StringRef(path + format_specifier_split + 1, path + end - 1)))
    {
      variable.format = *format;
    }
    else {
      /* Invalid format specifier. Bail!
       *
       * TODO: is this the right thing to do when we encounter this? */
      return std::nullopt;
    }
  }

  return variable;
}

/**
 * \return length of the produced string.
 */
static int format_int_to_string(const VariableFormat &format,
                                char *output_string,
                                int64_t integer_value)
{
  sprintf(output_string, "%ld", integer_value);
  int length = strlen(output_string);

  /* Ensure the length of the string is at least the minimum specified digits,
   * if that was specified. */
  if (format.fixed_integer_digits && length < *format.fixed_integer_digits) {
    const int diff = *format.fixed_integer_digits - length;
    for (int i = 0; i < diff; i++) {
      output_string[i] = '0';
    }
    sprintf(output_string + diff, "%ld", integer_value);
    length = *format.fixed_integer_digits;
  }

  return length;
}

/**
 * \return length of the produced string.
 */
static int format_float_to_string(const VariableFormat &format,
                                  char *output_string,
                                  double float_value)
{
  /* If an integer format was specified, defer to the integer formatter with a
   * rounded value. */
  if (format.type == VariableFormatType::INTEGER) {
    const int int_length = format_int_to_string(format, output_string, std::round(float_value));
    return int_length;
  }

  /* Round to the desired number of fractional decimal digits. Note that this
   * needs to be done *before* we take the integer part, because rounding can
   * propagate from the fractional part to the integer part.
   *
   * TODO: this isn't 100% correct due to floating point rounding error, but
   * since we're using doubles here it shouldn't cause any practical problems.
   * Nevertheless, doing something actually correct to format floats would be
   * nice! */
  if (format.fixed_fractional_digits) {
    uint64_t factor = 1;
    for (int i = 0; i < *format.fixed_fractional_digits; i++) {
      factor *= 10;
    }
    float_value = std::round(float_value * factor) / factor;
  }

  const int64_t integer_part = float_value;
  const int int_length = format_int_to_string(format, output_string, integer_part);

  double tmp; /* Just needed for the call to `modf()`. We don't actually use it. */
  const double fractional_part = std::abs(std::modf(float_value, &tmp));
  char frac_string_buffer[128];
  sprintf(frac_string_buffer, "%f", fractional_part);
  int frac_length = strlen(frac_string_buffer);

  if (frac_length < 3 || frac_string_buffer[0] != '0' || frac_string_buffer[1] != '.') {
    /* Fractional component is weird! Just return the int part.
     *
     * TODO: is this really the right thing to do here? */
    return int_length;
  }

  /* Ensure the number of fractional digits exactly matches digit count
   * specified, if it was specified. */
  const int offset = 2; /* For the leading "0.". */
  if (format.fixed_fractional_digits && (frac_length - offset) != *format.fixed_fractional_digits)
  {
    const int diff = *format.fixed_fractional_digits - (frac_length - offset);
    for (int i = 0; i < diff; i++) {
      frac_string_buffer[frac_length + i + offset] = '0';
    }
    frac_string_buffer[*format.fixed_fractional_digits + offset] = '\0';
    frac_length = *format.fixed_integer_digits + offset;
  }

  BLI_strncpy(output_string + int_length, frac_string_buffer + 1, 64);

  return int_length + frac_length - 1;
}

bool BKE_path_apply_variables(char path[FILE_MAX], const VariableMap &variables)
{
  bool was_modified = false;

  const int length = strlen(path);
  int processed = 0;
  while (processed < length) {
    const auto parsed_variable = next_path_variable(path + processed, FILE_MAX - processed);

    /* Check for parse error.
     *
     * TODO: right now we're stupid and just keep on trying to parse at the next
     * byte, which is O(N^2) in pathological cases! We should return useful
     * information about the parse error from `next_path_variable() that lets us
     * do smarter things, and report issues to the user. */
    if (!parsed_variable.has_value()) {
      processed++;
      continue;
    }

    /* For computing strings for integer and float variables. */
    char string_buffer[128];

    /* Will point to the string to substitute the variable with in `path`. If no
     * corresponding variable is found, is left null. */
    const char *replacement_string = nullptr;

    /* Try to find a matching variable, and construct a string for it. */
    if (std::optional<blender::StringRefNull> string_value = variables.get_string(
            parsed_variable->name))
    {
      /* String variable found. */
      replacement_string = string_value->c_str();
    }
    else if (std::optional<int64_t> integer_value = variables.get_integer(parsed_variable->name)) {
      /* Integer variable found. */
      format_int_to_string(parsed_variable->format, string_buffer, *integer_value);
      replacement_string = string_buffer;
    }
    else if (std::optional<double> float_value = variables.get_float(parsed_variable->name)) {
      /* Float variable found. */
      format_float_to_string(parsed_variable->format, string_buffer, *float_value);
      replacement_string = string_buffer;
    }

    /* Perform the replacement if we found a matching variable, otherwise skip. */
    if (replacement_string != nullptr) {
      BLI_string_replace_range(path + processed,
                               FILE_MAX - processed,
                               parsed_variable->replacement_range.start(),
                               parsed_variable->replacement_range.one_after_last(),
                               replacement_string);

      processed += parsed_variable->replacement_range.one_after_last();
      processed -= parsed_variable->replacement_range.size();
      processed += strlen(replacement_string);

      was_modified = true;
    }
    else {
      /* No matching variable, so skip. */
      processed += parsed_variable->replacement_range.one_after_last();
    }
  }

  return was_modified;
}
