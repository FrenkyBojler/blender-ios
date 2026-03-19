/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

CCL_NAMESPACE_BEGIN

using ImplicitSharingUserAddFn = void (*)(const void *);
using ImplicitSharingUserRemoveFn = void (*)(const void *);

extern ImplicitSharingUserAddFn g_implicit_sharing_user_add_fn;
extern ImplicitSharingUserRemoveFn g_implicit_sharing_user_remove_fn;

void implicit_sharing_init(ImplicitSharingUserAddFn add_fn, ImplicitSharingUserRemoveFn remove_fn);

CCL_NAMESPACE_END
