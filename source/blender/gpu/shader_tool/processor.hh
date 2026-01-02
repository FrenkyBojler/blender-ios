/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_tool
 */

#pragma once

#include "intermediate.hh"
#include "metadata.hh"

namespace blender::gpu::shader {

enum class Language {
  UNKNOWN = 0,
  /* Shared header. */
  CPP,
  /* Metal Shading Language. */
  MSL,
  /* OpenGL Shading Language. */
  GLSL,
  /* Blender Shading Language. */
  BSL,
  /* Same as GLSL but enable partial C++ feature support like template, references,
   * include system, etc ... */
  BLENDER_GLSL,
};

static inline Language language_from_filename(const std::string &filename)
{
  if (filename.find(".msl") != std::string::npos) {
    return Language::MSL;
  }
  if (filename.find(".glsl") != std::string::npos || filename.find(".bsl.hh") != std::string::npos)
  {
    return Language::GLSL;
  }
  if (filename.find(".hh") != std::string::npos) {
    return Language::CPP;
  }
  return Language::UNKNOWN;
}

/**
 * Shader source preprocessor that allow to mutate shader sources into cross API source that can be
 * interpreted by the different GPU backends. Some syntax are mutated or reported as incompatible.
 */
class SourceProcessor {
 public:
  using report_callback = parser::report_callback;
  using Parser = parser::IntermediateForm;
  using Tokens = std::vector<parser::Token>;

 private:
  const std::string source;
  const std::string filepath;
  metadata::Source metadata;

  Language language;

  parser::report_callback report_error;

 public:
  SourceProcessor(
      const std::string &source,
      const std::string &filepath,
      Language language,
      parser::report_callback report_error = [](int, int, std::string, const char *) {})
      : source(source), filepath(filepath), language(language), report_error(report_error)
  {
  }

  struct Result {
    /* Resulting Intermediate Language source. */
    std::string source;
    /* Parsed metadata. */
    metadata::Source metadata;
  };

  /* Convert to intermediate language. Also outputs metadata.
   * symbols_set is the set of namespace symbols from external files / dependencies. */
  Result convert(std::vector<metadata::Symbol> symbols_set = {});

  /* Lightweight parsing. Only Source::dependencies and Source::symbol_table are populated. */
  metadata::Source parse_include_and_symbols();

  /* String hash are outputted inside GLSL and needs to fit 32 bits. */
  static uint32_t hash_string(const std::string &str)
  {
    uint64_t hash_64 = metadata::hash(str);
    uint32_t hash_32 = uint32_t(hash_64 ^ (hash_64 >> 32));
    return hash_32;
  }

 private:
  std::string remove_comments(const std::string &str, const report_callback &report_error);

  /* Remove trailing white spaces. */
  void cleanup_whitespace(Parser &parser, report_callback /*report_error*/);

  /* Safer version without Parser. */
  std::string cleanup_whitespace(const std::string &str, const report_callback & /*report_error*/);

  static std::string template_arguments_mangle(const shader::parser::Scope template_args);

  void parse_template_definition(const parser::Scope arg,
                                 std::vector<std::string> &arg_list,
                                 const parser::Scope fn_args,
                                 bool &all_template_args_in_function_signature,
                                 report_callback &report_error);

  void process_instantiation(Parser &parser,
                             const std::vector<parser::Token> &toks,
                             const parser::Scope &parent_scope,
                             const parser::Token &fn_start,
                             const parser::Token &fn_name,
                             const std::vector<std::string> &arg_list,
                             const std::string &fn_decl,
                             const bool all_template_args_in_function_signature,
                             report_callback &report_error);

  /**
   * Given our codestyle, we don't need the disambiguation.
   * Example: `x.template foo<int>()` > `x.foo<int>()`
   */
  void lower_template_dependent_names(Parser &parser, report_callback & /*report_error*/);

  void lower_templates(Parser &parser, report_callback &report_error);

  /* Parse defines in order to output them with the create infos.
   * This allow the create infos to use shared defines values. */
  void parse_defines(Parser &parser, report_callback /*report_error*/);

  void parse_namespace_symbols(shader::parser::Scope ns);

  void parse_local_symbols(Parser &parser, report_callback /*report_error*/);

  std::string get_create_info_placeholder(const std::string &name);

  /* Legacy create info parsing and removing. */
  void parse_legacy_create_info(Parser &parser, report_callback report_error);

  void parse_includes(Parser &parser, report_callback /*report_error*/);

  void parse_pragma_runtime_generated(Parser &parser);

