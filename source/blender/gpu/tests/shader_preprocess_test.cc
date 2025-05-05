/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "glsl_preprocess/glsl_preprocess.hh"

#include "gpu_testing.hh"

namespace blender::gpu::tests {

static void test_preprocess_utilities()
{
  using namespace shader;
  using namespace std;

  string input = "test (u, u(s,(s,s)), u) {t{{}},t,{};(,)} {u{}} end";
  EXPECT_EQ(Preprocessor::get_content_between_balanced_pair(input, '{', '}'), "t{{}},t,{};(,)");
  EXPECT_EQ(Preprocessor::get_content_between_balanced_pair(input, '{', '}', true), "u{}");

  EXPECT_EQ(Preprocessor::replace_char_between_balanced_pair(input, '(', ')', ',', '!'),
            "test (u! u(s!(s!s))! u) {t{{}},t,{};(!)} {u{}} end");

  vector<string> split_expect{"test (u, u(s,(s,s", "", ", u", " {t{{}},t,{};(,", "} {u{}} end"};
  vector<string> split_result = Preprocessor::split_string(input, ')');
  EXPECT_EQ_VECTOR(split_expect, split_result);

  string input2 = "u, u(s,(s,s)), u";
  vector<string> split_expect2{"u", " u(s,(s,s))", " u"};
  vector<string> split_result2 = Preprocessor::split_string_not_between_balanced_pair(
      input2, ',', '(', ')');
  EXPECT_EQ_VECTOR(split_expect2, split_result2);

  string input_reference = "void func(int &a, int (&c)[2]) {{ int &b = a; }} int &b = a;";
  int fn_ref_count = 0, arg_ref_count = 0, global_ref_count = 0;
  Preprocessor::reference_search(
      input_reference, [&](int parenthesis_depth, int bracket_depth, char & /*c*/) {
        if ((parenthesis_depth == 1 || parenthesis_depth == 2) && bracket_depth == 0) {
          arg_ref_count += 1;
        }
        else if (bracket_depth > 0) {
          fn_ref_count += 1;
        }
        else if (bracket_depth == 0 && parenthesis_depth == 0) {
          global_ref_count += 1;
        }
      });
  EXPECT_EQ(arg_ref_count, 2);
  EXPECT_EQ(fn_ref_count, 1);
  EXPECT_EQ(global_ref_count, 1);
}
GPU_TEST(preprocess_utilities);

static std::string process_test_string(std::string str,
                                       std::string &first_error,
                                       shader::metadata::Source *r_metadata = nullptr)
{
  using namespace shader;
  Preprocessor preprocessor;
  shader::metadata::Source metadata;
  std::string result = preprocessor.process(
      Preprocessor::SourceLanguage::BLENDER_GLSL,
      str,
      "test.glsl",
      true,
      true,
      [&](const std::smatch & /*match*/, const char *err_msg) {
        if (first_error.empty()) {
          first_error = err_msg;
        }
      },
      metadata);

  if (r_metadata != nullptr) {
    *r_metadata = metadata;
  }

  /* Strip first line directive as they are platform dependent. */
  size_t newline = result.find('\n');
  return result.substr(newline + 1);
}

static void test_preprocess_unroll()
{
  using namespace shader;
  using namespace std;

  {
    string input = R"([[gpu::unroll]] for (int i = 2; i < 4; i++, y++) { content += i; })";
    string expect = R"({ int i = 2;
#line 1
{ content += i; }
#line 1
i++, y++;
#line 1
{ content += i; }
#line 1
i++, y++;
#line 1
})";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"([[gpu::unroll]] for (int i = 2; i < 4 && i < y; i++, y++) { cont += i; })";
    string expect = R"({ int i = 2;
#line 1
if (i < y) { cont += i; }
#line 1
i++, y++;
#line 1
if (i < y) { cont += i; }
#line 1
i++, y++;
#line 1
})";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"([[gpu::unroll(2)]] for (; i < j;) { content += i; })";
    string expect = R"({ ;
#line 1
if (i < j) { content += i; }
#line 1
;
#line 1
if (i < j) { content += i; }
#line 1
;
#line 1
})";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"([[gpu::unroll(2)]] for (; i < j;) { [[gpu::unroll(2)]] for (; j < k;) {} })";
    string expect = R"({ ;
#line 1
if (i < j) { { ;
#line 1
 if (j < k) {}
#line 1
 ;
#line 1
 if (j < k) {}
#line 1
 ;
#line 1
 } }
#line 1
;
#line 1
if (i < j) { { ;
#line 1
 if (j < k) {}
#line 1
 ;
#line 1
 if (j < k) {}
#line 1
 ;
#line 1
 } }
#line 1
;
#line 1
})";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"([[gpu::unroll(2)]] for (; i < j;) { break; })";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(error, "Error: Unrolled loop cannot contain \"break\" statement.");
  }
  {
    string input = R"([[gpu::unroll(2)]] for (; i < j;) { continue; })";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(error, "Error: Unrolled loop cannot contain \"continue\" statement.");
  }
  {
    string input = R"([[gpu::unroll(2)]] for (; i < j;) { for (; j < k;) {break;continue;} })";
    string expect = R"({ ;
#line 1
if (i < j) { for (; j < k;) {break;continue;} }
#line 1
;
#line 1
if (i < j) { for (; j < k;) {break;continue;} }
#line 1
;
#line 1
})";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(output, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"([[gpu::unroll]] for (int i = 3; i > 2; i++) {})";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(error, "Error: Unsupported condition in unrolled loop.");
  }
}
GPU_TEST(preprocess_unroll);

static void test_preprocess_reference()
{
  using namespace shader;
  using namespace std;

  {
    string input = R"(void func() { auto &a = b; a.a = 0; c = a(a); })";
    string expect = R"(void func() { b.a = 0; c = a(b); })";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(input, expect);
    EXPECT_EQ(error, "");
  }
  {
    string input = R"(void func() { auto &a = b(0); })";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(error, "Reference definitions cannot contain function calls.");
  }
  {
    string input = R"(void func() { auto &a = b[0 + 1]; })";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(error, "Array subscript inside reference declaration must be a single variable.");
  }
  {
    string input = R"(void func() { const int c = 0; auto &a = b[c]; })";
    string error;
    string output = process_test_string(input, error);
    EXPECT_EQ(error,
              "Cannot locate array subscript variable declaration. "
              "If it is a global variable, assign it to a temporary const variable for "
              "indexing inside the reference.");
  }
}
GPU_TEST(preprocess_reference);

}  // namespace blender::gpu::tests
