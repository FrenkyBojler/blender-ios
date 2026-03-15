// Copyright (c) 2026 libmv authors.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include "libmv/global_pipeline/pipeline.h"

#include "libmv/simple_pipeline/bundle.h"
#include "libmv/simple_pipeline/initialize_reconstruction.h"
#include "libmv/simple_pipeline/pipeline.h"
#include "libmv/simple_pipeline/reconstruction.h"

#include "ceres/ceres.h"
#include "ceres/rotation.h"

#include "libmv/logging/logging.h"

namespace libmv {
namespace {
class GlobalIterationCallback : public ceres::IterationCallback {
 public:
  GlobalIterationCallback(int max_iterations,
                          const char* message,
                          ProgressUpdateCallback* update_callback = NULL)
      : max_iterations_(max_iterations),
        message_(message),
        update_callback_(update_callback) {}

  ceres::CallbackReturnType operator()(
      const ceres::IterationSummary& summary) final {
    update_callback_->invoke(
        (double)summary.iteration / (double)max_iterations_, message_);

    return ceres::CallbackReturnType::SOLVER_CONTINUE;
  }

 private:
  int max_iterations_;
  const char* message_;
  ProgressUpdateCallback* update_callback_;
};
}  // namespace

struct RelativeRotationCost {
  RelativeRotationCost(const Mat3& relative_rotation)
      : relative_rotation_(relative_rotation) {}

  template <typename T>
  bool operator()(const T* const angle_axis_1,
                  const T* const angle_axis_2,
                  T* residuals) const {
    typedef Eigen::Matrix<T, 3, 3> Mat3;
    Mat3 global_rotation_1;
    ceres::AngleAxisToRotationMatrix(
      angle_axis_1,
      global_rotation_1.data());
    Mat3 global_rotation_2;
    ceres::AngleAxisToRotationMatrix(
      angle_axis_2,
      global_rotation_2.data());

    Mat3 error = global_rotation_2.transpose() *
                              relative_rotation_.cast<T>() * global_rotation_1;
    ceres::RotationMatrixToAngleAxis(error.data(), residuals);
    residuals[0] = -residuals[0];
    residuals[1] = -residuals[1];
    residuals[2] = -residuals[2];

    return true;
  }

  static ceres::CostFunction* Create(const Mat3& relative_rotation) {
    return (new ceres::AutoDiffCostFunction<RelativeRotationCost, 3, 3, 3>(
        new RelativeRotationCost(relative_rotation)));
  }

 private:
  const Mat3 relative_rotation_;
};

bool GlobalEstimateRotations(EuclideanReconstruction* reconstruction,
                             ProgressUpdateCallback* update_callback) {
  ceres::Problem problem;

  // Setup parameters.
  std::unordered_map<int, Vec3> parameters;
  parameters.reserve(reconstruction->AllCameras().size());
  for (const EuclideanCamera& camera : reconstruction->AllCameras()) {
    parameters[camera.image] = RotationToAngleAxis(camera.R);
    problem.AddParameterBlock(parameters[camera.image].data(), 3);
  }

  std::unordered_map<int, int> camera_edge_counts;
  int most_connected_camera_id = -1;
  int most_connected_camera_edges = 0;

  ceres::LossFunction* loss_fn = new ceres::HuberLoss(2 * (EIGEN_PI / 180));
  for (const auto& [pair_id, image_pair] : reconstruction->AllImagePairs()) {
    if (parameters.count(image_pair.camera_id_1) == 0 ||
        parameters.count(image_pair.camera_id_2) == 0) {
      continue;
    }

    // Find the most connected camera.
    int camera_id_1_edges = ++camera_edge_counts[image_pair.camera_id_1];
    if (camera_id_1_edges > most_connected_camera_edges) {
      most_connected_camera_id = image_pair.camera_id_1;
      most_connected_camera_edges = camera_id_1_edges;
    }

    int camera_id_2_edges = ++camera_edge_counts[image_pair.camera_id_2];
    if (camera_id_2_edges > most_connected_camera_edges) {
      most_connected_camera_id = image_pair.camera_id_2;
      most_connected_camera_edges = camera_id_2_edges;
    }

    // Setup relative rotation constraints.
    ceres::CostFunction* cost_fn = RelativeRotationCost::Create(image_pair.R);
    problem.AddResidualBlock(cost_fn,
                             loss_fn,
                             parameters[image_pair.camera_id_1].data(),
                             parameters[image_pair.camera_id_2].data());
  }

  // Make the most connected camera constant.
  problem.SetParameterBlockConstant(parameters[most_connected_camera_id].data());

  // Solve.
  ceres::Solver::Options options;
  options.max_num_iterations = 100;
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  options.num_threads = std::thread::hardware_concurrency();

  GlobalIterationCallback callback = GlobalIterationCallback(
      options.max_num_iterations, "Rotation Averaging", update_callback);
  options.callbacks.push_back(&callback);

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);

