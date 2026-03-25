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

/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __HW_INTERFACE_H__
#define __HW_INTERFACE_H__

#include <core/buffer_allocator.h>
#include <core/display_interface.h>
#include <private/hw_info_types.h>
#include <private/color_interface.h>
#include <private/panel_feature_property_intf.h>
#include <utils/constants.h>
#include <drm_interface.h>
#include <string>

#include "hw_info_interface.h"

namespace sdm {

enum HWScanSupport {
  kScanNotSupported,
  kScanAlwaysOverscanned,
  kScanAlwaysUnderscanned,
  kScanBoth,
};

enum HWSSRType {
  kSSR,    // Standard full subsystem restart
  kSSRLsr  // Late Stage Reprojection subsystem restart
};

struct HWScanInfo {
  HWScanSupport pt_scan_support;    // Scan support for preferred timing
  HWScanSupport it_scan_support;    // Scan support for digital monitor or industry timings
  HWScanSupport cea_scan_support;   // Scan support for CEA resolution timings

  HWScanInfo() : pt_scan_support(kScanNotSupported), it_scan_support(kScanNotSupported),
                 cea_scan_support(kScanNotSupported) { }
  bool operator != (const HWScanInfo& hw_scan_info) {
    return pt_scan_support != hw_scan_info.pt_scan_support ||
           it_scan_support != hw_scan_info.it_scan_support ||
           cea_scan_support != hw_scan_info.cea_scan_support;
  }

  bool operator == (const HWScanInfo& hw_scan_info) {
    return !(operator !=(hw_scan_info));
  }
};

struct ReprojectionConfig {
  struct drm_msm_opaque_config reproj_sparse_grid =
      {};                      // reproj_reproj_sparse_grid data field is 2D array.
  uint32_t reproj_grid_w = 0;  // grid width
  uint32_t reproj_grid_h = 0;  // grid height
  struct drm_msm_opaque_config reproj_radial_dis_grid =
      {};                           // reproj_radial_dis_grid data field is 2D array.
  uint32_t distort_resolution = 0;  // num column of reproj_radial_dis_grid data array
  struct sde_drm_lsr_point reproj_optical_axis_offset = {};  // config to for optical_axis_offset
  float reproj_r_max = 0;
  uint32_t reproj_error_to_l = 0;
  uint32_t reproj_disp_im_width = 0;
  uint32_t reproj_disp_im_height = 0;
  uint32_t reproj_tile_w = 0;
  uint32_t reproj_tile_h = 0;
  struct drm_msm_opaque_config repro_session_config = {};
  struct drm_msm_opaque_config repro_session_data_config = {};
  uint32_t reprojection_mode_enabled = true;
  float reproj_to_lrgb_left = 0;
  float reproj_to_lrgb_right = 0;
  uint32_t reproj_min_bbox_w = 0;
  uint32_t reproj_min_bbox_h = 0;
};

enum HWFeature {
  kAllowedModeSwitch,
  kHasCwbCrop,
  kHasDedicatedCwb,
  kMaxSupportedCwb,
};

enum DriverCommitPath {
  kHWIO,
  kHFI,
};

inline std::string to_string(DriverCommitPath path) {
  switch (path) {
    case kHWIO:
      return "HWIO";
      break;
    case kHFI:
      return "HFI";
      break;
    default:
      return "Unknown";
      break;
  }
}

// HWEventHandler - Implemented in DisplayBase and HWInterface implementation
class HWEventHandler {
 public:
  virtual DisplayError VSync(int64_t timestamp) = 0;
  virtual DisplayError Blank(bool blank) = 0;
  virtual void CECMessage(char *message) = 0;
  virtual void IdlePowerCollapse() = 0;
  virtual void PingPongTimeout() = 0;
  virtual void PanelDead() = 0;
  virtual void HwRecovery(const HWRecoveryEvent sdm_event_code) = 0;
  virtual void Histogram(int histogram_fd, uint32_t blob_id) = 0;
  virtual void HandleBacklightEvent(float brightness_level) = 0;
  virtual void MMRMEvent(uint32_t clk) = 0;
  virtual void HandlePowerEvent() = 0;
  virtual void HandleVmReleaseEvent() = 0;
  virtual void HandleVmReclaimEvent() = 0;
  virtual void GetDRMDisplayToken(uint32_t core_id, sde_drm::DRMDisplayToken *token) = 0;
  virtual bool IsPrimaryDisplay() = 0;
  virtual DisplayError GetPanelBrightnessBasePath(std::string *base_path) = 0;
  virtual void HandleSSREvent(SSREventType ssr_event) = 0;
  virtual void HandleLSR_SSREvent(LSR_SSREventType lsr_ssr_event) = 0;

