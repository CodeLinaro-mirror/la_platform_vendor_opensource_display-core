/*
* Copyright (c) 2014 - 2021, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted
* provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright notice, this list of
*      conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright notice, this list of
*      conditions and the following disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written
*      permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <utils/constants.h>
#include <utils/debug.h>
#include <private/hw_interface.h>
#include <private/hw_info_interface.h>
#include <algorithm>
#include <vector>
#include <utility>
#include "display_virtual.h"

#define __CLASS__ "DisplayVirtual"

namespace sdm {

DisplayVirtual::DisplayVirtual(DisplayEventHandler *event_handler,
                               sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                               BufferAllocator *buffer_allocator, CompManager *comp_manager,
                               const std::vector<Hdr> &hdr_types, float max_lum, float min_lum)
    : DisplayBase(kVirtual, event_handler, kDeviceVirtual, buffer_allocator, comp_manager,
                  hw_info_intf),
      hdr_types_(hdr_types),
      max_lum_(max_lum),
      min_lum_(min_lum) {}

DisplayVirtual::DisplayVirtual(DisplayId display_id, DisplayEventHandler *event_handler,
                               sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                               BufferAllocator *buffer_allocator, CompManager *comp_manager,
                               const std::vector<Hdr> &hdr_types, float max_lum, float min_lum)
    : DisplayBase(display_id, kVirtual, event_handler, kDeviceVirtual, buffer_allocator,
                  comp_manager, hw_info_intf),
      hdr_types_(hdr_types),
      max_lum_(max_lum),
      min_lum_(min_lum) {}

DisplayError DisplayVirtual::Init() {
  ClientLock lock(disp_mutex_);

  DisplayError error = comp_manager_->AllocateVirtualDisplayId(&display_id_);
  if (error != kErrorNone) {
    return error;
  }

  display_id_info_ = DisplayId(display_id_);
  error = DPUCoreFactory::Create(display_id_info_, kVirtual, hw_info_intf_, buffer_allocator_,
                                 &dpu_core_mux_);
  if (error != kErrorNone) {
    return error;
  }

  SetHdrCapabilities(hdr_types_, max_lum_, min_lum_);

  dpu_core_mux_->GetHWInterface(&hw_intf_);

  if (-1 == display_id_info_.GetDisplayId()) {
    dpu_core_mux_->GetDisplayId(&display_id_);
    display_id_info_ = DisplayId(display_id_);
  }

  core_id_ = display_id_info_.GetCoreIdMap();
  std::bitset<32> core_id_bitset = std::bitset<32>(core_id_);
  core_count_ = core_id_bitset.count();

  for (int i = 0; i < core_id_.size(); i++) {
    if (!core_id_[i]) {
      continue;
    }
    default_clock_hz_.insert(std::pair<uint32_t, uint32_t>(i, 0));
    cached_framebuffer_.insert(std::pair<uint32_t, LayerBuffer>(i, {}));
    cached_qos_data_.insert(std::pair<uint32_t, HWQosData>(i, {}));
    disp_layer_stack_->info.insert(std::pair<uint32_t, HWLayersInfo>(i, HWLayersInfo()));
  }

  for (auto info_intf = hw_info_intf_.Begin(); info_intf != hw_info_intf_.End(); info_intf++) {
    HWResourceInfo hw_resource_info = HWResourceInfo();
    info_intf->second->GetHWResourceInfo(&hw_resource_info);
    hw_resource_info_.push_back(hw_resource_info);
  }

  uint32_t max_mixer_stages = INT_MAX;

  for (auto& res_info : hw_resource_info_) {
    max_mixer_stages = std::min(max_mixer_stages, res_info.num_blending_stages);
  }

  int property_value = Debug::GetMaxPipesPerMixer(display_type_);
  if (property_value >= 0) {
    max_mixer_stages = std::min(UINT32(property_value), max_mixer_stages);
  }
  DisplayBase::SetMaxMixerStages(max_mixer_stages);

  // STC color mode is supported in PQ type, so this func does not need to be called.
  if (!NeedsDspp()) {
    InitializeColorModes();
  }

  return error;
}

DisplayError DisplayVirtual::Deinit() {
  auto error = DisplayBase::Deinit();
  if (display_id_ != -1) {
    comp_manager_->DeallocateVirtualDisplayId(display_id_);
  }
  return error;
}

DisplayError DisplayVirtual::GetNumVariableInfoConfigs(uint32_t *count) {
  ClientLock lock(disp_mutex_);
  *count = 1;
  return kErrorNone;
}

DisplayError DisplayVirtual::GetConfig(uint32_t index, DisplayConfigVariableInfo *variable_info) {
  ClientLock lock(disp_mutex_);
  *variable_info = client_ctx_.display_attributes;
  return kErrorNone;
}

DisplayError DisplayVirtual::GetActiveConfig(uint32_t *index) {
  ClientLock lock(disp_mutex_);
  *index = 0;
  return kErrorNone;
}

DisplayError DisplayVirtual::SetActiveConfig(DisplayConfigVariableInfo *variable_info) {
  ClientLock lock(disp_mutex_);

  if (!variable_info) {
    return kErrorParameters;
  }

  DisplayError error = kErrorNone;
  DisplayClientContext client_ctx = {};
  DisplayDeviceContext device_ctx;
  client_ctx = client_ctx_;
  device_ctx = device_ctx_;

  client_ctx.display_attributes.x_pixels = variable_info->x_pixels;
  client_ctx.display_attributes.y_pixels = variable_info->y_pixels;
  client_ctx.display_attributes.fps = variable_info->fps;
  client_ctx.display_attributes.needs_dspp = NeedsDspp();

  if (client_ctx.display_attributes == client_ctx_.display_attributes) {
    return kErrorNone;
  }

  error = dpu_core_mux_->SetDisplayAttributes(client_ctx.display_attributes);
  if (error != kErrorNone) {
    return error;
  }

  uint32_t active_index = 0;
  dpu_core_mux_->GetActiveConfig(&active_index);
  dpu_core_mux_->GetDisplayAttributes(active_index, &device_ctx, &client_ctx);
  dpu_core_mux_->GetHWPanelInfo(&device_ctx, &client_ctx);

  error = dpu_core_mux_->GetMixerAttributes(&device_ctx, &client_ctx);
  if (error != kErrorNone) {
    return error;
  }

  // fb_config will be updated only once after creation of virtual display
  if (client_ctx.fb_config.x_pixels == 0 || client_ctx.fb_config.y_pixels == 0) {
    error = dpu_core_mux_->GetFbConfig(client_ctx.display_attributes.x_pixels,
                                       client_ctx.display_attributes.y_pixels,
                                       &device_ctx, &client_ctx);
      if (error != kErrorNone) {
        return error;
      }
  }

  // if display is already connected, reconfigure the display with new configuration.
  if (!display_comp_ctx_) {
    error = comp_manager_->RegisterDisplay(display_id_info_, display_type_, device_ctx, client_ctx,
                                           &display_comp_ctx_, &cached_qos_data_, this);
  } else {
    error = comp_manager_->ReconfigureDisplay(display_comp_ctx_, device_ctx, client_ctx,
                                              &cached_qos_data_);
  }
  if (error != kErrorNone) {
    return error;
  }

  for (auto& qos_data : cached_qos_data_) {
    default_clock_hz_.at(qos_data.first) = qos_data.second.clock_hz;
  }

  client_ctx_ = client_ctx;
  device_ctx_ = device_ctx;

  DLOGI("Virtual display %d-%d resolution changed to [%dx%d]", display_id_,
        display_type_, client_ctx_.display_attributes.x_pixels,
        client_ctx_.display_attributes.y_pixels);

  return kErrorNone;
}

DisplayError DisplayVirtual::Prepare(LayerStack *layer_stack) {
  ClientLock lock(disp_mutex_);

  DisplayError error = PrePrepare(layer_stack);
  if (error == kErrorNone) {
    return error;
  }

  if (error == kErrorNeedsLutRegen && (ForceToneMapUpdate(layer_stack) == kErrorNone)) {
    return kErrorNone;
  }

  return DisplayBase::Prepare(layer_stack);
}

DisplayError DisplayVirtual::GetColorModeCount(uint32_t *mode_count) {
  ClientLock lock(disp_mutex_);
  if (!mode_count) {
    return kErrorParameters;
  }

  DLOGI("Display = %d Number of modes = %d", display_type_, num_color_modes_);
  *mode_count = num_color_modes_;

  return kErrorNone;
}

DisplayError DisplayVirtual::SetPanelLuminanceAttributes(float min_lum, float max_lum) {
  DLOGW("setPanelLuminanceAttributes is unsupported - call setHDRCapabilities for virtual display");
  return kErrorNone;
}

DisplayError DisplayVirtual::SetHdrCapabilities(const std::vector<Hdr> &hdr_types,
                                                float max_avg_luminance, float min_luminance) {
  return dpu_core_mux_->SetHdrCapabilities(hdr_types, max_avg_luminance, min_luminance);
}

DisplayError DisplayVirtual::colorSamplingOn() {
    return kErrorNone;
}

DisplayError DisplayVirtual::colorSamplingOff() {
    return kErrorNone;
}

DisplayError DisplayVirtual::InitializeColorModes() {
  dpu_core_mux_->GetHWPanelInfo(&device_ctx_, &client_ctx_);
  PrimariesTransfer pt = {};
  AttrVal var = {};

  // SRGB
  pt.primaries = QtiColorPrimaries_BT709_5;
  pt.transfer = QtiTransfer_sRGB;
  var.clear();
  var.push_back(std::make_pair(kColorGamutAttribute, kSrgb));
  var.push_back(std::make_pair(kDynamicRangeAttribute, kSdr));
  var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
  var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
  color_modes_cs_.push_back(pt);
  color_mode_attr_map_.insert(std::make_pair(kSrgb, var));

  // native mode
  pt.primaries = QtiColorPrimaries_Max;
  pt.transfer = QtiTransfer_Max;
  var.clear();
  var.push_back(std::make_pair(kColorGamutAttribute, kNative));
  var.push_back(std::make_pair(kGammaTransferAttribute, kNative));
  var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
  color_modes_cs_.push_back(pt);
  color_mode_attr_map_.insert(std::make_pair("hal_native", var));

  if (client_ctx_.hw_panel_info.hdr_enabled) {
    // kDisplayBt2020
    pt.primaries = QtiColorPrimaries_BT2020;
    pt.transfer = QtiTransfer_sRGB;
    var.clear();
    var.push_back(std::make_pair(kColorGamutAttribute, kBt2020));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
    var.push_back(std::make_pair(kGammaTransferAttribute, kSrgb));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kDisplayBt2020, var));

    // BT2020_PQ
    if (client_ctx_.hw_panel_info.hdr_eotf & kHdrEOTFHDR10) {
      pt.transfer = QtiTransfer_SMPTE_ST2084;
      var.pop_back();
      var.push_back(std::make_pair(kGammaTransferAttribute, kSt2084));
      color_modes_cs_.push_back(pt);
      color_mode_attr_map_.insert(std::make_pair(kBt2020Pq, var));
    }

    // BT2020_HLG
    if (client_ctx_.hw_panel_info.hdr_eotf & kHdrEOTFHLG) {
      pt.transfer = QtiTransfer_HLG;
      var.pop_back();
      var.push_back(std::make_pair(kGammaTransferAttribute, kHlg));
      color_modes_cs_.push_back(pt);
      color_mode_attr_map_.insert(std::make_pair(kBt2020Hlg, var));
    }
  }
  current_color_mode_ = kSrgb;
  UpdateColorModes();

  return kErrorNone;
}

DisplayError DisplayVirtual::GetColorModes(uint32_t *mode_count,
                                           std::vector<std::string> *color_modes) {
  ClientLock lock(disp_mutex_);
  if (!mode_count || !color_modes) {
    return kErrorParameters;
  }

  for (uint32_t i = 0; i < num_color_modes_; i++) {
    DLOGI_IF(kTagDisplay, "DisplayVirtual: ColorMode[%d] = %s", i, color_modes_[i].name);
    color_modes->at(i) = color_modes_[i].name;
  }

  return kErrorNone;
}

DisplayError DisplayVirtual::GetColorModeAttr(const std::string &color_mode, AttrVal *attr) {
  ClientLock lock(disp_mutex_);
  if (!attr) {
    return kErrorParameters;
  }

  auto it = color_mode_attr_map_.find(color_mode);
  if (it == color_mode_attr_map_.end()) {
    DLOGI("Mode %s has no attribute for display %d-%d", color_mode.c_str(), display_id_,
          display_type_);
    return kErrorNotSupported;
  }
  *attr = it->second;

  return kErrorNone;
}

static PrimariesTransfer GetBlendSpaceFromAttributes(const std::string &color_gamut,
                                                     const std::string &transfer) {
  PrimariesTransfer blend_space_ = {};
  if (color_gamut == kNative) {  // Native mode is identified by Max
    blend_space_.primaries = QtiColorPrimaries_Max;
    blend_space_.transfer = QtiTransfer_Max;
  } else if (color_gamut == kBt2020) {
    blend_space_.primaries = QtiColorPrimaries_BT2020;
    if (transfer == kHlg) {
      blend_space_.transfer = QtiTransfer_HLG;
    } else if (transfer == kSt2084) {
      blend_space_.transfer = QtiTransfer_SMPTE_ST2084;
    } else if (transfer == kSrgb) {
      blend_space_.transfer = QtiTransfer_sRGB;
    }
  } else if (color_gamut == kSrgb) {
    blend_space_.primaries = QtiColorPrimaries_BT709_5;
    blend_space_.transfer = QtiTransfer_sRGB;
  } else {
    DLOGW("Failed to Get blend space color_gamut = %s transfer = %s", color_gamut.c_str(),
          transfer.c_str());
  }
  DLOGI("Blend Space Primaries = %d Transfer = %d", blend_space_.primaries, blend_space_.transfer);

  return blend_space_;
}

DisplayError DisplayVirtual::SetColorMode(const std::string &color_mode) {
  auto current_color_attr_ = color_mode_attr_map_.find(color_mode);
  if (current_color_attr_ == color_mode_attr_map_.end()) {
    DLOGE("Failed to get the color mode for display %d-%d = %s", display_id_, display_type_,
          color_mode.c_str());
    return kErrorNone;
  }
  AttrVal attr = current_color_attr_->second;
  std::string color_gamut = kNative, transfer = {};

  if (attr.begin() != attr.end()) {
    for (auto &it : attr) {
      if (it.first.find(kColorGamutAttribute) != std::string::npos) {
        color_gamut = it.second;
      } else if (it.first.find(kGammaTransferAttribute) != std::string::npos) {
        transfer = it.second;
      }
    }
  }

  DisplayError error = kErrorNone;
  PrimariesTransfer blend_space = GetBlendSpaceFromAttributes(color_gamut, transfer);
  error = comp_manager_->SetBlendSpace(display_comp_ctx_, blend_space);
  if (error != kErrorNone) {
    DLOGE("Failed Set blend space, error = %d for display %d-%d", error, display_id_,
          display_type_);
  }

  error = dpu_core_mux_->SetBlendSpace(blend_space);
  if (error != kErrorNone) {
    DLOGE("Failed to pass blend space, error = %d for display %d-%d", error, display_id_,
          display_type_);
  }

  current_color_mode_ = color_mode;
  DLOGI(
      "Set color mode %s for display %d-%d, blend_space.primaries = %d, blend_space.transfer = %d",
      color_mode.c_str(), display_id_, display_type_, blend_space.primaries, blend_space.transfer);
  return kErrorNone;
}

#undef __CLASS__
#define __CLASS__ "DisplayVirtualPQ"

DisplayVirtualPQ::DisplayVirtualPQ(DisplayId display_id, DisplayEventHandler *event_handler,
                                   sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                                   BufferAllocator *buffer_allocator, CompManager *comp_manager,
                                   const std::vector<Hdr> &hdr_types, float max_lum, float min_lum)
    : DisplayVirtual(display_id, event_handler, hw_info_intf, buffer_allocator, comp_manager,
                     hdr_types, max_lum, min_lum) {}

DisplayError DisplayVirtualPQ::Init() {
  ClientLock lock(disp_mutex_);

  // Initialize base virtual display logic
  DisplayError error = DisplayVirtual::Init();
  if (error != kErrorNone) {
    DLOGE("Failed to init virtual display base, error=%d", error);
    return error;
  }

  // Fetch necessary device/client contexts and mixer attributes
  uint32_t active_index = 0;
  dpu_core_mux_->GetActiveConfig(&active_index);
  dpu_core_mux_->GetDisplayAttributes(active_index, &device_ctx_, &client_ctx_);
  dpu_core_mux_->GetHWPanelInfo(&device_ctx_, &client_ctx_);

  error = dpu_core_mux_->GetMixerAttributes(&device_ctx_, &client_ctx_);
  if (error != kErrorNone) {
    DLOGE("Failed to get mixer attributes, error=%d", error);
    return error;
  }

  // Create ColorManager to support color features (STC)
  auto dpps_intf = comp_manager_->GetDppsControlIntf();
  auto color_mgr_factory = GetColorMgrFactoryIntf();
  if (color_mgr_factory) {
    DLOGI("Creating color_mgr_ for virtual display_id=%d", display_id_info_.GetDisplayId());

    // The WB connector has no panel_name, so STC cannot locate calibration files.
    // Override panel_name with the configured virtual panel name before creating ColorManager.
    DisplayClientContext client_ctx_for_color = client_ctx_;
    if (!panel_name_.empty()) {
      snprintf(client_ctx_for_color.hw_panel_info.panel_name,
               sizeof(client_ctx_for_color.hw_panel_info.panel_name), "%s", panel_name_.c_str());
      DLOGI("Panel name: %s", panel_name_.c_str());
    }
    color_mgr_ = color_mgr_factory->CreateColorManagerIntf(
        display_type_, dpu_core_mux_, device_ctx_, client_ctx_for_color, dpps_intf, this,
        hw_resource_info_, display_id_info_);
    if (!color_mgr_) {
      DLOGE("Failed to create color_mgr_");
    } else {
      DLOGI("color_mgr_ created successfully for virtual display");
    }
  } else {
    DLOGE("Failed to get color manager factory interface");
  }

  if (color_mgr_) {
    color_mgr_->ColorMgrGetStcModes(&stc_color_modes_);
  }

  // This is a dummy interface used to ensure LTM init succeeds.
  // Failure of this intf should not impact the overall feature.
  prop_intf_ = hw_intf_->GetPanelFeaturePropertyIntf();
  if (!prop_intf_) {
    DLOGE("Failed to create PanelFeaturePropertyIntf");
  }

  return kErrorNone;
}

PrimariesTransfer DisplayVirtualPQ::GetBlendSpaceFromStcColorMode(
    const snapdragoncolor::ColorMode &color_mode) {
  PrimariesTransfer blend_space = {};
  if (!color_mgr_) {
    DLOGE("color_mgr_ is not initialized");
    return blend_space;
  }

  // Set sRGB as default blend space.
  bool native_mode = (color_mode.intent == snapdragoncolor::kNative) ||
                     (color_mode.gamut == ColorPrimaries_Max && color_mode.gamma == Transfer_Max);
  if (stc_color_modes_.list.empty() || (native_mode && allow_tonemap_native_)) {
    return blend_space;
  }

  blend_space.primaries = qti_primaries_map[color_mode.gamut];
  blend_space.transfer = qti_transfer_map[color_mode.gamma];

  return blend_space;
}

DisplayError DisplayVirtualPQ::GetStcColorModes(snapdragoncolor::ColorModeList *mode_list) {
  ClientLock lock(disp_mutex_);
  if (!mode_list) {
    DLOGE("Invalid mode_list pointer");
    return kErrorParameters;
  }

  if (!color_mgr_) {
    DLOGE("color_mgr_ is not initialized");
    return kErrorNotSupported;
  }

  mode_list->list = stc_color_modes_.list;
  return kErrorNone;
}

DisplayError DisplayVirtualPQ::SetStcColorMode(const snapdragoncolor::ColorMode &color_mode) {
  ClientLock lock(disp_mutex_);
  DisplayError ret = kErrorNone;
  PrimariesTransfer blend_space = {};

  if (!color_mgr_) {
    DLOGE("color_mgr_ is not initialized");
    return kErrorNotSupported;
  }

  // Get and set blend space on composition manager
  blend_space = GetBlendSpaceFromStcColorMode(color_mode);
  if (display_comp_ctx_) {
    ret = comp_manager_->SetBlendSpace(display_comp_ctx_, blend_space);
    if (ret != kErrorNone) {
      DLOGE("SetBlendSpace failed, ret=%d on display %d-%d", ret, display_id_, display_type_);
    }
  }

  // Set blend space on DPU
  ret = dpu_core_mux_->SetBlendSpace(blend_space);
  if (ret != kErrorNone) {
    DLOGE("Failed to pass blend space to DPU, ret=%d on display %d-%d", ret, display_id_,
          display_type_);
  }

  // Apply STC mode in ColorManager
  ret = color_mgr_->ColorMgrSetStcMode(color_mode);
  if (ret != kErrorNone) {
    DLOGE("Failed to set STC color mode, ret=%d on display %d-%d", ret, display_id_, display_type_);
    return ret;
  }

  current_color_mode_ = color_mode;

  // Evaluate and update dynamic range and DPPS control
  DynamicRangeType dynamic_range = kSdrType;
  if (std::find(color_mode.hw_assets.begin(), color_mode.hw_assets.end(),
                snapdragoncolor::kPbHdrBlob) != color_mode.hw_assets.end()) {
    dynamic_range = kHdrType;
  }
  if ((color_mode.gamut == ColorPrimaries_BT2020 && color_mode.gamma == Transfer_SMPTE_ST2084) ||
      (color_mode.gamut == ColorPrimaries_BT2020 && color_mode.gamma == Transfer_HLG)) {
    dynamic_range = kHdrType;
  }

  comp_manager_->ControlDpps(dynamic_range != kHdrType);

  DLOGI("Set STC color mode on display %d-%d: gamut %d, gamma %d, intent %d", display_id_,
        display_type_, color_mode.gamut, color_mode.gamma, color_mode.intent);

  return ret;
}

DisplayError DisplayVirtualPQ::PostCommit() {
  DisplayError error = DisplayVirtual::PostCommit();
  if (error != kErrorNone) {
    return error;
  }

  dpps_info_.Init(this, panel_name_, this, prop_intf_);
  return kErrorNone;
}

DisplayError DisplayVirtualPQ::TurnOffColorFeature() {
  int display_type = display_type_;

  DLOGV_IF(kTagDisplay, "Turn off ltm feature on display %d-%d", display_id_, display_type_);

  dpps_info_.DppsNotifyOps(kDppsLtmForceOffEvent, &display_type, sizeof(display_type));
  return kErrorNone;
}

DisplayError DisplayVirtualPQ::DppsProcessOps(enum DppsOps op, void *payload, size_t size) {
  DisplayError error = kErrorNone;
  DppsDisplayInfo *info = nullptr;

  switch (op) {
    case kDppsSetFeature:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      {
        ClientLock lock(disp_mutex_);
        error = dpu_core_mux_->SetDppsFeature(payload, size);
      }
      break;
    case kDppsGetFeatureInfo:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      error = dpu_core_mux_->GetDppsFeatureInfo(payload, size);
      break;
    case kDppsScreenRefresh:
      if (event_handler_) {
        event_handler_->Refresh();
      }
      break;
    case kDppsPartialUpdate:
      // Partial update is not supported on virtual display.
      break;
    case kDppsGetDisplayInfo:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      info = reinterpret_cast<DppsDisplayInfo *>(payload);
      info->width = client_ctx_.display_attributes.x_pixels;
      info->height = client_ctx_.display_attributes.y_pixels;
      info->is_primary = false;
      info->display_id = display_id_;
      info->display_type = display_type_;
      info->fps = client_ctx_.display_attributes.fps;
      info->flags |= kDppsFlagVirtualDispNeedsLtm;

      error = dpu_core_mux_->GetPanelBrightnessBasePath(&(info->brightness_base_path));
      if (error != kErrorNone) {
        DLOGE("Failed to get brightness base path %d", error);
      }
      break;
    case kDppsSetPccConfig:
      if (color_mgr_) {
        error = color_mgr_->ColorMgrSetLtmPccConfig(payload, size);
        if (error != kErrorNone) {
          DLOGE("Failed to set PCC config to ColorManagerProxy, error %d", error);
        }
      }
      break;
    default:
      DLOGE("Invalid input op %d", op);
      error = kErrorParameters;
      break;
  }
  return error;
}

std::string DisplayVirtualPQ::Dump() {
  std::ostringstream os;
  os << DisplayBase::Dump();

  os << "\n------ DisplayVirtualPQ ------";
  os << "\n needs_dspp_: " << NeedsDspp();
  os << "\n color_mgr_: " << (color_mgr_ ? "yes" : "no");

  DynamicRangeType curr_dynamic_range = kSdrType;
  if (std::find(current_color_mode_.hw_assets.begin(), current_color_mode_.hw_assets.end(),
                snapdragoncolor::kPbHdrBlob) != current_color_mode_.hw_assets.end()) {
    curr_dynamic_range = kHdrType;
  }
  os << "\nCurrent Color Mode: gamut " << current_color_mode_.gamut << " gamma "
     << current_color_mode_.gamma << " intent " << current_color_mode_.intent << " Dynamice_range"
     << (curr_dynamic_range == kSdrType ? " SDR" : " HDR");

  return os.str();
}

}  // namespace sdm

