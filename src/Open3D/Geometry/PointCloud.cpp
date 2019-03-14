// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
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
// ----------------------------------------------------------------------------

#include "PointCloud.h"

#include <Eigen/Dense>
#include <Open3D/Utility/Console.h>
#include <Open3D/Geometry/KDTreeFlann.h>

#include <cuda.h>
#include <cuda_runtime.h>

#include "Open3D/Types/Matrix3d.h"

#include "Open3D/Utility/CUDA.cuh"

#include <iostream>
#include <iomanip>
using namespace std;

extern void cumulantGPU(double *const d_points,
                        const int &nrPoints,
                        double *const d_cumulants);

namespace open3d {
namespace geometry {

void PointCloud::Clear() {
    points_.clear();
    normals_.clear();
    colors_.clear();
}

bool PointCloud::IsEmpty() const { return !HasPoints(); }

Eigen::Vector3d PointCloud::GetMinBound() const {
    if (!HasPoints()) {
        return Eigen::Vector3d(0.0, 0.0, 0.0);
    }
    auto itr_x = std::min_element(
            points_.begin(), points_.end(),
            [](const Eigen::Vector3d &a, const Eigen::Vector3d &b) {
                return a(0) < b(0);
            });
    auto itr_y = std::min_element(
            points_.begin(), points_.end(),
            [](const Eigen::Vector3d &a, const Eigen::Vector3d &b) {
                return a(1) < b(1);
            });
    auto itr_z = std::min_element(
            points_.begin(), points_.end(),
            [](const Eigen::Vector3d &a, const Eigen::Vector3d &b) {
                return a(2) < b(2);
            });
    return Eigen::Vector3d((*itr_x)(0), (*itr_y)(1), (*itr_z)(2));
}

Eigen::Vector3d PointCloud::GetMaxBound() const {
    if (!HasPoints()) {
        return Eigen::Vector3d(0.0, 0.0, 0.0);
    }
    auto itr_x = std::max_element(
            points_.begin(), points_.end(),
            [](const Eigen::Vector3d &a, const Eigen::Vector3d &b) {
                return a(0) < b(0);
            });
    auto itr_y = std::max_element(
            points_.begin(), points_.end(),
            [](const Eigen::Vector3d &a, const Eigen::Vector3d &b) {
                return a(1) < b(1);
            });
    auto itr_z = std::max_element(
            points_.begin(), points_.end(),
            [](const Eigen::Vector3d &a, const Eigen::Vector3d &b) {
                return a(2) < b(2);
            });
    return Eigen::Vector3d((*itr_x)(0), (*itr_y)(1), (*itr_z)(2));
}

void PointCloud::Transform(const Eigen::Matrix4d &transformation) {
    for (auto &point : points_) {
        Eigen::Vector4d new_point =
                transformation *
                Eigen::Vector4d(point(0), point(1), point(2), 1.0);
        point = new_point.block<3, 1>(0, 0);
    }
    for (auto &normal : normals_) {
        Eigen::Vector4d new_normal =
                transformation *
                Eigen::Vector4d(normal(0), normal(1), normal(2), 0.0);
        normal = new_normal.block<3, 1>(0, 0);
    }
}

PointCloud &PointCloud::operator+=(const PointCloud &cloud) {
    // We do not use std::vector::insert to combine std::vector because it will
    // crash if the pointcloud is added to itself.
    if (cloud.IsEmpty()) return (*this);
    size_t old_vert_num = points_.size();
    size_t add_vert_num = cloud.points_.size();
    size_t new_vert_num = old_vert_num + add_vert_num;
    if ((!HasPoints() || HasNormals()) && cloud.HasNormals()) {
        normals_.resize(new_vert_num);
        for (size_t i = 0; i < add_vert_num; i++)
            normals_[old_vert_num + i] = cloud.normals_[i];
    } else {
        normals_.clear();
    }
    if ((!HasPoints() || HasColors()) && cloud.HasColors()) {
        colors_.resize(new_vert_num);
        for (size_t i = 0; i < add_vert_num; i++)
            colors_[old_vert_num + i] = cloud.colors_[i];
    } else {
        colors_.clear();
    }
    points_.resize(new_vert_num);
    for (size_t i = 0; i < add_vert_num; i++)
        points_[old_vert_num + i] = cloud.points_[i];
    return (*this);
}

PointCloud PointCloud::operator+(const PointCloud &cloud) const {
    return (PointCloud(*this) += cloud);
}

std::vector<double> ComputePointCloudToPointCloudDistance(
        const PointCloud &source, const PointCloud &target) {
    std::vector<double> distances(source.points_.size());
    KDTreeFlann kdtree;
    kdtree.SetGeometry(target);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < (int)source.points_.size(); i++) {
        std::vector<int> indices(1);
        std::vector<double> dists(1);
        if (kdtree.SearchKNN(source.points_[i], 1, indices, dists) == 0) {
            utility::PrintDebug(
                    "[ComputePointCloudToPointCloudDistance] Found a point "
                    "without neighbors.\n");
            distances[i] = 0.0;
        } else {
            distances[i] = std::sqrt(dists[0]);
        }
    }
    return distances;
}

std::tuple<Eigen::Vector3d, Eigen::Matrix3d> ComputePointCloudMeanAndCovariance(
        const PointCloud &input) {
    if (input.IsEmpty()) {
        return std::make_tuple(Eigen::Vector3d::Zero(),
                               Eigen::Matrix3d::Identity());
    }
    Eigen::Matrix<double, 9, 1> cumulants;
    cumulants.setZero();
    for (const auto &point : input.points_) {
        cumulants(0) += point(0);
        cumulants(1) += point(1);
        cumulants(2) += point(2);
        cumulants(3) += point(0) * point(0);
        cumulants(4) += point(0) * point(1);
        cumulants(5) += point(0) * point(2);
        cumulants(6) += point(1) * point(1);
        cumulants(7) += point(1) * point(2);
        cumulants(8) += point(2) * point(2);
    }
    cumulants /= (double)input.points_.size();
    Eigen::Vector3d mean;
    Eigen::Matrix3d covariance;
    mean(0) = cumulants(0);
    mean(1) = cumulants(1);
    mean(2) = cumulants(2);
    covariance(0, 0) = cumulants(3) - cumulants(0) * cumulants(0);
    covariance(1, 1) = cumulants(6) - cumulants(1) * cumulants(1);
    covariance(2, 2) = cumulants(8) - cumulants(2) * cumulants(2);
    covariance(0, 1) = cumulants(4) - cumulants(0) * cumulants(1);
    covariance(1, 0) = covariance(0, 1);
    covariance(0, 2) = cumulants(5) - cumulants(0) * cumulants(2);
    covariance(2, 0) = covariance(0, 2);
    covariance(1, 2) = cumulants(7) - cumulants(1) * cumulants(2);
    covariance(2, 1) = covariance(1, 2);
    return std::make_tuple(mean, covariance);
}

std::tuple<Eigen::Vector3d, Eigen::Matrix3d>
ComputePointCloudMeanAndCovarianceCUDA(PointCloud &input) {
    if (input.IsEmpty()) {
        return std::make_tuple(Eigen::Vector3d::Zero(),
                               Eigen::Matrix3d::Identity());
    }

    int devID = 0;
    cudaSetDevice(devID);

    // Error code to check return values for CUDA calls
    cudaError_t status = cudaSuccess;

    // nr. of dimensions
    int nrPoints = input.points_.size();

    int inputSize = nrPoints * Vector3d::SIZE;
    int outputSize = nrPoints * Matrix3d::SIZE;

    // host memory
    double *h_points = NULL;
    double *hCumulants = NULL;

    // device memory
    double *d_cumulants = NULL;

    h_points = (double *)input.points_.data();
    if (!AlocateHstMemory(&hCumulants, outputSize, "hCumulants")) exit(1);
    input.UpdateDeviceMemory();
    if (!AlocateDevMemory(&d_cumulants, outputSize, "d_cumulants")) exit(1);

    cumulantGPU(input.d_points_, nrPoints, d_cumulants);
    status = cudaGetLastError();

    if (cudaSuccess != status) {
        cout << "status: " << cudaGetErrorString(status) << endl;
        cout << "Failed to launch cuda kernel" << endl;
        exit(1);
    }

    // Copy results to the host
    CopyDev2HstMemory(d_cumulants, hCumulants, outputSize);

    Matrix3d *cumulants = (Matrix3d *)hCumulants;
    Matrix3d cumulant = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < nrPoints; i++) {
        cumulant[0][0] += (double)cumulants[i][0][0];
        cumulant[0][1] += (double)cumulants[i][0][1];
        cumulant[0][2] += (double)cumulants[i][0][2];
        cumulant[1][0] += (double)cumulants[i][1][0];
        cumulant[1][1] += (double)cumulants[i][1][1];
        cumulant[1][2] += (double)cumulants[i][1][2];
        cumulant[2][0] += (double)cumulants[i][2][0];
        cumulant[2][1] += (double)cumulants[i][2][1];
        cumulant[2][2] += (double)cumulants[i][2][2];
    }

