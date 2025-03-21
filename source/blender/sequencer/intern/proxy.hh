/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

struct ImBuf;
struct MovieReader;
struct SeqRenderData;
struct Strip;
struct anim;

namespace blender::seq {

#define PROXY_MAXFILE (2 * FILE_MAXDIR + FILE_MAXFILE)
ImBuf *proxy_fetch(const RenderData *context, Strip *strip, int timeline_frame);
bool proxy_custom_file_filepath_get(Strip *strip, char *filepath, int view_id);
void proxy_free(Strip *strip);
void proxy_index_dir_set(MovieReader *anim, const char *base_dir);

}  // namespace blender::seq