  void lint_pragma_once(Parser &parser, const std::string &filename, report_callback report_error);

  void lower_loop_unroll(Parser &parser, report_callback report_error);

  void process_static_branch(Parser &parser,
                             shader::parser::Token if_tok,
                             shader::parser::Scope condition,
                             shader::parser::Token attribute,
                             shader::parser::Scope body,
                             report_callback report_error);

  void lower_static_branch(Parser &parser, report_callback report_error);

  /* Lower namespaces by adding namespace prefix to all the contained structs and functions. */
  void lower_namespaces(Parser &parser, report_callback report_error);

  /**
   * Needs to run before namespace mutation so that `using` have more precedence.
   * Otherwise the following would fail.
   *  ```cpp
   *  namespace B {
   *  int test(int a) {}
   *  }
   *
   *  namespace A {
   *  int test(int a) {}
   *  int func(int a) {
   *    using B::test;
   *    return test(a); // Should reference B::test and not A::test
   *  }
   *  ```
   */
  void lower_using(Parser &parser, report_callback report_error);

  void lower_scope_resolution_operators(Parser &parser, report_callback /*report_error*/);

  std::string disabled_code_mutation(const std::string &str, report_callback &report_error);

  void lower_preprocessor(Parser &parser, report_callback /*report_error*/);

  /* Support for BLI swizzle syntax. */
  void lower_swizzle_methods(Parser &parser, report_callback /*report_error*/);

  std::string threadgroup_variables_parse_and_remove(const std::string &str,
                                                     report_callback &report_error);

  void parse_library_functions(Parser &parser, report_callback report_error);

  void parse_builtins(const std::string &str, const std::string &filename, bool pure_glsl = false);

  /* Change printf calls to "recursive" call to implementation functions.
   * This allows to emulate the variadic arguments of printf. */
  void lower_printf(Parser &parser, report_callback /*report_error*/);

  /* Turn assert into a printf. */
  void lower_assert(Parser &parser, const std::string &filename, report_callback report_error);

  /* Parse SRT and interfaces, remove their attributes and create init function for SRT structs. */
  void lower_resource_table(Parser &parser, report_callback report_error);

  void lower_strings_sequences(Parser &parser, report_callback /*report_error*/);

  /* Replace string literals by their hash and store the original string in the file metadata. */
  void lower_strings(Parser &parser, report_callback /*report_error*/);

  /* `class` -> `struct` */
  void lower_classes(Parser &parser, report_callback /*report_error*/);

  /* Create default initializer (empty brace) for all classes. */
  void lower_default_constructors(Parser &parser, report_callback report_error);

  /* Make all members of a class to be referenced using `this->`. */
  void lower_implicit_member(Parser &parser, report_callback report_error);

  /* Move all method definition outside of struct definition blocks. */
  void lower_method_definitions(Parser &parser, report_callback report_error);

  /* Add padding member to empty structs. */
  void lower_empty_struct(Parser &parser, report_callback /*report_error*/);

  /* Transform `a.fn(b)` into `fn(a, b)`. */
  void lower_method_calls(Parser &parser, report_callback report_error);

  /* Parse, convert to create infos, and erase declaration. */
  void lower_pipeline_definition(Parser &parser,
                                 const std::string &filename,
                                 report_callback /*report_error*/);

  void lower_stage_function(Parser &parser, report_callback /*report_error*/);

  /* Add #ifdef directive around functions using SRT arguments.
   * Need to run after `lower_entry_points_signature`. */
  void lower_srt_arguments(Parser &parser, report_callback /*report_error*/);

  /* Add ifdefs guards around scopes using resource accessors. */
  void lower_resource_access_functions(Parser &parser, report_callback /*report_error*/);

  void guarded_scope_mutation(Parser &parser,
                              parser::Scope scope,
                              const std::string &condition,
                              parser::Token fn_type = parser::Token::invalid());

  void lower_enums(Parser &parser, report_callback report_error);

  /* Merge attribute scopes. They are equivalent in the C++ standard.
   * This allow to simplify parsing later on.
   * `[[a]] [[b]]` > `[[a, b]]` */
  void lower_attribute_sequences(Parser &parser, report_callback /*report_error*/);

  /* Lint host shared structure for padding and alignment.
   * Remove the [[host_shared]] attribute. */
  void lower_host_shared_structures(Parser &parser, report_callback report_error);

  void lint_unbraced_statements(Parser &parser, report_callback report_error);

  void lint_reserved_tokens(Parser &parser, report_callback report_error);

