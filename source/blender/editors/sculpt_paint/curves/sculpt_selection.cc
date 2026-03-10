/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"

#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

bke::SpanAttributeWriter<float> float_selection_ensure(Curves &curves_id)
{
  /* TODO: Use a generic attribute conversion utility instead of this function. */
  bke::CurvesGeometry &curves = curves_id.geometry.wrap();
  bke::MutableAttributeAccessor attributes = curves.attributes_for_write();

  if (const auto meta_data = attributes.lookup_meta_data(".selection"_ustr)) {
    if (meta_data->data_type == bke::AttrType::Bool) {
      const VArray<float> selection = *attributes.lookup<float>(".selection"_ustr);
      float *dst = MEM_new_array_uninitialized<float>(selection.size(), __func__);
      selection.materialize({dst, selection.size()});

      attributes.remove(".selection"_ustr);
      attributes.add(".selection"_ustr,
                     meta_data->domain,
                     bke::AttrType::Float,
                     bke::AttributeInitMoveArray(dst));
    }
  }
  else {
    attributes.add(".selection"_ustr,
                   bke::AttrDomain(curves_id.selection_domain),
                   bke::AttrType::Float,
                   bke::AttributeInitValue(1.0f));
  }

  return curves.attributes_for_write().lookup_for_write_span<float>(".selection"_ustr);
}

}  // namespace blender::ed::sculpt_paint