 protected:
  virtual ~HWEventHandler() { }
};

class HWInterface {
 public:
  static DisplayError Create(int32_t display_id, SDMDisplayType type, HWInfoInterface *hw_info_intf,
                             BufferAllocator *buffer_allocator, HWInterface **intf);
  static DisplayError Destroy(HWInterface *intf);

  virtual DisplayError Init() = 0;
  virtual DisplayError Deinit() = 0;
  virtual DisplayError GetDisplayId(int32_t *display_id) = 0;
  virtual DisplayError GetActiveConfig(uint32_t *active_config) = 0;
  virtual DisplayError GetDefaultConfig(uint32_t *default_config) = 0;
  virtual DisplayError GetNumDisplayAttributes(uint32_t *count) = 0;
  virtual DisplayError GetDisplayAttributes(uint32_t index,
                                            HWDisplayAttributes *display_attributes) = 0;
  virtual DisplayError GetHWPanelInfo(HWPanelInfo *panel_info) = 0;
  virtual DisplayError SetDisplayAttributes(uint32_t index) = 0;
  virtual DisplayError SetDisplayAttributes(const HWDisplayAttributes &display_attributes) = 0;
  virtual DisplayError GetConfigIndex(char *mode, uint32_t *index) = 0;
  virtual DisplayError PowerOn(const HWQosData &qos_data, SyncPoints *sync_points) = 0;
  virtual DisplayError PowerOff(bool teardown, SyncPoints *sync_points) = 0;
  virtual DisplayError Doze(const HWQosData &qos_data, SyncPoints *sync_points) = 0;
  virtual DisplayError DozeSuspend(const HWQosData &qos_data, SyncPoints *sync_points) = 0;
  virtual DisplayError Standby(SyncPoints *sync_points) = 0;
  virtual DisplayError Validate(HWLayersInfo *hw_layers_info) = 0;
  virtual DisplayError Commit(HWLayersInfo *hw_layers_info) = 0;
  virtual DisplayError Flush(HWLayersInfo *hw_layers_info) = 0;
  virtual DisplayError GetPPFeaturesVersion(PPFeatureVersion *vers) = 0;
  virtual DisplayError SetPPFeature(PPFeatureInfo *feature) = 0;
  virtual DisplayError SetVSyncState(bool enable) = 0;
  virtual DisplayError SetOffloadMode(bool enable) = 0;
  virtual void SetIdleTimeoutMs(uint32_t timeout_ms) = 0;
  virtual DisplayError SetDisplayMode(const HWDisplayMode hw_display_mode) = 0;
  virtual DisplayError SetBppMode(uint32_t bpp) = 0;
  virtual DisplayError SetRefreshRate(uint32_t refresh_rate) = 0;
  virtual DisplayError SetPanelBrightness(int level, bool apply_immediately) = 0;
  virtual DisplayError GetHWScanInfo(HWScanInfo *scan_info) = 0;
  virtual DisplayError GetVideoFormat(uint32_t config_index, uint32_t *video_format) = 0;
  virtual DisplayError GetMaxCEAFormat(uint32_t *max_cea_format) = 0;
  virtual DisplayError SetCursorPosition(HWLayersInfo *hw_layers_info, int x, int y) = 0;
  virtual DisplayError OnMinHdcpEncryptionLevelChange(uint32_t min_enc_level) = 0;
  virtual DisplayError GetPanelBrightness(int *level) = 0;
  virtual DisplayError SetAutoRefresh(bool enable) = 0;
  virtual DisplayError SetScaleLutConfig(HWScaleLutInfo *lut_info) = 0;
  virtual DisplayError UnsetScaleLutConfig() = 0;
  virtual DisplayError SetMixerAttributes(const HWMixerAttributes &mixer_attributes) = 0;
  virtual DisplayError GetMixerAttributes(HWMixerAttributes *mixer_attributes) = 0;
  virtual DisplayError DumpDebugData() = 0;
  virtual DisplayError SetDppsFeature(void *payload, size_t size) = 0;
  virtual DisplayError GetDppsFeatureInfo(void *payload, size_t size) = 0;
  virtual DisplayError HandleSecureEvent(SecureEvent secure_event, const HWQosData &qos_data) = 0;
  virtual DisplayError ControlIdlePowerCollapse(bool enable, bool synchronous) = 0;
  virtual DisplayError SetDisplayDppsAdROI(void *payload) = 0;
  virtual DisplayError SetJitterConfig(uint32_t jitter_type, float value, uint32_t time) = 0;
  virtual DisplayError SetDynamicDSIClock(uint64_t bit_clk_rate) = 0;
  virtual DisplayError GetDynamicDSIClock(uint64_t *bit_clk_rate) = 0;
  virtual DisplayError GetDisplayIdentificationData(uint8_t *out_port, uint32_t *out_data_size,
                                                    uint8_t *out_data) = 0;
  virtual DisplayError SetFrameTrigger(FrameTriggerMode mode) = 0;
  virtual DisplayError SetBLScale(uint32_t level) = 0;
  virtual DisplayError GetPanelBlMaxLvl(uint32_t *max_bl) = 0;
  virtual DisplayError SetPPConfig(void *payload, size_t size) = 0;
  virtual DisplayError GetPanelBrightnessBasePath(std::string *base_path) const = 0;
  virtual DisplayError SetBlendSpace(const PrimariesTransfer &blend_space) = 0;
  virtual DisplayError EnableSelfRefresh(SelfRefreshState self_refresh_state) = 0;
  virtual PanelFeaturePropertyIntf *GetPanelFeaturePropertyIntf() = 0;
  virtual DisplayError GetFeatureSupportStatus(const HWFeature feature, uint32_t *status) = 0;
  virtual void FlushConcurrentWriteback() = 0;
  virtual DisplayError SetAlternateDisplayConfig(uint32_t *alt_config) = 0;
  virtual DisplayError GetQsyncFps(uint32_t *qsync_fps) = 0;
  virtual DisplayError UpdateTransferTime(uint32_t transfer_time) = 0;
  virtual DisplayError CancelDeferredPowerMode() = 0;
  virtual void HandleCwbTeardown(bool sync_teardown) = 0;
  virtual void SetDestScalarData(const HWLayersInfo &hw_layer_info) = 0;
  virtual void GetDRMDisplayToken(sde_drm::DRMDisplayToken *token) const = 0;
  virtual uint32_t GetAVRStep(uint32_t config_index) = 0;
  virtual DisplayError NotifyExpectedPresent(uint64_t expected_present_time,
                                             uint32_t frame_interval_ns) = 0;
  virtual bool IsVRRSupported() = 0;
  virtual void DisplayEarlyWakeUp() = 0;
  virtual DisplayError setDriverCommitPath(DriverCommitPath path) = 0;
  virtual uint32_t GetMaxPrivacyRegionsSupported() = 0;
  virtual DisplayError SetDisplayDeviceConfig(SDMDisplayDeviceConfig sdm_display_device_config) = 0;
  virtual DisplayError SetReprojectionConfig(
      const struct ReprojectionConfig &reprojection_config) = 0;
  virtual void SetSSRState(bool active, HWSSRType type = kSSR) = 0;
  virtual bool IsEPTSupported() = 0;
  virtual DisplayError SetIllumination(uint32_t eye, const IlluminationConfig &config) = 0;
  virtual DisplayError SetPixelShift(uint32_t eye, const PixelShiftConfig &config) = 0;
  virtual DisplayError IsLedDriverUp(bool *is_led_driver_up) = 0;
  virtual DisplayError SetHdrCapabilities(const std::vector<Hdr> &hdr_types,
                                          float max_avg_luminance, float min_luminance) = 0;

 protected:
  virtual ~HWInterface() { }
};

}  // namespace sdm

#endif  // __HW_INTERFACE_H__
