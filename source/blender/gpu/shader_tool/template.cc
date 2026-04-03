/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#include <algorithm>
#include <unordered_map>

#include "intermediate.hh"
#include "processor.hh"

namespace blender::gpu::shader {
using namespace std;
using namespace shader::parser;
using namespace metadata;

string SourceProcessor::template_arguments_mangle(const Scope template_args)
{
  string args_concat;
  template_args.foreach_scope(ScopeType::TemplateArg, [&](const Scope &scope) {
    string str;
    if (scope[1] == '<') {
      str = string(scope[0].str()) + template_arguments_mangle(scope[1].scope());
    }
    else {
      str = scope.str();
    }
    /* In order to support negative integer literals. Replace minus sign by underscore. */
    replace(str.begin(), str.end(), '-', '_');
    args_concat += 'T' + str;
  });
  return args_concat;
}

static void parse_template_definition(const Scope arg,
                                      vector<string> &arg_list,
                                      const Scope fn_args,
                                      bool &all_template_args_in_function_signature,
                                      report_callback report_error)
{
  const Token type = arg.front();
  const Token name = type.str() == "enum" ? type.next().next() : type.next();
  const string_view name_str = name.str();
  const string_view type_str = type.str();

  arg_list.emplace_back(name_str);

  if (arg.contains_token('=')) {
    report_error(ERROR_TOK(arg[0]),
                 "Default arguments are not supported inside template declaration");
  }

  if (type_str == "typename") {
    bool found = false;
    /* Search argument list for type-names. If type-name matches, the template argument is
     * present inside the function signature. */
    fn_args.foreach_match("AA", [&](const vector<Token> &tokens) {
      if (tokens[0].str() == name_str) {
        found = true;
      }
    });
    all_template_args_in_function_signature &= found;
  }
  else if (type_str == "enum" || type_str == "bool") {
    /* Values cannot be resolved using type deduction. */
    all_template_args_in_function_signature = false;
  }
  else if (type_str == "int" || type_str == "uint" || type_str == "char" || type_str == "uchar" ||
           type_str == "short" || type_str == "ushort")
  {
    /* Values cannot be resolved using type deduction. */
    all_template_args_in_function_signature = false;
  }
  else {
    report_error(ERROR_TOK(type), "Invalid template argument type");
  }
}

void SourceProcessor::lower_template_instantiation(
    SourceProcessor::Parser &parser,
    const Token &inst_start,
    const Token &inst_name,
    const Scope &inst_args,
    const string_view ns,
    const Token &fn_start,
    const Token &fn_end,
    const Token &fn_name,
    /* Method template instantiation reside outside of their struct.
     * For this reason they have the struct name_prepended. */
    const string_view full_specified_name,
    const bool is_method,
    const vector<string> &arg_list,
    const string &fn_decl,
    const int template_def_line_number,
    const string_view &template_filename,
    const string_view &instance_filename,
    const bool all_template_args_in_function_signature)
{
  if (full_specified_name != inst_name.str()) {
    return;
  }

  const Token inst_end = inst_start.find_next(SemiColon);

  /* Parse template values. */
  vector<pair<string, string>> arg_name_value_pairs;
  int i = 0;
  inst_args.foreach_scope(ScopeType::TemplateArg, [&](const Scope &arg) {
    if (i < arg_list.size()) {
      arg_name_value_pairs.emplace_back(arg_list[i], arg.str());
    }
    i++;
  });
  if (i != arg_list.size()) {
    report_error_(ERROR_TOK(inst_args.front()),
                  "Invalid amount of argument in template instantiation.");
  }

  const bool is_struct = (fn_name.prev() == Struct);

  /* Specialize template content. */
  SourceProcessor::Parser instance_parser(fn_decl, report_error_);

  /* Inject namespace around definition. */
  if (ns.empty()) {
    instance_parser.insert_before(instance_parser.front(), "\n");
    instance_parser.insert_after(instance_parser.back(), "\n");
  }
  else {
    /* Remove suffix "::". */
    string ns_name(ns.substr(0, ns.size() - 2));
    instance_parser.insert_before(instance_parser.front(), "namespace " + ns_name + " {\n");
    instance_parser.insert_after(instance_parser.back(), "\n}\n");
  }

  instance_parser().foreach_token(Word, [&](const Token &word) {
    string_view token_str = word.str();
    for (const auto &arg_name_value : arg_name_value_pairs) {
      if (token_str == arg_name_value.first) {
        instance_parser.replace(word, arg_name_value.second, true);
      }
    }
    if (is_struct && word.next() != AngleOpen && token_str == fn_name.str()) {
      /* Append template args after unspecified struct typename references.
       * `A func(A<T> b) {}` > `A<T> func(A<T> b) {}`. */
      instance_parser.insert_after(word.str_index_last_no_whitespace(),
                                   SourceProcessor::template_arguments_mangle(inst_args));
    }
  });

  size_t symbol_name_pos = instance_parser.str().find(" " + string(fn_name.str()));
  /* Append namespace to symbol name because appended mangled arguments make namespace
   * resolution impossible. */
  instance_parser.insert_after(symbol_name_pos, string(ns));
  if (!is_struct && !all_template_args_in_function_signature) {
    /* Append template args after function name.
     * `void func() {}` > `void func<a, 1>() {}`. */
    instance_parser.insert_after(symbol_name_pos + fn_name.str().size(),
                                 SourceProcessor::template_arguments_mangle(inst_args));
  }
  instance_parser.apply_mutations();

  lower_pre_template(instance_parser);

  /* Paste template content in place of instantiation. */
  string instance = instance_parser.result_get();
  /* Remove added newline from the injected namespace. */
  instance = instance.substr(instance.find_first_of('\n') + 1);

  if (is_method) {
    /* Method are put back in their classes. */
    parser.insert_line_number(fn_end, template_def_line_number, template_filename);
    parser.insert_after(fn_end, instance);
    parser.insert_line_number(fn_end, inst_end.line_number(true), instance_filename);
  }
  else {
    parser.insert_line_number(inst_end, template_def_line_number, template_filename);
    parser.insert_after(inst_end, instance);
    parser.insert_line_number(inst_end, inst_end.line_number(true), instance_filename);
  }
}

void SourceProcessor::lower_template_dependent_names(Parser &parser)
{
  parser().foreach_match("tA<..>", [&](const Tokens &toks) {
    if (toks[0].prev() == '.' || (toks[0].prev().prev() == '-' && toks[0].prev() == '>')) {
      parser.erase(toks[0]);
    }
  });
}

void SourceProcessor::lower_pre_template(Parser &parser)
{
  /* Lower noop attributes after linting them. */
  lower_maybe_unused(parser);
  /* Lint and remove C++ accessor templates before lowering template. */
  lower_srt_accessor_templates(parser);
  lower_union_accessor_templates(parser);
  /* Lower implicit members before we remove SRT member from their struct. */
  lower_implicit_member(parser);
  /* Lower namespaces. */
  lower_using(parser);
  lower_namespaces(parser);
  lower_scope_resolution_operators(parser);

  lower_template_calls(parser);
  lower_template_specialization(parser);
}

/* Mangle template parameter into the symbol name. */
void SourceProcessor::lower_template_calls(Parser &parser)
{
  /* Process templated function calls first to avoid matching them later. */
  parser().foreach_match("A<..>(..)", [&](const vector<Token> &tokens) {
    const Scope template_args = tokens[1].scope();
    template_args.foreach_match("A<..>", [&parser](const vector<Token> &tokens) {
      parser.replace(tokens[1].scope(), template_arguments_mangle(tokens[1].scope()), true);
    });
  });
  /* Likewise, process templated struct method definitions. */
  parser().foreach_match("A<..>A<", [&](const vector<Token> &tokens) {
    parser.replace(tokens[1].scope(), template_arguments_mangle(tokens[1].scope()), true);
  });

  parser.apply_mutations();
}

void SourceProcessor::lower_template_specialization(Parser &parser)
{
  auto process_specialization = [&](const Token specialization_start, const Scope template_args) {
    parser.erase(specialization_start, specialization_start.next().next());
    parser.replace(template_args, template_arguments_mangle(template_args), true);
  };

  parser().foreach_match("t<>AA<", [&](const vector<Token> &tokens) {
    process_specialization(tokens[0], tokens[5].scope());
  });

  parser().foreach_match("t<>sA<..>", [&](const vector<Token> &tokens) {
    process_specialization(tokens[0], tokens[5].scope());
  });

  parser.apply_mutations();
}

void SourceProcessor::process_template_struct(
    blender::gpu::shader::metadata::TemplateDefinition &template_def,
    SourceProcessor::Parser &parser)
{
  if (parser.str().find(template_def.identifier) == string::npos) {
    /* Avoid running parser if there is no instantiation. */
    return;
  }

  SourceProcessor::Parser def_parser(template_def.definition, report_error_);

  assert(def_parser[0] == Template);

  Scope template_scope = def_parser[1].scope();
  assert(template_scope.type() == ScopeType::Template);

  /* Parse template declaration. */
  Token struct_start = template_scope.back().next();
  assert(struct_start == Struct);

  Token struct_name = struct_start.next();
  Scope struct_body = struct_name.next().scope();

  Token struct_end = struct_body.back().next();
  const string struct_decl = def_parser.substr_range_inclusive(struct_start, struct_end);

  vector<string> arg_list;
  bool all_template_args_in_function_signature = false;
  template_scope.foreach_scope(ScopeType::TemplateArg, [&](Scope arg) {
    parse_template_definition(
        arg, arg_list, Scope(def_parser), all_template_args_in_function_signature, report_error_);
  });

  /* Remove template parameters declaration. */
  Token template_keyword = template_scope.front().prev();
  def_parser.erase(template_keyword, struct_end);

  string full_specified_name(template_def.name_space + template_def.identifier);
  SourceProcessor::Parser name_parser(full_specified_name, report_error_);
  lower_scope_resolution_operators(name_parser);
  full_specified_name = name_parser.result_get();

  string template_filename = template_def.filepath.substr(filepath_.find_last_of('/') + 1);
  string instance_filename = filepath_.substr(filepath_.find_last_of('/') + 1);

  if (template_filename == instance_filename) {
    /* Avoid adding noise in the source file if instance is inside the same file as declaration. */
    template_filename = "";
    instance_filename = "";
  }

  /* Replace instantiations. */
  parser().foreach_match("tsA<", [&](const vector<Token> &tokens) {
    lower_template_instantiation(parser,
                                 tokens[0],
                                 tokens[2],
                                 tokens[3].scope(),
                                 template_def.name_space,
                                 struct_start,
                                 struct_end,
                                 struct_name,
                                 full_specified_name,
                                 false,
                                 arg_list,
                                 struct_decl,
                                 template_def.definition_line,
                                 template_filename,
                                 instance_filename,
                                 all_template_args_in_function_signature);
  });
}

void SourceProcessor::process_template_function(
    blender::gpu::shader::metadata::TemplateDefinition &template_def,
    SourceProcessor::Parser &parser)
{
  if (parser.str().find(template_def.identifier) == string::npos) {
    /* Avoid running parser if there is no instantiation. */
    return;
  }

  SourceProcessor::Parser def_parser(template_def.definition, report_error_);

  const Scope template_scope = def_parser[1].scope();
  assert(template_scope.type() == ScopeType::Template);

  /* Parse template declaration. */
  const Token fn_start = template_scope.back().next();
  Token after_attr = fn_start;
  /* Skip attributes. */
  while (after_attr == SquareOpen) {
    after_attr = after_attr.scope().back().next();
  }
  const Scope fn_args = after_attr.find_next(ParOpen).scope();
  const Token fn_name = fn_args.front().prev();
  const Token fn_end = fn_args.back().find_next(BracketOpen).scope().back();

  assert(fn_start.is_valid() && fn_name.is_valid() && fn_args.is_valid() &&
         template_scope.is_valid() && fn_end.is_valid());

  bool error = false;
  template_scope.foreach_match("=", [&](const vector<Token> &tokens) {
    report_error_(tokens[0].line_number(),
                  tokens[0].char_number(),
                  tokens[0].line_str(),
                  "Default arguments are not supported inside template declaration");
    error = true;
  });
  if (error) {
    return;
  }

  /* Remove template parameters declaration. */
  Token template_keyword = template_scope.front().prev();
  def_parser.erase(template_keyword, fn_end);

  vector<string> arg_list;
  bool all_template_args_in_function_signature = true;
  template_scope.foreach_scope(ScopeType::TemplateArg, [&](Scope arg) {
    parse_template_definition(
        arg, arg_list, fn_args, all_template_args_in_function_signature, report_error_);
  });

  const string fn_decl = def_parser.substr_range_inclusive(fn_start, fn_end);

  string full_specified_name(template_def.name_space + template_def.identifier);
  SourceProcessor::Parser name_parser(full_specified_name, report_error_);
  lower_scope_resolution_operators(name_parser);
  full_specified_name = name_parser.result_get();

  string template_filename = template_def.filepath.substr(filepath_.find_last_of('/') + 1);
  string instance_filename = filepath_.substr(filepath_.find_last_of('/') + 1);

  if (template_filename == instance_filename) {
    /* Avoid adding noise in the source file if instance is inside the same file as declaration. */
    template_filename = "";
    instance_filename = "";
  }

  parser().foreach_match("tAA<", [&](const vector<Token> &tokens) {
    lower_template_instantiation(parser,
                                 tokens[0],
                                 tokens[2],
                                 tokens[3].scope(),
                                 template_def.name_space,
                                 fn_start,
                                 fn_end,
                                 fn_name,
                                 full_specified_name,
                                 template_def.is_method,
                                 arg_list,
                                 fn_decl,
                                 template_def.definition_line,
                                 template_filename,
                                 instance_filename,
                                 all_template_args_in_function_signature);
  });
}

void SourceProcessor::lower_templates(Parser &parser)
{
  auto lint_explicit = [&](const Token symbol_name) {
    if (symbol_name.next().scope().type() != parser::ScopeType::Template) {
      report_error_(
          ERROR_TOK(symbol_name),
          "Template instantiation and specialization require explicit template arguments");
    }
  };
  parser().foreach_match("t<>AA", [&](const vector<Token> &toks) { lint_explicit(toks[4]); });
  parser().foreach_match("t<>A<..>A", [&](const vector<Token> &toks) { lint_explicit(toks[8]); });
  parser().foreach_match("tAA", [&](const vector<Token> &toks) { lint_explicit(toks[2]); });
  parser().foreach_match("tA<..>A", [&](const vector<Token> &toks) { lint_explicit(toks[6]); });

  /* Delete definitions. */
  parser().foreach_match("t<..>", [&](const vector<Token> &toks) {
    Token end = toks[0].find_next(BracketOpen).scope().back();
    if (toks[4].next() == Struct) {
      /* Capture end semicolon. */
      end = end.next();
    }
    /* This can fail as it might try to erase templated method inside templated struct. */
    parser.erase_try(toks[0], end);
  });

  parser.apply_mutations();

  /* Deduplicate symbols (can happen because the main file is parsed twice). */
  unordered_map<string, TemplateDefinition> unique_symbols;
  for (const auto &symbol : metadata_.template_definitions) {
    unique_symbols.try_emplace(symbol.name_space + symbol.identifier, symbol);
  }

  /* For each template definition, process all instantiation . */
  for (auto [_, template_def] : unique_symbols) {
    if (template_def.is_struct) {
      process_template_struct(template_def, parser);
    }
    else {
      process_template_function(template_def, parser);
    }
  }

  parser.apply_mutations();

  /* Remove template instantiation. */
  parser().foreach_match("tA", [&](const vector<Token> &toks) {
    parser.erase(toks[0], toks[0].find_next(SemiColon));
  });

  parser.apply_mutations();

  /* Process calls to templated types or functions. */
  parser().foreach_match("A<..>", [&](const vector<Token> &tokens) {
    parser.replace(tokens[1].scope(), template_arguments_mangle(tokens[1].scope()), true);
  });

  parser.apply_mutations();
}

}  // namespace blender::gpu::shader
