#include "libmv/simple_pipeline/pipeline.h"
#include "libmv/simple_pipeline/reconstruction.h"
#include "libmv/simple_pipeline/initialize_reconstruction.h"
#include "libmv/simple_pipeline/bundle.h"

#include "ceres/ceres.h"
#include "ceres/rotation.h"

#include "libmv/logging/logging.h"

namespace libmv {
namespace {
  template <typename T>
  Eigen::Matrix3<T> AngleAxisToRotation(const Eigen::Vector3<T>& aa_vec) {
    constexpr double EPS = 1e-12;

    T aa_norm = aa_vec.norm();
    if (aa_norm > EPS) {
      return Eigen::AngleAxis<T>(aa_norm, aa_vec.normalized())
          .toRotationMatrix();
    } else {
      Eigen::Matrix3<T> R;
      R(0, 0) = T(1);
      R(1, 0) = aa_vec[2];
      R(2, 0) = -aa_vec[1];
      R(0, 1) = -aa_vec[2];
      R(1, 1) = T(1);
      R(2, 1) = aa_vec[0];
      R(0, 2) = aa_vec[1];
      R(1, 2) = -aa_vec[0];
      R(2, 2) = T(1);
      return R;
    }
  }

  template <typename T>
  Eigen::Vector3<T> RotationToAngleAxis(const Eigen::Matrix3<T>& rot) {
    Eigen::AngleAxis<T> aa(rot);
    Eigen::Vector3<T> aa_vec = aa.angle() * aa.axis();
    return aa_vec;
  }

  class GlobalIterationCallback : public ceres::IterationCallback {
    public:
      GlobalIterationCallback(
        int max_iterations,
        const char *message,
        ProgressUpdateCallback* update_callback = NULL
      ) : max_iterations_(max_iterations), message_(message), update_callback_(update_callback) {}

      ceres::CallbackReturnType operator()(const ceres::IterationSummary& summary) final {
        update_callback_->invoke((double)summary.iteration / (double)max_iterations_, message_);

        return ceres::CallbackReturnType::SOLVER_CONTINUE;
      }

    private:
      int max_iterations_;
      const char* message_;
      ProgressUpdateCallback* update_callback_;
  };
}

struct RelativeRotationCost {
  RelativeRotationCost(const Mat3& relative_rotation)
      : relative_rotation_(relative_rotation) {}

  template <typename T>
  bool operator()(const T* const angle_axis_1,
                  const T* const angle_axis_2,
                  T* residuals) const {
    Eigen::Matrix3<T> global_rotation_1 = AngleAxisToRotation(Eigen::Vector3<T>(angle_axis_1[0], angle_axis_1[1], angle_axis_1[2]));
    Eigen::Matrix3<T> global_rotation_2 = AngleAxisToRotation(Eigen::Vector3<T>(angle_axis_2[0], angle_axis_2[1], angle_axis_2[2]));

    Eigen::Matrix3<T> error = global_rotation_2.transpose() * relative_rotation_.cast<T>() * global_rotation_1;
    Eigen::Map<Eigen::Vector3<T>> residual_map(residuals);
    residual_map = -RotationToAngleAxis(error);

    return true;
  }

  static ceres::CostFunction* Create(const Mat3& relative_rotation) {
    return (
        new ceres::AutoDiffCostFunction<RelativeRotationCost, 3, 3, 3>(
            new RelativeRotationCost(relative_rotation)));
  }

