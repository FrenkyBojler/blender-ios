#ifndef LIBMV_GLOBAL_PIPELINE_H
#define LIBMV_GLOBAL_PIPELINE_H
#include "libmv/simple_pipeline/pipeline.h"
#include "libmv/simple_pipeline/reconstruction.h"

namespace libmv {
  void GlobalCompleteReconstruction(const Tracks& tracks,
                                    EuclideanReconstruction* reconstruction,
                                    CameraIntrinsics* camera_intrinsics,
                                    ProgressUpdateCallback* update_callback);
}
#endif // LIBMV_GLOBAL_PIPELINE_H
