/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <array>

#include "BLI_span.hh"

#include "DNA_attribute_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_storage.hh"

struct CustomData;

namespace blender::bke {

AttributeStorage attribute_legacy_convert_customdata_to_storage(
    const Span<AttrDomain> domains,
    const Span<CustomData *> custom_datas,
    const Span<int> domain_sizes);

void attribute_legacy_convert_storage_to_customdata(
    AttributeStorage &storage,
    const std::array<CustomData *, ATTR_DOMAIN_NUM> custom_data_domains);

}  // namespace blender::bke
