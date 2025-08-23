/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "gl_shader.hh"

#include "GPU_platform.hh"

namespace blender::gpu {

size_t line_start_get(StringRefNull source_combined, size_t target_line)
{
  size_t cursor = 0;
  size_t current_line = 1;
  for (char c : source_combined) {
    if (current_line >= target_line) {
      return cursor;
    }
    if (c == '\n') {
      current_line++;
    }
    cursor++;
  }
  return -1;
}

StringRef filename_get(StringRefNull source_combined, size_t pos)
{
  StringRef sub_str = source_combined.substr(0, pos);
  StringRefNull directive = "//ine 1 \"";
  size_t nearest_line_directive = sub_str.rfind(directive);
  size_t line_count = 1;
  if (nearest_line_directive != std::string::npos) {
    size_t start_of_file_name = nearest_line_directive + directive.size() + 1;
    size_t end_of_file_name = sub_str.find('\"', start_of_file_name);
    if (end_of_file_name != std::string::npos) {
      return sub_str.substr(start_of_file_name, end_of_file_name - start_of_file_name);
    }
  }
  return {};
}

/* Original source file line. Found by looking up commented #line directives. */
size_t source_line_get(StringRefNull source_combined, size_t pos)
{
  StringRef sub_str = source_combined.substr(0, pos);
  StringRefNull directive = "//ine ";
  size_t nearest_line_directive = sub_str.rfind(directive);
  size_t line_count = 1;
  if (nearest_line_directive != std::string::npos) {
    sub_str = sub_str.substr(nearest_line_directive + directive.size());
    line_count = std::stoll(sub_str) - 1;
  }
  return line_count + std::count(sub_str.begin(), sub_str.end(), '\n');
}

const char *GLLogParser::parse_line(const char *source_combined,
                                    const char *log_line,
                                    GPULogItem &log_item)
{
  /* Skip ERROR: or WARNING:. */
  log_line = skip_severity_prefix(log_line, log_item);
  log_line = skip_separators(log_line, "(: ");

  /* Parse error line & char numbers. */
  if (at_number(log_line)) {
    const char *error_line_number_end;
    log_item.cursor.row = parse_number(log_line, &error_line_number_end);
    /* Try to fetch the error character (not always available). */
    if (at_any(error_line_number_end, "(:") && at_number(&error_line_number_end[1])) {
      log_item.cursor.column = parse_number(error_line_number_end + 1, &log_line);
    }
    else {
      log_line = error_line_number_end;
    }
    /* There can be a 3rd number (case of mesa driver). */
    if (at_any(log_line, "(:") && at_number(&log_line[1])) {
      log_item.cursor.source = log_item.cursor.row;
      log_item.cursor.row = log_item.cursor.column;
      log_item.cursor.column = parse_number(log_line + 1, &error_line_number_end);
      log_line = error_line_number_end;
    }
  }

  if ((log_item.cursor.row != -1) && (log_item.cursor.column != -1)) {
    if (GPU_type_matches(GPU_DEVICE_NVIDIA, GPU_OS_ANY, GPU_DRIVER_OFFICIAL)) {
      /* source:row */
      log_item.cursor.source = log_item.cursor.row;
      log_item.cursor.row = log_item.cursor.column;
      log_item.cursor.column = -1;
    }
    else if (GPU_type_matches(GPU_DEVICE_ATI, GPU_OS_UNIX, GPU_DRIVER_OFFICIAL)) {
      /* source:row */
      log_item.cursor.source = log_item.cursor.row;
      log_item.cursor.row = log_item.cursor.column;
      log_item.cursor.column = -1;
    }
    else {
      /* line:char */
    }
  }

  if (log_item.cursor.row != -1) {
    /* Get to the wanted line. */
    size_t line_start_character = line_start_get(log_item.cursor.row);
    StringRef filename = filename_get(source_combined, line_start_character);
    size_t line_number = source_line_get(source_combined, line_start_character);
    log_item.cursor.file_name_and_error_line = filename + ':' + std::to_string(line_number);
  }

  log_line = skip_separators(log_line, ":) ");

  /* Skip to message. Avoid redundant info. */
  log_line = skip_severity_keyword(log_line, log_item);
  log_line = skip_separators(log_line, ":) ");

  return log_line;
}

const char *GLLogParser::skip_severity_prefix(const char *log_line, GPULogItem &log_item)
{
  return skip_severity(log_line, log_item, "ERROR", "WARNING", "NOTE");
}

const char *GLLogParser::skip_severity_keyword(const char *log_line, GPULogItem &log_item)
{
  return skip_severity(log_line, log_item, "error", "warning", "note");
}

}  // namespace blender::gpu
