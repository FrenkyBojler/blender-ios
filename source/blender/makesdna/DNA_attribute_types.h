/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_implicit_sharing.h"

#ifdef __cplusplus

#  include <cstdint>

namespace blender {
namespace bke {
class AttributeStorage;
class AttributeStorageRuntime;
enum class AttrDomain : int8_t;
enum class AttrType : int16_t;
enum class AttrStorageType : int8_t;
}  // namespace bke
}  // namespace blender
using AttributeStorageRuntimeHandle = blender::bke::AttributeStorageRuntime;
#else
typedef struct AttributeStorageRuntimeHandle AttributeStorageRuntimeHandle;
#endif

struct AttributeArrayDNA {
  void *data;
  const ImplicitSharingInfoHandle *sharing_info;
  int elements_num;
  char _pad[4];
};

struct AttributeDNA {
  const char *name;
  int16_t data_type;   /* bke::AttrType. */
  int8_t domain;       /* bke::AttrDomain. */
  int8_t storage_type; /* bke::AttrStorageType */
  char _pad[4];

  /** Type depends on storage type. */
  void *data;
};

struct AttributeStorage {
  /* Array only used in files, otherwise #AttributeStorageRuntime::attributes is used. */
  struct AttributeDNA **attributes_array;
  int attributes_num;

  char _pad[4];

  AttributeStorageRuntimeHandle *runtime;

#ifdef __cplusplus
  blender::bke::AttributeStorage &wrap();
  const blender::bke::AttributeStorage &wrap() const;
#endif
};
