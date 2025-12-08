/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_armature.hh"
#include "BKE_idprop.hh"

#include "BLI_listbase.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_armature_types.h"

#include "testing/testing.h"

namespace blender::bke::tests {

TEST(idproperties, CreateGroup)
{
  IDProperty *prop = idprop::create_group("test").release();
  IDP_FreeProperty(prop);
}

TEST(idproperties, AddToGroup)
{
  IDProperty *group = idprop::create_group("test").release();
  EXPECT_EQ(IDP_GetPropertyFromGroup(group, "a"), nullptr);
  EXPECT_TRUE(IDP_AddToGroup(group, idprop::create("a", 3.0f).release()));
  EXPECT_TRUE(IDP_AddToGroup(group, idprop::create("b", 5).release()));
  EXPECT_EQ(IDP_float_get(IDP_GetPropertyFromGroup(group, "a")), 3.0f);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group, "b")), 5);
  IDProperty *duplicate_prop = idprop::create("a", 10).release();
  EXPECT_FALSE(IDP_AddToGroup(group, duplicate_prop));
  EXPECT_EQ(IDP_float_get(IDP_GetPropertyFromGroup(group, "a")), 3.0f);
  EXPECT_EQ(IDP_GetPropertyFromGroup(group, "c"), nullptr);
  IDP_FreeProperty(duplicate_prop);
  IDP_FreeProperty(group);
}

TEST(idproperties, ReplaceInGroup)
{
  IDProperty *group = idprop::create_group("test").release();
  EXPECT_TRUE(IDP_AddToGroup(group, idprop::create("a", 3.0f).release()));
  EXPECT_EQ(IDP_float_get(IDP_GetPropertyFromGroup(group, "a")), 3.0f);
  IDP_ReplaceInGroup(group, idprop::create("a", 5.0f).release());
  EXPECT_EQ(IDP_float_get(IDP_GetPropertyFromGroup(group, "a")), 5.0f);
  IDP_ReplaceInGroup(group, idprop::create("b", 5).release());
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group, "b")), 5);
  IDP_FreeProperty(group);
}

TEST(idproperties, RemoveFromGroup)
{
  IDProperty *group = idprop::create_group("test").release();
  EXPECT_EQ(IDP_GetPropertyFromGroup(group, "a"), nullptr);
  IDProperty *prop_a = idprop::create("a", 3.0f).release();
  EXPECT_TRUE(IDP_AddToGroup(group, prop_a));
  EXPECT_EQ(IDP_float_get(IDP_GetPropertyFromGroup(group, "a")), 3.0f);
  IDP_RemoveFromGroup(group, prop_a);
  EXPECT_EQ(IDP_GetPropertyFromGroup(group, "a"), nullptr);
  IDP_FreeProperty(prop_a);
  IDP_FreeProperty(group);
}

TEST(idproperties, ReplaceGroupInGroup)
{
  IDProperty *group1 = idprop::create_group("test").release();
  IDP_AddToGroup(group1, idprop::create("a", 1).release());
  IDP_AddToGroup(group1, idprop::create("b", 2).release());
  IDProperty *group2 = idprop::create_group("test2").release();
  IDP_AddToGroup(group2, idprop::create("b", 3).release());
  IDP_AddToGroup(group2, idprop::create("c", 4).release());

  IDP_ReplaceGroupInGroup(group1, group2);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group1, "a")), 1);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group1, "b")), 3);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group1, "c")), 4);

  IDP_FreeProperty(group1);
  IDP_FreeProperty(group2);
}

TEST(idproperties, SyncGroupValues)
{
  IDProperty *group1 = idprop::create_group("test").release();
  IDProperty *group2 = idprop::create_group("test").release();
  IDP_AddToGroup(group1, idprop::create("a", 1).release());
  IDP_AddToGroup(group1, idprop::create("b", 2).release());
  IDP_AddToGroup(group1, idprop::create("x", 2).release());
  IDP_AddToGroup(group2, idprop::create("a", 3).release());
  IDP_AddToGroup(group2, idprop::create("c", 4).release());
  IDP_AddToGroup(group2, idprop::create("x", "value").release());

  IDP_SyncGroupValues(group1, group2);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group1, "a")), 3);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group1, "b")), 2);
  EXPECT_EQ(IDP_int_get(IDP_GetPropertyFromGroup(group1, "x")), 2);
  EXPECT_EQ(IDP_GetPropertyFromGroup(group1, "c"), nullptr);

  IDP_FreeProperty(group1);
  IDP_FreeProperty(group2);
}

