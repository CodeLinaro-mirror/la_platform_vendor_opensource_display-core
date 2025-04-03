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
 * ​​​​​Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <algorithm>
#include <utils/constants.h>
#include <utils/debug.h>
#include <dlfcn.h>

#include "sdm_debugger.h"
#include "sdm_display_null.h"

#define __CLASS__ "SDMDisplayNull"

namespace sdm {

SDMDisplayNull::SDMDisplayNull(CoreInterface *core_intf, BufferAllocator *buffer_allocator,
                               SDMCompositorCallbacks *callbacks,
                               SDMDisplayEventHandler *event_handler, Display id, int32_t sdm_id)
    : SDMDisplay(core_intf, buffer_allocator, callbacks, event_handler, kBuiltIn, id, sdm_id,
                 DISPLAY_CLASS_BUILTIN) {}

DisplayError SDMDisplayNull::Create(CoreInterface *core_intf, BufferAllocator *buffer_allocator,
                                    SDMCompositorCallbacks *callbacks,
                                    SDMDisplayEventHandler *event_handler, Display id,
                                    int32_t sdm_id, SDMDisplay **sdm_display) {
  uint32_t null_width = 0;
  uint32_t null_height = 0;

  SDMDisplay *sdm_display_null =
      new SDMDisplayNull(core_intf, buffer_allocator, callbacks, event_handler, id, sdm_id);

  auto status = sdm_display_null->Init();
  if (status) {
    DLOGE("SDM Display Null Init Failed\n");
    delete sdm_display_null;
    return status;
  }

  DisplayError error = sdm_display_null->GetMixerResolution(&null_width, &null_height);
  if (error != kErrorNone) {
    Destroy(sdm_display_null);
    return error;
  }

  status = sdm_display_null->SetFrameBufferResolution(null_width, null_height);
  if (status) {
    DLOGE("Error in SetFrameBufferResolution\n");
    Destroy(sdm_display_null);
    return status;
  }

  error = sdm_display_null->GetMixerResolution(&null_width, &null_height);
  if (error != kErrorNone) {
    Destroy(sdm_display_null);
    return error;
  }

  DLOGI("SDMDisplayNull created successfully\n");
  *sdm_display = sdm_display_null;
  return status;
}

DisplayError SDMDisplayNull::CommitOrPrepare(bool validate_only,
                                             shared_ptr<Fence> *out_retire_fence,
                                             uint32_t *out_num_types, uint32_t *out_num_requests,
                                             bool *needs_commit) {
  *needs_commit = false;
  return display_intf_->CommitOrPrepare(&layer_stack_);
}

void SDMDisplayNull::Destroy(SDMDisplay *sdm_display) {
  sdm_display->Flush();
  sdm_display->Deinit();
  delete sdm_display;
}

DisplayError SDMDisplayNull::Deinit(bool deinit_layer_builder) {
  auto error = core_intf_->DestroyNullDisplay(display_intf_);
  if (kErrorNone != error) {
    DLOGE("NullDisplay destroy failed. Error = %d", error);
    return error;
  }

  return kErrorNone;
}

DisplayError SDMDisplayNull::Init() {
  auto status = core_intf_->CreateNullDisplay(&display_intf_);
  if (status) {
    DLOGE("Error in creating null display: %d\n", status);
    return status;
  }

  SDMDisplay::TryDrawMethod(DisplayDrawMethod::kDrawUnified);

  client_target_ = new SDMLayer(id_, buffer_allocator_);

  auto error = display_intf_->GetNumVariableInfoConfigs(&num_configs_);
  if (error != kErrorNone) {
    DLOGE("Getting config count failed. Error = %d", error);
    return kErrorNotSupported;
  }

  UpdateConfigs();

  const std::string snapalloc_lib_name = "vendor.qti.hardware.display.snapalloc-impl.so";
  void *snap_impl_lib_ = ::dlopen(snapalloc_lib_name.c_str(), RTLD_NOW);
  if (!snap_impl_lib_) {
    DLOGE("Dlopen error for snapalloc impl: %s", dlerror());
    return kErrorPermission;
  }

  std::shared_ptr<ISnapMapper> (*LINK_FETCH_ISnapMapper)(DebugCallbackIntf *) = nullptr;
  *reinterpret_cast<void **>(&LINK_FETCH_ISnapMapper) =
      ::dlsym(snap_impl_lib_, "FETCH_ISnapMapper");
  if (LINK_FETCH_ISnapMapper) {
    snapmapper_ = LINK_FETCH_ISnapMapper(nullptr);
  } else {
    DLOGE("Failed to get snapalloc instance");
  }

  display_intf_->GetQsyncFps(&qsync_fps_);

  display_intf_->GetRefreshRateRange(&min_refresh_rate_, &max_refresh_rate_);
  current_refresh_rate_ = max_refresh_rate_;

  DisplayConfigFixedInfo fixed_info = {};
  display_intf_->GetConfig(&fixed_info);
  is_cmd_mode_ = fixed_info.is_cmdmode;

  if (!sdm_layer_stack_) {
    DLOGE("sdm layer stack not present");
    return kErrorParameters;
  }

  return status;
}

DisplayError SDMDisplayNull::Present(shared_ptr<Fence> *out_retire_fence) {
  auto status = kErrorNone;
  status = SDMDisplay::CommitLayerStack();
  if (status == kErrorNone) {
    status = SDMDisplay::PostCommitLayerStack(out_retire_fence);
  }
  return status;
}

DisplayError SDMDisplayNull::Flush() {
  return display_intf_->Flush(&layer_stack_);
}

}  // namespace sdm