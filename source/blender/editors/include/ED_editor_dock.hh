#pragma once

#include "DNA_space_enums.h"

struct ScrArea;

namespace blender::ed::editor_dock {

void add_docked_space(ScrArea *area, eSpace_Type type);

}
