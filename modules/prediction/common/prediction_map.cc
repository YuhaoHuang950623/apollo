/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/prediction/common/prediction_map.h"

#include <cmath>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "modules/common/configs/config_gflags.h"
#include "modules/common/math/linear_interpolation.h"
#include "modules/common/math/vec2d.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/map/proto/map_id.pb.h"
#include "modules/prediction/common/prediction_gflags.h"

namespace apollo {
namespace prediction {

using apollo::hdmap::LaneInfo;
using apollo::hdmap::Id;
using apollo::hdmap::MapPathPoint;
using apollo::hdmap::HDMapUtil;

PredictionMap::PredictionMap() {}

Eigen::Vector2d PredictionMap::PositionOnLane(
    std::shared_ptr<const LaneInfo> lane_info,
    const double s) {
  apollo::common::PointENU point = lane_info->get_smooth_point(s);
  return {point.x(), point.y()};
}

double PredictionMap::HeadingOnLane(
    std::shared_ptr<const LaneInfo> lane_info,
    const double s) {
  const std::vector<double>& headings = lane_info->headings();
  const std::vector<double>& accumulated_s = lane_info->accumulate_s();
  CHECK(headings.size() == accumulated_s.size());
  size_t size = headings.size();

  if (size == 0) {
    return 0.0;
  }

  if (size == 1) {
    return headings[0];
  }

  const auto low_itr =
      std::lower_bound(accumulated_s.begin(), accumulated_s.end(), s);
  size_t index = low_itr - accumulated_s.begin();
  if (index >= size - 1) {
    return headings.back();
  } else {
    return apollo::common::math::slerp(headings[index], accumulated_s[index],
                                       headings[index + 1],
                                       accumulated_s[index + 1], s);
  }
}

double PredictionMap::LaneTotalWidth(
    std::shared_ptr<const apollo::hdmap::LaneInfo> lane_info,
    const double s) {
  double left = 0.0;
  double right = 0.0;
  lane_info->get_width(s, &left, &right);
  return left + right;
}

std::shared_ptr<const LaneInfo> PredictionMap::LaneById(
    const std::string& str_id) {
  return HDMapUtil::instance()->BaseMapRef().get_lane_by_id(
      apollo::hdmap::MakeMapId(str_id));
}

void PredictionMap::GetProjection(
    const Eigen::Vector2d& position,
    std::shared_ptr<const LaneInfo> lane_info,
    double* s,
    double* l) {
  if (lane_info == nullptr) {
    return;
  }
  apollo::common::math::Vec2d pos(position[0], position[1]);
  lane_info->get_projection(pos, s, l);
}

bool PredictionMap::ProjectionFromLane(
    std::shared_ptr<const LaneInfo> lane_info, double s,
    MapPathPoint* path_point) {
  if (lane_info == nullptr) {
    return false;
  }
  apollo::common::PointENU point = lane_info->get_smooth_point(s);
  double heading = HeadingOnLane(lane_info, s);
  path_point->set_x(point.x());
  path_point->set_y(point.y());
  path_point->set_heading(heading);
  return true;
}

void PredictionMap::OnLane(
    const std::vector<std::shared_ptr<const LaneInfo>>& prev_lanes,
    const Eigen::Vector2d& point, const double heading,
    const double radius,
    const bool on_lane,
    std::vector<std::shared_ptr<const LaneInfo>>* lanes) {
  std::vector<std::shared_ptr<const LaneInfo>> candidate_lanes;

  apollo::common::PointENU hdmap_point;
  hdmap_point.set_x(point[0]);
  hdmap_point.set_y(point[1]);
  if (HDMapUtil::instance()->BaseMapRef().get_lanes_with_heading(
        hdmap_point, radius, heading, M_PI, &candidate_lanes) != 0) {
    return;
  }

  apollo::common::math::Vec2d vec_point(point[0], point[1]);
  for (const auto &candidate_lane : candidate_lanes) {
    if (candidate_lane == nullptr) {
      continue;
    }
    if (on_lane && !candidate_lane->is_on_lane(vec_point)) {
      continue;
    }
    if (!IsIdenticalLane(candidate_lane, prev_lanes) &&
        !IsSuccessorLane(candidate_lane, prev_lanes) &&
        !IsLeftNeighborLane(candidate_lane, prev_lanes) &&
        !IsRightNeighborLane(candidate_lane, prev_lanes)) {
      continue;
    }
    double distance = 0.0;
    apollo::common::PointENU nearest_point =
        candidate_lane->get_nearest_point(vec_point, &distance);
    double nearest_point_heading =
        PathHeading(candidate_lane, nearest_point);
    double diff = std::fabs(
        apollo::common::math::AngleDiff(heading, nearest_point_heading));
    if (diff <= FLAGS_max_lane_angle_diff) {
      lanes->push_back(candidate_lane);
    }
  }
}

double PredictionMap::PathHeading(
    std::shared_ptr<const LaneInfo> lane_info,
    const apollo::common::PointENU& point) {
  apollo::common::math::Vec2d vec_point = {point.x(), point.y()};
  double s = -1.0;
  double l = 0.0;
  lane_info->get_projection(vec_point, &s, &l);
  return HeadingOnLane(lane_info, s);
}

bool PredictionMap::SmoothPointFromLane(
    const std::string& id,
    const double s, const double l,
    Eigen::Vector2d* point,
    double* heading) {
  if (point == nullptr || heading == nullptr) {
    return false;
  }
  std::shared_ptr<const LaneInfo> lane = LaneById(id);
  apollo::common::PointENU hdmap_point = lane->get_smooth_point(s);
  *heading = PathHeading(lane, hdmap_point);
  point->operator[](0) = hdmap_point.x() - std::sin(*heading) * l;
  point->operator[](1) = hdmap_point.y() + std::cos(*heading) * l;
  return true;
}

void PredictionMap::NearbyLanesByCurrentLanes(
    const Eigen::Vector2d& point,
    double heading,
    double radius,
    const std::vector<std::shared_ptr<const LaneInfo>>& lanes,
    std::vector<std::shared_ptr<const LaneInfo>>* nearby_lanes) {
  if (lanes.size() == 0) {
    std::vector<std::shared_ptr<const LaneInfo>> prev_lanes(0);
    OnLane(prev_lanes, point, heading, radius, false, nearby_lanes);
  } else {
    std::unordered_set<std::string> lane_ids;
    for (auto& lane_ptr : lanes) {
      if (lane_ptr == nullptr) {
        continue;
      }
      for (auto& lane_id : lane_ptr->lane().left_neighbor_forward_lane_id()) {
        const std::string& id = lane_id.id();
        if (lane_ids.find(id) != lane_ids.end()) {
          continue;
        }
        std::shared_ptr<const LaneInfo> nearby_lane = LaneById(id);
        double s = -1.0;
        double l = 0.0;
        GetProjection(point, nearby_lane, &s, &l);
        if (::apollo::common::math::DoubleCompare(s, 0.0) >= 0 &&
            ::apollo::common::math::DoubleCompare(std::fabs(l), radius) > 0) {
          continue;
        }
        lane_ids.insert(id);
        nearby_lanes->push_back(nearby_lane);
      }
      for (auto& lane_id : lane_ptr->lane().right_neighbor_forward_lane_id()) {
        const std::string& id = lane_id.id();
        if (lane_ids.find(id) != lane_ids.end()) {
          continue;
        }
        std::shared_ptr<const LaneInfo> nearby_lane = LaneById(id);
        double s = -1.0;
        double l = 0.0;
        GetProjection(point, nearby_lane, &s, &l);
        if (::apollo::common::math::DoubleCompare(s, 0.0) >= 0 &&
            ::apollo::common::math::DoubleCompare(std::fabs(l), radius) > 0) {
          continue;
        }
        lane_ids.insert(id);
        nearby_lanes->push_back(nearby_lane);
      }
    }
  }
}

bool PredictionMap::IsLeftNeighborLane(
    std::shared_ptr<const LaneInfo> left_lane,
    std::shared_ptr<const LaneInfo> curr_lane) {
  if (curr_lane == nullptr) {
    return true;
  }
  if (left_lane == nullptr) {
    return false;
  }
  for (auto& left_lane_id : curr_lane->lane().left_neighbor_forward_lane_id()) {
    if (left_lane->id().id() == left_lane_id.id()) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsLeftNeighborLane(
    std::shared_ptr<const LaneInfo> left_lane,
    const std::vector<std::shared_ptr<const LaneInfo>>& lanes) {
  if (lanes.size() == 0) {
    return true;
  }
  for (auto& lane : lanes) {
    if (IsLeftNeighborLane(left_lane, lane)) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsRightNeighborLane(
    std::shared_ptr<const LaneInfo> right_lane,
    std::shared_ptr<const LaneInfo> curr_lane) {
  if (curr_lane == nullptr) {
    return true;
  }
  if (right_lane == nullptr) {
    return false;
  }
  for (auto& right_lane_id :
       curr_lane->lane().right_neighbor_forward_lane_id()) {
    if (right_lane->id().id() == right_lane_id.id()) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsRightNeighborLane(
    std::shared_ptr<const LaneInfo> right_lane,
    const std::vector<std::shared_ptr<const LaneInfo>>& lanes) {
  if (lanes.size() == 0) {
    return true;
  }
  for (auto& lane : lanes) {
    if (IsRightNeighborLane(right_lane, lane)) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsSuccessorLane(
    std::shared_ptr<const LaneInfo> succ_lane,
    std::shared_ptr<const LaneInfo> curr_lane) {
  if (curr_lane == nullptr) {
    return true;
  }
  if (succ_lane == nullptr) {
    return false;
  }
  for (auto& successor_lane_id : curr_lane->lane().successor_id()) {
    if (succ_lane->id().id() == successor_lane_id.id()) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsSuccessorLane(
    std::shared_ptr<const LaneInfo> succ_lane,
    const std::vector<std::shared_ptr<const LaneInfo>>& lanes) {
  if (lanes.size() == 0) {
    return true;
  }
  for (auto& lane : lanes) {
    if (IsSuccessorLane(succ_lane, lane)) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsPredecessorLane(
    std::shared_ptr<const LaneInfo> pred_lane,
    std::shared_ptr<const LaneInfo> curr_lane) {
  if (curr_lane == nullptr) {
    return true;
  }
  if (pred_lane == nullptr) {
    return false;
  }
  for (auto& predecessor_lane_id : curr_lane->lane().predecessor_id()) {
    if (pred_lane->id().id() == predecessor_lane_id.id()) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsPredecessorLane(
    std::shared_ptr<const LaneInfo> pred_lane,
    const std::vector<std::shared_ptr<const LaneInfo>>& lanes) {
  if (lanes.size() == 0) {
    return true;
  }
  for (auto& lane : lanes) {
    if (IsPredecessorLane(pred_lane, lane)) {
      return true;
    }
  }
  return false;
}

bool PredictionMap::IsIdenticalLane(
    std::shared_ptr<const LaneInfo> other_lane,
    std::shared_ptr<const LaneInfo> curr_lane) {
  if (curr_lane == nullptr || other_lane == nullptr) {
    return true;
  }
  return other_lane->id().id() == curr_lane->id().id();
}

bool PredictionMap::IsIdenticalLane(
    std::shared_ptr<const LaneInfo> other_lane,
    const std::vector<std::shared_ptr<const LaneInfo>>& lanes) {
  if (lanes.size() == 0) {
    return true;
  }
  for (auto& lane : lanes) {
    if (IsIdenticalLane(other_lane, lane)) {
      return true;
    }
  }
  return false;
}

int PredictionMap::LaneTurnType(const std::string& lane_id) {
  std::shared_ptr<const LaneInfo> lane = LaneById(lane_id);
  if (lane != nullptr) {
    return static_cast<int>(lane->lane().turn());
  }
  return 1;
}

}  // namespace prediction
}  // namespace apollo
