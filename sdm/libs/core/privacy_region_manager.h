/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __PRIVACY_REGION_MANAGER_H__
#define __PRIVACY_REGION_MANAGER_H__

#include <core/sdm_types.h>
#include <display_base.h>
#include <private/hw_info_types.h>
#include <unordered_map>

using std::pair;
using std::unordered_map;

namespace sdm {

#define MAX_AREA_MODE_LAYERS 2

class PrivacyRegionManager {
 public:
  PrivacyRegionManager(uint32_t max_privacy_regions, PrivacyRegionMode mode);
  DisplayError ConfigurePrivacyRegions(DispLayerStack *disp_layer_stack,
                                       const DisplayClientContext &client_ctx,
                                       bool mixer_resolution_updated,
                                       std::vector<PrivacyRegion> *regions);

 private:
  DisplayError LoadPrivacyRegionsOffsetsFromFile();
  SDMRect unionRect(SDMRect a, SDMRect b);
  bool isEmptyRegion(SDMRect rect);

  void ApplyCollapsing(const DispLayerStack *disp_layer_stack,
                       std::vector<PrivacyRegion> &consolidated_regions, Resolution &mixer_res);
  void ApplyScaling(std::vector<PrivacyRegion> &consolidated_regions, Resolution &panel_res,
                    Resolution &mixer_res);
  void ApplyOffsets(std::vector<PrivacyRegion> &consolidated_regions, Resolution &panel_res,
                    Resolution &mixer_res);
  void LayerModeCollapsing(const DispLayerStack *disp_layer_stack,
                           std::vector<PrivacyRegion> &consolidated_regions, Resolution &mixer_res);
  void AreaModeCollapsing(const DispLayerStack *disp_layer_stack,
                          std::vector<PrivacyRegion> &consolidated_regions, Resolution &mixer_res);
  void LayerModeBounds(std::vector<PrivacyRegion> &consolidated_regions, Resolution &panel_res,
                       Resolution &mixer_res);
  void AreaModeBounds(std::vector<PrivacyRegion> &consolidated_regions, Resolution &panel_res,
                      Resolution &mixer_res);

  size_t num_privacy_regions_ = 0;
  uint32_t max_privacy_regions_ = 0;
  int spatial_dimming_width_ = 0;
  PrivacyRegionMode mode_ = PrivacyRegionMode::PR_NONE;
  bool prev_region_state_[MAX_AREA_MODE_LAYERS] = {false};
  unordered_map<Resolution, PrivacyRegion, ResolutionHash> privacy_regions_offsets_;
};

}  //namespace sdm

#endif
