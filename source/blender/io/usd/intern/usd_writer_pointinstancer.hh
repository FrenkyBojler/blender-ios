/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_math_quaternion_types.hh"
#include "usd_attribute_utils.hh"
#include "usd_writer_abstract.hh"
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <vector>

struct USDExporterContext;

namespace blender::io::usd {

class USDPointInstancerWriter final : public USDAbstractWriter {
 public:
  USDPointInstancerWriter(const USDExporterContext &ctx,
                          std::set<std::pair<pxr::SdfPath, Object *>> &paths);
  ~USDPointInstancerWriter() final = default;
  std::set<std::pair<pxr::SdfPath, Object *>> proto_paths;
  const std::string proto_name = "Prototype";

  void set_base_writer(std::unique_ptr<USDAbstractWriter> writer);

 protected:
  virtual void do_write(HierarchyContext &context) override;

 private:
  std::unique_ptr<USDAbstractWriter> base_writer_;

  void write_attribute_data(const bke::AttributeIter &attr,
                            const pxr::UsdGeomPointInstancer &usd_instancer,
                            const pxr::UsdTimeCode timecode);

  void handle_collection_prototypes(
      const pxr::UsdGeomPointInstancer &usd_instancer,
      const pxr::UsdTimeCode timecode,
      int instance_num,
      const std::vector<std::pair<int, int>> &collection_instance_object_count_map);
};

}  // namespace blender::io::usd
