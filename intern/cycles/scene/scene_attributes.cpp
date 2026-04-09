/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "device/device.h"

#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/stats.h"
#include "scene/tabulated_sobol.h"
#include "scene/volume.h"

#include "kernel/types.h"

#include "util/hash.h"
#include "util/log.h"
#include "util/task.h"
#include "util/time.h"


#include "scene/shader.h"


CCL_NAMESPACE_BEGIN

NODE_DEFINE(SceneAttributes)
{
  NodeType *type = NodeType::add("scene_attributes", create);

  SOCKET_FLOAT(time, "Time", 0);
  SOCKET_FLOAT(frame, "Frame", 1);

  return type;
}

SceneAttributes::SceneAttributes() : Node(get_node_type()) {}

SceneAttributes::~SceneAttributes() = default;

void SceneAttributes::device_update(Device *device, DeviceScene *dscene, Scene *scene)
{
  if (!is_modified()) {
    return;
  }
  dscene->data.scene_time.time = time; 
  dscene->data.scene_time.frame = frame;

  clear_modified();
}

bool SceneAttributes::is_modified() const
{
  return Node::is_modified();
}

void SceneAttributes::clear_modified()
{
  Node::clear_modified();
}

void SceneAttributes::device_free(Device * /*unused*/, DeviceScene *dscene, bool force_free) {}

void SceneAttributes::tag_update(Scene *scene, const uint32_t flag)
{
  if (flag == UPDATE_ALL) {
    tag_modified();

    scene->shader_manager->tag_update(scene, ShaderManager::SHADER_MODIFIED);
  }
}

CCL_NAMESPACE_END
