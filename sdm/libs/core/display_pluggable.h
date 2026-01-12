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

#ifndef __DISPLAY_PLUGGABLE_H__
#define __DISPLAY_PLUGGABLE_H__

#include <private/hw_events_interface.h>
#include <utils/multi_core_instantiator.h>
#include <private/aiqe_ssrc_feature_interface.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <map>
#include <string>
#include <vector>

#include "display_base.h"

namespace sdm {

class DisplayPluggable : public DisplayBase, HWEventHandler {
 public:
  DisplayPluggable(DisplayEventHandler *event_handler,
                   sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                   BufferAllocator *buffer_allocator, CompManager *comp_manager);
  DisplayPluggable(DisplayId display_id, DisplayEventHandler *event_handler,
                   sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                   BufferAllocator *buffer_allocator, CompManager *comp_manager);
  DisplayError Init() override;
  DisplayError Deinit() override;
  DisplayError Prepare(LayerStack *layer_stack) override;
  DisplayError GetRefreshRateRange(uint32_t *min_refresh_rate,
                                   uint32_t *max_refresh_rate) override;
  DisplayError SetRefreshRate(uint32_t refresh_rate, bool final_rate, bool idle_screen) override;
  bool IsUnderscanSupported() override;
  DisplayError InitializeColorModes() override;
  DisplayError SetColorMode(const std::string &color_mode) override;
  DisplayError GetColorModeCount(uint32_t *mode_count) override;
  DisplayError GetColorModes(uint32_t *mode_count,
                             std::vector<std::string> *color_modes) override;
  DisplayError GetColorModeAttr(const std::string &color_mode, AttrVal *attr) override;
  DisplayError GetStcColorModes(snapdragoncolor::ColorModeList *mode_list) override;
  DisplayError SetStcColorMode(const snapdragoncolor::ColorMode &color_mode) override;
  DisplayError colorSamplingOn() override;
  DisplayError colorSamplingOff() override;

  // Implement the HWEventHandlers
  DisplayError VSync(int64_t timestamp) override;
  DisplayError PFlip(int fd, unsigned int sequence,
                             unsigned int tv_sec, unsigned int tv_usec,
                             void *data) override;
  DisplayError Blank(bool blank) override { return kErrorNone; }
  void CECMessage(char *message) override;
  void IdlePowerCollapse() override {}
  void PingPongTimeout() override {}
  void PanelDead() override {}
  void HwRecovery(const HWRecoveryEvent sdm_event_code) override;
  void HandleBacklightEvent(float brightness_level) override;
  void Histogram(int histogram_fd, uint32_t blob_id) override;
  void MMRMEvent(uint32_t clk) override;
  void HandlePowerEvent() override;
  void HandleVmReleaseEvent() override;
  void GetDRMDisplayToken(uint32_t core_id, sde_drm::DRMDisplayToken *token) override;
  bool IsPrimaryDisplay() override;
  DisplayError GetPanelBrightnessBasePath(std::string *base_path) override;

  void UpdateColorModes();
  void InitializeColorModesFromColorspace();
  DisplayError NotifyDisplayCalibrationMode(bool in_calibration) override;
  DisplayError SetPaHistCollection(
    const std::string &client_name, bool enable,
    SdmDisplayCbInterface<PaHistCollectionPayload> *cb_intf) override;
  DisplayError GetPaHistBins(std::array<uint32_t,
    HIST_BIN_SIZE> *buf) override;

  DisplayError SetVRRState(bool state) override;
  //DisplayError GetQsyncFps(uint32_t *qsync_fps) override;
  DisplayError GetQSyncMode(QSyncMode *qsync_mode) override;
  DisplayError SetQSyncMode(QSyncMode qsync_mode) override;
  std::string Dump() override;
  DisplayError PrePrepare(LayerStack *layer_stack) override;
  DisplayError UpdateTransferTime(uint32_t transfer_time) override;
  DisplayError BuildLayerStackStats(LayerStack *layer_stack) override;
  DisplayError SetDisplayState(DisplayState state, bool teardown,
                               shared_ptr<Fence> *release_fence) override;
  DisplayError SetActiveConfig(uint32_t index) override;

