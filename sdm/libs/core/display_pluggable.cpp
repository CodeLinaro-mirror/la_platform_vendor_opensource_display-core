/*
* Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
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

/* Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <utils/constants.h>
#include <utils/debug.h>
#include <private/hw_interface.h>
#include <private/hw_info_interface.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "display_pluggable.h"

#define __CLASS__ "DisplayPluggable"

namespace sdm {


static uint64_t GetTimeInMs(struct timespec ts) {
  return (ts.tv_sec * 1000 + (ts.tv_nsec + 500000) / 1000000);
}

DisplayPluggable::DisplayPluggable(DisplayEventHandler *event_handler,
                                   sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                                   BufferAllocator *buffer_allocator, CompManager *comp_manager)
    : DisplayBase(kPluggable, event_handler, kDevicePluggable, buffer_allocator, comp_manager,
                  hw_info_intf) {}

DisplayPluggable::DisplayPluggable(DisplayId display_id, DisplayEventHandler *event_handler,
                                   sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                                   BufferAllocator *buffer_allocator, CompManager *comp_manager)
    : DisplayBase(display_id, kPluggable, event_handler, kDevicePluggable, buffer_allocator,
                  comp_manager, hw_info_intf) {}

DisplayError DisplayPluggable::Init() {
  ClientLock lock(disp_mutex_);

  DisplayError error = DPUCoreFactory::Create(display_id_info_, kPluggable, hw_info_intf_,
                                              buffer_allocator_, &dpu_core_mux_);
  if (error != kErrorNone) {
    if (kErrorDeviceRemoved == error) {
      DLOGW("Aborted creating hardware interface. Device removed.");
    } else {
      DLOGE("Failed to create hardware interface. Error = %d", error);
    }
    return error;
  }

  dpu_core_mux_->GetHWInterface(&hw_intf_);
  if (!hw_intf_) {
    DLOGW("Invalid value for hw_intf_.");
    return kErrorParameters;
  }

  if (-1 == display_id_info_.GetDisplayId()) {
    dpu_core_mux_->GetDisplayId(&display_id_);
    display_id_info_ = DisplayId(display_id_);
    core_id_ = display_id_info_.GetCoreIdMap();
    std::bitset<32> core_id_bitset = std::bitset<32>(core_id_);
    core_count_ = core_id_bitset.count();
  }

  uint32_t active_mode_index = 0;
  error = dpu_core_mux_->GetActiveConfig(&active_mode_index);
  if (error != kErrorNone) {
    dpu_core_mux_->Destroy();
    return error;
  }

  uint32_t override_mode_index = active_mode_index;
  error = GetOverrideConfig(&override_mode_index);
  if (error == kErrorNone && override_mode_index != active_mode_index) {
    DLOGI("Overriding display mode %d with mode %d.", active_mode_index, override_mode_index);
    error = dpu_core_mux_->SetDisplayAttributes(override_mode_index);
    if (error != kErrorNone) {
      DLOGI("Failed overriding display mode %d with mode %d. Continuing with display mode %d.",
            active_mode_index, override_mode_index, active_mode_index);
    }
  }

  error = DisplayBase::Init();
  if (error == kErrorResources) {
    DLOGI("Reattempting display creation for Pluggable display %d-%d", display_id_, display_type_);
    uint32_t default_mode_index = 0;
    error = dpu_core_mux_->GetDefaultConfig(&default_mode_index);
    if (error == kErrorNone) {
      dpu_core_mux_->SetDisplayAttributes(default_mode_index);
      error = DisplayBase::Init();
    } else {
      DLOGE("640x480 default mode not found, failing creation!");
    }
  }
  if (error != kErrorNone) {
    dpu_core_mux_->Destroy();
    return error;
  }

  GetScanSupport();
  underscan_supported_ = (scan_support_ == kScanAlwaysUnderscanned) || (scan_support_ == kScanBoth);

  std::vector<HWEvent> events = {HWEvent::VSYNC, HWEvent::EXIT, HWEvent::CEC_READ_MESSAGE,
                                 HWEvent::HW_RECOVERY, HWEvent::POWER_EVENT};
  std::bitset<8> core_id_map = display_id_info_.GetCoreIdMap();
  for (int i = 0; i < core_id_map.size(); i++) {
    if (!core_id_map[i]) {
      continue;
    }

    event_list_[i] = events;
    primary_core_id_ = i;
    break;
  }

  error = HWEventsInterface::Create(display_id_info_, kPluggable, this, event_list_,
                                    &hw_events_intf_);
  if (error != kErrorNone) {
    DisplayBase::Deinit();
    dpu_core_mux_->Destroy();
    DLOGE("Failed to create hardware events interface. Error = %d for display %d-%d", error,
          display_id_, display_type_);
  }

  master_hw_events_intf_ = hw_events_intf_[primary_core_id_];

  if (master_hw_events_intf_)
    hw_intf_->SetPageFlipState(true, (void *)master_hw_events_intf_);

  // if qdcm colormodes are not enabled, initialize colormodes by using panel info(EOTF).
  if (enable_qdcm_colormodes_on_external_ == QdcmOnExternal::NO_QDCM) {
    InitializeColorModes();
  } else if (enable_qdcm_colormodes_on_external_ == QdcmOnExternal::STC_QDCM) { //stc
    if (color_mgr_) {
      color_mgr_->ColorMgrGetStcModes(&stc_color_modes_);
    }
  }

  /* If the panel feature is not supported, do not create event_intf to avoid the display
   * being incorrectly released.
   */
  if (prop_intf_) {
    error = event_proxy_info_.Init(client_ctx_.hw_panel_info.panel_name, this,
                                   extension_lib_, prop_intf_);
    if (error != kErrorNone) {
      DLOGW("Failed to initialize event proxy info");
      event_proxy_info_.Deinit();
      error = kErrorNone;
    }
  }

  if (pf_factory_ && prop_intf_) {
    // Get status of RC enablement property. Default RC is disabled.
    int rc_prop_value = 0;
    Debug::GetProperty(ENABLE_ROUNDED_CORNER, &rc_prop_value);

    if (rc_prop_value && client_ctx_.hw_panel_info.is_rc_supported && EnableRC()) {
      rc_enable_prop_ = true;
    }
  }

  DLOGI("RC feature %s on %s for display %d-%d, is_rc_supported %d",
         rc_enable_prop_ ? "enabled" : "disabled",
         client_ctx_.hw_panel_info.is_primary_panel ? "primary" : "secondary",
         display_id_, display_type_, client_ctx_.hw_panel_info.is_rc_supported);

  current_refresh_rate_ = client_ctx_.hw_panel_info.max_fps;

  int value = 0;
  DebugHandler::Get()->GetProperty(ENABLE_QSYNC_IDLE, &value);
  enable_qsync_idle_ = client_ctx_.hw_panel_info.qsync_support && (value == 1);
  if (enable_qsync_idle_) {
    DLOGI("Enabling qsync on idling");

    if (client_ctx_.hw_panel_info.transfer_time_us_min) {
      DLOGI("Setting transfer time to min: %d", client_ctx_.hw_panel_info.transfer_time_us_min);
      UpdateTransferTime(client_ctx_.hw_panel_info.transfer_time_us_min);
    }
  }

  return error;
}

DisplayError DisplayPluggable::Deinit() {
  ClientLock lock(disp_mutex_);

  for (auto &res_info : hw_resource_info_)
    hw_rc_blocks_in_use_[res_info.core_id] -= rc_blocks_reserved_;

  event_proxy_info_.Deinit();
  return DisplayBase::Deinit();
}

DisplayError DisplayPluggable::GetStcColorModes(snapdragoncolor::ColorModeList *mode_list) {
  ClientLock lock(disp_mutex_);
  if (!mode_list) {
    return kErrorParameters;
  }

  if (!color_mgr_) {
    return kErrorNotSupported;
  }

  mode_list->list = stc_color_modes_.list;
  return kErrorNone;
}