  for (auto& camera : reconstruction->AllCameras()) {
    EuclideanCamera* image = reconstruction->CameraForImage(camera.image);
    if (parameters[camera.image].norm() < 1e-12) {
      image->R = Mat3::Identity();
    } else {
      image->R = RotationRodrigues(parameters[camera.image]);
    }

    // Restore the prior position (t = -Rc = R * R_ori * t_ori = R * t_ori).
    image->t = (image->R * image->t);
  }

  return summary.IsSolutionUsable();
}

// ----------------------------------------
// BATAPairwiseDirectionError
// ----------------------------------------
// Computes the error between a translation direction and the direction formed
// from two positions such that t_ij - scale * (c_j - c_i) is minimized.
//
// (from colmap/glomap, BSD-3-Clause)
struct BATAPairwiseDirectionError {
  BATAPairwiseDirectionError(const Eigen::Vector3d& translation_obs)
      : translation_obs_(translation_obs) {}

  // The error is given by the position error described above.
  template <typename T>
  bool operator()(const T* position1,
                  const T* position2,
                  const T* scale,
                  T* residuals) const {
    for (int i = 0; i < 3; ++i) {
      residuals[i] = T(translation_obs_[i]) -
                     scale[0] * (position2[i] - position1[i]);
    }
    // Eigen::Map<Eigen::Matrix<T, 3, 1>> residuals_vec(residuals);
    // residuals_vec =
    //     translation_obs_.cast<T>() -
    //     scale[0] * (Eigen::Map<const Eigen::Matrix<T, 3, 1>>(position2) -
    //                 Eigen::Map<const Eigen::Matrix<T, 3, 1>>(position1));
    return true;
  }

  static ceres::CostFunction* Create(const Eigen::Vector3d& translation_obs) {
    return (
        new ceres::AutoDiffCostFunction<BATAPairwiseDirectionError, 3, 3, 3, 1>(
            new BATAPairwiseDirectionError(translation_obs)));
  }

  // TODO: add covariance
  const Eigen::Vector3d translation_obs_;
};

