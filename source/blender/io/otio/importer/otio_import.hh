/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include <opentimelineio/serializableObject.h>
#include <opentimelineio/timeline.h>

namespace blender {

struct ReportList;
struct Scene;
struct Main;

namespace io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;
void build_blender_timeline(Main *bmain,
                            Scene *scene,
                            SerializableObject::Retainer<Timeline> &timeline,
                            ReportList *reports);

}  // namespace io::otio
}  // namespace blender