PrimariesTransfer DisplayPluggable::GetBlendSpaceFromStcColorMode(
    const snapdragoncolor::ColorMode &color_mode) {
  PrimariesTransfer blend_space = {};
  if (!color_mgr_) {
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


DisplayError DisplayPluggable::SetStcColorMode(const snapdragoncolor::ColorMode &color_mode) {
  ClientLock lock(disp_mutex_);
  if (!color_mgr_) {
    return kErrorNotSupported;
  }
  DisplayError ret = kErrorNone;
  PrimariesTransfer blend_space = {};
  blend_space = GetBlendSpaceFromStcColorMode(color_mode);
  ret = comp_manager_->SetBlendSpace(display_comp_ctx_, blend_space);
  if (ret != kErrorNone) {
    DLOGE("SetBlendSpace failed, ret = %d on display %d-%d", ret, display_id_, display_type_);
  }

  ret = dpu_core_mux_->SetBlendSpace(blend_space);
  if (ret != kErrorNone) {
    DLOGE("Failed to pass blend space, ret = %d on display %d-%d", ret, display_id_,
          display_type_);
  }

  ret = color_mgr_->ColorMgrSetStcMode(color_mode);
  if (ret != kErrorNone) {
    DLOGE("Failed to set stc color mode, ret = %d on display %d-%d", ret,
          display_id_, display_type_);
    return ret;
  }

  current_stc_color_mode_ = color_mode;

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

  return ret;
}


DisplayError DisplayPluggable::HandleSPR() {
  if (spr_) {
    GenericPayload out;
    uint32_t *enable = nullptr;
    int ret = out.CreatePayload<uint32_t>(enable);
    if (ret) {
      DLOGE("Failed to create the payload. Error:%d", ret);
      validated_ = false;
      return kErrorUndefined;
    }
    ret = spr_->GetParameter(kSPRFeatureEnable, &out);
    if (ret) {
      DLOGE("Failed to get the spr status. Error:%d", ret);
      validated_ = false;
      return kErrorUndefined;
    }
    spr_enable_ = *enable;
  }

  return kErrorNone;
}


DisplayError DisplayPluggable::Prepare(LayerStack *layer_stack) {
  DTRACE_SCOPED();
  ClientLock lock(disp_mutex_);
  DisplayError error = kErrorNone;
  uint32_t new_mixer_width = 0;
  uint32_t new_mixer_height = 0;
  uint32_t display_width = client_ctx_.display_attributes.x_pixels;
  uint32_t display_height = client_ctx_.display_attributes.y_pixels;

  error = PrePrepare(layer_stack);
  if (error == kErrorNone) {
    return error;
  }

  if (error == kErrorNeedsLutRegen && (ForceToneMapUpdate(layer_stack) == kErrorNone)) {
    return kErrorNone;
  }

  if (NeedsMixerReconfiguration(layer_stack, &new_mixer_width, &new_mixer_height)) {
    error = ReconfigureMixer(new_mixer_width, new_mixer_height);
    if (error != kErrorNone) {
      ReconfigureMixer(display_width, display_height);
    }
  }

  error = DisplayBase::Prepare(layer_stack);
  if (error != kErrorNone) {
    return error;
  }

  UpdateQsyncConfig();

  return kErrorNone;
}

DisplayError DisplayPluggable::GetRefreshRateRange(uint32_t *min_refresh_rate,
                                                   uint32_t *max_refresh_rate) {
  ClientLock lock(disp_mutex_);
  DisplayError error = kErrorNone;

  if (client_ctx_.hw_panel_info.min_fps && client_ctx_.hw_panel_info.max_fps) {
    *min_refresh_rate = client_ctx_.hw_panel_info.min_fps;
    *max_refresh_rate = client_ctx_.hw_panel_info.max_fps;
  } else {
    error = DisplayBase::GetRefreshRateRange(min_refresh_rate, max_refresh_rate);
  }

  return error;
}

DisplayError DisplayPluggable::SetRefreshRate(uint32_t refresh_rate, bool final_rate,
                                              bool idle_screen) {
  ClientLock lock(disp_mutex_);

  if (!active_) {
    return kErrorPermission;
  }

  if (current_refresh_rate_ != refresh_rate) {
    DisplayError error = dpu_core_mux_->SetRefreshRate(refresh_rate);
    if (error != kErrorNone) {
      return error;
    }
  }

  current_refresh_rate_ = refresh_rate;
  return DisplayBase::ReconfigureDisplay();
}

bool DisplayPluggable::IsUnderscanSupported() {
  ClientLock lock(disp_mutex_);
  return underscan_supported_;
}

DisplayError DisplayPluggable::GetOverrideConfig(uint32_t *mode_index) {
  DisplayError error = kErrorNone;

  if (!mode_index) {
    DLOGE("Invalid mode index parameter.");
    return kErrorParameters;
  }

  char val[kPropertyMax] = {};
  // Used for changing HDMI Resolution - Override the preferred mode with user set config.
  bool user_config = Debug::GetExternalResolution(val);
  if (user_config) {
    uint32_t config_index = 0;
    // For the config, get the corresponding index
    error = dpu_core_mux_->GetConfigIndex(val, &config_index);
    if (error == kErrorNone) {
      *mode_index = config_index;
    }
  }

  return error;
}

void DisplayPluggable::GetScanSupport() {
  DisplayError error = kErrorNone;
  uint32_t video_format = 0;
  uint32_t max_cea_format = 0;
  HWScanInfo scan_info = HWScanInfo();
  dpu_core_mux_->GetHWScanInfo(&scan_info);

  uint32_t active_mode_index = 0;
  dpu_core_mux_->GetActiveConfig(&active_mode_index);

  error = dpu_core_mux_->GetVideoFormat(active_mode_index, &video_format);
  if (error != kErrorNone) {
    return;
  }

  error = dpu_core_mux_->GetMaxCEAFormat(&max_cea_format);
  if (error != kErrorNone) {
    return;
  }

  // The scan support for a given HDMI TV must be read from scan info corresponding to
  // Preferred Timing if the preferred timing of the display is currently active, and if it is
  // valid. In all other cases, we must read the scan support from CEA scan info if
  // the resolution is a CEA resolution, or from IT scan info for all other resolutions.
  if (active_mode_index == 0 && scan_info.pt_scan_support != kScanNotSupported) {
    scan_support_ = scan_info.pt_scan_support;
  } else if (video_format < max_cea_format) {
    scan_support_ = scan_info.cea_scan_support;
  } else {
    scan_support_ = scan_info.it_scan_support;
  }
}

void DisplayPluggable::CECMessage(char *message) {
  event_handler_->CECMessage(message);
}

// HWEventHandler overload, not DisplayBase
void DisplayPluggable::HwRecovery(const HWRecoveryEvent sdm_event_code) {
  DisplayBase::HwRecovery(sdm_event_code);
}

void DisplayPluggable::Histogram(int /* histogram_fd */, uint32_t /* blob_id */) {}

void DisplayPluggable::HandleBacklightEvent(float /* brightness_level */) {}

DisplayError DisplayPluggable::VSync(int64_t timestamp) {
  // if (vsync_enable_) {
  //   DisplayEventVSync vsync;
  //   vsync.timestamp = timestamp;
  //   event_handler_->VSync(vsync);
  // }
  DTRACE_SCOPED();
  bool qsync_enabled = enable_qsync_idle_ && (active_qsync_mode_ != kQSyncModeNone);
  // Client isn't aware of underlying qsync mode.
  // Disable vsync propagation as long as qsync is enabled.
  bool propagate_vsync = vsync_enable_ && !drop_hw_vsync_ && !qsync_enabled;
  if (!propagate_vsync) {
    // Re enable when display updates.
    SetVsyncStatus(false /*Disable vsync events.*/);
    return kErrorNone;
  }

  DisplayEventVSync vsync;
  vsync.timestamp = timestamp;
  event_handler_->VSync(vsync);

  return kErrorNone;
}

void DisplayPluggable::SetVsyncStatus(bool enable) {
  string trace_name = enable ? "enable" : "disable";
  DTRACE_BEGIN(trace_name.c_str());
  if (enable) {
    // Enable if vsync is still enabled.
    vsync_enable_pending_ |= vsync_enable_;
    vsync_enable_ = false;
    SetVSyncStateLocked(vsync_enable_pending_);
  } else {
    master_hw_events_intf_->SetEventState(HWEvent::VSYNC, false);
  }
  DTRACE_END();
}

DisplayError DisplayPluggable::PFlip(int fd,
                                unsigned int sequence,
                                unsigned int tv_sec,
                                unsigned int tv_usec,
                                void *data) {
  if (pflip_enable_) {
    event_handler_->PFlip(fd, sequence, tv_sec, tv_usec, data);
  }

  return kErrorNone;
}

DisplayError DisplayPluggable::InitializeColorModes() {
  PrimariesTransfer pt = {};
  AttrVal var = {};
  bool hdr_supported = true;

  for (auto& res_info : hw_resource_info_) {
    hdr_supported &= res_info.has_hdr;
  }

  if ((!client_ctx_.hw_panel_info.hdr_enabled &&
       !client_ctx_.hw_panel_info.supported_colorspaces) ||
      !hdr_supported) {
    return kErrorNone;
  } else {
    if (client_ctx_.hw_panel_info.supported_colorspaces) {
      InitializeColorModesFromColorspace();
    }
    color_modes_cs_.push_back(pt);
    var.push_back(std::make_pair(kColorGamutAttribute, kSrgb));
    var.push_back(std::make_pair(kDynamicRangeAttribute, kSdr));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
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
  }

  var.clear();
  var.push_back(std::make_pair(kColorGamutAttribute, kBt2020));
  var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
  var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
  if (client_ctx_.hw_panel_info.hdr_eotf & kHdrEOTFHDR10) {
    pt.transfer = QtiTransfer_SMPTE_ST2084;
    var.push_back(std::make_pair(kGammaTransferAttribute, kSt2084));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kBt2020Pq, var));
  }
  if (client_ctx_.hw_panel_info.hdr_eotf & kHdrEOTFHLG) {
    pt.transfer = QtiTransfer_HLG;
    var.pop_back();
    var.push_back(std::make_pair(kGammaTransferAttribute, kHlg));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kBt2020Hlg, var));
  }
  current_color_mode_ = kSrgb;
  UpdateColorModes();

  return kErrorNone;
}

void DisplayPluggable::InitializeColorModesFromColorspace() {
  PrimariesTransfer pt = {};
  AttrVal var = {};
  if (client_ctx_.hw_panel_info.supported_colorspaces & kColorspaceDcip3) {
    pt.primaries = QtiColorPrimaries_DCIP3;
    pt.transfer = QtiTransfer_sRGB;
    var.clear();
    var.push_back(std::make_pair(kColorGamutAttribute, kDcip3));
    var.push_back(std::make_pair(kGammaTransferAttribute, kSrgb));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kDisplayP3, var));
  }
  if (client_ctx_.hw_panel_info.supported_colorspaces & kColorspaceBt2020rgb) {
    pt.primaries = QtiColorPrimaries_BT2020;
    pt.transfer = QtiTransfer_sRGB;
    var.clear();
    var.push_back(std::make_pair(kColorGamutAttribute, kBt2020));
    var.push_back(std::make_pair(kGammaTransferAttribute, kSrgb));
    var.push_back(std::make_pair(kPictureQualityAttribute, kStandard));
    var.push_back(std::make_pair(kRenderIntentAttribute, "0"));
    color_modes_cs_.push_back(pt);
    color_mode_attr_map_.insert(std::make_pair(kDisplayBt2020, var));
  }
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
    } else if (transfer == kGamma2_2) {
      blend_space_.transfer = QtiTransfer_Gamma2_2;
    }
  } else if (color_gamut == kDcip3) {
    blend_space_.primaries = QtiColorPrimaries_DCIP3;
    blend_space_.transfer = QtiTransfer_sRGB;
  } else if (color_gamut == kSrgb) {
    blend_space_.primaries = QtiColorPrimaries_BT709_5;
    blend_space_.transfer = QtiTransfer_sRGB;
  } else {
    DLOGW("Failed to Get blend space color_gamut = %s transfer = %s",
          color_gamut.c_str(), transfer.c_str());
  }
  DLOGI("Blend Space Primaries = %d Transfer = %d", blend_space_.primaries, blend_space_.transfer);

  return blend_space_;
}

DisplayError DisplayPluggable::SetColorMode(const std::string &color_mode) {
  if (enable_qdcm_colormodes_on_external_ == QdcmOnExternal::LEGACY_QDCM) {
    return DisplayBase::SetColorMode(color_mode);
  }
  auto current_color_attr_ = color_mode_attr_map_.find(color_mode);
  if (current_color_attr_ == color_mode_attr_map_.end()) {
    DLOGE("Failed to get the color mode for display %d-%d = %s", display_id_,
          display_type_, color_mode.c_str());
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
    DLOGE("Failed Set blend space, error = %d for display %d-%d", error,
          display_id_, display_type_);
  }

  error = dpu_core_mux_->SetBlendSpace(blend_space);
  if (error != kErrorNone) {
    DLOGE("Failed to pass blend space, error = %d for display %d-%d", error,
    display_id_, display_type_);
  }

  current_color_mode_ = color_mode;

  return kErrorNone;
}

DisplayError DisplayPluggable::GetColorModeCount(uint32_t *mode_count) {
  ClientLock lock(disp_mutex_);
  if (enable_qdcm_colormodes_on_external_ == QdcmOnExternal::LEGACY_QDCM) {
    return DisplayBase::GetColorModeCount(mode_count);
  }

  if (!mode_count) {
    return kErrorParameters;
  }

  DLOGI("Display = %d Number of modes = %d", display_type_, num_color_modes_);
  *mode_count = num_color_modes_;

  return kErrorNone;
}

