/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Used for custom mesh data types (stored per vert/edge/loop/face)
 */

#pragma once

#include "DNA_defs.h"

#include "BLI_implicit_sharing.h"

#ifdef __cplusplus
namespace blender {
namespace bke {
struct AttributeStorageRuntime;
}  // namespace bke
}  // namespace blender
using AttributeStorageRuntimeHandle = blender::bke::AttributeStorageRuntime;
#else
typedef struct AttributeStorageRuntimeHandle AttributeStorageRuntimeHandle;
#endif

struct Attribute {
  char *name;
  int8_t domain;       /* bke::AttrDomain. */
  int16_t data_type;   /* bke::AttrType. */
  int8_t storage_type; /* bke::AttrStorageType */

  /* What's stored here can depend on the storage type. */
  void *data;
  const ImplicitSharingInfoHandle *sharing_info;
};

struct AttributeStorage {
  Attribute **attributes_array;
  int attributes_num;
  int attributes_capacity;

  AttributeStorageRuntimeHandle *runtime;

  // #ifdef __cplusplus
  //   blender::Span<Attribute *> items() const
  //   {
  //     return blender::Span(this->attributes_array, this->attributes_num);
  //   }
  //   blender::MutableSpan<Attribute *> items()
  //   {
  //     return blender::MutableSpan(this->attributes_array, this->attributes_num);
  //   }
  // #endif
};
