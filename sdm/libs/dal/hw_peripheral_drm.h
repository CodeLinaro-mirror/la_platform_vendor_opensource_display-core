/*
Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.
    * Neither the name of The Linux Foundation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __HW_PERIPHERAL_DRM_H__
#define __HW_PERIPHERAL_DRM_H__

#include <map>
#include <vector>
#include <string>
#include "hw_device_drm.h"

namespace sdm {

struct FieldFd {
  int32_t r_fd = -1;          // eye r illuminance node fd
  int32_t g_fd = -1;          // eye g illuminance node fd
  int32_t b_fd = -1;          // eye b illuminance node fd
  int32_t led_staus_fd = -1;  // Node to check eye leds up status
};

struct PanelShiftFd {
  int32_t l_fd = -1;  // panel left shift node fd
  int32_t t_fd = -1;  // panel top shift node fd
  int32_t r_fd = -1;  // panel right shift node fd
  int32_t b_fd = -1;  // panel bottom shift node fd
};

class HWPeripheralDRM : public HWDeviceDRM, public PanelFeaturePropertyIntf {
 public:
  explicit HWPeripheralDRM(int32_t display_id, BufferAllocator *buffer_allocator,
                           HWInfoInterface *hw_info_intf);
  virtual ~HWPeripheralDRM() {}
  virtual PanelFeaturePropertyIntf *GetPanelFeaturePropertyIntf() { return this; }
  virtual int GetPanelFeature(PanelFeaturePropertyInfo *feature_info);
  virtual int SetPanelFeature(const PanelFeaturePropertyInfo &feature_info);
  virtual DisplayError GetPanelBrightnessBasePath(std::string *base_path) const;
  virtual DisplayError GetQsyncFps(uint32_t *qsync_fps);

 protected:
  virtual DisplayError Init();
  virtual DisplayError Deinit();
  virtual DisplayError Validate(HWLayersInfo *hw_layers_info);
  virtual DisplayError Commit(HWLayersInfo *hw_layers_info);
  virtual DisplayError Flush(HWLayersInfo *hw_layers_info);
  virtual DisplayError SetDppsFeature(void *payload, size_t size);
  virtual DisplayError GetDppsFeatureInfo(void *payload, size_t size);
  virtual DisplayError HandleSecureEvent(SecureEvent secure_event, const HWQosData &qos_data);
  virtual DisplayError ControlIdlePowerCollapse(bool enable, bool synchronous);
  virtual DisplayError PowerOn(const HWQosData &qos_data, SyncPoints *sync_points);
  virtual DisplayError PowerOff(bool teardown, SyncPoints *sync_points);
  virtual DisplayError Doze(const HWQosData &qos_data, SyncPoints *sync_points);
  virtual DisplayError DozeSuspend(const HWQosData &qos_data, SyncPoints *sync_points);
  virtual DisplayError SetDisplayDppsAdROI(void *payload);
  virtual DisplayError SetJitterConfig(uint32_t jitter_type, float value, uint32_t time);
  virtual DisplayError SetDynamicDSIClock(uint64_t bit_clk_rate);
  virtual DisplayError GetDynamicDSIClock(uint64_t *bit_clk_rate);
  virtual DisplayError SetDisplayAttributes(uint32_t index);
  virtual DisplayError SetDisplayMode(const HWDisplayMode hw_display_mode);
  virtual DisplayError SetOffloadMode(bool enable);
  virtual DisplayError SetBppMode(uint32_t bpp);
  virtual DisplayError SetRefreshRate(uint32_t refresh_rate);
  virtual DisplayError SetFrameTrigger(FrameTriggerMode mode);
  virtual DisplayError SetPanelBrightness(int level, bool apply_immediately);
  virtual DisplayError GetPanelBrightness(int *level);
  virtual void GetHWPanelMaxBrightness();
  virtual DisplayError SetBLScale(uint32_t level);
  virtual DisplayError EnableSelfRefresh(SelfRefreshState self_refresh_state);
  virtual DisplayError SetAlternateDisplayConfig(uint32_t *alt_config);
  virtual DisplayError UpdateTransferTime(uint32_t transfer_time);
  void SetDestScalarData(const HWLayersInfo &hw_layer_info);
  virtual uint32_t GetAVRStep(uint32_t config_index);
  virtual bool IsVRRSupported();
  virtual DisplayError setDriverCommitPath(DriverCommitPath path);
  virtual uint32_t GetMaxPrivacyRegionsSupported();
  virtual DisplayError SetIllumination(uint32_t eye, const IlluminationConfig &config);
  virtual DisplayError SetPixelShift(uint32_t eye, const PixelShiftConfig &config);
  virtual DisplayError IsLedDriverUp(bool *is_led_driver_up);

 private:
  void InitDestScaler();
  void SetDestScalarData(const DestScaleInfoMap dest_scale_info_map);
  void SetAIScalerData(const AIScalerInfoMap ai_scale_info_map);
  void ResetDestScalarCache();
  void ResetAIScalarCache();
  void ResetDestScalarData();
  void CreatePanelFeaturePropertyMap();
  void SetIdlePCState() {
    drm_atomic_intf_->Perform(sde_drm::DRMOps::CRTC_SET_IDLE_PC_STATE, token_.crtc_id,
                              idle_pc_state_);
  }
  void CacheDestScalarData();
  void SetSelfRefreshState();
  void SetVMReqState();
  void ResetPropertyCache();
  void InitAIScaler();
  bool IsCACEnabled(const HWLayersInfo *hw_layers_info);
  DisplayError UpdateLoopBackConnector();
  DisplayError ConfigureLoopbackCAC(const bool cac_enabled);
  DisplayError WriteToNode(const std::string node_name, int32_t *fd, uint32_t data);
  DisplayError ReadFromNode(const std::string node_name, int32_t *fd, uint32_t *data);
  void InitCalibrationNodes();
  DisplayError OpenNode(std::string node_name, int32_t *fd);
  void PrintBrightnessPolicy();

  struct DestScalarCache {
    SDEScaler scalar_data = {};
    uint32_t flags = {};
  };

#ifndef TARGET_INCLUDES_NEO
  struct AIScalerCache {
    struct drm_msm_ai_scaler scaler_data = {};
    uint32_t mode_id;
  };
  struct drm_msm_ai_scaler sde_ai_scaler_cfg_ = {};
  std::vector<AIScalerCache> ai_scaler_cache_ = {};
  uint32_t ai_scaler_current_mode_id_ = 0;
#endif

  const std::string kPathLeftEyeIllumination = "/sys/bus/i2c/devices/4-0028/";
  const std::string kPathRightEyeIllumination = "/sys/bus/i2c/devices/4-0029/";
  const std::string kPathLeftEyePanelShift = "/sys/kernel/rtimd/rtimd_eye/left_eye_shift_";
  const std::string kPathRightEyePanelShift = "/sys/kernel/rtimd/rtimd_eye/right_eye_shift_";
  sde_drm_dest_scaler_data sde_dest_scalar_data_ = {};
  std::vector<SDEScaler> scalar_data_ = {};
  sde_drm::DRMIdlePCState idle_pc_state_ = sde_drm::DRMIdlePCState::NONE;
  bool idle_pc_enabled_ = true;
  std::vector<DestScalarCache> dest_scalar_cache_ = {};
  drm_msm_ad4_roi_cfg ad4_roi_cfg_ = {};
  bool needs_ds_update_ = false;
  bool needs_ai_scaler_update_ = false;
  void PopulateBitClkRates();
  std::vector<uint64_t> bitclk_rates_;
  std::string brightness_base_path_ = "";
  std::string offload_path_ = "";
  SelfRefreshState self_refresh_state_ = kSelfRefreshNone;
  bool ltm_hist_en_ = false;
  bool aba_hist_en_ = false;
  std::map<PanelFeaturePropertyID, sde_drm::DRMPanelFeatureID> panel_feature_property_map_ {};
  bool use_hfi_path_ = false;
  bool hwio_path_switch_pending_ = false;
  bool set_tui_none_ = false;
  sde_drm::DRMCacheState lsr_cache_state_ = sde_drm::DRMCacheState::DISABLED;
  FieldFd left_field_fds_;
  FieldFd right_field_fds_;
  PanelShiftFd left_panel_shifts_;
  PanelShiftFd right_panel_shifts_;
};

}  // namespace sdm

#endif  // __HW_PERIPHERAL_DRM_H__