DisplayError DisplayPluggable::GetColorModes(uint32_t *mode_count,
                                             std::vector<std::string> *color_modes) {
  ClientLock lock(disp_mutex_);
  if (enable_qdcm_colormodes_on_external_ == QdcmOnExternal::LEGACY_QDCM) {
    return DisplayBase::GetColorModes(mode_count, color_modes);
  }

  if (!mode_count || !color_modes) {
    return kErrorParameters;
  }

  for (uint32_t i = 0; i < num_color_modes_; i++) {
    DLOGI_IF(kTagDisplay, "ColorMode[%d] = %s", i, color_modes_[i].name);
    color_modes->at(i) = color_modes_[i].name;
  }

  return kErrorNone;
}

DisplayError DisplayPluggable::GetColorModeAttr(const std::string &color_mode, AttrVal *attr) {
  ClientLock lock(disp_mutex_);
  if (enable_qdcm_colormodes_on_external_ == QdcmOnExternal::LEGACY_QDCM) {
    return DisplayBase::GetColorModeAttr(color_mode, attr);
  }

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

void DisplayPluggable::UpdateColorModes() {
  uint32_t i = 0;
  num_color_modes_ = UINT32(color_mode_attr_map_.size());
  color_modes_.resize(num_color_modes_);
  for (ColorModeAttrMap::iterator it = color_mode_attr_map_.begin();
       ((i < num_color_modes_) && (it != color_mode_attr_map_.end())); i++, it++) {
    color_modes_[i].id = INT32(i);
    std::size_t length = (it->first).copy(color_modes_[i].name, sizeof(SDEDisplayMode::name) - 1);
    color_modes_[i].name[length] = '\0';
    color_mode_map_.insert(std::make_pair(color_modes_[i].name, &color_modes_[i]));
    DLOGI("Color mode = %s", color_modes_[i].name);
  }
  return;
}

DisplayError DisplayPluggable::colorSamplingOn() {
  return kErrorNone;
}

DisplayError DisplayPluggable::colorSamplingOff() {
  return kErrorNone;
}

void DisplayPluggable::MMRMEvent(uint32_t clk) {
  // Stub for future support
  return;
}

void DisplayPluggable::HandlePowerEvent() {
  return ProcessPowerEvent();
}

void DisplayPluggable::HandleVmReleaseEvent() {
}

void DisplayPluggable::GetDRMDisplayToken(uint32_t core_id, sde_drm::DRMDisplayToken *token) {
  dpu_core_mux_->GetDRMDisplayToken(core_id, token);
}

bool DisplayPluggable::IsPrimaryDisplay() {
  return DisplayBase::IsPrimaryDisplay();
}



CacVersion DisplayPluggable::GetCacVerion() {
  int cac_version = 0;
  for (int i = 0; i < core_count_; i++) {
    cac_version |= hw_resource_info_[i].cac_version;
  }

  return static_cast<CacVersion>(cac_version);
}

DisplayError DisplayPluggable::SetPaHistCollection(
    const std::string &client_name, bool enable,
    SdmDisplayCbInterface<PaHistCollectionPayload> *cb_intf) {
  return event_proxy_info_.SetPaHistCollection(client_name, enable, cb_intf);
}

DisplayError DisplayPluggable::GetPaHistBins(std::array<uint32_t, HIST_BIN_SIZE> *buf) {
  return event_proxy_info_.GetPaHistBins(buf);
}

DisplayError DisplayPluggable::GetPanelBrightnessBasePath(std::string *base_path) {
  return dpu_core_mux_->GetPanelBrightnessBasePath(base_path);
}

DisplayError DisplayPluggable::NotifyDisplayCalibrationMode(bool in_calibration) {
  ClientLock lock(disp_mutex_);
  if (!color_mgr_) {
    return kErrorNotSupported;
  }

  DisplayError ret = kErrorNone;
  ret = color_mgr_->NotifyDisplayCalibrationMode(in_calibration);
  if (ret != kErrorNone) {
    DLOGE("Failed to notify QDCM Mode status, ret = %d state = %d", ret, in_calibration);
  }

  return ret;
}
DisplayError DisplayPluggable::SetAVRStepState(bool enable) {
  ClientLock lock(disp_mutex_);

  if (enable && (client_ctx_.hw_panel_info.mode == kModeVideo)) {
    if (!client_ctx_.hw_panel_info.qsync_support || (qsync_mode_ == kQSyncModeNone)) {
      DLOGW("AVR Step feature is not supported without QSync on VID mode");
      return kErrorNotSupported;
    }
  }

  if (avr_step_enabled_ == enable) {
    DLOGI("AVR Step already set in requested state %d", enable);
    return kErrorNone;
  }

  avr_step_enabled_ = enable;
  needs_avr_update_.set(kUpdateAVRStepFlag);
  validated_ = false;
  event_handler_->Refresh();
  DLOGI("AVR Step state set to %d successfully", avr_step_enabled_);

  return kErrorNone;
}

DisplayError DisplayPluggable::SetVRRState(bool state) {
  if (!hw_intf_->IsVRRSupported()) {
    DLOGI("hw_intf does not support VRR");
    return kErrorNotSupported;
  }

  uint32_t active_index = 0;
  dpu_core_mux_->GetActiveConfig(&active_index);
  avr_step_ = hw_intf_->GetAVRStep(active_index);
  if (avr_step_ != 0) {
    DLOGI("Set VRR state %d in config %d", state, active_index);
    SetQSyncMode(state ? kQSyncModeContinuous : kQSyncModeNone);
    DisplayError error = SetAVRStepState(state);
    if (error != kErrorNone) {
      return error;
    }
  }

  vrr_enabled_ = state;
  return kErrorNone;
}

DisplayError DisplayPluggable::GetQSyncMode(QSyncMode *qsync_mode) {
  *qsync_mode = active_qsync_mode_;
  return kErrorNone;
}

DisplayError DisplayPluggable::SetQSyncMode(QSyncMode qsync_mode) {
  ClientLock lock(disp_mutex_);

  if (!client_ctx_.hw_panel_info.qsync_support) {
    DLOGW("Failed: qsync_support: %d", client_ctx_.hw_panel_info.qsync_support);
    return kErrorNotSupported;
  }

  // force clear qsync mode if set by idle timeout.
  if (qsync_mode_ ==  active_qsync_mode_ && qsync_mode_ == qsync_mode) {
    DLOGW("Qsync mode already set as requested mode: qsync_mode_=%d", qsync_mode_);
    return kErrorNone;
  }

  qsync_mode_ = qsync_mode;
  needs_avr_update_.set(kUpdateAVRModeFlag);
  validated_ = false;
  event_handler_->Refresh();
  return kErrorNone;
}

std::string DisplayPluggable::Dump() {
  ClientLock lock(disp_mutex_);
  uint32_t active_index = 0;
  uint32_t num_modes = 0;
  std::ostringstream os;
  char capabilities[16];
  CacVersion cac_version = GetCacVerion();
  HWPanelInfo hw_panel_info = client_ctx_.hw_panel_info;
  HWDisplayAttributes display_attributes = client_ctx_.display_attributes;
  HWMixerAttributes mixer_attributes = client_ctx_.mixer_attributes;

  dpu_core_mux_->GetNumDisplayAttributes(&num_modes);
  dpu_core_mux_->GetActiveConfig(&active_index);

  os << "device type:" << display_type_;
  os << " DrawMethod: " << draw_method_;
  os << "\nstate: " << state_ << " vsync on: " << vsync_enable_
     << " max. mixer stages: " << max_mixer_stages_;
  if (disp_layer_stack_->stack_info.noise_layer_info.enable) {
    os << "\nNoise z-orders: [" << disp_layer_stack_->stack_info.noise_layer_info.zpos_noise
       << "," << disp_layer_stack_->stack_info.noise_layer_info.zpos_attn << "]";
  }
  os << "\nnum configs: " << num_modes << " active config index: " << active_index;
  os << "\nDisplay Attributes:";
  os << "\n Mode:" << (hw_panel_info.mode == kModeVideo ? "Video" : "Command");
  os << std::boolalpha;
  os << " Primary:" << hw_panel_info.is_primary_panel;
  os << " DynFPS:" << hw_panel_info.dynamic_fps;
  os << "\n HDR Panel:" << hw_panel_info.hdr_enabled;
  os << " QSync:" << hw_panel_info.qsync_support;
  os << " DynBitclk:" << hw_panel_info.dyn_bitclk_support;
  os << "\n Left Split:" << hw_panel_info.split_info.left_split
     << " Right Split:" << hw_panel_info.split_info.right_split;
  os << "\n PartialUpdate:" << hw_panel_info.partial_update;
  if (hw_panel_info.partial_update) {
    os << "\n ROI Min w:" << hw_panel_info.min_roi_width;
    os << " Min h:" << hw_panel_info.min_roi_height;
    os << " NeedsMerge: " << hw_panel_info.needs_roi_merge;
    os << " Alignment: l:" << hw_panel_info.left_align << " w:" << hw_panel_info.width_align;
    os << " t:" << hw_panel_info.top_align << " b:" << hw_panel_info.height_align;
  }
  os << "\n FPS min:" << hw_panel_info.min_fps << " max:" << hw_panel_info.max_fps
     << " cur:" << display_attributes.fps;
  os << " TransferTime: " << hw_panel_info.transfer_time_us << "us";
  os << " Min TransferTime: " << hw_panel_info.transfer_time_us_min << "us";
  os << " Max TransferTime: " << hw_panel_info.transfer_time_us_max << "us";
  os << " AllowedModeSwitch: " << hw_panel_info.allowed_mode_switch;
  os << " PanelModeCaps: ";
  snprintf(capabilities, sizeof(capabilities), "0x%x", hw_panel_info.panel_mode_caps);
  os << capabilities;
  os << " MaxBrightness:" << hw_panel_info.panel_max_brightness;
  os << "\n Display WxH: " << display_attributes.x_pixels << "x" << display_attributes.y_pixels;
  os << " MixerWxH: " << mixer_attributes.width << "x" << mixer_attributes.height;
  os << " DPI: " << display_attributes.x_dpi << "x" << display_attributes.y_dpi;
  os << " LM_Split: " << display_attributes.is_device_split;
  os << "\n vsync_period " << display_attributes.vsync_period_ns;
  os << " v_back_porch: " << display_attributes.v_back_porch;
  os << " v_front_porch: " << display_attributes.v_front_porch;
  os << " v_pulse_width: " << display_attributes.v_pulse_width;
  os << "\n v_total: " << display_attributes.v_total;
  os << " h_total: " << display_attributes.h_total;
  os << " clk: " << display_attributes.clock_khz;
  os << " Topology: " << display_attributes.topology;
  os << " Qsync mode: " << active_qsync_mode_;
  os << " CAC enabled: " << disp_layer_stack_->stack_info.enable_cac;
  os << (disp_layer_stack_->stack_info.enable_cac
             ? (cac_version == kCacVersionLoopback) ? " (CACLoopback)" : " (CACV2)"
             : "");
  os << "\n Foveation enabled: " << disp_layer_stack_->stack_info.enable_anamorphic_fov;
  os << (disp_layer_stack_->stack_info.enable_anamorphic_fov ? " (Anamorphic Foveation)" : "");
  os << std::noboolalpha;

  DynamicRangeType curr_dynamic_range = kSdrType;
  if (std::find(current_stc_color_mode_.hw_assets.begin(), current_stc_color_mode_.hw_assets.end(),
                snapdragoncolor::kPbHdrBlob) != current_stc_color_mode_.hw_assets.end()) {
    curr_dynamic_range = kHdrType;
  }
  os << "\nCurrent Color Mode: gamut " << current_stc_color_mode_.gamut << " gamma "
     << current_stc_color_mode_.gamma << " intent " << current_stc_color_mode_.intent << " Dynamice_range"
     << (curr_dynamic_range == kSdrType ? " SDR" : " HDR");

  for (int j = 0; j < core_count_; j++) {
    uint32_t core_id = hw_resource_info_[j].core_id;
    uint32_t num_hw_layers = UINT32(disp_layer_stack_->info.at(core_id).hw_layers.size());

    if (num_hw_layers == 0) {
      os << "\nNo hardware layers programmed";
      return os.str();
    }

    os << "\n\n Table for DPU - " << j << "\n";
    if (cwb_active_) {
      os << "\n Output buffer res: " << cwb_output_buf_.width << "x" << cwb_output_buf_.height
         << " format: " << GetFormatString(cwb_output_buf_.format);
    }

    HWLayersInfo &layer_info = disp_layer_stack_->info.at(core_id);
    for (uint32_t i = 0; i < layer_info.left_frame_roi.size(); i++) {
      LayerRect &l_roi = layer_info.left_frame_roi.at(i);
      LayerRect &r_roi = layer_info.right_frame_roi.at(i);

      os << "\nROI(LTRB)#" << i << " LEFT(" << INT(l_roi.left) << " " << INT(l_roi.top) << " " <<
        INT(l_roi.right) << " " << INT(l_roi.bottom) << ")";
      if (IsValid(r_roi)) {
      os << " RIGHT(" << INT(r_roi.left) << " " << INT(r_roi.top) << " " << INT(r_roi.right) << " "
        << INT(r_roi.bottom) << ")";
      }
    }

    LayerRect &fb_roi = disp_layer_stack_->stack_info.partial_fb_roi;
    if (IsValid(fb_roi)) {
      os << "\nPartial FB ROI(LTRB):(" << INT(fb_roi.left) << " " << INT(fb_roi.top) << " " <<
        INT(fb_roi.right) << " " << INT(fb_roi.bottom) << ")";
    }

    AppendRCMaskData(os);

    const char *header  = "\n| Idx |   Comp Type   |   Split   | Pipe |    W x H    |          Format          |  Src Rect (L T R B) |  Dst Rect (L T R B) |  Z | Pipe Flags | Deci(HxV) | CS | Rng | Tr |";  //NOLINT
    const char *newline = "\n|-----|---------------|-----------|------|-------------|--------------------------|---------------------|---------------------|----|------------|-----------|----|-----|----|";  //NOLINT
    const char *format  = "\n| %3s | %13s | %9s | %4d | %4d x %4d | %24s | %4d %4d %4d %4d | %4d %4d %4d %4d | %2s | %10s | %9s | %2s | %3s | %2s |";  //NOLINT

    os << "\n";
    os << newline;
    os << header;
    os << newline;

    for (uint32_t i = 0; i < num_hw_layers; i++) {
      uint32_t layer_index = disp_layer_stack_->info.at(core_id).index.at(i);
      // hw-layer from hw layers info
      Layer &hw_layer = disp_layer_stack_->info.at(core_id).hw_layers.at(i);
      LayerBuffer *input_buffer = &hw_layer.input_buffer;
      HWLayerConfig &layer_config = disp_layer_stack_->info.at(core_id).config[i];
      HWRotatorSession &hw_rotator_session = layer_config.hw_rotator_session;

      const char *comp_type = GetCompositionName(hw_layer.composition);
      const char *buffer_format = GetFormatString(input_buffer->format);
      const char *pipe_split[2] = { "Pipe-1", "Pipe-2" };
      const char *rot_pipe[2] = { "Rot-inl-1", "Rot-inl-2" };
      char idx[8];

      snprintf(idx, sizeof(idx), "%d", layer_index);

      for (uint32_t count = 0; count < hw_rotator_session.hw_block_count; count++) {
        char row[1024];
        HWRotateInfo &rotate = hw_rotator_session.hw_rotate_info[count];
        LayerRect &src_roi = rotate.src_roi;
        LayerRect &dst_roi = rotate.dst_roi;
        char rot[12] = { 0 };

        snprintf(rot, sizeof(rot), "Rot-%s-%d", layer_config.use_inline_rot ?
                 "inl" : "off", count + 1);

        snprintf(row, sizeof(row), format, idx, comp_type, rot,
                 0, input_buffer->width, input_buffer->height, buffer_format,
                 INT(src_roi.left), INT(src_roi.top), INT(src_roi.right), INT(src_roi.bottom),
                 INT(dst_roi.left), INT(dst_roi.top), INT(dst_roi.right), INT(dst_roi.bottom),
                 "-", "-    ", "-    ", "-", "-", "-");
        os << row;
        // print the below only once per layer block, fill with spaces for rest.
        idx[0] = 0;
        comp_type = "";
      }

      if (hw_rotator_session.hw_block_count > 0) {
        input_buffer = &hw_rotator_session.output_buffer;
        buffer_format = GetFormatString(input_buffer->format);
      }

      if (layer_config.use_solidfill_stage) {
        LayerRect src_roi = layer_config.hw_solidfill_stage.roi;
        const char *decimation = "";
        char flags[16] = { 0 };
        char z_order[8] = { 0 };
        const char *color_primary = "";
        const char *range = "";
        const char *transfer = "";
        char row[1024] = { 0 };

        snprintf(z_order, sizeof(z_order), "%d", layer_config.hw_solidfill_stage.z_order);
        snprintf(flags, sizeof(flags), "0x%08x", hw_layer.flags.flags);
        snprintf(row, sizeof(row), format, idx, comp_type, pipe_split[0],
                 0, INT(src_roi.right), INT(src_roi.bottom),
                 buffer_format, INT(src_roi.left), INT(src_roi.top),
                 INT(src_roi.right), INT(src_roi.bottom), INT(src_roi.left),
                 INT(src_roi.top), INT(src_roi.right), INT(src_roi.bottom),
                 z_order, flags, decimation, color_primary, range, transfer);
        os << row;
        continue;
      }

      for (uint32_t count = 0; count < 2; count++) {
        char decimation[16] = { 0 };
        char flags[16] = { 0 };
        char z_order[8] = { 0 };
        char color_primary[8] = { 0 };
        char range[8] = { 0 };
        char transfer[8] = { 0 };
        bool rot = layer_config.use_inline_rot;

        HWPipeInfo &pipe = (count == 0) ? layer_config.left_pipe : layer_config.right_pipe;

        if (!pipe.valid) {
          continue;
        }

        LayerRect src_roi = pipe.src_roi;
        LayerRect &dst_roi = pipe.dst_roi;

        snprintf(z_order, sizeof(z_order), "%d", pipe.z_order);
        snprintf(flags, sizeof(flags), "0x%08x", pipe.flags);
        snprintf(decimation, sizeof(decimation), "%3d x %3d", pipe.horizontal_decimation,
                 pipe.vertical_decimation);
        Dataspace &color_metadata = hw_layer.input_buffer.dataspace;
        snprintf(color_primary, sizeof(color_primary), "%d", color_metadata.colorPrimaries);
        snprintf(range, sizeof(range), "%d", color_metadata.range);
        snprintf(transfer, sizeof(transfer), "%d", color_metadata.transfer);

        char row[1024];
        snprintf(row, sizeof(row), format, idx, comp_type, rot ? rot_pipe[count] :
                 pipe_split[count], pipe.pipe_id, input_buffer->width, input_buffer->height,
                 buffer_format, INT(src_roi.left), INT(src_roi.top),
                 INT(src_roi.right), INT(src_roi.bottom), INT(dst_roi.left),
                 INT(dst_roi.top), INT(dst_roi.right), INT(dst_roi.bottom),
                 z_order, flags, decimation, color_primary, range, transfer);

        os << row;
        // print the below only once per layer block, fill with spaces for rest.
        idx[0] = 0;
        comp_type = "";
      }
    }
    os << comp_manager_->Dump(display_comp_ctx_);
    os << newline << "\n";
  }
  return os.str();
}


void DisplayPluggable::InitCWBBuffer() {
  if (client_ctx_.hw_panel_info.mode != kModeVideo || !HasConcurrentWriteback()
      || !client_ctx_.hw_panel_info.is_primary_panel) {
    return;
  }

  if (disable_cwb_idle_fallback_ || cwb_buffer_initialized_) {
    return;
  }

  // Initialize CWB buffer with display resolution to get full size buffer
  // as mixer or fb can init with custom values based on property
  output_buffer_info_.buffer_config.width = client_ctx_.display_attributes.x_pixels;
  output_buffer_info_.buffer_config.height = client_ctx_.display_attributes.y_pixels;

  output_buffer_info_.buffer_config.format = kFormatRGBX8888Ubwc;
  output_buffer_info_.buffer_config.buffer_count = 1;
  if (buffer_allocator_->AllocateBuffer(&output_buffer_info_) != 0) {
    DLOGE("Buffer allocation failed");
    return;
  }

  LayerBuffer buffer = {};
  buffer.planes[0].fd = output_buffer_info_.alloc_buffer_info.fd;
  buffer.planes[0].offset = 0;
  buffer.planes[0].stride = output_buffer_info_.alloc_buffer_info.stride;
  buffer.size = output_buffer_info_.alloc_buffer_info.size;
  buffer.handle_id = output_buffer_info_.alloc_buffer_info.id;
  buffer.width = output_buffer_info_.alloc_buffer_info.aligned_width;
  buffer.height = output_buffer_info_.alloc_buffer_info.aligned_height;
  buffer.format = output_buffer_info_.alloc_buffer_info.format;
  buffer.unaligned_width = output_buffer_info_.buffer_config.width;
  buffer.unaligned_height = output_buffer_info_.buffer_config.height;

  cwb_layer_.composition = kCompositionCWBTarget;
  cwb_layer_.input_buffer = buffer;
  cwb_layer_.input_buffer.buffer_id = reinterpret_cast<uint64_t>(output_buffer_info_.private_data);
  cwb_layer_.src_rect = {0, 0, FLOAT(cwb_layer_.input_buffer.unaligned_width),
                         FLOAT(cwb_layer_.input_buffer.unaligned_height)};
  cwb_layer_.dst_rect = {0, 0, FLOAT(cwb_layer_.input_buffer.unaligned_width),
                         FLOAT(cwb_layer_.input_buffer.unaligned_height)};

  cwb_layer_.flags.is_cwb = 1;
  cwb_buffer_initialized_ = true;
  return;
}

void DisplayPluggable::DeinitCWBBuffer() {
  if (!cwb_buffer_initialized_) {
    return;
  }

  buffer_allocator_->FreeBuffer(&output_buffer_info_);
  cwb_layer_ = {};
  cwb_buffer_initialized_ = false;
}

void DisplayPluggable::AppendCWBLayer(LayerStack *layer_stack) {
  if (cwb_buffer_initialized_ &&
      (cwb_layer_.input_buffer.unaligned_width < client_ctx_.display_attributes.x_pixels ||
       cwb_layer_.input_buffer.unaligned_height < client_ctx_.display_attributes.y_pixels)) {
    DLOGI("Resetting CWB layer due to insufficient buffer size(%dx%d) compare to output(%dx%d).",
          cwb_layer_.input_buffer.unaligned_width, cwb_layer_.input_buffer.unaligned_height,
          client_ctx_.display_attributes.x_pixels, client_ctx_.display_attributes.y_pixels);
    DeinitCWBBuffer();
  }

  if (!cwb_buffer_initialized_) {
    // If CWB buffer is not initialized, then it must be initialized for video mode
    InitCWBBuffer();
  }

  if (!client_ctx_.hw_panel_info.is_primary_panel || disable_cwb_idle_fallback_ ||
      !cwb_buffer_initialized_) {
    return;
  }

  uint32_t new_mixer_width = client_ctx_.fb_config.x_pixels;
  uint32_t new_mixer_height = client_ctx_.fb_config.y_pixels;
  NeedsMixerReconfiguration(layer_stack, &new_mixer_width, &new_mixer_height);
  // Set cwb src_rect same as mixer resolution since LM tappoint
  // and dest_rect equal to fb resolution as strategy scales HWLayer dest rect based on fb
  cwb_layer_.src_rect = {0, 0, FLOAT(new_mixer_width), FLOAT(new_mixer_height)};
  cwb_layer_.dst_rect = {0, 0, FLOAT(client_ctx_.fb_config.x_pixels),
                         FLOAT(client_ctx_.fb_config.y_pixels)};
  cwb_layer_.composition = kCompositionCWBTarget;
  layer_stack->layers.push_back(&cwb_layer_);
}

DisplayError DisplayPluggable::HandleDemuraLayer(LayerStack *layer_stack) {
  if (!layer_stack) {
    DLOGE("layer_stack is null");
    return kErrorParameters;
  }
  std::vector<Layer *> &layers = layer_stack->layers;
  if (comp_manager_->GetDemuraStatus() && comp_manager_->GetDemuraStatusForDisplay(display_id_) &&
      demura_layer_[0].input_buffer.planes[0].fd > 0) {
    if (disp_layer_stack_->stack_info.demura_target_index == -1) {
      // If demura layer added for first time, do not skip validate
      needs_validate_ = true;
    }

    for (int buf_idx = 0; buf_idx < demura_layer_.size(); buf_idx++) {
      layers.push_back(&demura_layer_.at(buf_idx));
    }

    DLOGI_IF(kTagDisplay, "Demura layer added to layer stack on display %d-%d", display_id_,
             display_type_);
  } else if (disp_layer_stack_->stack_info.demura_target_index != -1) {
    // Demura was present last frame but is now disabled
    needs_validate_ = true;
    disp_layer_stack_->stack_info.demura_present = false;
    DLOGD_IF(kTagDisplay, "Demura layer to be removed on display %d-%d in this frame",
             display_id_, display_type_);
  }
  return kErrorNone;
}


DisplayError DisplayPluggable::UpdateTransferTime(uint32_t transfer_time) {
  DisplayError error = kErrorNone;
  {
    ClientLock lock(disp_mutex_);

    if (!active_) {
      DLOGW("Invalid display state = %d. Panel must be on.", state_);
      return kErrorNotSupported;
    }

    if (transfer_time == client_ctx_.hw_panel_info.transfer_time_us) {
      DLOGW("Same transfer time requested. Current = %d, Requested = %d",
            client_ctx_.hw_panel_info.transfer_time_us, transfer_time);
      return kErrorNone;
    } else if (transfer_time > client_ctx_.hw_panel_info.transfer_time_us_max ||
               transfer_time < client_ctx_.hw_panel_info.transfer_time_us_min) {
      DLOGW(
          "Invalid transfer time requested or panel info missing valid range. Min = %d, Max = %d, "
          "Requested = %d, Current = %d",
          client_ctx_.hw_panel_info.transfer_time_us_min,
          client_ctx_.hw_panel_info.transfer_time_us_max, transfer_time,
          client_ctx_.hw_panel_info.transfer_time_us);
      return kErrorParameters;
    }

    error = hw_intf_->UpdateTransferTime(transfer_time);
    if (error != kErrorNone) {
      DLOGW("Retaining the older transfer time.");
      return error;
    }

    DLOGV_IF(kTagDisplay, "Updated transfer time to %d", transfer_time);

    DisplayBase::ReconfigureDisplay();
  }

  event_handler_->Refresh();

  return error;
}


DisplayError DisplayPluggable::PrePrepare(LayerStack *layer_stack) {
  DTRACE_SCOPED();
  uint32_t new_mixer_width = 0;
  uint32_t new_mixer_height = 0;
  uint32_t display_width = client_ctx_.display_attributes.x_pixels;
  uint32_t display_height = client_ctx_.display_attributes.y_pixels;
  GenericPayload bool_payload;
  bool *force_update;

  DisplayError error = HandleDemuraLayer(layer_stack);
  if (error != kErrorNone) {
    return error;
  }

  error = HandleSPR();
  if (error != kErrorNone) {
    return error;
  }
  disp_layer_stack_->stack_info.spr_enable = spr_enable_;

  AppendCWBLayer(layer_stack);

  error = DisplayBase::PrePrepare(layer_stack);
  if (error == kErrorNone || error == kErrorNeedsLutRegen) {
    return error;
  }

  if (NeedsMixerReconfiguration(layer_stack, &new_mixer_width, &new_mixer_height)) {
    error = ReconfigureMixer(new_mixer_width, new_mixer_height);
    if (error != kErrorNone) {
      ReconfigureMixer(display_width, display_height);
    }
  } else {
    if (CanSkipDisplayPrepare(layer_stack)) {
      UpdateQsyncConfig();
      return kErrorNone;
    }
  }
  error = ChangeFps();
  lower_fps_ = disp_layer_stack_->stack_info.lower_fps;

  if (color_mgr_ && client_ctx_.hw_panel_info.mode == kModeVideo && idle_fallback_on_dspp_) {
    CwbTapPoint tap_point = CwbTapPoint::kDsppTapPoint;
    bool destination_scaler =
        (client_ctx_.display_attributes.x_pixels != client_ctx_.mixer_attributes.width ||
         client_ctx_.display_attributes.y_pixels != client_ctx_.mixer_attributes.height);
    tap_point = destination_scaler ? CwbTapPoint::kLmTapPoint : CwbTapPoint::kDsppTapPoint;
    if (tap_point == CwbTapPoint::kDsppTapPoint) {
      color_mgr_->ColorMgrIdleFallback(lower_fps_);
      needs_validate_ |= color_mgr_->IsValidateNeeded();
    }
  }

  if (ssrc_feature_enabled_) {
    if (bool_payload.CreatePayload(force_update) != 0) {
      DLOGE("Unable to create force update payload");
      return kErrorMemory;
    }

    *force_update = false;
    if (ssrc_feature_interface_->SetParameter(aiqe::kSsrcFeatureCommitFeature, bool_payload) != 0) {
      DLOGE("Unable to set commit on SSRC feature interface");
      return kErrorNotSupported;
    }
  }

  return kErrorNotValidated;
}


bool DisplayPluggable::CanSkipDisplayPrepare(LayerStack *layer_stack) {
  if (!CanCompareFrameROI(layer_stack)) {
    return false;
  }

  if (disp_layer_stack_->stack_info.iwe_target_index != -1) {
    return false;
  }

  for (auto& info : disp_layer_stack_->info) {
    info.second.left_frame_roi.clear();
    info.second.right_frame_roi.clear();
    info.second.dest_scale_info_map.clear();
  }
  comp_manager_->GenerateROI(display_comp_ctx_, disp_layer_stack_);

  for (int i = 0; i < core_count_; i++) {
    uint32_t core_id = hw_resource_info_[i].core_id;
    if (!disp_layer_stack_->info.at(core_id).left_frame_roi.size() ||
        !disp_layer_stack_->info.at(core_id).right_frame_roi.size()) {
      return false;
    }

    // Compare the cached and calculated Frame ROIs.
    bool same_roi = IsCongruent(left_frame_roi_[i],
                                disp_layer_stack_->info.at(core_id).left_frame_roi.at(0)) &&
                    IsCongruent(right_frame_roi_[i],
                                disp_layer_stack_->info.at(core_id).right_frame_roi.at(0));

    if (!same_roi) {
      return same_roi;
    }
  }

  for (auto& info : disp_layer_stack_->info) {
    // Update Surface Damage rectangle(s) in HW layers.
    uint32_t hw_layer_count = UINT32(info.second.hw_layers.size());
    for (uint32_t j = 0; j < hw_layer_count; j++) {
      Layer &hw_layer = info.second.hw_layers.at(j);
      Layer *sdm_layer = layer_stack->layers.at(info.second.index.at(j));
      if (hw_layer.dirty_regions.size() != sdm_layer->dirty_regions.size()) {
        return false;
      }
      for (uint32_t k = 0; k < hw_layer.dirty_regions.size(); k++) {
        hw_layer.dirty_regions.at(k) = sdm_layer->dirty_regions.at(k);
      }
    }

    // Set the composition type for SDM layers.
    size_t size_ff = 1;  // GPU Target Layer always present in input
    if (layer_stack->flags.stitch_present)
      size_ff++;
    if (layer_stack->flags.demura_present)
      size_ff++;
    if (disp_layer_stack_->stack_info.common_info.flags.noise_present)
      size_ff++;

    for (uint32_t j = 0; j < (layer_stack->layers.size() - size_ff); j++) {
      layer_stack->layers.at(j)->composition = kCompositionSDE;
    }
  }

  return true;
}


void DisplayPluggable::UpdateQsyncConfig() {
  // QSync and AVR Step features are de-coupled on CMD Mode panel.
  if ((client_ctx_.hw_panel_info.mode == kModeVideo) && !client_ctx_.hw_panel_info.qsync_support) {
    return;
  }

  // Get qsync min fps for the current mode
  uint32_t qsync_mode_min_fps = 0;
  dpu_core_mux_->GetQsyncFps(&qsync_mode_min_fps);
  QSyncMode mode = kQSyncModeNone;
  if (!qsync_mode_min_fps) {
    // Set qsync mode to 0 when the current mode doesn't support it.
    mode = kQSyncModeNone;
    DLOGV_IF(kTagDisplay, "Qsync disabled as current mode doesn't support it");
  } else if (lower_fps_ && enable_qsync_idle_) {
    // Override to continuous mode upon idling.
    mode = kQSyncModeContinuous;
    DLOGV_IF(kTagDisplay, "Qsync entering continuous mode");
  } else {
    // Set Qsync mode requested by client.
    mode = qsync_mode_;
    DLOGV_IF(kTagDisplay, "Restoring display %d-%d client's qsync mode: %d", display_id_,
             display_type_, mode);
  }

  disp_layer_stack_->stack_info.common_info.hw_avr_info.update = needs_avr_update_;
  if (mode != active_qsync_mode_) {
    disp_layer_stack_->stack_info.common_info.hw_avr_info.update.set(kUpdateAVRModeFlag);
  }
  disp_layer_stack_->stack_info.common_info.hw_avr_info.mode = GetAvrMode(mode);
  disp_layer_stack_->stack_info.common_info.hw_avr_info.step_enabled = avr_step_enabled_;

  DLOGV_IF(kTagDisplay, "display %d-%d update: %lu mode: %u AVR Step state: %d", display_id_,
           display_type_, disp_layer_stack_->stack_info.common_info.hw_avr_info.update.to_ulong(), mode,
           avr_step_enabled_);

  // Store active mode.
  active_qsync_mode_ = mode;
}


DisplayError DisplayPluggable::ChangeFps() {
  ClientLock lock(disp_mutex_);

  if (!active_ || !client_ctx_.hw_panel_info.dynamic_fps || qsync_mode_ != kQSyncModeNone ||
      disable_dyn_fps_) {
    return kErrorNotSupported;
  }

  uint32_t num_updating_layers = GetUpdatingLayersCount();
  bool one_updating_layer = (num_updating_layers == 1);
  uint32_t refresh_rate = GetOptimalRefreshRate(one_updating_layer);

  if (refresh_rate < client_ctx_.hw_panel_info.min_fps ||
      refresh_rate > client_ctx_.hw_panel_info.max_fps) {
    DLOGE("Invalid Fps = %d request", refresh_rate);
    return kErrorParameters;
  }

  bool idle_screen = GetUpdatingAppLayersCount(disp_layer_stack_->stack) == 0;
  if (!disp_layer_stack_->stack->force_refresh_rate && IdleFallbackLowerFps(idle_screen) &&
      !enable_qsync_idle_) {
    refresh_rate = client_ctx_.hw_panel_info.min_fps;
  }

  if (current_refresh_rate_ != refresh_rate) {
    DisplayError error = dpu_core_mux_->SetRefreshRate(refresh_rate);
    if (error != kErrorNone) {
      // Attempt to update refresh rate can fail if rf interference settings is detected.
      // Just drop min fps settting for now.
      if (disp_layer_stack_->stack_info.lower_fps) {
        disp_layer_stack_->stack_info.lower_fps = false;
      }
      return error;
    }

    error = comp_manager_->CheckEnforceSplit(display_comp_ctx_, refresh_rate);
    if (error != kErrorNone) {
      return error;
    }
  }

  // Set safe mode upon success.
  if (enhance_idle_time_ && (refresh_rate == client_ctx_.hw_panel_info.min_fps) &&
      (disp_layer_stack_->stack_info.lower_fps)) {
    comp_manager_->ProcessIdleTimeout(display_comp_ctx_);
  }

  // On success, set current refresh rate to new refresh rate
  current_refresh_rate_ = refresh_rate;
  deferred_config_.MarkDirty();

  return ReconfigureDisplay();
}

bool DisplayPluggable::IdleFallbackLowerFps(bool idle_screen) {
  if (!enhance_idle_time_) {
    return (disp_layer_stack_->stack_info.lower_fps);
  }
  if (!idle_screen || !disp_layer_stack_->stack_info.lower_fps) {
    return false;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t elapsed_time_ms = GetTimeInMs(now) - GetTimeInMs(idle_timer_start_);
  bool can_lower = elapsed_time_ms >= UINT32(idle_time_ms_);
  DLOGV_IF(kTagDisplay, "display %d-%d , lower fps: %d", display_id_, display_type_, can_lower);

  return can_lower;
}


bool DisplayPluggable::CanCompareFrameROI(LayerStack *layer_stack) {
  // Check Display validation and safe-mode states.
  if (needs_validate_ || comp_manager_->IsSafeMode() || layer_stack->needs_validate) {
    return false;
  }

  // Check Panel and Layer Stack attributes.
  int8_t stack_fudge_factor = 1;  // GPU Target Layer always present in input
  if (layer_stack->flags.stitch_present)
    stack_fudge_factor++;
  if (layer_stack->flags.demura_present)
    stack_fudge_factor++;

  if (!client_ctx_.hw_panel_info.partial_update || (client_ctx_.hw_panel_info.left_roi_count != 1)
      || layer_stack->flags.geometry_changed || layer_stack->flags.skip_present ||
      (layer_stack->layers.size() !=
       (disp_layer_stack_->stack_info.app_layer_count + stack_fudge_factor))) {
    return false;
  }

  // Check for Partial Update disable requests/scenarios.
  if (color_mgr_ && color_mgr_->NeedsPartialUpdateDisable()) {
    DisablePartialUpdateOneFrameInternal();
  }

  if (!partial_update_control_ || disable_pu_one_frame_) {
    return false;
  }

  bool surface_damage = false;
  uint32_t surface_damage_mask_value = (1 << kSurfaceDamage);
  for (uint32_t i = 0; i < layer_stack->layers.size(); i++) {
    Layer *layer = layer_stack->layers.at(i);
    if (layer->update_mask.none()) {
      continue;
    }
    // Only kSurfaceDamage bit should be set in layer's update-mask.
    if (layer->update_mask.to_ulong() == surface_damage_mask_value) {
      surface_damage = true;
    } else {
      return false;
    }
  }

  return surface_damage;
}

HWAVRModes DisplayPluggable::GetAvrMode(QSyncMode mode) {
  switch (mode) {
     case kQSyncModeNone:
       return kQsyncNone;
     case kQSyncModeContinuous:
       return kContinuousMode;
     case kQsyncModeOneShot:
     case kQsyncModeOneShotContinuous:
       return kOneShotMode;
     default:
       return kQsyncNone;
  }
}

uint32_t DisplayPluggable::GetUpdatingLayersCount() {
  uint32_t updating_count = 0;

  for (uint i = 0; i < disp_layer_stack_->stack->layers.size(); i++) {
    auto layer = disp_layer_stack_->stack->layers.at(i);
    if (layer->flags.updating) {
      updating_count++;
    }
  }
  return updating_count;
}

uint32_t DisplayPluggable::GetOptimalRefreshRate(bool one_updating_layer) {
  LayerStack *layer_stack = disp_layer_stack_->stack;
  if (layer_stack->force_refresh_rate) {
    return layer_stack->force_refresh_rate;
  }

  uint32_t metadata_refresh_rate = CalculateMetaDataRefreshRate();
  if (layer_stack->flags.use_metadata_refresh_rate && one_updating_layer &&
      metadata_refresh_rate) {
    return metadata_refresh_rate;
  }

  return active_refresh_rate_;
}

uint32_t DisplayPluggable::GetUpdatingAppLayersCount(LayerStack *layer_stack) {
  uint32_t updating_count = 0;

  for (uint i = 0; i < layer_stack->layers.size(); i++) {
    auto layer = layer_stack->layers.at(i);
    if (layer->composition == kCompositionGPUTarget) {
      break;
    }
    if (layer->flags.updating) {
      updating_count++;
    }
  }

  return updating_count;
}

uint32_t DisplayPluggable::CalculateMetaDataRefreshRate() {
  LayerStack *layer_stack = disp_layer_stack_->stack;
  uint32_t metadata_refresh_rate = 0;
  if (!layer_stack->flags.use_metadata_refresh_rate) {
    return 0;
  }

  uint32_t max_refresh_rate = 0;
  uint32_t min_refresh_rate = 0;
  GetRefreshRateRange(&min_refresh_rate, &max_refresh_rate);

  for (uint i = 0; i < layer_stack->layers.size(); i++) {
    auto layer = layer_stack->layers.at(i);
    if (layer->flags.has_metadata_refresh_rate && layer->frame_rate > metadata_refresh_rate) {
      metadata_refresh_rate = SanitizeRefreshRate(layer->frame_rate, max_refresh_rate,
                                                  min_refresh_rate);
    }
  }
  return metadata_refresh_rate;
}

uint32_t DisplayPluggable::SanitizeRefreshRate(uint32_t req_refresh_rate, uint32_t max_refresh_rate,
                                             uint32_t min_refresh_rate) {
  uint32_t refresh_rate = req_refresh_rate;

  if (refresh_rate < min_refresh_rate) {
    // Pick the next multiple of request which is within the range
    refresh_rate = (((min_refresh_rate / refresh_rate) +
                     ((min_refresh_rate % refresh_rate) ? 1 : 0)) * refresh_rate);
  }

  if (refresh_rate > max_refresh_rate) {
    refresh_rate = max_refresh_rate;
  }

  return refresh_rate;
}


DisplayError DisplayPluggable::BuildLayerStackStats(LayerStack *layer_stack) {
  std::vector<Layer *> &layers = layer_stack->layers;
  LayerStackInfo &stack_info = disp_layer_stack_->stack_info;
  stack_info.app_layer_count = 0;
  stack_info.gpu_target_index = -1;
  stack_info.stitch_target_index = -1;
  stack_info.demura_target_index = -1;
  stack_info.noise_layer_index = -1;
  stack_info.cwb_target_index = -1;

  disp_layer_stack_->stack = layer_stack;
  stack_info.common_info.flags = layer_stack->flags;
  stack_info.common_info.blend_cs = layer_stack->blend_cs;
  stack_info.wide_color_primaries.clear();
  stack_info.enable_cac = enable_cac_;
  stack_info.enable_anamorphic_fov = IsAnamorphicFoveationEnabled(layer_stack);
  stack_info.cac_config = cac_config_;

  int index = 0;
  for (auto &layer : layers) {
    if (layer->buffer_map == nullptr) {
      layer->buffer_map = std::make_shared<LayerBufferMap>();
    }
    if (layer->composition == kCompositionGPUTarget) {
      stack_info.gpu_target_index = index;
    } else if (layer->composition == kCompositionStitchTarget) {
      stack_info.stitch_target_index = index;
      disp_layer_stack_->stack->flags.stitch_present = true;
      stack_info.stitch_present = true;
    } else if (layer->composition == kCompositionDemura && stack_info.demura_target_index == -1) {
      stack_info.demura_target_index = index;
      disp_layer_stack_->stack->flags.demura_present = true;
      stack_info.demura_present = true;
      DLOGD_IF(kTagDisplay, "Display %d-%d shall request Demura in this frame", display_id_,
               display_type_);
    } else if (layer->composition == kCompositionDemura) {
      DLOGV_IF(kTagDisplay, "Adding Aiqe ABC feature - UDC layer");
    } else if (layer->flags.is_noise) {
      stack_info.common_info.flags.noise_present = true;
      stack_info.noise_layer_index = index;
      stack_info.noise_layer_info = noise_layer_info_;
      DLOGV_IF(kTagDisplay, "Display %d-%d requested Noise at index = %d with zpos_n = %d",
               display_id_, display_type_, index, noise_layer_info_.zpos_noise);
    } else if (layer->composition == kCompositionCWBTarget) {
      stack_info.cwb_target_index = index;
      stack_info.cwb_present = true;
    } else {
      stack_info.app_layer_count++;
    }
    if (IsWideColor(layer->input_buffer.dataspace.colorPrimaries)) {
      stack_info.wide_color_primaries.push_back(
          layer->input_buffer.dataspace.colorPrimaries);
    }
    if (layer->flags.is_game) {
      stack_info.game_present = true;
    }
    index++;
  }

  DLOGI_IF(kTagDisplay, "LayerStack layer_count: %zu, app_layer_count: %d "
            "gpu_target_index: %d, stitch_index: %d demura_index: %d cwb_target_index: %d "
            "game_present: %d noise_present: %d display: %d-%d", layers.size(),
            stack_info.app_layer_count, stack_info.gpu_target_index,
            stack_info.stitch_target_index, stack_info.demura_target_index,
            stack_info.cwb_target_index, stack_info.game_present,
            stack_info.common_info.flags.noise_present, display_id_, display_type_);

  if (!stack_info.app_layer_count) {
    DLOGW("Layer count is zero");
    return kErrorNoAppLayers;
  }

  if (stack_info.gpu_target_index > 0) {
    return ValidateGPUTargetParams();
  }

  return kErrorNone;
}

bool DisplayPluggable::IsAnamorphicFoveationEnabled(LayerStack *layer_stack) {
  if (!xr_variant_) {
    return false;
  }

  const std::vector<Layer *> &layers = layer_stack->layers;
  for (uint32_t i = 0; i < layers.size(); i++) {
    QtiAnamorphicMetadata anamorphic_md = layers[i]->input_buffer.anamorphicMetadata;
    if (anamorphic_md.leftEyeDataValid || anamorphic_md.rightEyeDataValid) {
      return true;
    }
  }

  return false;
}


void DisplayPluggable::HandleQsyncPostCommit() {
  if (qsync_mode_ == kQsyncModeOneShot) {
    // Reset qsync mode.
    SetQSyncMode(kQSyncModeNone);
  } else if (qsync_mode_ == kQsyncModeOneShotContinuous) {
    // No action needed.
  } else if (qsync_mode_ == kQSyncModeContinuous) {
    if (!avoid_qsync_mode_change_) {
      needs_avr_update_.reset();
    } else if (needs_avr_update_.any()) {
      validated_ = false;
      event_handler_->Refresh();
    }
  } else if (qsync_mode_ == kQSyncModeNone) {
    needs_avr_update_.reset();
  }

  avoid_qsync_mode_change_ = false;
  SetVsyncStatus(true /*Re-enable vsync.*/);

  bool notify_idle = enable_qsync_idle_ && (active_qsync_mode_ != kQSyncModeNone) &&
                     handle_idle_timeout_;
  if (notify_idle) {
    event_handler_->HandleEvent(kPostIdleTimeout);
  }

  bool qsync_enabled = (active_qsync_mode_ != kQSyncModeNone);
  if (qsync_enabled == qsync_enabled_) {
    return;
  }

  QsyncEventData event_data;
  event_data.enabled = qsync_enabled;
  event_data.refresh_rate = client_ctx_.display_attributes.fps;
  dpu_core_mux_->GetQsyncFps(&event_data.qsync_refresh_rate);
  event_handler_->HandleQsyncState(event_data);

  qsync_enabled_ = qsync_enabled;
}


DisplayError DisplayPluggable::SetDisplayState(DisplayState state, bool teardown,
                                             shared_ptr<Fence> *release_fence) {
  ClientLock lock(disp_mutex_);
  DisplayError error = kErrorNone;
  HWDisplayMode panel_mode = client_ctx_.hw_panel_info.mode;

  if ((state == kStateOn) && deferred_config_.IsDeferredState()) {
    SetDeferredFpsConfig();
  }

  // Must go in NullCommit
  if (((demura_intended_ && demura_dynamic_enabled_) || abc_enabled_) &&
      comp_manager_->GetDemuraStatusForDisplay(display_id_) && (state == kStateOff)) {
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    SetDemuraIntfStatus(false, demura_current_idx_);
  }

  error = DisplayBase::SetDisplayState(state, teardown, release_fence);
  if (error != kErrorNone) {
    return error;
  }

  if (secure_event_ == kTUITransitionEnd && state == kStateOff) {
    error = SetPanelBrightness(cached_brightness_, true);
    pending_brightness_ = false;
    if (error == kErrorNone) {
      HandleDemuraScreenRefresh();
    }
  }

  if (client_ctx_.hw_panel_info.mode != panel_mode) {
    UpdateDisplayModeParams();
  }

  // Set vsync enable state to false, as driver disables vsync during display power off.
  if (state == kStateOff) {
    vsync_enable_ = false;
    if (qsync_mode_ != kQSyncModeNone) {
      needs_avr_update_.set(kUpdateAVRModeFlag);
    }
  }

  if (pending_power_state_ != kPowerStateNone) {
    event_handler_->Refresh();
  }

  // Must only happen after NullCommit and get applied in next frame
  if (((demura_intended_ && demura_dynamic_enabled_) || abc_enabled_) &&
      !comp_manager_->GetDemuraStatusForDisplay(display_id_) &&
      (state == kStateOn || state == kStateDoze)) {
    comp_manager_->SetDemuraStatusForDisplay(display_id_, true);

    // Enable default idx if demura calib files are reloaded
    if (demura_calib_files_reloaded_) {
      demura_calib_files_reloaded_ = false;
      demura_current_idx_ = kDemuraDefaultIdx;
    }
    SetDemuraIntfStatus(true, demura_current_idx_);
  }

  if (demuratn_ && demuratn_enabled_) {
    SetDisplayStateForDemuraTn(state);
  }

  return kErrorNone;
}


void DisplayPluggable::SetDeferredFpsConfig() {
  // Update with the deferred Fps Config.
  client_ctx_.display_attributes.fps = deferred_config_.fps;
  client_ctx_.display_attributes.vsync_period_ns = deferred_config_.vsync_period_ns;
  client_ctx_.hw_panel_info.transfer_time_us = deferred_config_.transfer_time_us;
  for (uint32_t i = 0; i < device_ctx_.size(); i++) {
    device_ctx_[i].display_attributes.fps = deferred_config_.fps;
    device_ctx_[i].display_attributes.vsync_period_ns = deferred_config_.vsync_period_ns;
    device_ctx_[i].hw_panel_info.transfer_time_us = deferred_config_.transfer_time_us;
  }
  deferred_config_.Clear();
}

int DisplayPluggable::SetDemuraIntfStatus(bool enable, int current_idx) {
  int ret = 0;
  bool *reconfig = nullptr;
  GenericPayload reconfig_pl;
  if (!demura_) {
    DLOGE("demura_ is nullptr");
    return -EINVAL;
  }

  if (enable) {
    if ((ret = reconfig_pl.CreatePayload<bool>(reconfig))) {
      DLOGE("Failed to create payload for reconfig, error = %d", ret);
      return ret;
    }

    ret = demura_->GetParameter(kDemuraFeatureParamPendingReconfig, &reconfig_pl);
    if (ret) {
      DLOGE("Failed to get reconfig, error %d", ret);
      return ret;
    }

    if (*reconfig) {
      DLOGI("SetDemuraLayer for Anti-Aging reconfig");
      // TBD: handle for ABC during reconfig verification
      ret = SetupCorrectionLayer();
      if (ret) {
        DLOGE("Failed to setup Demura layer, error %d", ret);
        return ret;
      }
    }
  }

  GenericPayload config_pl;
  if (abc_prop_) {
    DemuraFeatureParamConfigIdx<std::string> *config_mode_name = nullptr;
    if ((ret = config_pl.CreatePayload(config_mode_name))) {
      DLOGE("Failed to create payload for config_mode_name, error = %d", ret);
      return ret;
    }

    config_mode_name->modeinfo = "";
    if ((ret = demura_->SetParameter(kDemuraFeatureParamConfigIdx, config_pl))) {
      DLOGE("Failed to set Config Idx, error = %d", ret);
      return ret;
    }

    if (SetupCorrectionLayer() != kErrorNone) {
      DLOGE("Unable to setup abc_intf layer on Display %d-%d", display_id_, display_type_);
      return kErrorUndefined;
    }

  } else {
    uConfigIdx *config_idx = nullptr;
    if ((ret = config_pl.CreatePayload<uConfigIdx>(config_idx))) {
      DLOGE("Failed to create payload for config_idx, error = %d", ret);
      return ret;
    }

    config_idx->modeinfo = current_idx;
    if ((ret = demura_->SetParameter(kDemuraFeatureParamConfigIdx, config_pl))) {
      DLOGE("Failed to set Config Idx, error = %d", ret);
      return ret;
    }
  }

  GenericPayload pl;
  bool* enable_ptr = nullptr;
  if ((ret = pl.CreatePayload<bool>(enable_ptr))) {
    DLOGE("Failed to create payload for enable, error = %d", ret);
    return ret;
  } else {
    *enable_ptr = enable;
    if ((ret = demura_->SetParameter(kDemuraFeatureParamActive, pl))) {
      DLOGE("Failed to set Active, error = %d", ret);
      return ret;
    }
  }

  if (enable && reconfig && (*reconfig)) {
    *reconfig = false;
    ret = demura_->SetParameter(kDemuraFeatureParamPendingReconfig, reconfig_pl);
    if (ret) {
      DLOGE("Failed to set reconfig, error %d", ret);
      return ret;
    }
  }
  DLOGI("Demura is now %s and current index is %d ", enable ? "Enabled" : "Disabled", current_idx);
  return ret;
}


DisplayError DisplayPluggable::SetDisplayStateForDemuraTn(DisplayState state) {
  int ret = 0;
  DisplayState *disp_state = nullptr;
  GenericPayload pl;

  ret = pl.CreatePayload<DisplayState>(disp_state);
  if (ret) {
    DLOGE("failed to create the payload. Error:%d", ret);
    return kErrorUndefined;
  }
  *disp_state = state;

  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamDisplayState, pl);
  if (ret) {
    DLOGE("SetParameter for DisplayState failed ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}


DisplayError DisplayPluggable::SetupCorrectionLayer() {
  if (abc_prop_) {
    return SetupABCLayer();
  } else {
    return SetupDemuraLayer();
  }
}


DisplayError DisplayPluggable::SetupDemuraLayer() {
  int ret = 0;
  GenericPayload pl;

  DemuraCorrectionSurfaces *corrdata = nullptr;
  if ((ret = pl.CreatePayload<DemuraCorrectionSurfaces>(corrdata))) {
    DLOGE("Failed to create payload for BufferInfo, error = %d", ret);
    return kErrorResources;
  }

  if ((ret = demura_->GetParameter(kDemuraFeatureParamCorrectionBuffer, &pl))) {
    DLOGE("Failed to get BufferInfo, error = %d", ret);
    return kErrorResources;
  }
  demura_layer_.clear();  // This will clear the old demura layers

  for (int buf_idx = 0; buf_idx < corrdata->surfaces.size(); buf_idx++) {
    if (!corrdata->valid[buf_idx])
      continue;
    Layer demura_layer = {};
#ifndef TRUSTED_VM
    demura_layer.input_buffer.buffer_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
    demura_layer.input_buffer.handle_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
#endif
    demura_layer.input_buffer.size = corrdata->surfaces[buf_idx].alloc_buffer_info.size;
    demura_layer.input_buffer.format = corrdata->surfaces[buf_idx].alloc_buffer_info.format;
    demura_layer.input_buffer.width = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.unaligned_width =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.height = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.unaligned_height =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].fd = corrdata->surfaces[buf_idx].alloc_buffer_info.fd;
    demura_layer.input_buffer.planes[0].stride =
        corrdata->surfaces[buf_idx].alloc_buffer_info.stride;
    hfc_buffer_width_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    hfc_buffer_height_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].offset = 0;
    demura_layer.input_buffer.flags.demura = 1;
    demura_layer.composition = kCompositionDemura;
    demura_layer.blending = kBlendingSkip;
    demura_layer.flags.is_demura = 1;
    // ROI must match input dimensions
    demura_layer.src_rect.top = 0;
    demura_layer.src_rect.left = 0;
    demura_layer.src_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.src_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura src: ", demura_layer.src_rect);
    demura_layer.dst_rect.top = 0;
    demura_layer.dst_rect.left = 0;
    demura_layer.dst_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.dst_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura dst: ", demura_layer.dst_rect);
    demura_layer.buffer_map = std::make_shared<LayerBufferMap>();
    demura_layer_.push_back(demura_layer);
  }
  return kErrorNone;
}


DisplayError DisplayPluggable::HandleDemuraScreenRefresh() {
  if (demura_intended_ && comp_manager_->GetDemuraStatusForDisplay(display_id_)) {
    if (!demura_) {
      DLOGE("demura_ is nullptr");
      return kErrorParameters;
    }

    GenericPayload pl;
    int32_t *need_screen_refresh = nullptr;
    int rc = 0;
    if ((rc = pl.CreatePayload<int32_t>(need_screen_refresh))) {
      DLOGE("Failed to create payload for need_screen_refresh, error = %d", rc);
      return kErrorParameters;
    }

    rc = demura_->GetParameter(kDemuraFeatureParamNeedScreenRefresh, &pl);
    if (rc) {
      DLOGE("Failed to get need screen refresh, error %d", rc);
      return kErrorParameters;
    }

    if (*need_screen_refresh) {
      event_handler_->Refresh();
    }
  }

  return kErrorNone;
}

void DisplayPluggable::UpdateDisplayModeParams() {
  if (client_ctx_.hw_panel_info.mode == kModeVideo) {
    uint32_t pending = 0;
    ControlPartialUpdateLocked(false /* enable */, &pending);
  } else if (client_ctx_.hw_panel_info.mode == kModeCommand) {
    // Flush idle timeout value currently set.
    comp_manager_->SetIdleTimeoutMs(display_comp_ctx_, 0, 0);
    switch_to_cmd_ = true;
  }
}


DisplayError DisplayPluggable::SetupABCLayer() {
  int ret = 0;
  GenericPayload pl;

  DemuraCorrectionSurfaces *corrdata = nullptr;
  if ((ret = pl.CreatePayload<DemuraCorrectionSurfaces>(corrdata))) {
    DLOGE("Failed to create payload for BufferInfo, error = %d", ret);
    return kErrorResources;
  }

  if ((ret = demura_->GetParameter(kDemuraFeatureParamCorrectionBuffer, &pl))) {
    DLOGE("Failed to get BufferInfo, error = %d", ret);
    return kErrorResources;
  }
  demura_layer_.clear();  // This will clear the old abc layers

  for (int buf_idx = 0; buf_idx < corrdata->surfaces.size(); buf_idx++) {
    if (!corrdata->valid[buf_idx])
      continue;
    Layer demura_layer = {};
    demura_layer.input_buffer.size = corrdata->surfaces[buf_idx].alloc_buffer_info.size;
    demura_layer.input_buffer.buffer_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
    demura_layer.input_buffer.handle_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
    demura_layer.input_buffer.format = corrdata->surfaces[buf_idx].alloc_buffer_info.format;
    demura_layer.input_buffer.width = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.unaligned_width =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.height = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.unaligned_height =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].fd = corrdata->surfaces[buf_idx].alloc_buffer_info.fd;
    demura_layer.input_buffer.planes[0].stride =
        corrdata->surfaces[buf_idx].alloc_buffer_info.stride;
    hfc_buffer_width_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    hfc_buffer_height_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].offset = 0;
    demura_layer.input_buffer.flags.demura = 1;
    demura_layer.composition = kCompositionDemura;
    demura_layer.blending = kBlendingSkip;
    demura_layer.flags.is_abc = 1;
    // ROI must match input dimensions
    demura_layer.src_rect.top = 0;
    demura_layer.src_rect.left = 0;
    demura_layer.src_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.src_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura src: ", demura_layer.src_rect);
    demura_layer.dst_rect.top = 0;
    demura_layer.dst_rect.left = 0;
    demura_layer.dst_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.dst_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura dst: ", demura_layer.dst_rect);
    demura_layer.buffer_map = std::make_shared<LayerBufferMap>();
    demura_layer_.push_back(demura_layer);
  }
  return kErrorNone;
}


