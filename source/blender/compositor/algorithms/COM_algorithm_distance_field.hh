/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "COM_context.hh"
#include "COM_result.hh"
namespace blender::compositor {
/* Produces a Distance Field from a 2D matte.
 * Ideally the matte should only have completely black and completely white pixels*/
void distance_field(Context &context,
              const Result &input,
              Result &nearest,
              Result &distance,
              bool is_signed,
              bool normalize);

void calculate_edges(Context &context,
              const Result &input,
              Result &edges,
              bool include_diagonal);

void calculate_edges_cpu(Context &context,
              const Result &input,
              Result &edges,
              bool include_diagonal);

void calculate_edges_gpu(Context &context,
              const Result &input,
              Result &edges,
              bool include_diagonal);

void calculate_nearest(Context &context,
              Result &edges,
              Result &output,
              bool normalize);

void position_to_distance(Context &context,
              const Result &matte,
              const Result &positions,
              Result &output,
              bool is_signed,
              bool normalize);

void position_to_distance_cpu(Context &context,
              const Result &matte,
              const Result &positions,
              Result &output,
              bool is_signed,
              bool normalize);

void position_to_distance_gpu(Context &context,
              const Result &matte,
              const Result &positions,
              Result &output,
              bool is_signed,
              bool normalize);

}  // namespace blender::compositor