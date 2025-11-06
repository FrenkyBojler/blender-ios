/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BKE_curves.hh"

namespace blender::geometry::lsystem {

struct LSystemParams {
  std::string axiom;
  Vector<std::string> rules;
  float generations = 0.0f;
  float angle = DEG2RAD(90.0f);
};

std::variant<bke::CurvesGeometry, std::string> lsystem_to_curves(LSystemParams &params);

};  // namespace blender::geometry::lsystem
