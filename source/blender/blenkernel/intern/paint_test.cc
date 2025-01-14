/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_appdir.hh"
#include "BKE_callbacks.hh"
#include "BKE_cpp_types.hh"
#include "BKE_idtype.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.h"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_scene.hh"

#include "CLG_log.h"

#include "DEG_depsgraph.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "GEO_mesh_primitive_cuboid.hh"

#include "IMB_imbuf.hh"

#include "testing/testing.h"

namespace blender::bke::tests {
  class PaintTest : public testing::Test {
  public:
    Main *bmain;
    Scene *scene;
    Depsgraph *depsgraph;

    Object *cube;
    Mesh *cube_mesh;

    static void SetUpTestSuite()
    {
      CLG_init();

      BKE_cpp_types_init();
      BKE_idtype_init();

      BKE_callback_global_init();

      DEG_register_node_types();

      BKE_appdir_init();
      IMB_init();
    }

    void SetUp() override
    {
      bmain = BKE_main_new();
      scene = BKE_scene_add(bmain, "Test Scene");

      ViewLayer* view_layer = BKE_view_layer_default_view(scene);
      depsgraph = BKE_scene_ensure_depsgraph(bmain, scene, view_layer);
      DEG_make_active(depsgraph);

      cube = BKE_object_add(bmain, scene, view_layer, OB_MESH, "Test Cube");

      cube_mesh = geometry::create_cuboid_mesh(float3(1.0, 1.0, 1.0), 10, 10, 10);
      BKE_mesh_assign_object(bmain, cube, cube_mesh);

      BKE_scene_graph_update_tagged(depsgraph, bmain);

      cube->mode = OB_MODE_SCULPT;
      BKE_object_sculpt_data_create(cube);
    }

    void TearDown() override
    {
      BKE_id_free(bmain, cube_mesh);
      BKE_id_free(bmain, cube);
      BKE_id_free(bmain, scene);
      BKE_main_free(bmain);
    }

    static void TearDownTestSuite()
    {
      BKE_callback_global_finalize();

      IMB_exit();
      BKE_appdir_exit();
      CLG_exit();
    }
  };

  TEST_F(PaintTest, pbvh_ensure) {
    EXPECT_FALSE(bke::object::pbvh_get(*cube)) << "Paint BVH should not exist!";
    pbvh::Tree& tree = bke::object::pbvh_ensure(*depsgraph, *cube);
    EXPECT_GT(tree.nodes<pbvh::MeshNode>().size(), 0)
        << "Paint BVH should have some non zero amount of nodes";
  }
}
