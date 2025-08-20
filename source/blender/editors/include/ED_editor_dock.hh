#pragma once

#include "DNA_space_enums.h"

struct Scene;
struct ScrArea;

namespace blender::ed::editor_dock {

void add_docked_space(ScrArea *area, const eSpace_Type type, const Scene *scene);

}
