/*
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
* Changes from Qualcomm Technologies, Inc. are provided under the following license:
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/
#include <algorithm>
#include <utils/constants.h>
#include <utils/debug.h>
#include <display_properties.h>

#include "sdm_debugger.h"
#include "sdm_display_pluggable.h"
#include "sdm_color_mode_stc.h"

#define __CLASS__ "SDMDisplayPluggable"

namespace sdm {

DisplayError SDMDisplayPluggable::Create(
    CoreInterface *core_intf, BufferAllocator *buffer_allocator, SDMCompositorCallbacks *callbacks,
    SDMDisplayEventHandler *event_handler, Display id, int32_t sdm_id, uint32_t primary_width,
    uint32_t primary_height, bool use_primary_res, SDMDisplay **sdm_display) {
  uint32_t pluggable_width = 0;
  uint32_t pluggable_height = 0;
  DisplayError error = kErrorNone;

  SDMDisplay *sdm_display_pluggable = new SDMDisplayPluggable(
      core_intf, buffer_allocator, callbacks, event_handler, id, sdm_id);
  auto status = sdm_display_pluggable->Init();
  if (status) {
    delete sdm_display_pluggable;
    return status;
  }

  error = sdm_display_pluggable->GetMixerResolution(&pluggable_width,
                                                    &pluggable_height);
  if (error != kErrorNone) {
    Destroy(sdm_display_pluggable);
    return error;
  }

  if (primary_width && primary_height) {
    // use_primary_res means SDMDisplayPluggable should directly set framebuffer
    // resolution to the provided primary_width and primary_height
    if (use_primary_res) {
      pluggable_width = primary_width;
      pluggable_height = primary_height;
    } else {
      int downscale_enabled = 0;
      SDMDebugHandler::Get()->GetProperty(ENABLE_EXTERNAL_DOWNSCALE_PROP,
                                          &downscale_enabled);
      if (downscale_enabled) {
        GetDownscaleResolution(primary_width, primary_height, &pluggable_width,
                               &pluggable_height);
      }
    }
  }

  status = sdm_display_pluggable->SetFrameBufferResolution(pluggable_width,
                                                           pluggable_height);
  if (status) {
    Destroy(sdm_display_pluggable);
    return status;
  }

  *sdm_display = sdm_display_pluggable;

  return status;
}

DisplayError SDMDisplayPluggable::Init() {
  int enable_qdcm_colormodes_on_external_ = QdcmOnExternal::STC_QDCM; //set stc colormodes default
  Debug::GetProperty(ENABLE_QDCM_COLORMODES_ON_EXTERNAL, &enable_qdcm_colormodes_on_external_);
  auto status = SDMDisplay::Init();
  if (status) {
    return status;
  }

  if (enable_qdcm_colormodes_on_external_ <= QdcmOnExternal::LEGACY_QDCM) {
    color_mode_ = new SDMColorModeMgr(display_intf_);
  } else {
    color_mode_ = new SDMColorModeStc(display_intf_);
  }

  color_mode_->Init();

  SDMDisplay::TryDrawMethod(DisplayDrawMethod::kDrawUnified);

  return status;
}

void SDMDisplayPluggable::Destroy(SDMDisplay *sdm_display) {
  // Flush the display to have outstanding fences signaled.
  sdm_display->Flush();
  sdm_display->Deinit();
  delete sdm_display;
}

SDMDisplayPluggable::SDMDisplayPluggable(CoreInterface *core_intf,
                                         BufferAllocator *buffer_allocator,
                                         SDMCompositorCallbacks *callbacks,
                                         SDMDisplayEventHandler *event_handler, Display id,
                                         int32_t sdm_id)
    : SDMDisplay(core_intf, buffer_allocator, callbacks, event_handler, kPluggable, id, sdm_id,
                 DISPLAY_CLASS_PLUGGABLE) {}

DisplayError SDMDisplayPluggable::PreValidateDisplay(bool *exit_validate) {
  DTRACE_SCOPED();

  // Draw method gets set as part of first commit.
  SetDrawMethod();

  auto status = kErrorNone;
  bool res_exhausted = false;
  // If no resources are available for the current display, mark it for GPU by
  // pass and continue to do invalidate until the resources are available
  if (active_secure_sessions_[kSecureDisplay] || display_paused_ ||
      (mmrm_restricted_ &&
       (current_power_mode_ == SDMPowerMode::POWER_MODE_OFF ||
        current_power_mode_ == SDMPowerMode::POWER_MODE_DOZE_SUSPEND)) ||
      CheckResourceState(&res_exhausted)) {
    MarkLayersForGPUBypass();
    *exit_validate = true;
    return status;
  }

  BuildLayerStack();

  if (sdm_layer_stack_->layer_set_.empty()) {
    flush_ = !client_connected_;
    *exit_validate = true;
    return status;
  }

  // Checks and replaces layer stack for solid fill
  SolidFillPrepare();

  // Apply current Color Mode and Render Intent.
  status = color_mode_->ApplyCurrentColorModeWithRenderIntent(
      static_cast<bool>(layer_stack_.flags.hdr_present));
  if (status != kErrorNone || color_tranform_failed_) {
    // Fallback to GPU Composition if Color Mode can't be applied or if a color
    // tranform needs to be applied.
    MarkLayersForClientComposition();
  }

  *exit_validate = false;

  return status;
}

DisplayError SDMDisplayPluggable::Validate(uint32_t *out_num_types,
                                           uint32_t *out_num_requests) {
  bool exit_validate = false;
  auto status = PreValidateDisplay(&exit_validate);
  if (exit_validate) {
    return status;
  }

  // TODO(user): SetRefreshRate need to follow new interface when added.

  return PrepareLayerStack(out_num_types, out_num_requests);
}

DisplayError
SDMDisplayPluggable::PostCommitLayerStack(shared_ptr<Fence> *out_retire_fence) {
  DTRACE_SCOPED();
  auto status = kErrorNone;

  HandleFrameOutput();
  status = SDMDisplay::PostCommitLayerStack(out_retire_fence);

  return status;
}

DisplayError SDMDisplayPluggable::Present(shared_ptr<Fence> *out_retire_fence) {
  auto status = kErrorNone;
  bool res_exhausted = false;

  if (!active_secure_sessions_[kSecureDisplay] && !display_paused_ &&
      !(mmrm_restricted_ &&
        (current_power_mode_ == SDMPowerMode::POWER_MODE_OFF ||
         current_power_mode_ == SDMPowerMode::POWER_MODE_DOZE_SUSPEND))) {
    // Proceed only if any resources are available to be allocated for the
    // current display, Otherwise keep doing invalidate
    if (CheckResourceState(&res_exhausted)) {
      Refresh();
      return status;
    }

    status = SDMDisplay::CommitLayerStack();
    if (status == kErrorNone) {
      status = PostCommitLayerStack(out_retire_fence);
    }
  }
  return status;
}

void SDMDisplayPluggable::ApplyScanAdjustment(SDMRect *display_frame) {
  if ((underscan_width_ <= 0) || (underscan_height_ <= 0)) {
    return;
  }

  float width_ratio = FLOAT(underscan_width_) / 100.0f;
  float height_ratio = FLOAT(underscan_height_) / 100.0f;

  uint32_t mixer_width = 0;
  uint32_t mixer_height = 0;
  GetMixerResolution(&mixer_width, &mixer_height);

  if (mixer_width == 0 || mixer_height == 0) {
    DLOGV("Invalid mixer dimensions (%d, %d)", mixer_width, mixer_height);
    return;
  }

  uint32_t new_mixer_width = UINT32(mixer_width * FLOAT(1.0f - width_ratio));
  uint32_t new_mixer_height = UINT32(mixer_height * FLOAT(1.0f - height_ratio));

  int x_offset = INT((FLOAT(mixer_width) * width_ratio) / 2.0f);
  int y_offset = INT((FLOAT(mixer_height) * height_ratio) / 2.0f);

  display_frame->left =
      (display_frame->left * INT32(new_mixer_width) / INT32(mixer_width)) +
      x_offset;
  display_frame->top =
      (display_frame->top * INT32(new_mixer_height) / INT32(mixer_height)) +
      y_offset;
  display_frame->right =
      ((display_frame->right * INT32(new_mixer_width)) / INT32(mixer_width)) +
      x_offset;
  display_frame->bottom = ((display_frame->bottom * INT32(new_mixer_height)) /
                           INT32(mixer_height)) +
                          y_offset;
}

static void AdjustSourceResolution(uint32_t dst_width, uint32_t dst_height,
                                   uint32_t *src_width, uint32_t *src_height) {
  *src_height = (dst_width * (*src_height)) / (*src_width);
  *src_width = dst_width;
}

void SDMDisplayPluggable::GetDownscaleResolution(uint32_t primary_width,
                                                 uint32_t primary_height,
                                                 uint32_t *non_primary_width,
                                                 uint32_t *non_primary_height) {
  uint32_t primary_area = primary_width * primary_height;
  uint32_t non_primary_area = (*non_primary_width) * (*non_primary_height);

  if (primary_area > non_primary_area) {
    if (primary_height > primary_width) {
      std::swap(primary_height, primary_width);
    }
    AdjustSourceResolution(primary_width, primary_height, non_primary_width,
                           non_primary_height);
  }
}

void SDMDisplayPluggable::GetUnderScanConfig() {
  if (!display_intf_->IsUnderscanSupported()) {
    // Read user defined underscan width and height
    SDMDebugHandler::Get()->GetProperty(EXTERNAL_ACTION_SAFE_WIDTH_PROP,
                                        &underscan_width_);
    SDMDebugHandler::Get()->GetProperty(EXTERNAL_ACTION_SAFE_HEIGHT_PROP,
                                        &underscan_height_);
  }
}

DisplayError SDMDisplayPluggable::Flush() {
  return display_intf_->Flush(&layer_stack_);
}

DisplayError SDMDisplayPluggable::GetColorModes(uint32_t *out_num_modes,
                                                SDMColorMode *out_modes) {
  if (out_modes == nullptr) {
    *out_num_modes = color_mode_->GetColorModeCount();
  } else {
    color_mode_->GetColorModes(out_num_modes, out_modes);
  }
  return kErrorNone;
}

DisplayError
SDMDisplayPluggable::GetRenderIntents(SDMColorMode mode,
                                      uint32_t *out_num_intents,
                                      SDMRenderIntent *out_intents) {
  if (out_intents == nullptr) {
    *out_num_intents = color_mode_->GetRenderIntentCount(mode);
  } else {
    color_mode_->GetRenderIntents(mode, out_num_intents, out_intents);
  }
  return kErrorNone;
}

DisplayError SDMDisplayPluggable::SetColorMode(SDMColorMode mode) {
  return SetColorModeWithRenderIntent(mode, SDMRenderIntent::COLORIMETRIC);
}

DisplayError
SDMDisplayPluggable::SetColorModeWithRenderIntent(SDMColorMode mode,
                                                  SDMRenderIntent intent) {
  auto status = color_mode_->CacheColorModeWithRenderIntent(mode, intent);
  if (status != kErrorNone) {
    DLOGE("failed for mode = %d intent = %d", mode, intent);
    return status;
  }

  callbacks_->OnRefresh(id_);

  return status;
}

DisplayError SDMDisplayPluggable::RestoreColorTransform() {
  auto status = color_mode_->RestoreColorTransform();
  if (status != kErrorNone) {
    DLOGE("failed to RestoreColorTransform");
    return status;
  }

  callbacks_->OnRefresh(id_);

  return status;
}

DisplayError SDMDisplayPluggable::SetColorTransform(const float *matrix, SDMColorTransform hint) {
  if (!matrix) {
    return kErrorNotSupported;
  }

  auto status = color_mode_->SetColorTransform(matrix, hint);
  if (status != kErrorNone) {
    DLOGE("failed for hint = %d", hint);
    color_tranform_failed_ = true;
    return status;
  }

  callbacks_->OnRefresh(id_);
  color_tranform_failed_ = false;

  return status;
}

DisplayError SDMDisplayPluggable::Perform(uint32_t operation, ...) {
  va_list args;
  va_start(args, operation);
  int val = 0;
  LayerSolidFill *solid_fill_color;
  LayerRect *rect = NULL;

  switch (operation) {
  case SET_QDCM_SOLID_FILL_INFO:
    solid_fill_color = va_arg(args, LayerSolidFill *);
    SetQDCMSolidFillInfo(true, *solid_fill_color);
    break;
  case UNSET_QDCM_SOLID_FILL_INFO:
    solid_fill_color = va_arg(args, LayerSolidFill *);
    SetQDCMSolidFillInfo(false, *solid_fill_color);
    break;
  case SET_QDCM_SOLID_FILL_RECT:
    rect = va_arg(args, LayerRect *);
    solid_fill_rect_ = *rect;
    break;
  default:
    DLOGW("Invalid operation %d", operation);
    va_end(args);
    return kErrorNotSupported;
  }
  va_end(args);

  return kErrorNone;
}

void SDMDisplayPluggable::SetQDCMSolidFillInfo(bool enable,
                           const LayerSolidFill &color) {
  solid_fill_enable_ = enable;
  solid_fill_color_ = color;
}

DisplayError SDMDisplayPluggable::SetDetailEnhancerConfig(
    const DisplayDetailEnhancerData &de_data) {
  DisplayError error = kErrorNotSupported;

  if (display_intf_) {
    error = display_intf_->SetDetailEnhancerData(de_data);
  }
  return error;
}

DisplayError SDMDisplayPluggable::SetHWDetailedEnhancerConfig(void *params) {
  DisplayError err = kErrorNone;
  DisplayDetailEnhancerData de_data;

  PPDETuningCfgData *de_tuning_cfg_data =
    reinterpret_cast<PPDETuningCfgData *>(params);
  if (de_tuning_cfg_data->cfg_pending) {
    if (!de_tuning_cfg_data->cfg_en) {
      de_data.enable = 0;
      DLOGV_IF(kTagQDCM, "Disable DE config");
  } else {
      de_data.override_flags = kOverrideDEEnable;
      de_data.enable = 1;
#ifdef DISP_DE_LPF_BLEND
      DLOGV_IF(
        kTagQDCM,
        "Enable DE: flags %u, sharp_factor %d, thr_quiet %d, thr_dieout %d, "
        "thr_low %d, thr_high %d, clip %d, quality %d, content_type %d, "
        "de_blend %d, "
        "de_lpf_h %d, de_lpf_m %d, de_lpf_l %d",
        de_tuning_cfg_data->params.flags,
        de_tuning_cfg_data->params.sharp_factor,
        de_tuning_cfg_data->params.thr_quiet,
        de_tuning_cfg_data->params.thr_dieout,
        de_tuning_cfg_data->params.thr_low,
        de_tuning_cfg_data->params.thr_high, de_tuning_cfg_data->params.clip,
        de_tuning_cfg_data->params.quality,
        de_tuning_cfg_data->params.content_type,
        de_tuning_cfg_data->params.de_blend,
        de_tuning_cfg_data->params.de_lpf_h,
        de_tuning_cfg_data->params.de_lpf_m,
        de_tuning_cfg_data->params.de_lpf_l);
#endif
      if (de_tuning_cfg_data->params.flags & kDeTuningFlagSharpFactor) {
        de_data.override_flags |= kOverrideDESharpen1;
        de_data.sharp_factor = de_tuning_cfg_data->params.sharp_factor;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagClip) {
        de_data.override_flags |= kOverrideDEClip;
        de_data.clip = de_tuning_cfg_data->params.clip;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrQuiet) {
        de_data.override_flags |= kOverrideDEThrQuiet;
        de_data.thr_quiet = de_tuning_cfg_data->params.thr_quiet;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrDieout) {
        de_data.override_flags |= kOverrideDEThrDieout;
        de_data.thr_dieout = de_tuning_cfg_data->params.thr_dieout;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrLow) {
        de_data.override_flags |= kOverrideDEThrLow;
        de_data.thr_low = de_tuning_cfg_data->params.thr_low;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagThrHigh) {
        de_data.override_flags |= kOverrideDEThrHigh;
        de_data.thr_high = de_tuning_cfg_data->params.thr_high;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagContentQualLevel) {
        switch (de_tuning_cfg_data->params.quality) {
        case kDeContentQualLow:
          de_data.quality_level = kContentQualityLow;
          break;
        case kDeContentQualMedium:
          de_data.quality_level = kContentQualityMedium;
          break;
        case kDeContentQualHigh:
          de_data.quality_level = kContentQualityHigh;
          break;
        case kDeContentQualUnknown:
        default:
          de_data.quality_level = kContentQualityUnknown;
          break;
        }
      }

      switch (de_tuning_cfg_data->params.content_type) {
      case kDeContentTypeVideo:
        de_data.content_type = kContentTypeVideo;
        break;
      case kDeContentTypeGraphics:
        de_data.content_type = kContentTypeGraphics;
        break;
      case kDeContentTypeUnknown:
      default:
        de_data.content_type = kContentTypeUnknown;
        break;
      }

      if (de_tuning_cfg_data->params.flags & kDeTuningFlagDeBlend) {
        de_data.override_flags |= kOverrideDEBlend;
        de_data.de_blend = de_tuning_cfg_data->params.de_blend;
      }
#ifdef DISP_DE_LPF_BLEND
      if (de_tuning_cfg_data->params.flags & kDeTuningFlagDeLpfBlend) {
        de_data.override_flags |= kOverrideDELpfBlend;
        de_data.de_lpf_en = true;
        de_data.de_lpf_h = de_tuning_cfg_data->params.de_lpf_h;
        de_data.de_lpf_m = de_tuning_cfg_data->params.de_lpf_m;
        de_data.de_lpf_l = de_tuning_cfg_data->params.de_lpf_l;
      }
#endif
    }
    err = SetDetailEnhancerConfig(de_data);
    if (err) {
      DLOGW("SetDetailEnhancerConfig failed. err = %d", err);
    }
    de_tuning_cfg_data->cfg_pending = false;
    }
    return err;
}

DisplayError
SDMDisplayPluggable::NotifyDisplayCalibrationMode(bool in_calibration) {
  auto status = color_mode_->NotifyDisplayCalibrationMode(in_calibration);
  if (status != kErrorNone) {
    DLOGE("Failed for notify QDCM mode = %d", in_calibration);
    return status;
  }

  return status;
}

DisplayError SDMDisplayPluggable::SetupVRRConfig() {
  // Enable Variable Refresh Rate state
  DisplayError error = display_intf_->SetVRRState(true);
  if (error != kErrorNone) {
    return error;
  }

  for (auto &[config_id, config] : variable_config_map_) {
    if (config.avr_step > 0) {
      // Publish AVR Step period as the Vsync Period for an AVR Step enabled mode.
      config.vsync_period_ns = (1000.f / static_cast<float>(config.avr_step)) * 1000000;
    }
  }

  return error;
}


DisplayError SDMDisplayPluggable::SetQSyncMode(QSyncMode qsync_mode) {
  // Client needs to ensure that config change and qsync mode change
  // are not triggered in the same drawcycle.
  if (pending_config_) {
    DLOGE("Failed to set qsync mode. Pending active config transition");
    return kErrorNotSupported;
  }

  auto err = display_intf_->SetQSyncMode(qsync_mode);
  if (err != kErrorNone) {
    return kErrorNotSupported;
  }

  return kErrorNone;
}

} // namespace sdm
