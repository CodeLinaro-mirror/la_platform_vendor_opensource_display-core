/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DPU_MULTI_CORE_H__
#define __DPU_MULTI_CORE_H__

#include "dpu_core_mux.h"
#include <future>
#include <thread>

namespace sdm {

class DPUMultiCore : public DPUCoreMux {
 public:
  DisplayError Destroy();

  DPUMultiCore(DisplayId display_id, SDMDisplayType type,
               MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
               BufferAllocator *buffer_allocator);
  DisplayError Init();
  DisplayError GetDisplayId(int32_t *display_id);
  DisplayError GetActiveConfig(uint32_t *active_config);
  DisplayError GetDefaultConfig(uint32_t *default_config);
  DisplayError GetNumDisplayAttributes(uint32_t *count);
  DisplayError GetDisplayAttributes(uint32_t index, DisplayDeviceContext *device_ctx,
                                    DisplayClientContext *client_ctx);
  DisplayError GetHWPanelInfo(DisplayDeviceContext *device_ctx, DisplayClientContext *client_ctx);
  DisplayError SetDisplayAttributes(uint32_t index);
  DisplayError SetDisplayAttributes(const HWDisplayAttributes &display_attributes);
  DisplayError GetConfigIndex(char *mode, uint32_t *index);
  DisplayError PowerOn(std::map<uint32_t, HWQosData> &qos_data, SyncPoints *sync_points);
  DisplayError PowerOff(bool teardown, SyncPoints *sync_points);
  DisplayError Doze(std::map<uint32_t, HWQosData> &qos_data, SyncPoints *sync_points);
  DisplayError DozeSuspend(std::map<uint32_t, HWQosData> &qos_data, SyncPoints *sync_points);
  DisplayError Standby(SyncPoints *sync_points);
  DisplayError Validate(std::map<uint32_t, HWLayersInfo> &hw_layers_info);
  DisplayError Commit(std::map<uint32_t, HWLayersInfo> &hw_layers_info);
  DisplayError Flush(std::map<uint32_t, HWLayersInfo> &hw_layers_info);
  DisplayError GetPPFeaturesVersion(PPFeatureVersion *vers, uint32_t core_id);
  DisplayError SetPPFeature(PPFeatureInfo *feature, uint32_t &core_id);
  DisplayError SetVSyncState(bool enable);
  void SetIdleTimeoutMs(uint32_t timeout_ms);
  DisplayError SetDisplayMode(const HWDisplayMode hw_display_mode);
  DisplayError SetRefreshRate(uint32_t refresh_rate);
  DisplayError SetPanelBrightness(int level, bool apply_immediately);
  DisplayError SetIllumination(const uint32_t eye, const IlluminationConfig &config);
  DisplayError SetPixelShift(const uint32_t eye, const PixelShiftConfig &config);
  DisplayError IsLedDriverUp(bool *is_led_driver_up);
  DisplayError GetHWScanInfo(HWScanInfo *scan_info);
  DisplayError GetVideoFormat(uint32_t config_index, uint32_t *video_format);
  DisplayError GetMaxCEAFormat(uint32_t *max_cea_format);
  DisplayError SetCursorPosition(std::map<uint32_t, HWLayersInfo> &hw_layers_info, int x, int y);
  DisplayError OnMinHdcpEncryptionLevelChange(uint32_t min_enc_level);
  DisplayError GetPanelBrightness(int *level);
  DisplayError SetAutoRefresh(bool enable);
  DisplayError SetScaleLutConfig(HWScaleLutInfo *lut_info);
  DisplayError UnsetScaleLutConfig();
  DisplayError SetMixerAttributes(const HWMixerAttributes &mixer_attributes);
  DisplayError GetMixerAttributes(DisplayDeviceContext *device_ctx,
                                  DisplayClientContext *client_ctx);
  DisplayError DumpDebugData();
  DisplayError SetDppsFeature(void *payload, size_t size);
  DisplayError GetDppsFeatureInfo(void *payload, size_t size);
  DisplayError HandleSecureEvent(SecureEvent secure_event, std::map<uint32_t, HWQosData> &qos_data);
  DisplayError ControlIdlePowerCollapse(bool enable, bool synchronous);
  DisplayError SetDisplayDppsAdROI(void *payload);
  DisplayError SetDynamicDSIClock(uint64_t bit_clk_rate);
  DisplayError GetDynamicDSIClock(uint64_t *bit_clk_rate);
  DisplayError GetDisplayIdentificationData(uint8_t *out_port, uint32_t *out_data_size,
                                            uint8_t *out_data);
  DisplayError SetFrameTrigger(FrameTriggerMode mode, uint32_t core_id);
  DisplayError SetFrameTrigger(FrameTriggerMode mode);
  DisplayError SetBLScale(uint32_t level);
  DisplayError GetPanelBlMaxLvl(uint32_t *max_bl);
  DisplayError GetPanelBrightnessBasePath(std::string *base_path) const;
  DisplayError SetBlendSpace(const PrimariesTransfer &blend_space);
  DisplayError EnableSelfRefresh(SelfRefreshState self_refresh_state);
  PanelFeaturePropertyIntf *GetPanelFeaturePropertyIntf();
  DisplayError GetFeatureSupportStatus(const HWFeature feature, uint32_t *status);
  void FlushConcurrentWriteback();
  DisplayError SetAlternateDisplayConfig(uint32_t *alt_config);
  DisplayError GetQsyncFps(uint32_t *qsync_fps);
  DisplayError CancelDeferredPowerMode();
  void GetHWInterface(HWInterface **intf);
  void GetDRMDisplayToken(uint32_t core_id, sde_drm::DRMDisplayToken *token) const;
  DisplayError SetPPConfig(void *payload, size_t size);
  DisplayError GetFbConfig(uint32_t width, uint32_t height, DisplayDeviceContext *device_ctx,
                           DisplayClientContext *client_ctx);
  void SetSSRState(bool active, HWSSRType type = kSSR);
  bool IsEPTSupported();
  DisplayError SetHdrCapabilities(const std::vector<Hdr> &hdr_types, float max_avg_luminance,
                                  float min_luminance);
  void PerformAsyncCommitOnCores(std::map<uint32_t, HWLayersInfo> &hw_layers_info);
  void CommitOnCore(int core_id);
  ~DPUMultiCore() {}

 private:
  struct CommitRequest {
    CommitRequest(HWLayersInfo *info) : hw_layers_info(info) {}
    HWLayersInfo *hw_layers_info = nullptr;
  };
  struct CommitThreadContext {
    std::shared_ptr<CommitRequest> commit_req;
    std::mutex lock;
    std::condition_variable worker_thread_cv;
    std::condition_variable commit_response_cv;
    std::future<void> future;
    bool commit_thread_running = false;
    bool commit_pending = false;
    DisplayError commit_response = kErrorUndefined;
  };
  std::map<uint32_t, HWInterface *> hw_intf_;
  std::vector<uint32_t> core_ids_;
  DisplayId display_id_ = {};
  SDMDisplayType type_;
  MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf_;
  BufferAllocator *buffer_allocator_;
  bool dpu_ctl_op_sync_ = false;
  std::vector<uint32_t> op_sync_sequence_;
  // map of core id to commit thread context for corresponding core
  std::map<int, CommitThreadContext> display_commit_thread_map_;
  template <typename T>
  bool AreAllEntriesSame(std::vector<T> &vec);
  void SetOpSyncHint(bool dpu_ctl_op_sync);
};

}  // namespace sdm

#endif  // __DPU_MULTI_CORE_H__
