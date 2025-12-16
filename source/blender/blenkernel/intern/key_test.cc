/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_idtype.hh"
#include "BKE_key.hh"
#include "BKE_main.hh"

#include "testing/testing.h"

class ShapekeyTest : public testing::Test {
 public:
  Main *bmain = nullptr;

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }
};

namespace blender::bke::tests {
TEST(ShapekeyTest, evaluation) {}

TEST(ShapekeyTest, evaluate_different_element_count)
{
  // BKE_key_evaluate_object_ex
}
}  // namespace blender::bke::tests
