/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "ED_fileselect.hh"

#include "BKE_main.hh"
#include "BKE_path_templates.hh"

#include "BLI_string.h"

#include "DNA_space_types.h"

namespace blender::bke::path_templates::tests {

using namespace blender::bke::path_templates;

/* -------------------------------------------------------------------- */
/** \name Test Helpers
 * \{ */

/** Initialize a FileSelectParams with all fields cleared. */
static FileSelectParams create_empty_params()
{
  FileSelectParams params = {};
  params.dir[0] = '\0';
  params.dir_template[0] = '\0';
  return params;
}

/** Initialize a FileSelectParams with preset directory values. */
static FileSelectParams create_params_with_state(const char *dir, const char *dir_template)
{
  FileSelectParams params = {};
  BLI_strncpy(params.dir, dir, sizeof(params.dir));
  BLI_strncpy(params.dir_template, dir_template, sizeof(params.dir_template));
  return params;
}

/** Create a standard variable map for testing with common template variables. */
static VariableMap create_test_variables()
{
  VariableMap variables;
  variables.add_string("project_name", "my_project");
  variables.add_filepath("blend_name", "my_file");
  variables.add_filepath("blend_dir", "blender_files");
  variables.add_integer("frame", 42);
  return variables;
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Initialize Tests
 * \{ */

TEST(path_template_navigation, Initialize_WithVariable)
{
  FileSelectParams params = create_empty_params();
  BLI_strncpy(params.dir, "/home/{project_name}/renders", sizeof(params.dir));
  VariableMap variables = create_test_variables();

  nav_initialize(&params, variables);

  EXPECT_STREQ(params.dir_template, "/home/{project_name}/renders");
  EXPECT_STREQ(params.dir, "/home/my_project/renders");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Handle Text Input Tests
 * \{ */

TEST(path_template_navigation, HandleText_WithSingleVariable)
{
  FileSelectParams params = create_empty_params();
  VariableMap variables = create_test_variables();

  nav_handle_text_input(&params, "/home/{project_name}/renders", variables);

  EXPECT_STREQ(params.dir_template, "/home/{project_name}/renders");
  EXPECT_STREQ(params.dir, "/home/my_project/renders");
}

TEST(path_template_navigation, HandleText_WithNestedVariable)
{
  FileSelectParams params = create_empty_params();
  VariableMap variables = create_test_variables();
  variables.add_filepath("nested_path", "scenes/sequence/01");

  nav_handle_text_input(&params, "/project/{nested_path}/assets", variables);

  EXPECT_STREQ(params.dir_template, "/project/{nested_path}/assets");
  EXPECT_STREQ(params.dir, "/project/scenes/sequence/01/assets");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Browse Navigation Tests
 * \{ */

TEST(path_template_navigation, Browse_UpWithSingleVariable)
{
  FileSelectParams params = create_params_with_state("/home/my_project/renders",
                                                     "/home/{project_name}/renders");
  VariableMap variables = create_test_variables();

  nav_handle_browse(&params, "/home/my_project", variables);

  EXPECT_STREQ(params.dir_template, "/home/{project_name}/");
  EXPECT_STREQ(params.dir, "/home/my_project/");
}

TEST(path_template_navigation, Browse_DownWithSingleVariable)
{
  FileSelectParams params = create_params_with_state("/home/my_project", "/home/{project_name}");
  VariableMap variables = create_test_variables();

  nav_handle_browse(&params, "/home/my_project/renders", variables);

  EXPECT_STREQ(params.dir_template, "/home/{project_name}/renders");
  EXPECT_STREQ(params.dir, "/home/my_project/renders");
}

TEST(path_template_navigation, Browse_UpWithNestedVariable)
{
  FileSelectParams params = create_params_with_state("/project/scenes/sequence/01/assets",
                                                     "/project/{nested_path}/assets");
  VariableMap variables = create_test_variables();
  variables.add_filepath("nested_path", "scenes/sequence/01");

  nav_handle_browse(&params, "/project/scenes/sequence/01", variables);

  EXPECT_STREQ(params.dir_template, "/project/{nested_path}/");
  EXPECT_STREQ(params.dir, "/project/scenes/sequence/01/");
}

TEST(path_template_navigation, Browse_DownWithNestedVariable)
{
  FileSelectParams params = create_params_with_state("/project/scenes/sequence/01",
                                                     "/project/{nested_path}");
  VariableMap variables = create_test_variables();
  variables.add_filepath("nested_path", "scenes/sequence/01");

  nav_handle_browse(&params, "/project/scenes/sequence/01/assets", variables);

  EXPECT_STREQ(params.dir_template, "/project/{nested_path}/assets");
  EXPECT_STREQ(params.dir, "/project/scenes/sequence/01/assets");
}

TEST(path_template_navigation, Browse_UpWithMultiPathVariable)
{
  /* Testing filepath as variable (instead of a single string).
  expectation is to treat the entire variable as a unit. */
  FileSelectParams params = create_params_with_state("/root/project", "/{project_root}");
  VariableMap variables = create_test_variables();
  variables.add_filepath("project_root", "/root/project");

  nav_handle_browse(&params, "/", variables);

  EXPECT_STREQ(params.dir_template, "/");
  EXPECT_STREQ(params.dir, "/");
}

/** \} */

}  // namespace blender::bke::path_templates::tests
