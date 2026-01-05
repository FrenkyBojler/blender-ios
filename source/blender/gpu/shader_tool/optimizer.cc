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
    string_view directive_str = directive.str_view();
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
  if (t.str_view() == "define") {
    defines.insert(t.next().str_view());
  }
  else if (t.str_view() == "ifndef") {
    if (defines.find(t.next().str_view()) != defines.end()) {
      cursor = process_disabled_scope(parser, t.prev());
    }
  }
  else if (t.str_view() == "ifdef") {
    if (defines.find(t.next().str_view()) == defines.end()) {
      cursor = process_disabled_scope(parser, t.prev());
    }
  }
  else if (t.str_view() == "if") {
    if (t.next().str_view() == "defined") {
      if (defines.find(t.next().next().next().str_view()) == defines.end()) {
        cursor = process_disabled_scope(parser, t.prev());
      }
    }
  }
}

struct FunctionGraph {
  /* Function ID that is unique for each function and all its overloads. */
  using FnId = int;
  int counter = 0;
  /* Map declarations (name token) to a function id. */
  vector<pair<Token, FnId>> decl;
  /* Map identifier to id. */
  unordered_map<string_view, FnId> map;
  /* Function call (from, to). */
  vector<pair<FnId, FnId>> edges;
};

static void process_functions(parser::IntermediateForm & /*parser*/,
                              Token par_tok,
                              FunctionGraph &functions,
                              FunctionGraph::FnId &current_function)
{
  ScopeType scope_type = par_tok.scope().type();
  if (scope_type != ScopeType::FunctionArgs && scope_type != ScopeType::FunctionCall) {
    return;
  }
  Token fn_name = par_tok.prev();
  if (scope_type == ScopeType::FunctionArgs && fn_name.prev() == Word) {
    /* Definition. */
    auto [it, success] = functions.map.emplace(fn_name.str_view(), 0);
    FunctionGraph::FnId &id = it->second;

    if (success) {
      id = functions.counter++;
    }

    functions.decl.emplace_back(fn_name, id);

    if (par_tok.scope().back().next() == '{') {
      current_function = id;
    }
    return;
  }

  if (current_function != -1) {
    auto it = functions.map.find(fn_name.str_view());
    if (it == functions.map.end()) {
      /* Functions not defined: builtins, macros etc... */
    }
    else {
      /* Functions Call. */
      FunctionGraph::FnId &id = it->second;
      functions.edges.emplace_back(current_function, id);
    }
  }
}

static void first_pass(parser::IntermediateForm &parser, FunctionGraph &functions)
{
  unordered_set<string_view> defines;

  int bracket_scope_depth = 0;

  FunctionGraph::FnId current_function = -1;

  TokenStream *data = &parser.data_;

  for (int cursor = 0; cursor < data->token_types.size(); cursor++) {
    TokenType tok_type = TokenType(data->token_types[cursor]);
    if (tok_type == Word) {
      /* Disabled scopes will advance the cursor so we don't parse anything in them. */
      process_directives(parser, Token::from_position(data, cursor), defines, cursor);
    }
    else if (tok_type == ParOpen) {
      process_functions(parser, Token::from_position(data, cursor), functions, current_function);
    }
    else if (tok_type == BracketOpen) {
      bracket_scope_depth++;
    }
    else if (tok_type == BracketClose) {
      bracket_scope_depth--;
      if (bracket_scope_depth == 0) {
        current_function = -1;
      }
    }
  }
}
using FnId = FunctionGraph::FnId;

static unordered_map<FnId, vector<FnId>> build_adjacency(const FunctionGraph &g)
{
  unordered_map<FnId, vector<FnId>> adj;
  adj.reserve(g.counter);

  for (const auto &[from, to] : g.edges) {
    adj[from].push_back(to);
  }
  return adj;
}

unordered_set<FnId> compute_used_functions(const FunctionGraph &g, const vector<FnId> &roots)
{
  unordered_set<FnId> used;
  used.reserve(g.counter);

  auto adj = build_adjacency(g);

  std::vector<FnId> stack;
  stack.reserve(64);

  for (FnId root : roots) {
    if (used.insert(root).second) {
      stack.push_back(root);
    }

    while (!stack.empty()) {
      FnId f = stack.back();
      stack.pop_back();

      auto it = adj.find(f);
      if (it == adj.end()) {
        continue;
      }

      for (FnId callee : it->second) {
        if (used.insert(callee).second) {
          stack.push_back(callee);
        }
      }
    }
  }

  return used;
}

static void prune_functions(parser::IntermediateForm &parser, FunctionGraph &functions)
{
  unordered_set<FnId> used = compute_used_functions(functions, {functions.map["main"]});
  // std::cout << "functions.decl " << functions.decl.size() << std::endl;
  // std::cout << "functions.edges " << functions.edges.size() << std::endl;
  // std::cout << "functions.map " << functions.map.size() << std::endl;
  // std::cout << "used " << used.size() << std::endl;

  for (auto [name_tok, id] : functions.decl) {
    if (used.find(id) != used.end()) {
      continue;
    }
    Token type = name_tok.prev();
    Token end_of_args = name_tok.next().scope().back();
    if (end_of_args.next() == '{') {
      /* Full definition. */
      Token end_of_body = end_of_args.next().scope().back();
      // parser.erase(type, end_of_body);
      bool success = parser.replace_try(type, end_of_body, "");
      if (!success) {
        // std::cout << "Failed deleting \"" << parser.substr_range_inclusive(type, end_of_body)
        //           << "\"" << std::endl;
      }
    }
    else {
      /* Prototype. */
      parser.erase(type, end_of_args);
      // bool success = parser.replace_try(type, end_of_args, "");
      // if (!success) {
      //   std::cout << "Failed deleting \"" << parser.substr_range_inclusive(type, end_of_args)
      //             << "\"" << std::endl;
      // }
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
        FunctionGraph functions;
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
