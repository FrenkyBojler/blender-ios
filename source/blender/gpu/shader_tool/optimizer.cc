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

/* Return the next valid token index. */
static int process_disabled_scope(parser::IntermediateForm &parser, Token start_tok)
{
  int stack = 0;
  Token hash = start_tok;
  while ((hash = hash.find_next(Hash)).is_valid()) {
    Token directive = hash.next();
    string_view directive_str = directive.str();
    if (directive_str.substr(0, 2) == "if") {
      stack++;
    }
    /* elif/else */
    if (stack == 0 && directive_str.substr(0, 2) == "el") {
      /* Only erase the content and keep the preprocessor directives. */
      parser.erase(start_tok.scope().back().next(), hash.prev());
      return hash.index;
    }
    if (directive_str == "endif") {
      if (stack == 0) {
        /* Erase the content and the preprocessor directives. */
        parser.erase(start_tok, directive);
        return directive.index + 1;
      }
      stack--;
    }
  }
  /* Unterminated scope? */
  return start_tok.index + 1;
}

static void process_directives(parser::IntermediateForm &parser,
                               Token t,
                               unordered_set<string_view> &defines,
                               int &cursor)
{
  if (t.prev() != '#' || t.next() != Word) {
    return;
  }
  /* Preprocessor. */
  if (t.str() == "define") {
    defines.insert(t.next().str());
  }
  else if (t.str() == "ifndef") {
    if (defines.find(t.next().str()) != defines.end()) {
      cursor = process_disabled_scope(parser, t.prev());
    }
  }
  else if (t.str() == "ifdef") {
    if (defines.find(t.next().str()) == defines.end()) {
      cursor = process_disabled_scope(parser, t.prev());
    }
  }
  else if (t.str() == "if") {
    if (t.next().str() == "defined") {
      if (defines.find(t.next().next().next().str()) == defines.end()) {
        cursor = process_disabled_scope(parser, t.prev());
      }
    }
  }
}

static void process_functions(parser::IntermediateForm & /*parser*/,
                              Token t,
                              unordered_map<string_view, Token> &functions)
{
  Token fn_name = t.prev();
  Token fn_type = fn_name.prev();
  /* Functions. */
  if (fn_type == Word && fn_name.scope().type() != ScopeType::Preprocessor) {
    /* Definition. */
    functions.emplace(fn_name.str(), fn_name);
    return;
  }
  auto it = functions.find(fn_name.str());
  if (it == functions.end()) {
    /* Functions not defined: builtins, macros etc... */
  }
  else {
    /* Functions Call. */
    it->second = Token::invalid();
  }
}

static void first_pass(parser::IntermediateForm &parser,
                       unordered_map<string_view, Token> &functions)
{
  unordered_set<string_view> defines;

  TokenStream *data = &parser.data_;

  for (int cursor = 0; cursor < data->token_types.size(); cursor++) {
    TokenType tok_type = TokenType(data->token_types[cursor]);
    if (tok_type == Word) {
      /* Disabled scopes will advance the cursor so we don't parse anything in them. */
      process_directives(parser, Token::from_position(data, cursor), defines, cursor);
    }
    else if (tok_type == ParOpen) {
      process_functions(parser, Token::from_position(data, cursor), functions);
    }
  }
}

static void prune_functions(parser::IntermediateForm &parser,
                            unordered_map<string_view, Token> &functions)
{
  int count = 0, worked = 0, prototypes = 0;
  for (auto [_, name_tok] : functions) {
    if (name_tok.is_valid() && name_tok.str() != "main") {
      Token type = name_tok.prev();
      Token end_of_args = name_tok.next().scope().back();
      if (end_of_args.next() == '{') {
        /* Full definition. */
        Token end_of_body = end_of_args.next().scope().back();
        count++;
        // parser.erase(type, end_of_body);
        worked += parser.replace_try(type, end_of_body, "");
      }
      else {
        /* Prototype. */
        count++;
        prototypes++;
        parser.erase(type, end_of_args);
        // worked += parser.replace_try(type, end_of_args, "");
      }
    }
  }
  // std::cout << "Removed functions " << worked << " / " << functions.size() << std::endl;
  // std::cout << "Removed prototypes " << prototypes << " / " << functions.size() << std::endl;
}

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

  string test = buffer.str();
  // for (int i = 0; i < 9; i++) {
  //   test += "\n" + buffer.str();
  // }

  int iter = 100;
  string result;
  {
    TimeIt time_it(time);
    for (int i = 0; i < iter; i++) {
      parser::IntermediateForm parser(test, report_error);

      {
        unordered_map<string_view, Token> functions;
        first_pass(parser, functions);
        prune_functions(parser, functions);
      }
      result = parser.result_get();
    }
  }

  float percent = 100 - (result.size() * 100.0f / test.size());
  std::cout << "Input Size: " << (test.size()) / 1000000.0f << " MB" << std::endl;
  std::cout << "Output Size: " << (result.size()) / 1000000.0f << " MB (-" << percent << "%)"
            << std::endl;
  std::cout << "Time: " << time.count() / (1000.0f * iter) << " ms" << std::endl;
  std::cout << "Throughput: " << ((test.size() * iter) / float(time.count())) << " MB/s"
            << std::endl;

  output_file << result;

  input_file.close();
  output_file.close();

  return error;
}
