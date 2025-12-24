// Copyright (c) 2011 libmv authors.
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

#include "libmv/simple_pipeline/reconstruction.h"
#include "libmv/logging/logging.h"
#include "libmv/numeric/numeric.h"

namespace libmv {

EuclideanReconstruction::EuclideanReconstruction() {
}
EuclideanReconstruction::EuclideanReconstruction(
    const EuclideanReconstruction& other) {
  image_to_cameras_map_ = other.image_to_cameras_map_;
  points_ = other.points_;
}

EuclideanReconstruction& EuclideanReconstruction::operator=(
    const EuclideanReconstruction& other) {
  if (&other != this) {
    image_to_cameras_map_ = other.image_to_cameras_map_;
    points_ = other.points_;
  }
  return *this;
}

void EuclideanReconstruction::InsertCamera(int image,
                                           const Mat3& R,
                                           const Vec3& t) {
  LG << "InsertCamera " << image << ":\nR:\n" << R << "\nt:\n" << t;

  EuclideanCamera camera;
  camera.image = image;
  camera.R = R;
  camera.t = t;

  image_to_cameras_map_.insert(make_pair(image, camera));
}

void EuclideanReconstruction::RemoveCamera(int image) {
  image_to_cameras_map_.erase(image);
}

void EuclideanReconstruction::InsertPoint(int track, const Vec3& X) {
  LG << "InsertPoint " << track << ":\n" << X;
  if (track >= points_.size()) {
    points_.resize(track + 1);
  }
  points_[track].track = track;
  points_[track].X = X;
}

EuclideanCamera* EuclideanReconstruction::CameraForImage(int image) {
  return const_cast<EuclideanCamera*>(
      static_cast<const EuclideanReconstruction*>(this)->CameraForImage(image));
}

const EuclideanCamera* EuclideanReconstruction::CameraForImage(
    int image) const {
  ImageToCameraMap::const_iterator it = image_to_cameras_map_.find(image);
  if (it == image_to_cameras_map_.end()) {
    return NULL;
  }
  return &it->second;
}

vector<EuclideanCamera> EuclideanReconstruction::AllCameras() const {
  vector<EuclideanCamera> cameras;
  for (const ImageToCameraMap::value_type& image_and_camera :
       image_to_cameras_map_) {
    cameras.push_back(image_and_camera.second);
  }
  return cameras;
}

EuclideanPoint* EuclideanReconstruction::PointForTrack(int track) {
  return const_cast<EuclideanPoint*>(
      static_cast<const EuclideanReconstruction*>(this)->PointForTrack(track));
}

const EuclideanPoint* EuclideanReconstruction::PointForTrack(int track) const {
  if (track < 0 || track >= points_.size()) {
    return NULL;
  }
  const EuclideanPoint* point = &points_[track];
  if (point->track == -1) {
    return NULL;
  }
  return point;
}

vector<EuclideanPoint> EuclideanReconstruction::AllPoints() const {
  vector<EuclideanPoint> points;
  for (int i = 0; i < points_.size(); ++i) {
    if (points_[i].track != -1) {
      points.push_back(points_[i]);
    }
  }
  return points;
}

map<uint64_t, ImagePair> EuclideanReconstruction::AllImagePairs() const {
  return image_pairs_;
}

/// Add an image pair to the reconstruction.
void EuclideanReconstruction::InsertImagePair(ImagePair pair) {
  uint64_t id;
  if (pair.camera_id_1 > pair.camera_id_2) {
    id = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) *
             pair.camera_id_2 +
         pair.camera_id_1;
  } else {
    id = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) *
             pair.camera_id_1 +
         pair.camera_id_2;
  }
  image_pairs_[id] = pair;
}

ImagePair* EuclideanReconstruction::ImagePairForImages(int camera_id_1,
                                                       int camera_id_2) {
  return const_cast<ImagePair*>(
      static_cast<const EuclideanReconstruction*>(this)->ImagePairForImages(
          camera_id_1, camera_id_2));
}

const ImagePair* EuclideanReconstruction::ImagePairForImages(
    int camera_id_1, int camera_id_2) const {
  uint64_t id;
  if (camera_id_1 > camera_id_2) {
    id = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) *
             camera_id_2 +
         camera_id_1;
  } else {
    id = static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) *
             camera_id_1 +
         camera_id_2;
  }
  if (image_pairs_.find(id) == image_pairs_.end()) {
    return NULL;
  }
  const ImagePair* pair = &image_pairs_.at(id);
  return pair;
}

void EuclideanReconstruction::RemoveImagePair(uint64_t pair_id) {
  image_pairs_.erase(pair_id);
}

void ProjectiveReconstruction::InsertCamera(int image, const Mat34& P) {
  LG << "InsertCamera " << image << ":\nP:\n" << P;

  ProjectiveCamera camera;
  camera.image = image;
  camera.P = P;

  image_to_cameras_map_.insert(make_pair(image, camera));
}

void ProjectiveReconstruction::InsertPoint(int track, const Vec4& X) {
  LG << "InsertPoint " << track << ":\n" << X;
  if (track >= points_.size()) {
    points_.resize(track + 1);
  }
  points_[track].track = track;
  points_[track].X = X;
}

ProjectiveCamera* ProjectiveReconstruction::CameraForImage(int image) {
  return const_cast<ProjectiveCamera*>(
      static_cast<const ProjectiveReconstruction*>(this)->CameraForImage(
          image));
}

const ProjectiveCamera* ProjectiveReconstruction::CameraForImage(
    int image) const {
  ImageToCameraMap::const_iterator it = image_to_cameras_map_.find(image);
  if (it == image_to_cameras_map_.end()) {
    return NULL;
  }
  return &it->second;
}

vector<ProjectiveCamera> ProjectiveReconstruction::AllCameras() const {
  vector<ProjectiveCamera> cameras;
  for (const ImageToCameraMap::value_type& image_and_camera :
       image_to_cameras_map_) {
    cameras.push_back(image_and_camera.second);
  }
  return cameras;
}

ProjectivePoint* ProjectiveReconstruction::PointForTrack(int track) {
  return const_cast<ProjectivePoint*>(
      static_cast<const ProjectiveReconstruction*>(this)->PointForTrack(track));
}

const ProjectivePoint* ProjectiveReconstruction::PointForTrack(
    int track) const {
  if (track < 0 || track >= points_.size()) {
    return NULL;
  }
  const ProjectivePoint* point = &points_[track];
  if (point->track == -1) {
    return NULL;
  }
  return point;
}

vector<ProjectivePoint> ProjectiveReconstruction::AllPoints() const {
  vector<ProjectivePoint> points;
  for (int i = 0; i < points_.size(); ++i) {
    if (points_[i].track != -1) {
      points.push_back(points_[i]);
    }
  }
  return points;
}

}  // namespace libmv