bool GlobalPositioning(const Tracks& tracks,
                       CameraIntrinsics* camera_intrinsics,
                       EuclideanReconstruction* reconstruction,
                       ProgressUpdateCallback* update_callback) {
  ceres::Problem problem;

  ceres::LossFunction* loss_fn = new ceres::HuberLoss(1e-1);

  std::vector<double> scales;
  scales.reserve(tracks.NumMarkers());

  // Randomize camera start positions.
  for (EuclideanCamera& camera : reconstruction->AllCameras()) {
    reconstruction->CameraForImage(camera.image)->t = 100.0 * Vec3::Random();
  }

  // Randomize track start positions.
  for (int i = 0; i < tracks.MaxTrack(); i++) {
    if (reconstruction->PointForTrack(i) == NULL) {
      reconstruction->InsertPoint(i, 100.0 * Vec3::Random());
    }
  }
  // Add point <-> camera constraints.
  for (int i = 0; i < tracks.MaxTrack(); i++) {
    EuclideanPoint* point = reconstruction->PointForTrack(i);
    for (Marker marker : tracks.MarkersForTrack(i)) {
      EuclideanCamera* camera = reconstruction->CameraForImage(marker.image);
      double xn, yn;
      // FIXME: Inverting distortion is causing all points to go to the bottom
      // left of the camera frame.
      // camera_intrinsics->InvertIntrinsics(marker.x, marker.y, &xn, &yn);
      xn = marker.x;
      yn = marker.y;
      const Vec3 translation =
          camera->R.transpose() * Vec3(xn, yn, 1.0).normalized();

      ceres::CostFunction* cost_fn =
          BATAPairwiseDirectionError::Create(translation);

      scales.push_back(1);
      double* scale = &scales.back();

      problem.AddResidualBlock(
          cost_fn, loss_fn, camera->t.data(), point->X.data(), scale);

      problem.SetParameterLowerBound(scale, 0, 1e-5);
    }
  }

  ceres::ParameterBlockOrdering* parameter_ordering =
      new ceres::ParameterBlockOrdering();

  for (double& scale : scales) {
    parameter_ordering->AddElementToGroup(&scale, 0);
  }

  for (EuclideanPoint point : reconstruction->AllPoints()) {
    parameter_ordering->AddElementToGroup(
        reconstruction->PointForTrack(point.track)->X.data(), 1);
  }

  for (EuclideanCamera camera : reconstruction->AllCameras()) {
    parameter_ordering->AddElementToGroup(
        reconstruction->CameraForImage(camera.image)->t.data(), 2);
  }

  // Solve.
  ceres::Solver::Options options;

  options.max_num_iterations = 100;
  options.function_tolerance = 1e-5;

  options.linear_solver_type = ceres::SPARSE_SCHUR;
  options.preconditioner_type = ceres::CLUSTER_TRIDIAGONAL;
  options.linear_solver_ordering.reset(parameter_ordering);

  options.num_threads = std::thread::hardware_concurrency();

  GlobalIterationCallback callback = GlobalIterationCallback(
      options.max_num_iterations, "Global Positioning", update_callback);
  options.callbacks.push_back(&callback);

  ceres::Solver::Summary summary;

  ceres::Solve(options, &problem, &summary);

  for (EuclideanCamera image : reconstruction->AllCameras()) {
    EuclideanCamera* camera = reconstruction->CameraForImage(image.image);
    camera->t = -(camera->R * camera->t);
  }

  return summary.IsSolutionUsable();
}

void InternalCompleteReconstruction(
    const Tracks& tracks,
    EuclideanReconstruction* reconstruction,
    CameraIntrinsics* camera_intrinsics,
    ProgressUpdateCallback* update_callback = NULL) {
  int max_image = tracks.MaxImage();

  // Estimate relative poses.
  for (int i = 1; i <= max_image; i++) {
    for (int j = i + 1; j <= max_image; j++) {
      auto markers = tracks.MarkersForTracksInBothImages(i, j);

      // Clear any previous estimation.
      reconstruction->RemoveCamera(i);
      reconstruction->RemoveCamera(j);

      if (!EuclideanReconstructTwoFrames(markers, reconstruction)) {
        continue;
      }
      auto camera_id_2 = reconstruction->CameraForImage(j);

      ImagePair relative_pose;
      relative_pose.R = camera_id_2->R;
      relative_pose.t = camera_id_2->t;
      relative_pose.camera_id_1 = i;
      relative_pose.camera_id_2 = j;

      reconstruction->InsertImagePair(relative_pose);
    }
    update_callback->invoke((float)i / (float)max_image,
                            "Estimating Relative Poses");
  }

  // Reset all camera transforms.
  for (int i = 1; i <= max_image; i++) {
    reconstruction->InsertCamera(i, Mat3::Identity(), Vec3::Zero());
  }

  // Rotation averaging step.
  update_callback->invoke(0.5f, "Rotation Averaging");
  if (!GlobalEstimateRotations(reconstruction, update_callback)) {
    return;
  }

  // Global positioning step.
  update_callback->invoke(0.75f, "Global Positioning");
  if (!GlobalPositioning(
          tracks, camera_intrinsics, reconstruction, update_callback)) {
    return;
  }

  // Bundle adjustment step.
  update_callback->invoke(0.9f, "Bundle adjustment");
  EuclideanBundle(tracks, reconstruction);

  update_callback->invoke(1.0f, "Finalizing Reconstruction");
}

void GlobalCompleteReconstruction(const Tracks& tracks,
                                  EuclideanReconstruction* reconstruction,
                                  CameraIntrinsics* camera_intrinsics,
                                  ProgressUpdateCallback* update_callback) {
  InternalCompleteReconstruction(
      tracks, reconstruction, camera_intrinsics, update_callback);
}

}  // namespace libmv
