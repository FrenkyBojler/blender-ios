#pragma once

struct wmOperatorType;

namespace blender::io::otio {

void register_exporter();
void OTIO_OT_export(wmOperatorType *ot);

} // namespace blender::io::otio