  private:
    const Mat3 relative_rotation_;
};

bool GlobalEstimateRotations(
  EuclideanReconstruction* reconstruction,
  ProgressUpdateCallback* update_callback
) {
  ceres::Problem problem;

  // setup parameters
  std::unordered_map<int, Vec3> parameters;
  parameters.reserve(reconstruction->AllCameras().size());
  for (const auto& camera : reconstruction->AllCameras()) {
    parameters[camera.image] = RotationToAngleAxis(camera.R);
    problem.AddParameterBlock(parameters[camera.image].data(), 3);
  }

  // setup relative rotation constraints
  ceres::LossFunction *loss_fn = new ceres::HuberLoss(2 * (EIGEN_PI / 180));
  for (const auto& [pair_id, image_pair] : reconstruction->AllImagePairs()) {
    ceres::CostFunction* cost_fn = RelativeRotationCost::Create(image_pair.R);
    problem.AddResidualBlock(
      cost_fn,
      loss_fn,
      parameters[image_pair.camera_id_1].data(),
      parameters[image_pair.camera_id_2].data()
    );
  }

  problem.SetParameterBlockConstant(parameters[1].data());

  // solve
  ceres::Solver::Options options;
  options.max_num_iterations = 100;
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  options.num_threads = std::thread::hardware_concurrency();

  GlobalIterationCallback callback = GlobalIterationCallback(options.max_num_iterations, "Rotation Averaging", update_callback);
  options.callbacks.push_back(&callback);

  ceres::Solver::Summary summary;
  ceres::Solve(options, &problem, &summary);

  std::cout << "\n=== Ceres Solver Summary ===" << std::endl;
  std::cout << summary.BriefReport() << std::endl;
  std::cout << "Total iterations: " << summary.iterations.size() << std::endl;
  std::cout << "Final cost: " << summary.final_cost << std::endl;

  for (auto& camera : reconstruction->AllCameras()) {
    EuclideanCamera *image = reconstruction->CameraForImage(camera.image);
    image->R = AngleAxisToRotation(parameters[camera.image]);

    // Restore the prior position (t = -Rc = R * R_ori * t_ori = R * t_ori)
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
    Eigen::Map<Eigen::Matrix<T, 3, 1>> residuals_vec(residuals);
    residuals_vec =
        translation_obs_.cast<T>() -
        scale[0] * (Eigen::Map<const Eigen::Matrix<T, 3, 1>>(position2) -
                    Eigen::Map<const Eigen::Matrix<T, 3, 1>>(position1));
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

bool GlobalPositioning(
  const Tracks& tracks,
  CameraIntrinsics* camera_intrinsics,
  EuclideanReconstruction* reconstruction,
  ProgressUpdateCallback* update_callback
) {
  ceres::Problem problem;

  ceres::LossFunction *loss_fn = new ceres::HuberLoss(1e-1);

  std::vector<double> scales;
  // for camera <-> camera constraints:
  // scales.reserve(reconstruction->AllImagePairs().size() + tracks.NumMarkers());
  scales.reserve(tracks.NumMarkers());

  // random camera start positions
  for (EuclideanCamera &camera : reconstruction->AllCameras()) {
    reconstruction->CameraForImage(camera.image)->t = 100.0 * Vec3::Random();
  }

  // camera <-> camera constraints
  // TODO: add an option to enable/disable these constraints
  // for (auto &[pair_id, pair] : reconstruction->AllImagePairs()) {
  //   const Vec3 translation = -(
  //     reconstruction->CameraForImage(pair.camera_id_2)->R.inverse()
  //       * pair.t
  //   );
  //   ceres::CostFunction* cost_fn = BATAPairwiseDirectionError::Create(translation);

  //   scales.push_back(1);
  //   double *scale = &scales.back();

  //   problem.AddResidualBlock(
  //     cost_fn,
  //     loss_fn,
  //     reconstruction->CameraForImage(pair.camera_id_1)->t.data(),
  //     reconstruction->CameraForImage(pair.camera_id_2)->t.data(),
  //     scale
  //   );

  //   problem.SetParameterLowerBound(scale, 0, 1e-5);
  // }

  // random track start positions
  for (int i = 0; i < tracks.MaxTrack(); i++) {
    if (reconstruction->PointForTrack(i) == NULL) {
      reconstruction->InsertPoint(i, 100.0 * Vec3::Random());
    }
  }
  // point <-> camera constraints
  for (int i = 0; i < tracks.MaxTrack(); i++) {
    EuclideanPoint *point = reconstruction->PointForTrack(i);
    for (Marker marker : tracks.MarkersForTrack(i)) {
      EuclideanCamera *camera = reconstruction->CameraForImage(marker.image);
      double xn, yn;
      // FIXME: inverting distortion is causing all points to go to the bottom left of the camera frame.
      // camera_intrinsics->InvertIntrinsics(marker.x, marker.y, &xn, &yn);
      xn = marker.x;
      yn = marker.y;
      const Vec3 translation = camera->R.inverse() * Vec3(xn, yn, 1.0).normalized();

      ceres::CostFunction* cost_fn = BATAPairwiseDirectionError::Create(translation);

      scales.push_back(1);
      double *scale = &scales.back();

      problem.AddResidualBlock(
        cost_fn,
        loss_fn,
        camera->t.data(),
        point->X.data(),
        scale
      );

      problem.SetParameterLowerBound(scale, 0, 1e-5);
    }
  }

  ceres::ParameterBlockOrdering* parameter_ordering = new ceres::ParameterBlockOrdering();

  for (double& scale : scales) {
    parameter_ordering->AddElementToGroup(&scale, 0);
  }

  for (EuclideanPoint point : reconstruction->AllPoints()) {
    parameter_ordering->AddElementToGroup(reconstruction->PointForTrack(point.track)->X.data(), 1);
  }

  for (EuclideanCamera camera : reconstruction->AllCameras()) {
    parameter_ordering->AddElementToGroup(reconstruction->CameraForImage(camera.image)->t.data(), 2);
  }

  // solve
  ceres::Solver::Options options;

  options.max_num_iterations = 100;
  options.function_tolerance = 1e-5;

  options.linear_solver_type = ceres::SPARSE_SCHUR;
  options.preconditioner_type = ceres::CLUSTER_TRIDIAGONAL;
  options.linear_solver_ordering.reset(parameter_ordering);

  options.num_threads = std::thread::hardware_concurrency();

  GlobalIterationCallback callback = GlobalIterationCallback(options.max_num_iterations, "Global Positioning", update_callback);
  options.callbacks.push_back(&callback);

  ceres::Solver::Summary summary;

  ceres::Solve(options, &problem, &summary);

  std::cout << summary.FullReport() << std::endl;

  for (EuclideanCamera image : reconstruction->AllCameras()) {
    EuclideanCamera *camera = reconstruction->CameraForImage(image.image);
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

  // estimate relative poses
  for (int i = 1; i <= max_image; i++) {
    for (int j = i + 1; j <= max_image; j++) {
      auto markers = tracks.MarkersForTracksInBothImages(i, j);

      // clear any previous estimation
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
    update_callback->invoke((float)i / (float)max_image, "Estimating Relative Poses");
  }

  // reset all camera transforms
  for (int i = 1; i <= max_image; i++) {
    reconstruction->InsertCamera(i, Mat3::Identity(), Vec3::Zero());
  }

  // rotation averaging
  update_callback->invoke(0.5f, "Rotation Averaging");
  if (!GlobalEstimateRotations(reconstruction, update_callback)) {
    return;
  }

  // global positioning
  update_callback->invoke(0.75f, "Global Positioning");
  if (!GlobalPositioning(tracks, camera_intrinsics, reconstruction, update_callback)) {
    return;
  }

  // bundle adjustment
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

} // namespace libmv
