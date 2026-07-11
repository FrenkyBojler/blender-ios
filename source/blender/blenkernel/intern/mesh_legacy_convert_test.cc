/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_gtest_base.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"

#include "DNA_customdata_types.h"
#include "DNA_mesh_types.h"

namespace blender::bke::tests {

class MeshLegacyConvertTest : public BlenderGTestBase {};

TEST_F(MeshLegacyConvertTest, TessfaceUVActiveAndDefaultLayers)
{
  Mesh *mesh = BKE_mesh_new_nomain(4, 4, 1, 4);
  mesh->vert_positions_for_write()[0] = float3(-1.0f, -1.0f, 0.0f);
  mesh->vert_positions_for_write()[1] = float3(1.0f, -1.0f, 0.0f);
  mesh->vert_positions_for_write()[2] = float3(1.0f, 1.0f, 0.0f);
  mesh->vert_positions_for_write()[3] = float3(-1.0f, 1.0f, 0.0f);
  mesh->edges_for_write()[0] = int2(0, 1);
  mesh->edges_for_write()[1] = int2(1, 2);
  mesh->edges_for_write()[2] = int2(2, 3);
  mesh->edges_for_write()[3] = int2(3, 0);
  mesh->face_offsets_for_write()[0] = 0;
  mesh->face_offsets_for_write()[1] = 4;
  mesh->corner_verts_for_write()[0] = 0;
  mesh->corner_verts_for_write()[1] = 1;
  mesh->corner_verts_for_write()[2] = 2;
  mesh->corner_verts_for_write()[3] = 3;
  mesh->corner_edges_for_write()[0] = 0;
  mesh->corner_edges_for_write()[1] = 1;
  mesh->corner_edges_for_write()[2] = 2;
  mesh->corner_edges_for_write()[3] = 3;

  MutableAttributeAccessor attributes = mesh->attributes_for_write();
  {
    SpanAttributeWriter<float2> uv_map = attributes.lookup_or_add_for_write_only_span<float2>(
        "UVMap", AttrDomain::Corner);
    uv_map.span.fill(float2(0.0f, 0.0f));
    uv_map.finish();
  }
  {
    SpanAttributeWriter<float2> automap = attributes.lookup_or_add_for_write_only_span<float2>(
        "automap", AttrDomain::Corner);
    automap.span.fill(float2(1.0f, 1.0f));
    automap.finish();
  }
  {
    SpanAttributeWriter<float2> render_uv = attributes.lookup_or_add_for_write_only_span<float2>(
        "RenderUV", AttrDomain::Corner);
    render_uv.span.fill(float2(2.0f, 2.0f));
    render_uv.finish();
  }

  mesh->uv_maps_active_set("automap");
  mesh->uv_maps_default_set("RenderUV");
  BKE_mesh_tessface_calc(mesh);

  EXPECT_STREQ(CustomData_get_active_layer_name(&mesh->fdata_legacy, CD_MTFACE), "automap");
  EXPECT_STREQ(CustomData_get_render_layer_name(&mesh->fdata_legacy, CD_MTFACE), "RenderUV");

  /* Legacy texture evaluation uses the active layer when no UV map is specified. */
  const void *implicit_uv = CustomData_get_layer(&mesh->fdata_legacy, CD_MTFACE);
  const void *active_uv = CustomData_get_layer_named(&mesh->fdata_legacy, CD_MTFACE, "automap");
  const void *explicit_uv = CustomData_get_layer_named(&mesh->fdata_legacy, CD_MTFACE, "UVMap");
  EXPECT_EQ(implicit_uv, active_uv);
  EXPECT_NE(implicit_uv, explicit_uv);

  BKE_id_free(nullptr, mesh);
}

}  // namespace blender::bke::tests