DisplayError DisplayPluggable::ControlPartialUpdateLocked(bool enable, uint32_t *pending) {
  if (!pending) {
    return kErrorParameters;
  }

  if (dpps_info_.disable_pu_ && enable) {
    // Nothing to be done.
    DLOGI("partial update is disabled by DPPS for display %d-%d", display_id_, display_type_);
    return kErrorNotSupported;
  }

  *pending = 0;
  if (enable == partial_update_control_) {
    DLOGI("Same state transition is requested.");
    return kErrorNone;
  }
  validated_ = false;
  partial_update_control_ = enable;

  if (!enable) {
    // If the request is to turn off feature, new draw call is required to have
    // the new setting into effect.
    *pending = 1;
  }

  return kErrorNone;
}


DisplayError DisplayPluggable::SetActiveConfig(uint32_t index) {
  deferred_config_.MarkDirty();

  if (vrr_enabled_) {
    // Set VRR State
    avr_step_ = hw_intf_->GetAVRStep(index);
    SetQSyncMode(avr_step_ ? kQSyncModeContinuous : kQSyncModeNone);
    SetAVRStepState(avr_step_ != 0);
  }

  auto error = DisplayBase::SetActiveConfig(index);
  shared_ptr<Fence> release_fence = nullptr;
  HWDMSType dms_type = client_ctx_.hw_panel_info.dms_type;
  if (dms_type == sdm::HWDMSType::kDMSVIDNonSeamless) {
    SetDisplayState(kStateOff, 0, &release_fence);
    sleep(1);
    SetDisplayState(kStateOn, 0, &release_fence);
  }
  return error;
}

