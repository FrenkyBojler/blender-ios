/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_asset.hh"

#include "BLI_uuid.h"

#include "DNA_asset_types.h"

#include "testing/testing.h"

namespace blender::bke::tests {

TEST(AssetMetadataTest, set_catalog_id)
{
  AssetMetaData meta{};
  const bUUID uuid = BLI_uuid_generate_random();

  /* Test trivial values. */
  BKE_asset_metadata_catalog_id_clear(&meta);
  EXPECT_TRUE(BLI_uuid_is_nil(meta.catalog_id));
  EXPECT_STREQ("", meta.catalog_simple_name);

  /* Test simple situation where the given short name is used as-is. */
  BKE_asset_metadata_catalog_id_set(&meta, uuid, "simple");
  EXPECT_TRUE(BLI_uuid_equal(uuid, meta.catalog_id));
  EXPECT_STREQ("simple", meta.catalog_simple_name);

  /* Test white-space trimming. */
  BKE_asset_metadata_catalog_id_set(&meta, uuid, " Govoriš angleško?    ");
  EXPECT_STREQ("Govoriš angleško?", meta.catalog_simple_name);

  /* Test length trimming to 63 chars + terminating zero. */
  constexpr char LEN66[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  constexpr char LEN63[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1";
  BKE_asset_metadata_catalog_id_set(&meta, uuid, LEN66);
  EXPECT_STREQ(LEN63, meta.catalog_simple_name);

  /* Test length trimming happens after white-space trimming. */
  constexpr char LEN68[] =
      "     \
      000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20 ";
  BKE_asset_metadata_catalog_id_set(&meta, uuid, LEN68);
  EXPECT_STREQ(LEN63, meta.catalog_simple_name);

  /* Test length trimming to 63 bytes, and not 63 characters. ✓ in UTF8 is three bytes long. */
  constexpr char WITH_UTF8[] =
      "00010203040506✓0708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
  BKE_asset_metadata_catalog_id_set(&meta, uuid, WITH_UTF8);
  EXPECT_STREQ("00010203040506✓0708090a0b0c0d0e0f101112131415161718191a1b1c1d",
               meta.catalog_simple_name);
}

}  // namespace blender::bke::tests
