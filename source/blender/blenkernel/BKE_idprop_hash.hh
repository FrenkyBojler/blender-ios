/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include <xxhash.h>

namespace blender {

struct IDProperty;

namespace bke::idprop {

void hash(const IDProperty &base_prop, XXH3_state_t *hash_state);

}  // namespace bke::idprop
}  // namespace blender
