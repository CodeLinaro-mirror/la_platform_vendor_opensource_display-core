/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "refresh_rate_manager.h"

#include <cmath>
#include <cstdlib>
#include <utils/debug.h>

#define __CLASS__ "RefreshRateMgr"

namespace sdm {
RefreshRateManager::RefreshRateManager(int32_t id, SDMDisplayType type, uint32_t avr_step)
    : display_id_(id), display_type_(type), avr_step_(avr_step) {
  DLOGV_IF(kTagRefreshRate, "Creating RefreshRateManager %p (display %d-%d avr_step %d)", this,
           display_id_, display_type_, avr_step_);
}

void RefreshRateManager::RecalculateRefreshRate(uint32_t current_fps) {
  sample_count_ = 0;
  sum_of_ept_deltas_ = 0;
  panel_refresh_rate_ = current_fps;
  average_refresh_rate_ = current_fps;
  average_vsync_period_ns_ = ToVsyncPeriodNs(current_fps);
  pending_recalculate_fps_ = true;
}

float RefreshRateManager::ToAverageFps(float vsync_period_ns) {
  return FLOAT(kOneSecondInNanoSeconds / vsync_period_ns);
}

float RefreshRateManager::ToNominalFps(float vsync_period_ns) {
  int nearest_divisor = 0;
  float nominal_fps = 0;
  if (avr_step_ != 0) {
    nearest_divisor =
        round(static_cast<float>(avr_step_) / (kOneSecondInNanoSeconds / vsync_period_ns));
    nominal_fps = static_cast<float>(avr_step_) / nearest_divisor;
  } else {
    nearest_divisor = round(kMaxRefreshRate / (kOneSecondInNanoSeconds / vsync_period_ns));
    nominal_fps = kMaxRefreshRate / nearest_divisor;
  }

  return nominal_fps;
}

float RefreshRateManager::ToVsyncPeriodNs(float fps) {
  return FLOAT(1000.f / fps * kOneMilliSecondInNanoSeconds);
}

bool RefreshRateManager::NeedsFpsRecalculation(bool is_idle, uint32_t current_fi,
                                               uint64_t current_ept, uint32_t current_fps) {
  int64_t current_ept_delta = 0;

  if (is_idle) {
    // TODO: Update idle fps based on min supported refresh rate from drm
    bool is_vrr = (avr_step_ > 0);
    float idle_vsync_period_ns = is_vrr ? 1e9f : current_fi;
    panel_refresh_rate_ = ToNominalFps(idle_vsync_period_ns);
    DLOGV_IF(kTagRefreshRate, "Recalculate fps (idle display %d-%d panelFps:%dHz)", display_id_,
             display_type_, panel_refresh_rate_);

    pending_recalculate_fps_ = true;
    return false;
  }

  current_ept_delta =
      static_cast<int64_t>(current_ept) - static_cast<int64_t>(last_expected_present_time_);

  // If ept delta between (n, n-1) frame > 100ms, recalculate. This indicates that there's a large
  // gap between frames
  if (current_ept_delta > kHundredMilliSecondInNanoSeconds) {
    RecalculateRefreshRate(current_fps);
    DLOGV_IF(kTagRefreshRate,
             "Recalculate fps (currentEpt:%" PRIu64 " - lastEpt:%" PRIu64 " ) > 100ms", current_ept,
             last_expected_present_time_);
    return true;
  }

  // If ept delta between (n, n-1) and (n-1, n-2) frames > 1ms, recalculate
  // This indicates that the upcoming frame's refresh rate is updated. This avoids having outliers
  // in the sample.
  if (sample_count_ > 1) {
    if (abs(static_cast<int64_t>(current_ept_delta) - static_cast<int64_t>(last_ept_delta_)) >
        kOneMilliSecondInNanoSeconds) {
      RecalculateRefreshRate(current_fps);
      DLOGV_IF(kTagRefreshRate,
               "Recalculate fps (currentEPTDelta:%" PRId64 " - lastEPTDelta:%" PRIu64 ") > 1ms",
               current_ept_delta, last_ept_delta_);
      return true;
    }
  }

  // If frame interval between (n, n-1) frame > 5Hz, recalculate
  // This indicates that there's a significant change in the refresh rate.
  float prev_fi_fps = ToAverageFps(last_frame_interval_);
  float cur_fi_fps = ToAverageFps(current_fi);
  if (abs(cur_fi_fps - prev_fi_fps) > kFpsDeltaThresholdHz) {
    RecalculateRefreshRate(current_fps);
    DLOGV_IF(kTagRefreshRate, "Recalculate fps (currentFI:%d (%.0fHz) - lastFI:%d (%.0fHz) > %dHz",
             current_fi, cur_fi_fps, last_frame_interval_, prev_fi_fps, kFpsDeltaThresholdHz);
    return true;
  }

  return pending_recalculate_fps_;
}

void RefreshRateManager::CalculateRefreshRate(const DispLayerStack *disp_layer_stack,
                                              const DisplayClientContext &client_ctx,
                                              bool is_idle) {
  if (!disp_layer_stack) {
    DLOGW("Invalid pointer to display layer stack");
    return;
  }

  DTRACE_SCOPED();
  bool recalculate_fps = false;
  uint32_t current_fps = client_ctx.display_attributes.fps;
  uint32_t current_vsync_period_ns = client_ctx.display_attributes.vsync_period_ns;
  uint32_t current_fi = disp_layer_stack->stack_info.common_info.frame_interval;
  uint64_t current_ept = disp_layer_stack->stack_info.common_info.expected_present_time;

  if (!panel_refresh_rate_) {
    panel_refresh_rate_ = current_fps;
    average_refresh_rate_ = current_fps;
    average_vsync_period_ns_ = current_vsync_period_ns;
    DLOGV_IF(kTagRefreshRate, "Initial fps for display %d-%d panelFPS:%dHz averageFPS:%dHz",
             display_id_, display_type_, panel_refresh_rate_, average_refresh_rate_);
  }

  recalculate_fps = NeedsFpsRecalculation(is_idle, current_fi, current_ept, current_fps);
  // If there's no need to recalculate the refresh rate, just cache the current ept and fi
  if (!recalculate_fps) {
    last_expected_present_time_ = current_ept;
    last_frame_interval_ = current_fi;
    return;
  }

  // Collect samples to calculate the refresh rate
  if (sample_count_ == 0) {
    last_expected_present_time_ = current_ept;
    last_ept_delta_ = 0;
    sample_count_++;
  } else if (sample_count_ > kMaxSamples) {
    float average_ept =
        static_cast<float>(sum_of_ept_deltas_) / static_cast<float>(sample_count_ - 1);
    float nominal_fps = ToNominalFps(average_ept);
    float calc_vsync_ms = ToVsyncPeriodNs(nominal_fps);
    average_refresh_rate_ = static_cast<uint32_t>(nominal_fps);
    average_vsync_period_ns_ = static_cast<uint32_t>(calc_vsync_ms);

    DLOGV_IF(kTagRefreshRate,
             "Calculated for display %d-%d averageFPS:%dHz vsyncPeriodMs:%d averageEPT:%.2f",
             display_id_, display_type_, average_refresh_rate_, average_vsync_period_ns_,
             average_ept);

    pending_recalculate_fps_ = false;

  } else {
    int64_t ept_delta = current_ept - last_expected_present_time_;
    sum_of_ept_deltas_ += ept_delta;
    last_ept_delta_ = ept_delta;
    DLOGV_IF(kTagRefreshRate,
             "Sample #%d eptDelta:%" PRId64 " (currentEpt:%" PRIu64 " - prevEpt:%" PRIu64
             ", sumOfEptDeltas:%" PRIu64 ")",
             sample_count_, ept_delta, current_ept, last_expected_present_time_,
             sum_of_ept_deltas_);

    sample_count_++;
  }

  last_expected_present_time_ = current_ept;
  last_frame_interval_ = current_fi;
}

}  // namespace sdm