    Eigen::Vector3d mean;
    Eigen::Matrix3d covariance;

    mean(0) = cumulant[0][0];
    mean(1) = cumulant[0][1];
    mean(2) = cumulant[0][2];

    covariance(0, 0) = cumulant[1][0] - cumulant[0][0] * cumulant[0][0];
    covariance(1, 1) = cumulant[2][0] - cumulant[0][1] * cumulant[0][1];
    covariance(2, 2) = cumulant[2][2] - cumulant[0][2] * cumulant[0][2];
    covariance(0, 1) = cumulant[1][1] - cumulant[0][0] * cumulant[0][1];
    covariance(1, 0) = covariance(0, 1);
    covariance(0, 2) = cumulant[1][2] - cumulant[0][0] * cumulant[0][2];
    covariance(2, 0) = covariance(0, 2);
    covariance(1, 2) = cumulant[2][1] - cumulant[0][1] * cumulant[0][2];
    covariance(2, 1) = covariance(1, 2);

    // Free device global memory
    freeDev(&d_cumulants, "d_cumulants");

    // Free host memory
    free(hCumulants);

    return std::make_tuple(mean, covariance);
}

std::vector<double> ComputePointCloudMahalanobisDistance(
        const PointCloud &input) {
    std::vector<double> mahalanobis(input.points_.size());
    Eigen::Vector3d mean;
    Eigen::Matrix3d covariance;
    std::tie(mean, covariance) = ComputePointCloudMeanAndCovariance(input);
    Eigen::Matrix3d cov_inv = covariance.inverse();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < (int)input.points_.size(); i++) {
        Eigen::Vector3d p = input.points_[i] - mean;
        mahalanobis[i] = std::sqrt(p.transpose() * cov_inv * p);
    }
    return mahalanobis;
}

