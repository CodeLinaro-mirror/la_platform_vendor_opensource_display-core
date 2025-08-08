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
                               BufferAllocator *buffer_allocator, CompManager *comp_manager)
    : DisplayBase(kVirtual, event_handler, kDeviceVirtual, buffer_allocator, comp_manager,
                  hw_info_intf) {}

DisplayVirtual::DisplayVirtual(DisplayId display_id, DisplayEventHandler *event_handler,
                               sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                               BufferAllocator *buffer_allocator, CompManager *comp_manager)
    : DisplayBase(display_id, kVirtual, event_handler, kDeviceVirtual, buffer_allocator,
                  comp_manager, hw_info_intf) {}

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

  InitializeColorModes();

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

  if (set_max_lum_ != -1.0 || set_min_lum_ != -1.0) {
    client_ctx.hw_panel_info.peak_luminance = set_max_lum_;
    client_ctx.hw_panel_info.blackness_level = set_min_lum_;
    DLOGI("set peak_luminance %f blackness_level %f for display %d-%d", display_id_,
          display_type_, client_ctx.hw_panel_info.peak_luminance,
          client_ctx.hw_panel_info.blackness_level);
  }

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
  set_max_lum_ = max_lum;
  set_min_lum_ = min_lum;
  return kErrorNone;
}

DisplayError DisplayVirtual::colorSamplingOn() {
    return kErrorNone;
}

DisplayError DisplayVirtual::colorSamplingOff() {
    return kErrorNone;
}

DisplayError DisplayVirtual::InitializeColorModes() {
  PrimariesTransfer pt = {};
  AttrVal var = {};
  int sink_support = 0, i = 0;

  Debug::Get()->GetProperty("vendor.display.wcm.sink_support", &sink_support);

  if (sink_support) {
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
    pt.primaries = QtiColorPrimaries_BT2020;
    pt.transfer = QtiTransfer_SMPTE_ST2084;
    var.clear();
    var.push_back(std::make_pair(kColorGamutAttribute, kBt2020));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
    var.push_back(std::make_pair(kGammaTransferAttribute, kSt2084));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kBt2020Pq, var));

    // BT2020_HLG
    pt.primaries = QtiColorPrimaries_BT2020;
    pt.transfer = QtiTransfer_SMPTE_ST2084;
    var.clear();
    var.push_back(std::make_pair(kColorGamutAttribute, kBt2020));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
    var.push_back(std::make_pair(kGammaTransferAttribute, kHlg));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kBt2020Hlg, var));

    current_color_mode_ = kDisplayBt2020;
  } else {
    // SRGB mode
    pt.primaries = QtiColorPrimaries_BT709_5;
    pt.transfer = QtiTransfer_sRGB;
    var.push_back(std::make_pair(kColorGamutAttribute, kSrgb));
    var.push_back(std::make_pair(kDynamicRangeAttribute, kSdr));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kSrgb, var));

    current_color_mode_ = kSrgb;
  }

  num_color_modes_ = UINT32(color_mode_attr_map_.size());
  color_modes_.resize(num_color_modes_);
  for (ColorModeAttrMap::iterator it = color_mode_attr_map_.begin();
       ((i < num_color_modes_) && (it != color_mode_attr_map_.end())); i++, it++) {
    color_modes_[i].id = INT32(i);
    std::size_t length = (it->first).copy(color_modes_[i].name, sizeof(SDEDisplayMode::name) - 1);
    color_modes_[i].name[length] = '\0';
    color_mode_map_.insert(std::make_pair(color_modes_[i].name, &color_modes_[i]));
    DLOGI("Sink support = %d, Color mode[%d] = %s", sink_support, i, color_modes_[i].name);
  }

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

}  // namespace sdm

