/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer/mapping_3d/sparse_pose_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>

#include "Eigen/Eigenvalues"
#include "cartographer/common/make_unique.h"
#include "cartographer/common/math.h"
#include "cartographer/mapping/sparse_pose_graph/proto/constraint_builder_options.pb.h"
#include "cartographer/sensor/compressed_point_cloud.h"
#include "cartographer/sensor/voxel_filter.h"
#include "glog/logging.h"

namespace cartographer {
namespace mapping_3d {

SparsePoseGraph::SparsePoseGraph(
    const mapping::proto::SparsePoseGraphOptions& options,
    common::ThreadPool* thread_pool)
    : options_(options),
      optimization_problem_(options_.optimization_problem_options(),
                            sparse_pose_graph::OptimizationProblem::FixZ::kNo),
      constraint_builder_(options_.constraint_builder_options(), thread_pool) {}

SparsePoseGraph::~SparsePoseGraph() {
  WaitForAllComputations();
  common::MutexLocker locker(&mutex_);
  CHECK(scan_queue_ == nullptr);
}

void SparsePoseGraph::GrowSubmapTransformsAsNeeded(
    const std::vector<const Submap*>& insertion_submaps) {
  CHECK(!insertion_submaps.empty());
  const mapping::SubmapId first_submap_id = GetSubmapId(insertion_submaps[0]);
  const int trajectory_id = first_submap_id.trajectory_id;
  CHECK_GE(trajectory_id, 0);
  const auto& submap_data = optimization_problem_.submap_data();
  if (insertion_submaps.size() == 1) {
    // If we don't already have an entry for the first submap, add one.
    CHECK_EQ(first_submap_id.submap_index, 0);
    if (static_cast<size_t>(trajectory_id) >= submap_data.size() ||
        submap_data[trajectory_id].empty()) {
      optimization_problem_.AddSubmap(trajectory_id,
                                      transform::Rigid3d::Identity());
    }
    return;
  }
  CHECK_EQ(2, insertion_submaps.size());
  const int next_submap_index = submap_data.at(trajectory_id).size();
  // CHECK that we have a index for the second submap.
  const mapping::SubmapId second_submap_id = GetSubmapId(insertion_submaps[1]);
  CHECK_EQ(second_submap_id.trajectory_id, trajectory_id);
  CHECK_LE(second_submap_id.submap_index, next_submap_index);
  // Extrapolate if necessary.
  if (second_submap_id.submap_index == next_submap_index) {
    const auto& first_submap_pose =
        submap_data.at(trajectory_id).at(first_submap_id.submap_index).pose;
    optimization_problem_.AddSubmap(
        trajectory_id, first_submap_pose *
                           insertion_submaps[0]->local_pose().inverse() *
                           insertion_submaps[1]->local_pose());
  }
}

void SparsePoseGraph::AddScan(
    common::Time time, const sensor::RangeData& range_data_in_tracking,
    const transform::Rigid3d& pose,
    const kalman_filter::PoseCovariance& covariance,
    const Submaps* const trajectory, const Submap* const matching_submap,
    const std::vector<const Submap*>& insertion_submaps) {
  const transform::Rigid3d optimized_pose(
      GetLocalToGlobalTransform(trajectory) * pose);

  common::MutexLocker locker(&mutex_);
  trajectory_ids_.emplace(trajectory, trajectory_ids_.size());
  const int trajectory_id = trajectory_ids_.at(trajectory);
  const int flat_scan_index = trajectory_nodes_.size();
  CHECK_LT(flat_scan_index, std::numeric_limits<int>::max());

  constant_node_data_.push_back(mapping::TrajectoryNode::ConstantData{
      time, sensor::RangeData{Eigen::Vector3f::Zero(), {}, {}},
      sensor::Compress(range_data_in_tracking), trajectory_id,
      transform::Rigid3d::Identity()});
  trajectory_nodes_.push_back(
      mapping::TrajectoryNode{&constant_node_data_.back(), optimized_pose});
  trajectory_connectivity_.Add(trajectory_id);

  if (submap_ids_.count(insertion_submaps.back()) == 0) {
    submap_states_.resize(
        std::max<size_t>(submap_states_.size(), trajectory_id + 1));
    auto& trajectory_submap_states = submap_states_.at(trajectory_id);
    submap_ids_.emplace(
        insertion_submaps.back(),
        mapping::SubmapId{trajectory_id,
                          static_cast<int>(trajectory_submap_states.size())});
    trajectory_submap_states.emplace_back();
    trajectory_submap_states.back().submap = insertion_submaps.back();
  }
  const Submap* const finished_submap =
      insertion_submaps.front()->finished ? insertion_submaps.front() : nullptr;

  // Make sure we have a sampler for this trajectory.
  if (!global_localization_samplers_[trajectory_id]) {
    global_localization_samplers_[trajectory_id] =
        common::make_unique<common::FixedRatioSampler>(
            options_.global_sampling_ratio());
  }

  AddWorkItem([=]() REQUIRES(mutex_) {
    ComputeConstraintsForScan(flat_scan_index, matching_submap,
                              insertion_submaps, finished_submap, pose,
                              covariance);
  });
}

int SparsePoseGraph::GetNextTrajectoryNodeIndex() {
  common::MutexLocker locker(&mutex_);
  return trajectory_nodes_.size();
}

void SparsePoseGraph::AddWorkItem(std::function<void()> work_item) {
  if (scan_queue_ == nullptr) {
    work_item();
  } else {
    scan_queue_->push_back(work_item);
  }
}

void SparsePoseGraph::AddImuData(const mapping::Submaps* trajectory,
                                 common::Time time,
                                 const Eigen::Vector3d& linear_acceleration,
                                 const Eigen::Vector3d& angular_velocity) {
  common::MutexLocker locker(&mutex_);
  trajectory_ids_.emplace(trajectory, trajectory_ids_.size());
  AddWorkItem([=]() REQUIRES(mutex_) {
    optimization_problem_.AddImuData(trajectory_ids_.at(trajectory), time,
                                     linear_acceleration, angular_velocity);
  });
}

void SparsePoseGraph::ComputeConstraint(const int scan_index,
                                        const mapping::SubmapId& submap_id) {
  const mapping::NodeId node_id = scan_index_to_node_id_.at(scan_index);
  const transform::Rigid3d relative_pose = optimization_problem_.submap_data()
                                               .at(submap_id.trajectory_id)
                                               .at(submap_id.submap_index)
                                               .pose.inverse() *
                                           optimization_problem_.node_data()
                                               .at(node_id.trajectory_id)
                                               .at(node_id.node_index)
                                               .point_cloud_pose;
  const int scan_trajectory_id =
      trajectory_nodes_[scan_index].constant_data->trajectory_id;

  // Only globally match against submaps not in this trajectory.
  if (scan_trajectory_id != submap_id.trajectory_id &&
      global_localization_samplers_[scan_trajectory_id]->Pulse()) {
    constraint_builder_.MaybeAddGlobalConstraint(
        submap_id,
        submap_states_.at(submap_id.trajectory_id)
            .at(submap_id.submap_index)
            .submap,
        node_id, scan_index, &trajectory_connectivity_, trajectory_nodes_);
  } else {
    const bool scan_and_submap_trajectories_connected =
        reverse_connected_components_.count(scan_trajectory_id) > 0 &&
        reverse_connected_components_.count(submap_id.trajectory_id) > 0 &&
        reverse_connected_components_.at(scan_trajectory_id) ==
            reverse_connected_components_.at(submap_id.trajectory_id);
    if (scan_trajectory_id == submap_id.trajectory_id ||
        scan_and_submap_trajectories_connected) {
      constraint_builder_.MaybeAddConstraint(
          submap_id,
          submap_states_.at(submap_id.trajectory_id)
              .at(submap_id.submap_index)
              .submap,
          node_id, scan_index, trajectory_nodes_, relative_pose);
    }
  }
}

void SparsePoseGraph::ComputeConstraintsForOldScans(const Submap* submap) {
  const auto submap_id = GetSubmapId(submap);
  const auto& submap_state =
      submap_states_.at(submap_id.trajectory_id).at(submap_id.submap_index);
  const int num_nodes = scan_index_to_node_id_.size();
  for (int scan_index = 0; scan_index < num_nodes; ++scan_index) {
    if (submap_state.node_ids.count(scan_index_to_node_id_[scan_index]) == 0) {
      ComputeConstraint(scan_index, submap_id);
    }
  }
}

void SparsePoseGraph::ComputeConstraintsForScan(
    const int scan_index, const Submap* matching_submap,
    std::vector<const Submap*> insertion_submaps, const Submap* finished_submap,
    const transform::Rigid3d& pose,
    const kalman_filter::PoseCovariance& covariance) {
  GrowSubmapTransformsAsNeeded(insertion_submaps);
  const mapping::SubmapId matching_id = GetSubmapId(matching_submap);
  const transform::Rigid3d optimized_pose =
      optimization_problem_.submap_data()
          .at(matching_id.trajectory_id)
          .at(matching_id.submap_index)
          .pose *
      matching_submap->local_pose().inverse() * pose;
  CHECK_EQ(scan_index, scan_index_to_node_id_.size());
  const mapping::NodeId node_id{
      matching_id.trajectory_id,
      num_nodes_in_trajectory_[matching_id.trajectory_id]};
  scan_index_to_node_id_.push_back(node_id);
  ++num_nodes_in_trajectory_[matching_id.trajectory_id];
  const mapping::TrajectoryNode::ConstantData* const scan_data =
      trajectory_nodes_[scan_index].constant_data;
  CHECK_EQ(scan_data->trajectory_id, matching_id.trajectory_id);
  optimization_problem_.AddTrajectoryNode(matching_id.trajectory_id,
                                          scan_data->time, optimized_pose);
  for (const Submap* submap : insertion_submaps) {
    const mapping::SubmapId submap_id = GetSubmapId(submap);
    CHECK(!submap_states_.at(submap_id.trajectory_id)
               .at(submap_id.submap_index)
               .finished);
    submap_states_.at(submap_id.trajectory_id)
        .at(submap_id.submap_index)
        .node_ids.emplace(node_id);
    // Unchanged covariance as (submap <- map) is a translation.
    const transform::Rigid3d constraint_transform =
        submap->local_pose().inverse() * pose;
    constraints_.push_back(
        Constraint{submap_id,
                   node_id,
                   {constraint_transform,
                    common::ComputeSpdMatrixSqrtInverse(
                        covariance, options_.constraint_builder_options()
                                        .lower_covariance_eigenvalue_bound())},
                   Constraint::INTRA_SUBMAP});
  }

  for (size_t trajectory_id = 0; trajectory_id < submap_states_.size();
       ++trajectory_id) {
    for (size_t submap_index = 0;
         submap_index < submap_states_.at(trajectory_id).size();
         ++submap_index) {
      if (submap_states_.at(trajectory_id).at(submap_index).finished) {
        CHECK_EQ(submap_states_.at(trajectory_id)
                     .at(submap_index)
                     .node_ids.count(node_id),
                 0);
        ComputeConstraint(scan_index,
                          mapping::SubmapId{static_cast<int>(trajectory_id),
                                            static_cast<int>(submap_index)});
      }
    }
  }

  if (finished_submap != nullptr) {
    const mapping::SubmapId finished_submap_id = GetSubmapId(finished_submap);
    SubmapState& finished_submap_state =
        submap_states_.at(finished_submap_id.trajectory_id)
            .at(finished_submap_id.submap_index);
    CHECK(!finished_submap_state.finished);
    // We have a new completed submap, so we look into adding constraints for
    // old scans.
    ComputeConstraintsForOldScans(finished_submap);
    finished_submap_state.finished = true;
  }
  constraint_builder_.NotifyEndOfScan(scan_index);
  ++num_scans_since_last_loop_closure_;
  if (options_.optimize_every_n_scans() > 0 &&
      num_scans_since_last_loop_closure_ > options_.optimize_every_n_scans()) {
    CHECK(!run_loop_closure_);
    run_loop_closure_ = true;
    // If there is a 'scan_queue_' already, some other thread will take care.
    if (scan_queue_ == nullptr) {
      scan_queue_ = common::make_unique<std::deque<std::function<void()>>>();
      HandleScanQueue();
    }
  }
}

void SparsePoseGraph::HandleScanQueue() {
  constraint_builder_.WhenDone(
      [this](const sparse_pose_graph::ConstraintBuilder::Result& result) {
        constraints_.insert(constraints_.end(), result.begin(), result.end());
        RunOptimization();

        common::MutexLocker locker(&mutex_);
        num_scans_since_last_loop_closure_ = 0;
        run_loop_closure_ = false;
        while (!run_loop_closure_) {
          if (scan_queue_->empty()) {
            LOG(INFO) << "We caught up. Hooray!";
            scan_queue_.reset();
            return;
          }
          scan_queue_->front()();
          scan_queue_->pop_front();
        }
        // We have to optimize again.
        HandleScanQueue();
      });
}

void SparsePoseGraph::WaitForAllComputations() {
  bool notification = false;
  common::MutexLocker locker(&mutex_);
  const int num_finished_scans_at_start =
      constraint_builder_.GetNumFinishedScans();
  while (!locker.AwaitWithTimeout(
      [this]() REQUIRES(mutex_) {
        return static_cast<size_t>(constraint_builder_.GetNumFinishedScans()) ==
               trajectory_nodes_.size();
      },
      common::FromSeconds(1.))) {
    std::ostringstream progress_info;
    progress_info << "Optimizing: " << std::fixed << std::setprecision(1)
                  << 100. *
                         (constraint_builder_.GetNumFinishedScans() -
                          num_finished_scans_at_start) /
                         (trajectory_nodes_.size() -
                          num_finished_scans_at_start)
                  << "%...";
    std::cout << "\r\x1b[K" << progress_info.str() << std::flush;
  }
  std::cout << "\r\x1b[KOptimizing: Done.     " << std::endl;
  constraint_builder_.WhenDone(
      [this, &notification](
          const sparse_pose_graph::ConstraintBuilder::Result& result) {
        constraints_.insert(constraints_.end(), result.begin(), result.end());
        common::MutexLocker locker(&mutex_);
        notification = true;
      });
  locker.Await([&notification]() { return notification; });
}

void SparsePoseGraph::RunFinalOptimization() {
  WaitForAllComputations();
  optimization_problem_.SetMaxNumIterations(
      options_.max_num_final_iterations());
  RunOptimization();
  optimization_problem_.SetMaxNumIterations(
      options_.optimization_problem_options()
          .ceres_solver_options()
          .max_num_iterations());
}

void SparsePoseGraph::RunOptimization() {
  if (optimization_problem_.submap_data().empty()) {
    return;
  }
  optimization_problem_.Solve(constraints_);
  common::MutexLocker locker(&mutex_);

  const auto& node_data = optimization_problem_.node_data();
  const size_t num_optimized_poses = scan_index_to_node_id_.size();
  for (size_t i = 0; i != num_optimized_poses; ++i) {
    const mapping::NodeId node_id = scan_index_to_node_id_[i];
    trajectory_nodes_[i].pose = node_data.at(node_id.trajectory_id)
                                    .at(node_id.node_index)
                                    .point_cloud_pose;
  }
  // Extrapolate all point cloud poses that were added later.
  std::unordered_map<int, transform::Rigid3d> extrapolation_transforms;
  for (size_t i = num_optimized_poses; i != trajectory_nodes_.size(); ++i) {
    const int trajectory_id = trajectory_nodes_[i].constant_data->trajectory_id;
    if (extrapolation_transforms.count(trajectory_id) == 0) {
      const auto new_submap_transforms = ExtrapolateSubmapTransforms(
          optimization_problem_.submap_data(), trajectory_id);
      const auto old_submap_transforms = ExtrapolateSubmapTransforms(
          optimized_submap_transforms_, trajectory_id);
      CHECK_EQ(new_submap_transforms.size(), old_submap_transforms.size());
      extrapolation_transforms[trajectory_id] =
          transform::Rigid3d(new_submap_transforms.back() *
                             old_submap_transforms.back().inverse());
    }
    trajectory_nodes_[i].pose =
        extrapolation_transforms[trajectory_id] * trajectory_nodes_[i].pose;
  }
  optimized_submap_transforms_ = optimization_problem_.submap_data();
  connected_components_ = trajectory_connectivity_.ConnectedComponents();
  reverse_connected_components_.clear();
  for (size_t i = 0; i != connected_components_.size(); ++i) {
    for (const int trajectory_id : connected_components_[i]) {
      reverse_connected_components_.emplace(trajectory_id, i);
    }
  }
}

std::vector<std::vector<mapping::TrajectoryNode>>
SparsePoseGraph::GetTrajectoryNodes() {
  common::MutexLocker locker(&mutex_);
  std::vector<std::vector<mapping::TrajectoryNode>> result(
      trajectory_ids_.size());
  for (const auto& node : trajectory_nodes_) {
    result.at(node.constant_data->trajectory_id).push_back(node);
  }
  return result;
}

std::vector<SparsePoseGraph::Constraint> SparsePoseGraph::constraints() {
  common::MutexLocker locker(&mutex_);
  return constraints_;
}

transform::Rigid3d SparsePoseGraph::GetLocalToGlobalTransform(
    const mapping::Submaps* const submaps) {
  const auto extrapolated_submap_transforms = GetSubmapTransforms(submaps);
  return extrapolated_submap_transforms.back() *
         submaps->Get(extrapolated_submap_transforms.size() - 1)
             ->local_pose()
             .inverse();
}

std::vector<std::vector<int>> SparsePoseGraph::GetConnectedTrajectories() {
  common::MutexLocker locker(&mutex_);
  return connected_components_;
}

std::vector<transform::Rigid3d> SparsePoseGraph::GetSubmapTransforms(
    const mapping::Submaps* const trajectory) {
  common::MutexLocker locker(&mutex_);
  if (trajectory_ids_.count(trajectory) == 0) {
    return {transform::Rigid3d::Identity()};
  }
  return ExtrapolateSubmapTransforms(optimized_submap_transforms_,
                                     trajectory_ids_.at(trajectory));
}

std::vector<transform::Rigid3d> SparsePoseGraph::GetSubmapTransforms(
    const int trajectory_id) {
  common::MutexLocker locker(&mutex_);
  return ExtrapolateSubmapTransforms(optimized_submap_transforms_,
                                     trajectory_id);
}

std::vector<transform::Rigid3d> SparsePoseGraph::ExtrapolateSubmapTransforms(
    const std::vector<std::vector<sparse_pose_graph::SubmapData>>&
        submap_transforms,
    const size_t trajectory_id) const {
  if (trajectory_id >= submap_states_.size()) {
    return {transform::Rigid3d::Identity()};
  }

  // Submaps for which we have optimized poses.
  std::vector<transform::Rigid3d> result;
  for (const auto& state : submap_states_.at(trajectory_id)) {
    if (trajectory_id < submap_transforms.size() &&
        result.size() < submap_transforms.at(trajectory_id).size()) {
      // Submaps for which we have optimized poses.
      result.push_back(
          submap_transforms.at(trajectory_id).at(result.size()).pose);
    } else if (result.empty()) {
      result.push_back(transform::Rigid3d::Identity());
    } else {
      // Extrapolate to the remaining submaps. Accessing local_pose() in Submaps
      // is okay, since the member is const.
      result.push_back(result.back() *
                       submap_states_.at(trajectory_id)
                           .at(result.size() - 1)
                           .submap->local_pose()
                           .inverse() *
                       state.submap->local_pose());
    }
  }

  if (result.empty()) {
    result.push_back(transform::Rigid3d::Identity());
  }
  return result;
}

}  // namespace mapping_3d
}  // namespace cartographer
