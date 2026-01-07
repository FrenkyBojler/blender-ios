/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 * \brief Generic memory manipulation API.
 */

#include <cstddef>

namespace blender {

/* it may be defined already */
bool BLI_memory_is_zero(const void *arr, size_t size);

}  // namespace blender