  void lint_attributes(Parser &parser, report_callback report_error);

  void lower_noop_keywords(Parser &parser, report_callback report_error);

  void lower_trailing_comma_in_list(Parser &parser, report_callback /*report_error*/);

  /* Allow easier parsing of struct member declaration.
   * Example: `int a, b;` > `int a; int b;` */
  void lower_comma_separated_declarations(Parser &parser, report_callback /*report_error*/);

  void lower_implicit_return_types(Parser &parser, report_callback /*report_error*/);

  void lower_initializer_implicit_types(Parser &parser, report_callback /*report_error*/);

  void lower_designated_initializers(Parser &parser, report_callback report_error);

  /* Support for **full** aggregate initialization.
   * They are converted to default constructor for GLSL. */
  void lower_aggregate_initializers(Parser &parser, report_callback report_error);

  /* Auto detect array length, and lower to GLSL compatible syntax.
   * TODO(fclem): GLSL 4.3 already supports initializer list. So port the old GLSL syntax to
   * initializer list instead. */
  void lower_array_initializations(Parser &parser, report_callback report_error);

  static std::string strip_whitespace(const std::string &str);

  /**
   * Expand functions with default arguments to function overloads.
   * Expects formatted input and that function bodies are followed by newline.
   */
  void lower_function_default_arguments(Parser &parser, report_callback /*report_error*/);

  /* Successive mutations can introduce a lot of unneeded line directives. */
  void cleanup_line_directives(Parser &parser, report_callback /*report_error*/);

  /* Successive mutations can introduce a lot of unneeded blank lines. */
  void cleanup_empty_lines(Parser &parser, report_callback /*report_error*/);

  /* Used to make GLSL matrix constructor compatible with MSL in pyGPU shaders.
   * This syntax is not supported in blender's own shaders. */
  std::string matrix_constructor_mutation(const std::string &str);

  /* To be run before `argument_decorator_macro_injection()`. */
  void lower_reference_arguments(Parser &parser, report_callback /*report_error*/);

  void lower_unions(Parser &parser, report_callback report_error);

  /**
   * For safety reason, union members need to be declared with the union_t template.
   * This avoid raw member access which we cannot emulate. Instead this forces the use of the `()`
   * operator for accessing the members of the enum.
   *
   * Need to run before lower_unions.
   */
  void lower_union_accessor_templates(Parser &parser, report_callback report_error);

  /**
   * For safety reason, nested resource tables need to be declared with the srt_t template.
   * This avoid chained member access which isn't well defined with the preprocessing we are doing.
   *
   * This linting phase make sure that [[resource_table]] members uses it and that no incorrect
   * usage is made. We also remove this template because it has no real meaning.
   *
   * Need to run before lower_resource_table.
   */
  void lower_srt_accessor_templates(Parser &parser, report_callback report_error);

  /* Add `srt_access` around all member access of SRT variables.
   * Need to run before local reference mutations. */
  void lower_srt_member_access(Parser &parser, report_callback report_error);

  /* Parse entry point definitions and mutating all parameter usage to global resources. */
  void lower_entry_points(Parser &parser, report_callback report_error);

  /* Removes entry point arguments to make it compatible with the legacy code.
   * Has to run after mutation related to function arguments. */
  void lower_entry_points_signature(Parser &parser, report_callback /*report_error*/);
  /* To be run after `lower_reference_arguments()`. */
  void lower_reference_variables(Parser &parser, report_callback report_error);

  void lower_argument_qualifiers(Parser &parser, report_callback /*report_error*/);

  /* Example: `out float var[2]` > `out float _out_sta var _out_end[2]` */
  std::string argument_decorator_macro_injection(const std::string &str);
  
  /* Example: `= float[2](0.0, 0.0)` > `= ARRAY_T(float) ARRAY_V(0.0, 0.0)` */
  std::string array_constructor_macro_injection(const std::string &str);

  /* Assume formatted source with our code style. Cannot be applied to python shaders. */
  void lint_global_scope_constants(Parser &parser, report_callback report_error);

  /* Search for constructor definition in active code. These are not supported. */
  void lint_constructors(Parser &parser, report_callback report_error);

  /* Forward declaration of types are not supported and makes no sense in a shader program where
   * there is no pointers. */
  void lint_forward_declared_structs(Parser &parser, report_callback report_error);

  int static_array_size(const shader::parser::Scope &array,
                        report_callback report_error,
                        int fallback_value);
  std::string line_directive_prefix(const std::string &filename);
};

}  // namespace blender::gpu::shader
