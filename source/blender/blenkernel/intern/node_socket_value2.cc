/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value2.hh"

namespace blender::bke {

namespace detail {

template<>
void SocketValueVariantTypeInfo::convert_to_fn<int>(const CPPType &dst_type,
                                                    SocketValueVariantAny &value)
{
  UNUSED_VARS(dst_type, value);
}

template<>
bool SocketValueVariantTypeInfo::is_interpretable_as_fn<int>(const CPPType &dst_type,
                                                             const SocketValueVariantAny &value)
{
  UNUSED_VARS(dst_type, value);
  return false;
}

}  // namespace detail

}  // namespace blender::bke