TEST(idproperties, ReprGroup)
{
  auto repr_fn = [](IDProperty *prop) -> std::string {
    uint result_len;
    char *c_str = IDP_reprN(prop, &result_len);
    std::string result = std::string(c_str, result_len);
    MEM_freeN(c_str);
    return result;
  };

  IDProperty *group = idprop::create_group("test").release();

  EXPECT_EQ(repr_fn(group), "{}");

  IDP_AddToGroup(group, idprop::create("a", 1).release());
  IDP_AddToGroup(group, idprop::create("b", 0.5f).release());
  IDP_AddToGroup(group, idprop::create_bool("c", true).release());
  IDP_AddToGroup(group, idprop::create_bool("d", false).release());
  IDP_AddToGroup(group, idprop::create("e", "ABC (escape \" \\)").release());
  IDP_AddToGroup(group, idprop::create("f", Span<int32_t>({-1, 0, 1})).release());
  IDP_AddToGroup(group, idprop::create("g", Span<float>({-0.5f, 0.0f, 0.5f})).release());
  IDP_AddToGroup(group, idprop::create_group("h").release());

  EXPECT_EQ(repr_fn(group),
            "{"
            "\"a\": 1, "
            "\"b\": 0.5, "
            "\"c\": True, "
            "\"d\": False, "
            "\"e\": \"ABC (escape \\\" \\\\)\", "
            "\"f\": [-1, 0, 1], "
            "\"g\": [-0.5, 0, 0.5], "
            "\"h\": {}"
            "}");

  IDP_FreeProperty(group);
}

class IDPropContainerIteratorFixture : public testing::Test {
 protected:
  bArmature arm = {};
  Bone bone1 = {}, bone2 = {}, bone3 = {};
  blender::Set<IDProperty *> created_user_properties = {};
  blender::Set<IDProperty *> created_system_properties = {};

  void SetUp() override
  {
    STRNCPY(this->arm.id.name, "ARArmature");

    this->arm.id.properties = idprop::create_group("arm_properties").release();
    this->created_user_properties.add(this->arm.id.properties);
    this->arm.id.system_properties = idprop::create_group("arm_system_properties").release();
    this->created_system_properties.add(this->arm.id.system_properties);

    STRNCPY(this->bone1.name, "bone1");
    STRNCPY(this->bone2.name, "bone2");
    STRNCPY(this->bone3.name, "bone3");

    this->bone1.prop = idprop::create_group("bone1_prop").release();
    this->created_user_properties.add(this->bone1.prop);
    this->bone2.system_properties = idprop::create_group("bone2_system_properties").release();
    this->created_system_properties.add(this->bone2.system_properties);
    this->bone3.prop = idprop::create_group("bone3_prop").release();
    this->created_user_properties.add(this->bone3.prop);
    this->bone3.system_properties = idprop::create_group("bone3_system_properties").release();
    this->created_system_properties.add(this->bone3.system_properties);

    BLI_addtail(&this->arm.bonebase, &this->bone1);    /* bone1 is root bone. */
    BLI_addtail(&this->arm.bonebase, &this->bone2);    /* bone2 is root bone. */
    BLI_addtail(&this->bone2.childbase, &this->bone3); /* bone3 has bone2 as parent. */
  }

  void TearDown() override
  {
    for (IDProperty *idprop_iter : this->created_user_properties) {
      IDP_FreeProperty(idprop_iter);
    }
    for (IDProperty *idprop_iter : this->created_system_properties) {
      IDP_FreeProperty(idprop_iter);
    }
  }
};

TEST_F(IDPropContainerIteratorFixture, idproperties_container_iterator)
{
  struct SeenResult {
    IDProperty *idproperty;
    eIDTypeInfoIDPropertyCallbackFlags flags;
  };
  blender::Vector<SeenResult> seen_idproperties_containers;
  int seen_properties_pointers = 0;

  blender::bke::idprop::foreach_id_idproperty_container(
      this->arm.id,
      [&seen_idproperties_containers, &seen_properties_pointers](
          IDProperty **idproperty, const eIDTypeInfoIDPropertyCallbackFlags flags) -> void {
        seen_properties_pointers++;
        if (*idproperty) {
          seen_idproperties_containers.append({*idproperty, flags});
        }
      });

  EXPECT_EQ(seen_idproperties_containers.size(),
            this->created_user_properties.size() + this->created_system_properties.size());
  EXPECT_EQ(seen_properties_pointers, 8);
  for (SeenResult &result : seen_idproperties_containers) {
    if (result.flags == eIDTypeInfoIDPropertyCallbackFlags::user_defined) {
      EXPECT_TRUE(this->created_user_properties.contains(result.idproperty));
    }
    else if (result.flags == eIDTypeInfoIDPropertyCallbackFlags::system_defined) {
      EXPECT_TRUE(this->created_system_properties.contains(result.idproperty));
    }
    else {
      FAIL() << "Unexpected eIDTypeInfoIDPropertyCallbackFlags value" << int(result.flags);
    }
  }
}

}  // namespace blender::bke::tests
