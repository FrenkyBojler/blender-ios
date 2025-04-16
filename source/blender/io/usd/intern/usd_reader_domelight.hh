/* SPDX-FileCopyrightText: 2023 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "usd.hh"
#include "usd_reader_xform.hh"

#include <pxr/usd/usdSkel/skeleton.h>

namespace blender::io::usd {

class USDDomeLightReader : public USDXformReader {

 private:
  pxr::UsdPrim light_;

 public:
  USDDomeLightReader(const pxr::UsdPrim &prim,
                    const USDImportParams &import_params,
                    const ImportSettings &settings)
      : USDXformReader(prim, import_params, settings), light_(prim)
  {
  }

  bool valid() const override
  {
    return bool(light_);
  }

  void create_world_material(Scene *scene, Main *bmain);
};

}  // namespace blender::io::usd
