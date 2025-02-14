/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <string>
#include <variant>

#include "BKE_main.hh"

#include "BLI_map.hh"
#include "BLI_vector.hh"

#include "DNA_ID.h"

namespace blender::bke::id_hash {

struct ValidDeepHashes {
  Map<const ID *, IDHash> hashes;
};

struct MissingBlendFiles {
  Vector<std::string> paths;
};

using IDHashResult = std::variant<ValidDeepHashes, MissingBlendFiles>;

/**
 * Compute a hash of the given ID, including all its dependencies.
 * This needs access to the original .blend files that the linked data-blocks come from to be able
 * to compute their hash.
 */
IDHashResult compute_linked_id_deep_hashes(const Main &bmain, Span<const ID *> root_ids);

std::string id_hash_to_hex(const IDHash &hash);

}  // namespace blender::bke::id_hash
