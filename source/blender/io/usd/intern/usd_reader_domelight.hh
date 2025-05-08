/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "usd.hh"
#include "usd_reader_prim.hh"

#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/domeLight_1.h>

namespace blender::io::usd {

class USDDomeLightReader : public USDPrimReader {

 public:
  USDDomeLightReader(const pxr::UsdPrim &prim,
                     const USDImportParams &import_params,
                     const ImportSettings &settings)
      : USDPrimReader(prim, import_params, settings)
  {
  }

  bool valid() const override
  {
    return prim_.IsA<pxr::UsdLuxDomeLight>() || prim_.IsA<pxr::UsdLuxDomeLight_1>();
  }

  void create_object(Main * /*bmain*/){};
  void create_object(Scene *scene, Main *bmain);
};

}  // namespace blender::io::usd
