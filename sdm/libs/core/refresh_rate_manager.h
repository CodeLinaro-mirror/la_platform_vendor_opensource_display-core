/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __REFRESH_RATE_MANAGER_H__
#define __REFRESH_RATE_MANAGER_H__

#include <private/hw_info_types.h>

namespace sdm {
class RefreshRateManager {
 public:
  RefreshRateManager(int32_t id, SDMDisplayType type, uint32_t avr_step);

  uint32_t GetPanelRefreshRate() { return panel_refresh_rate_; }
  uint32_t GetAverageRefreshRate() { return average_refresh_rate_; }
  uint32_t GetAverageVsyncPeriod() { return average_vsync_period_ns_; }

  void CalculateRefreshRate(const DispLayerStack *disp_layer_stack,
                            const DisplayClientContext &client_ctx, bool is_idle);

 private:
  void RecalculateRefreshRate(uint32_t current_fps);
  float ToAverageFps(float vsync_period_ns);
  float ToNominalFps(float vsync_period_ns);
  float ToVsyncPeriodNs(float fps);
  bool NeedsFpsRecalculation(bool is_idle, uint32_t current_fi, uint64_t current_ept,
                             uint32_t current_fps);

  constexpr static float kMaxRefreshRate = 240.0f;
  constexpr static int kFpsDeltaThresholdHz = 5;
  constexpr static size_t kMaxSamples = 30;
  constexpr static uint64_t kOneMilliSecondInNanoSeconds = 1000000;
  constexpr static uint64_t kHundredMilliSecondInNanoSeconds = 100000000;
  constexpr static uint64_t kOneSecondInNanoSeconds = 1000000000;

  int32_t display_id_ = -1;
  SDMDisplayType display_type_ = kDisplayMax;
  uint32_t avr_step_ = 0;
  uint32_t panel_refresh_rate_ = 0;
  uint32_t average_refresh_rate_ = 0;
  uint32_t average_vsync_period_ns_ = 0;
  bool pending_recalculate_fps_ = true;
  uint32_t last_frame_interval_ = 0;
  uint64_t last_expected_present_time_ = 0;
  uint64_t sum_of_ept_deltas_ = 0;
  uint64_t last_ept_delta_ = 0;
  size_t sample_count_ = 0;
};

}  // namespace sdm
#endif