 private:
  PrimariesTransfer GetBlendSpaceFromStcColorMode(
    const snapdragoncolor::ColorMode &color_mode);
  DisplayError GetOverrideConfig(uint32_t *mode_index);
  void GetScanSupport();
  void SetVsyncStatus(bool enable);
  DisplayError SetAVRStepState(bool enable);
  CacVersion GetCacVerion();
  DisplayError HandleDemuraLayer(LayerStack *layer_stack);
  DisplayError HandleSPR();
  void DeinitCWBBuffer();
  void AppendCWBLayer(LayerStack *layer_stack);
  void InitCWBBuffer();
  bool CanSkipDisplayPrepare(LayerStack *layer_stack);
  void UpdateQsyncConfig();
  DisplayError ChangeFps();
  bool IdleFallbackLowerFps(bool idle_screen);
  bool CanCompareFrameROI(LayerStack *layer_stack);
  HWAVRModes GetAvrMode(QSyncMode mode);
  uint32_t GetUpdatingLayersCount();
  uint32_t GetOptimalRefreshRate(bool one_updating_layer);
  uint32_t GetUpdatingAppLayersCount(LayerStack *layer_stack);
  uint32_t CalculateMetaDataRefreshRate();
  uint32_t SanitizeRefreshRate(uint32_t req_refresh_rate, uint32_t max_refresh_rate,
                               uint32_t min_refresh_rate);
  bool IsAnamorphicFoveationEnabled(LayerStack *layer_stack);
  void HandleQsyncPostCommit();
  void SetDeferredFpsConfig();
  int SetDemuraIntfStatus(bool enable, int current_idx = kDemuraDefaultIdx);
  DisplayError SetDisplayStateForDemuraTn(DisplayState state);
  DisplayError SetupCorrectionLayer();
  DisplayError SetupDemuraLayer();
  DisplayError HandleDemuraScreenRefresh();
  void UpdateDisplayModeParams();
  DisplayError SetupABCLayer();
  DisplayError ControlPartialUpdateLocked(bool enable, uint32_t *pending);

  static const int kPropertyMax = 256;

  bool underscan_supported_ = false;
  HWScanSupport scan_support_;
  std::map<uint32_t, std::vector<HWEvent>> event_list_;
  uint32_t current_refresh_rate_ = 0;
  snapdragoncolor::ColorMode current_stc_color_mode_ = {};
  snapdragoncolor::ColorModeList stc_color_modes_ = {};
  EventProxyInfo event_proxy_info_ = {};
  bool avr_step_enabled_ = false;
  bool vrr_enabled_ = false;
  bool qsync_enabled_ = false;
  QSyncMode active_qsync_mode_ = kQSyncModeNone;
  bool enable_qsync_idle_ = false;
  std::vector<Layer> demura_layer_ = {};
  std::shared_ptr<SPRIntf> spr_ = nullptr;
  bool cwb_buffer_initialized_ = false;
  Layer cwb_layer_ = {};
  BufferInfo output_buffer_info_ = {};
  bool lower_fps_ = false;
  DynLib ssrc_lib_;
  std::shared_ptr<aiqe::SsrcFeatureInterface> ssrc_feature_interface_;
  vector<LayerRect> left_frame_roi_ = {};
  vector<LayerRect> right_frame_roi_ = {};
  bool disable_dyn_fps_ = false;
  bool enhance_idle_time_ = false;
  DeferFpsConfig deferred_config_ = {};
  int idle_time_ms_ = 0;
  struct timespec idle_timer_start_;
  bool enable_cac_ = false;
  CacConfig cac_config_ = {};
  bool demuratn_enabled_ = false;
  std::shared_ptr<DemuraTnCoreUvmIntf> demuratn_ = nullptr;
  std::shared_ptr<DemuraIntf> demura_ = nullptr;
  uint32_t hfc_buffer_width_ = 0;
  uint32_t hfc_buffer_height_ = 0;
  bool demura_intended_ = false;
  bool demura_dynamic_enabled_ = true;
  int demura_current_idx_ = -1;
  float cached_brightness_ = 0.0f;
  bool pending_brightness_ = false;
  bool demura_calib_files_reloaded_ = false;
  bool abc_enabled_ = false;
  bool abc_prop_ = false;
  DppsInfo dpps_info_ = {};
  bool switch_to_cmd_ = false;
};

}  // namespace sdm

#endif  // __DISPLAY_PLUGGABLE_H__