DisplayError DisplayPluggable::DppsProcessOps(enum DppsOps op, void *payload, size_t size) {
  DisplayError error = kErrorNone;
  uint32_t pending;
  bool enable = false;
  DppsDisplayInfo *info;

  switch (op) {
    case kDppsSetFeature:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      {
        ClientLock lock(disp_mutex_);
        error = SetDppsFeatureLocked(payload, size);
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
      HandleSelfRefresh();
      break;
    case kDppsPartialUpdate: {
      int ret;
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      enable = *(reinterpret_cast<bool *>(payload));
      dpps_info_.disable_pu_ = !enable;
      ControlPartialUpdate(enable, &pending);
      event_handler_->Refresh();
      {
        ClientLock lock(disp_mutex_);
        validated_ = false;
        dpps_pu_nofiy_pending_ = true;
      }
      ret = dpps_pu_lock_.WaitFinite(kPuTimeOutMs);
      if (ret) {
        DLOGW("failed to %s partial update ret %d", ((enable) ? "enable" : "disable"), ret);
        error = kErrorTimeOut;
      }
      break;
    }
    case kDppsRequestCommit:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      {
        ClientLock lock(disp_mutex_);
        commit_event_enabled_ = *(reinterpret_cast<bool *>(payload));
      }
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
      info->is_primary = IsPrimaryDisplayLocked();
      info->display_id = display_id_;
      info->display_type = display_type_;
      info->fps = enable_dpps_dyn_fps_ ? client_ctx_.display_attributes.fps : 0;

      error = dpu_core_mux_->GetPanelBrightnessBasePath(&(info->brightness_base_path));
      if (error != kErrorNone) {
        DLOGE("Failed to get brightness base path, error %d", error);
      }
      break;
    case kDppsSetPccConfig:
      error = color_mgr_->ColorMgrSetLtmPccConfig(payload, size);
      if (error != kErrorNone) {
        DLOGE("Failed to set PCC config to ColorManagerProxy, error %d", error);
      } else {
        ClientLock lock(disp_mutex_);
        validated_ = false;
        DisablePartialUpdateOneFrameInternal();
      }
      break;
    default:
      DLOGE("Invalid input op %d", op);
      error = kErrorParameters;
      break;
  }
  return error;
}

DisplayError DisplayPluggable::PostCommit() {
  DisplayBase::PostCommit();

  if (commit_event_enabled_) {
    dpps_info_.DppsNotifyOps(kDppsCommitEvent, &display_type_, sizeof(display_type_));
  }

  dpps_info_.Init(this, client_ctx_.hw_panel_info.panel_name, this, prop_intf_);

  return kErrorNone;
}

DisplayError DisplayPluggable::SetDppsFeatureLocked(void *payload, size_t size) {
  return dpu_core_mux_->SetDppsFeature(payload, size);
}

}  // namespace sdm