std::vector<double> ComputePointCloudNearestNeighborDistance(
        const PointCloud &input) {
    std::vector<double> nn_dis(input.points_.size());
    KDTreeFlann kdtree(input);
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < (int)input.points_.size(); i++) {
        std::vector<int> indices(2);
        std::vector<double> dists(2);
        if (kdtree.SearchKNN(input.points_[i], 2, indices, dists) <= 1) {
            utility::PrintDebug(
                    "[ComputePointCloudNearestNeighborDistance] Found a point "
                    "without neighbors.\n");
            nn_dis[i] = 0.0;
        } else {
            nn_dis[i] = std::sqrt(dists[1]);
        }
    }
    return nn_dis;
}

// update the device memory on demand
bool PointCloud::UpdateDeviceMemory(double **d_data,
                                    const vector<Eigen::Vector3d> &data) {
    cudaError_t status = cudaSuccess;

    if (*d_data != NULL) {
        if (cudaSuccess != cudaFree(*d_data)) return false;
        *d_data = NULL;
    }
    status = cudaMalloc((void **)d_data, data.size() * sizeof(Eigen::Vector3d));
    if (cudaSuccess != status) return false;

    double *h_points = (double *)data.data();
    size_t size = data.size() * sizeof(Eigen::Vector3d);
    status = cudaMemcpy(*d_data, h_points, size, cudaMemcpyHostToDevice);
    if (cudaSuccess != status) {
        printf("%s", cudaGetErrorString(status));

        return true;
    }

    return true;
}  // namespace geometry

// update the memory assigned to d_points_
bool PointCloud::UpdateDevicePoints() {
    return UpdateDeviceMemory(&d_points_, points_);
}

// update the memory assigned to d_normals_
bool PointCloud::UpdateDeviceNormals() {
    return UpdateDeviceMemory(&d_normals_, normals_);
}

// update the memory assigned to d_colors_
bool PointCloud::UpdateDeviceColors() {
    return UpdateDeviceMemory(&d_colors_, colors_);
}

// update cuda device pointers
bool PointCloud::UpdateDeviceMemory() {
    return UpdateDevicePoints() && UpdateDeviceNormals() &&
           UpdateDeviceColors();
}

// perform cleanup
bool PointCloud::ReleaseDeviceMemory(double **d_data) {
    if (*d_data == NULL) return true;

    if (cudaSuccess != cudaFree(*d_data)) return false;

    *d_data = NULL;

    return true;
}

// release the memory asigned to d_points_
bool PointCloud::ReleaseDevicePoints() {
    return ReleaseDeviceMemory(&d_points_);
}

// release the memory asigned to d_normals_
bool PointCloud::ReleaseDeviceNormals() {
    return ReleaseDeviceMemory(&d_normals_);
}

// release the memory asigned to d_colors_
bool PointCloud::ReleaseDeviceColors() {
    return ReleaseDeviceMemory(&d_colors_);
}

// release the cuda device memory
bool PointCloud::ReleaseDeviceMemory() {
    return ReleaseDevicePoints() && ReleaseDeviceNormals() &&
           ReleaseDeviceColors();

    return true;
}

}  // namespace geometry
}  // namespace open3d