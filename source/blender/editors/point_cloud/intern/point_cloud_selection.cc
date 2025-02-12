/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edpointcloud
 */

#include "BLI_array_utils.hh"
#include "BLI_index_mask.hh"

#include "BKE_attribute.hh"

#include "ED_select_utils.hh"

#include "DNA_pointcloud_types.h"

namespace blender::ed::point_cloud {

static bool contains(const VArray<bool> &varray,
                     const IndexMask &indices_to_check,
                     const bool value)
{
  const CommonVArrayInfo info = varray.common_info();
  if (info.type == CommonVArrayInfo::Type::Single) {
    return *static_cast<const bool *>(info.data) == value;
  }
  if (info.type == CommonVArrayInfo::Type::Span) {
    const Span<bool> span(static_cast<const bool *>(info.data), varray.size());
    return threading::parallel_reduce(
        indices_to_check.index_range(),
        4096,
        false,
        [&](const IndexRange range, const bool init) {
          if (init) {
            return init;
          }
          const IndexMask sliced_mask = indices_to_check.slice(range);
          if (std::optional<IndexRange> range = sliced_mask.to_range()) {
            return span.slice(*range).contains(value);
          }
          for (const int64_t segment_i : IndexRange(sliced_mask.segments_num())) {
            const IndexMaskSegment segment = sliced_mask.segment(segment_i);
            for (const int i : segment) {
              if (span[i] == value) {
                return true;
              }
            }
          }
          return false;
        },
        std::logical_or());
  }
  return threading::parallel_reduce(
      indices_to_check.index_range(),
      2048,
      false,
      [&](const IndexRange range, const bool init) {
        if (init) {
          return init;
        }
        constexpr int64_t MaxChunkSize = 512;
        const int64_t slice_end = range.one_after_last();
        for (int64_t start = range.start(); start < slice_end; start += MaxChunkSize) {
          const int64_t end = std::min<int64_t>(start + MaxChunkSize, slice_end);
          const int64_t size = end - start;
          const IndexMask sliced_mask = indices_to_check.slice(start, size);
          std::array<bool, MaxChunkSize> values;
          auto values_end = values.begin() + size;
          varray.materialize_compressed(sliced_mask, values);
          if (std::find(values.begin(), values_end, value) != values_end) {
            return true;
          }
        }
        return false;
      },
      std::logical_or());
}

static bool contains(const VArray<bool> &varray, const IndexRange range_to_check, const bool value)
{
  return contains(varray, IndexMask(range_to_check), value);
}

bool has_anything_selected(const PointCloud &point_cloud)
{
  const VArray<bool> selection = *point_cloud.attributes().lookup<bool>(".selection");
  return !selection || contains(selection, selection.index_range(), true);
}

void remove_selection_attributes(bke::MutableAttributeAccessor &attributes)
{
  static const std::array<StringRef, 1> selection_attribute_names{".selection"};
  for (const StringRef selection_name : selection_attribute_names) {
    attributes.remove(selection_name);
  }
}

bke::GSpanAttributeWriter ensure_selection_attribute(PointCloud &point_cloud,
                                                     bke::AttrDomain selection_domain,
                                                     eCustomDataType create_type,
                                                     StringRef attribute_name)
{
  bke::MutableAttributeAccessor attributes = point_cloud.attributes_for_write();
  if (attributes.contains(attribute_name)) {
    bke::GSpanAttributeWriter selection_attr = attributes.lookup_for_write_span(attribute_name);
    /* Check domain type. */
    if (selection_attr.domain == selection_domain) {
      return selection_attr;
    }
    selection_attr.finish();
    attributes.remove(attribute_name);
  }
  const int domain_size = attributes.domain_size(selection_domain);
  switch (create_type) {
    case CD_PROP_BOOL:
      attributes.add(attribute_name,
                     selection_domain,
                     CD_PROP_BOOL,
                     bke::AttributeInitVArray(VArray<bool>::ForSingle(true, domain_size)));
      break;
    case CD_PROP_FLOAT:
      attributes.add(attribute_name,
                     selection_domain,
                     CD_PROP_FLOAT,
                     bke::AttributeInitVArray(VArray<float>::ForSingle(1.0f, domain_size)));
      break;
    default:
      BLI_assert_unreachable();
  }
  return attributes.lookup_for_write_span(attribute_name);
}

static Vector<bke::GSpanAttributeWriter> init_selection_writers(PointCloud &point_cloud,
                                                                bke::AttrDomain selection_domain)
{
  const eCustomDataType create_type = CD_PROP_BOOL;
  Span<StringRef> selection_attribute_names{".selection"};
  ;
  Vector<bke::GSpanAttributeWriter> writers;
  for (const int i : selection_attribute_names.index_range()) {
    writers.append(ensure_selection_attribute(
        point_cloud, selection_domain, create_type, selection_attribute_names[i]));
  };
  return writers;
}

static void finish_attribute_writers(MutableSpan<bke::GSpanAttributeWriter> attribute_writers)
{
  for (auto &attribute_writer : attribute_writers) {
    attribute_writer.finish();
  }
}

void foreach_selection_attribute_writer(
    PointCloud &point_cloud,
    bke::AttrDomain selection_domain,
    blender::FunctionRef<void(bke::GSpanAttributeWriter &selection)> fn)
{
  Vector<bke::GSpanAttributeWriter> selection_writers = init_selection_writers(point_cloud,
                                                                               selection_domain);
  for (bke::GSpanAttributeWriter &selection_writer : selection_writers) {
    fn(selection_writer);
  }
  finish_attribute_writers(selection_writers);
}

void fill_selection_true(GMutableSpan selection)
{
  if (selection.type().is<bool>()) {
    selection.typed<bool>().fill(true);
  }
  else if (selection.type().is<float>()) {
    selection.typed<float>().fill(1.0f);
  }
}

void fill_selection_false(GMutableSpan selection, const IndexMask &mask)
{
  if (selection.type().is<bool>()) {
    index_mask::masked_fill(selection.typed<bool>(), false, mask);
  }
  else if (selection.type().is<float>()) {
    index_mask::masked_fill(selection.typed<float>(), 0.0f, mask);
  }
}

void fill_selection_true(GMutableSpan selection, const IndexMask &mask)
{
  if (selection.type().is<bool>()) {
    index_mask::masked_fill(selection.typed<bool>(), true, mask);
  }
  else if (selection.type().is<float>()) {
    index_mask::masked_fill(selection.typed<float>(), 1.0f, mask);
  }
}

static void invert_selection(MutableSpan<float> selection, const IndexMask &mask)
{
  mask.foreach_index_optimized<int64_t>(
      GrainSize(2048), [&](const int64_t i) { selection[i] = 1.0f - selection[i]; });
}

static void invert_selection(GMutableSpan selection, const IndexMask &mask)
{
  if (selection.type().is<bool>()) {
    array_utils::invert_booleans(selection.typed<bool>(), mask);
  }
  else if (selection.type().is<float>()) {
    invert_selection(selection.typed<float>(), mask);
  }
}

void select_all(PointCloud &point_cloud, const IndexMask &mask, int action)
{
  UNUSED_VARS(point_cloud, mask, action);
  if (action == SEL_SELECT) {
    std::optional<IndexRange> range = mask.to_range();
    if (range.has_value() && (*range == IndexRange(point_cloud.attributes().domain_size(
                                            blender::bke::AttrDomain::Point))))
    {
      bke::MutableAttributeAccessor attributes = point_cloud.attributes_for_write();
      /* As an optimization, just remove the selection attributes when everything is selected. */
      remove_selection_attributes(attributes);
      return;
    }
  }
  foreach_selection_attribute_writer(
      point_cloud, blender::bke::AttrDomain::Point, [&](bke::GSpanAttributeWriter &selection) {
        if (action == SEL_SELECT) {
          fill_selection_true(selection.span, mask);
        }
        else if (action == SEL_DESELECT) {
          fill_selection_false(selection.span, mask);
        }
        else if (action == SEL_INVERT) {
          invert_selection(selection.span, mask);
        }
      });
}

void select_all(PointCloud &point_cloud, int action)
{
  const IndexRange selection(
      point_cloud.attributes().domain_size(blender::bke::AttrDomain::Point));
  select_all(point_cloud, selection, action);
}

}  // namespace blender::ed::point_cloud
