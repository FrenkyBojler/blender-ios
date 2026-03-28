#pragma once

#include "BKE_context.hh"
#include "WM_api.hh"

namespace blender::io::otio {

void export_otio(const char *filepath);
void test_stripe_iteration(bContext *C, wmOperator *op);

} // namespace blender::io::otio
