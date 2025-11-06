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
  float step_size = 1.0f;
  float step_size_scale = 0.5f;
  float angle_scale = 0.5f;
};

std::variant<bke::CurvesGeometry, std::string> lsystem_to_curves(LSystemParams &params);

};  // namespace blender::geometry::lsystem
