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

/* Helper to create a FileSelectParams with initialized fields */
static FileSelectParams create_params(const char *dir)
{
  FileSelectParams params = {};
  BLI_strncpy(params.dir, dir, sizeof(params.dir));
  params.dir_template[0] = '\0';
  params.dir_resolved[0] = '\0';
  return params;
}

/* Helper to create a basic variable map for testing */
static VariableMap create_test_variables()
{
  VariableMap variables;
  variables.add_string("project", "my_project");
  variables.add_string("blend_file", "scene_01");
  variables.add_string("blend_dir", "blender_files");
  variables.add_integer("frame", 42);
  return variables;
}

/* Keep a minimal focused set of tests exercising the public API with variables
 * and without, plus navigation cases (going up / going deeper) and a variable
 * that resolves to an entire folder component. */

TEST(path_template_navigation, HandleText_WithVariable)
{
  FileSelectParams params = {};
  VariableMap variables = create_test_variables();

  path_template_nav_handle_text(&params, "/home/user/{blend_file}/renders", &variables);

  EXPECT_STREQ(params.dir_template, "/home/user/{blend_file}/renders");
  EXPECT_STREQ(params.dir, "/home/user/scene_01/renders");
}

TEST(path_template_navigation, HandleText_WithoutVariable)
{
  FileSelectParams params = {};

  /* Call without providing a VariableMap (use nullptr) */
  path_template_nav_handle_text(&params, "/home/user/projects", nullptr);

  EXPECT_STREQ(params.dir_template, "/home/user/projects");
  EXPECT_STREQ(params.dir, "/home/user/projects");
}

TEST(path_template_navigation, Browse_GoingUpFolder)
{
  FileSelectParams params = {};
  VariableMap variables = create_test_variables();

  BLI_strncpy(params.dir_template, "/home/{blend_dir}/projects", sizeof(params.dir_template));
  BLI_strncpy(params.dir, "/home/blender_files/projects", sizeof(params.dir));
  BLI_strncpy(params.dir_resolved, "/home/blender_files/projects", sizeof(params.dir_resolved));

  path_template_nav_handle_browse(&params, "/home/blender_files", &variables);

  EXPECT_STREQ(params.dir_template, "/home/{blend_dir}/");
  EXPECT_STREQ(params.dir, "/home/blender_files/");
}

TEST(path_template_navigation, Browse_GoingDeeperFolder)
{
  FileSelectParams params = {};
  VariableMap variables = create_test_variables();

  BLI_strncpy(params.dir_template, "/home/{blend_dir}", sizeof(params.dir_template));
  BLI_strncpy(params.dir, "/home/blender_files", sizeof(params.dir));
  BLI_strncpy(params.dir_resolved, "/home/blender_files", sizeof(params.dir_resolved));

  path_template_nav_handle_browse(&params, "/home/blender_files/subdir", &variables);

  EXPECT_STREQ(params.dir_template, "/home/{blend_dir}/subdir");
  EXPECT_STREQ(params.dir, "/home/blender_files/subdir");
}

TEST(path_template_navigation, HandleText_VariableResolvesToFolder)
{
  FileSelectParams params = {};
  VariableMap variables = create_test_variables();
  /* Add a variable that contains a slash (folder component) */
  variables.add_filepath("folder_test", "scenes/01");

  path_template_nav_handle_text(&params, "/project/{folder_test}/assets", &variables);

  /* The entire variable value should be treated as a single unit in the path */
  EXPECT_STREQ(params.dir_template, "/project/{folder_test}/assets");
  EXPECT_STREQ(params.dir, "/project/scenes/01/assets");
}

TEST(path_template_navigation, Browse_VariableFolder_GoingUp)
{
  FileSelectParams params = {};
  VariableMap variables = create_test_variables();
  /* Variable contains a multi-component folder */
  variables.add_filepath("folder_test", "scenes/01");

  /* Start with the template and resolved path pointing at the variable-expanded folder. */
  BLI_strncpy(params.dir_template, "/project/{folder_test}", sizeof(params.dir_template));
  BLI_strncpy(params.dir, "/project/scenes/01", sizeof(params.dir));
  BLI_strncpy(params.dir_resolved, "/project/scenes/01", sizeof(params.dir_resolved));

  /* Navigate up to the parent of the variable-expanded folder. */
  path_template_nav_handle_browse(&params, "/project", &variables);

  /* The variable component should be removed and the resolved path should be /project/ (implementation keeps trailing slash). */
  EXPECT_STREQ(params.dir_template, "/project/");
  EXPECT_STREQ(params.dir, "/project/");
  EXPECT_STREQ(params.dir_resolved, "/project/");
}

}  // namespace blender::bke::path_templates::tests
