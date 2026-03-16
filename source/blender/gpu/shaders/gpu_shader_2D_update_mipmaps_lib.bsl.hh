
#pragma once

#include "gpu_shader_compat.hh"

namespace builtin::mipmaps {
void update_mipmaps(uint3 global_id,
                    const image2D &in_mip,
                    const image2D &out_mip1,
                    const image2D &out_mip2,
                    const image2D &out_mip3,
                    const image2D &out_mip4,
                    const image2D &out_mip5,
                    const image2D &out_mip6,
                    const image2D &out_mip7,
                    int num_levels)
{
}
}  // namespace builtin::mipmaps
