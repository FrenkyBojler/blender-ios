/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_idprop.hh"
#include "testing/testing.h"

namespace blender::bke::tests {

TEST(idproperties, CreateGroup)
{
  IDProperty *prop = idprop::create_group("test").release();
  IDP_FreeProperty(prop);
}

TEST(idproperties, AddElementsToGroup)
{
  IDProperty *group = idprop::create_group("test").release();
  IDP_AddToGroup(group, idprop::create("a", 3.0f).release());
  IDP_AddToGroup(group, idprop::create("b", 5).release());
  EXPECT_EQ(IDP_Float(IDP_GetPropertyFromGroup(group, "a")), 3.0f);
  EXPECT_EQ(IDP_Int(IDP_GetPropertyFromGroup(group, "b")), 5);
  IDP_FreeProperty(group);
}

}  // namespace blender::bke::tests
