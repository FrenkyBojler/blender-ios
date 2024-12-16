/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_implicit_sharing.h"

#ifdef __cplusplus

#  include "BLI_span.hh"
#  include "BLI_string_ref.hh"

struct BlendDataReader;
struct BlendWriter;
namespace blender {
class CPPType;
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

typedef struct AttributeDataArray {
  void *data;
  const ImplicitSharingInfoHandle *sharing_info;
  int elements_num;
  char _pad[4];
} AttributeDataArray;

typedef struct Attribute {
  char *name;
  int16_t data_type;   /* bke::AttrType. */
  int8_t domain;       /* bke::AttrDomain. */
  int8_t storage_type; /* bke::AttrStorageType */
  char _pad[4];

  /** Type depends on storage type. */
  void *data;

#ifdef __cplusplus
  void ensure_mutable();
#endif
} Attribute;

typedef struct AttributeStorage {
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
                 const AttributeDataArray &data);

 private:
  void ensure_attribute_array_capacity(int attributes_num);
  Attribute &add_without_data(blender::StringRef name,
                              blender::bke::AttrDomain domain,
                              blender::bke::AttrType data_type,
                              blender::bke::AttrStorageType storage_type);

  void blend_read(BlendDataReader &reader);
  void blend_write(BlendWriter &writer) const;
#endif
} AttributeStorage;
