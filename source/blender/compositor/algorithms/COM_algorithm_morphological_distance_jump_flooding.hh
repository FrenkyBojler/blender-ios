/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_context.hh"
#include "COM_result.hh"

namespace blender::compositor {

/* Possible morphological operators to apply. */
enum class MorphologicalOperatorType : uint8_t {
  Erode,
  Dilate,
};

/* Possible distance metrics to use for the structuring element. */
enum class MorphologicalDistanceMetric : uint8_t {
  Euclidean,
};

/* Apply a morphological operator with a structuring element that is active if the distance to its
 * center is less than the given radius, if the distance metric is euclidean, that would make the
 * structuring element circular.
 *
 * This is implemented internally using a special version of the jump flooding algorithm, meaning
 * it executes in O(log2(radius)) per pixel, which makes it significantly faster than convolution
 * like algorithms for large radius parameters. See the discussion on the jump_flooding algorithm
 * fir more information. */
void morphological_distance_jump_flooding(
    Context &context,
    const Result &input,
    Result &output,
    const int radius,
    const MorphologicalOperatorType operator_type,
    const MorphologicalDistanceMetric distance_metric = MorphologicalDistanceMetric::Euclidean);

}  // namespace blender::compositor
