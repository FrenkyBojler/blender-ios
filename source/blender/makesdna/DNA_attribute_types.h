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

#  include "BLI_span.hh"
#  include "BLI_string_ref.hh"

namespace blender {
namespace bke {
struct AttributeStorageRuntime;
enum class AttrDomain : int8_t;
enum class AttrType : int16_t;
enum class AttrStorageType : int8_t;
}  // namespace bke
}  // namespace blender
using AttributeStorageRuntimeHandle = blender::bke::AttributeStorageRuntime;
#else
typedef struct AttributeStorageRuntimeHandle AttributeStorageRuntimeHandle;
#endif

struct AttributeDataArray {
  const void *data;
  const ImplicitSharingInfoHandle *sharing_info;
};

struct Attribute {
  char *name;
  int8_t domain;       /* bke::AttrDomain. */
  int16_t data_type;   /* bke::AttrType. */
  int8_t storage_type; /* bke::AttrStorageType */

  /** Type depends on storage type. */
  const void *data;

#ifdef __cplusplus
  void ensure_mutable();
#endif
};

struct AttributeStorage {
  Attribute **attributes_array;
  int attributes_num;
  int attributes_capacity;

  AttributeStorageRuntimeHandle *runtime;

#ifdef __cplusplus
  blender::Span<const Attribute *> items() const
  {
    return blender::Span(this->attributes_array, this->attributes_num);
  }
  blender::MutableSpan<Attribute *> items()
  {
    return blender::MutableSpan(this->attributes_array, this->attributes_num);
  }
  const Attribute *lookup(blender::StringRef name) const;
  Attribute *lookup_for_write(blender::StringRef name);
  bool remove(blender::StringRef name);
  Attribute &add(blender::StringRef name,
                 blender::bke::AttrDomain domain,
                 blender::bke::AttrType data_type,
                 const AttributeDataArray *data);

 private:
  void ensure_attribute_array_capacity(int attributes_num);
  Attribute &add_without_data(blender::StringRef name,
                              blender::bke::AttrDomain domain,
                              blender::bke::AttrType data_type,
                              blender::bke::AttrStorageType storage_type);
#endif
};
