/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_attribute_storage.hh"

#include "BLI_map.hh"
#include "BLI_set.hh"

#include "DNA_customdata_types.h"

namespace blender::bke {

void attribute_storage_blend_write_prepare(
    AttributeStorage &data,
    const Map<AttrDomain, Vector<CustomDataLayer, 16> *> &layers_to_write,
    AttributeStorage::BlendWriteData &write_data);

}  // namespace blender::bke
