/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "intermediate.hh"

using namespace blender::gpu::shader;
using namespace blender::gpu::shader::parser;
using namespace std;

int main(int argc, char **argv)
{
  if (argc < 2) {
    std::cerr << "Usage: optimizer <data_file_from> <data_file_to>" << std::endl;
    exit(1);
  }

  const char *input_file_name = argv[1];
  const char *output_file_name = argv[2];

  /* Open the input file for reading */
  std::ifstream input_file(input_file_name);
  if (!input_file) {
    std::cerr << "Error: Could not open input file " << input_file_name << std::endl;
    exit(1);
  }

  /* We make the required directories here rather than having the build system
   * do the work for us, as having cmake do it leads to several thousand cmake
   * instances being launched, leading to significant overhead, see pr #141404
   * for details. */
  std::filesystem::path parent_dir = std::filesystem::path(output_file_name).parent_path();
  std::error_code ec;
  if (!std::filesystem::create_directories(parent_dir, ec)) {
    if (ec) {
      std::cerr << "Unable to create " << parent_dir << " : " << ec.message() << std::endl;
      exit(1);
    }
  }

  /* Open the output file for writing */
  std::ofstream output_file(output_file_name, std::ofstream::out | std::ofstream::binary);
  if (!output_file) {
    std::cerr << "Error: Could not open output file " << output_file_name << std::endl;
    input_file.close();
    exit(1);
  }

  std::stringstream buffer;
  buffer << input_file.rdbuf();

  int error = 0;

  parser::report_callback report_error =
      [&](int err_line, int err_char, std::string line, const char *err_msg) {
        std::cerr << input_file_name;
        std::cerr << ':' << std::to_string(err_line) << ':' << std::to_string(err_char + 1);
        std::cerr << ": error: " << err_msg << std::endl;
        std::cerr << line << std::endl;
        std::cerr << std::string(err_char, ' ') << '^' << std::endl;

        error++;
      };

  TimeIt::Duration time;

  string result;
  {
    TimeIt time_it(time);
    for (int i = 0; i < 100; i++) {
      parser::IntermediateForm parser(buffer.str(), report_error);
    }

    parser::IntermediateForm parser(buffer.str(), report_error);

    auto process_disabled_scope = [&](Token start_tok) {
      /* Search for endif with the same indentation. Assume formatted input. */
      string end_str = start_tok.str_with_whitespace() + "endif";
      size_t scope_end = parser.str().find(end_str, start_tok.str_index_start());
      if (scope_end == string::npos) {
        return;
      }
      /* Search for else/elif with the same indentation. Assume formatted input. */
      string else_str = start_tok.str_with_whitespace() + "el";
      size_t scope_else = parser.str().find(else_str, start_tok.str_index_start());
      if (scope_else != string::npos && scope_else < scope_end) {
        /* Only erase the content and keep the preprocessor directives. */
        parser.erase(start_tok.line_end() + 1, scope_else - 1);
      }
      else {
        /* Erase the content and the preprocessor directives. */
        parser.erase(start_tok.str_index_start(), scope_end + end_str.size());
      }
    };

    {
      unordered_set<string> defines;
      unordered_map<string, Token> functions;

      parser().foreach_token(Word, [&](const Token &t) {
        if (t.prev() == '#' && t.next() == Word) {
          /* Preprocessor. */
          if (t.str() == "define") {
            defines.insert(t.next().str());
          }
          else if (t.str() == "ifndef") {
            if (defines.find(t.next().str()) != defines.end()) {
              process_disabled_scope(t.prev());
            }
          }
          else if (t.str() == "ifdef") {
            if (defines.find(t.next().str()) == defines.end()) {
              process_disabled_scope(t.prev());
            }
          }
          else if (t.str() == "if") {
            if (t.next().str() == "defined") {
              if (defines.find(t.next().next().next().str()) == defines.end()) {
                process_disabled_scope(t.prev());
              }
            }
          }
        }
        else if (t.next() == '(') {
          /* Functions. */
          if (t.prev() == Word && t.prev().scope().type() != ScopeType::Preprocessor) {
            /* Definition. */
            functions.emplace(t.str(), t);
          }
          else {
            auto it = functions.find(t.str());
            if (it == functions.end()) {
            }
            else {
              it->second = Token::invalid();
            }
          }
        }
      });

      for (auto [_, value] : functions) {
        if (value.is_valid() && value.str() != "main") {
          Token type = value.prev();
          Token end_of_args = value.next().scope().back();
          if (end_of_args.next() == '{') {
            /* Full definition. */
            Token end_of_body = end_of_args.next().scope().back();
            parser.replace_try(type, end_of_body, "");
          }
          else {
            /* Prototype. */
            parser.replace_try(type, end_of_args, "");
          }
        }
      }
    }
    result = parser.result_get();
  }

  std::cout << "Optimize: " << time.count() << " µs" << std::endl;

  output_file << result;

  input_file.close();
  output_file.close();

  return error;
}
