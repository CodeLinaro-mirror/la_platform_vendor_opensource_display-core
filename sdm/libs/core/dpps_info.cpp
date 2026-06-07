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

#include <algorithm>
#include <dlfcn.h>
#include <utils/debug.h>
#include <display_properties.h>

#include "dpps_info.h"

#undef __CLASS__
#define __CLASS__ "DppsInfo"

namespace sdm {

DppsInterface *DppsInfo::dpps_intf_ = NULL;
std::vector<int32_t> DppsInfo::display_id_ = {};

void DppsInfo::Init(DppsPropIntf *intf, const std::string &panel_name,
                    DisplayInterface *display_intf, PanelFeaturePropertyIntf *prop_intf) {
  std::lock_guard<std::mutex> guard(lock_);
  int error = 0;
  int disable_dpps_features = 0;

  if (!intf || !display_intf || !prop_intf) {
    DLOGE("Invalid intf %pK display_intf %pK prop_intf %pK", intf, display_intf, prop_intf);
    return;
  }

  DppsDisplayInfo info_payload = {};
  DisplayError ret = intf->DppsProcessOps(kDppsGetDisplayInfo, &info_payload, sizeof(info_payload));
  if (ret != kErrorNone) {
    DLOGE("Get display information failed, ret %d", ret);
    return;
  }

  if (std::find(display_id_.begin(), display_id_.end(), info_payload.display_id) !=
      display_id_.end()) {
    return;
  }
  DLOGI("Ready to register display %d-%d ", info_payload.display_id, info_payload.display_type);

  Debug::GetProperty(DISABLE_DPPS_FEATURES, &disable_dpps_features);
  if (disable_dpps_features) {
    if (!dpps_intf_) {
      dpps_intf_ = new DppsDummyImpl();
    }
  }

  if (!dpps_intf_) {
    if (!dpps_impl_lib_.Open(kDppsLib_)) {
      DLOGW("Failed to load Dpps lib %s", kDppsLib_);
      goto exit;
    }

    if (!dpps_impl_lib_.Sym("GetDppsInterface", reinterpret_cast<void **>(&GetDppsInterface))) {
      DLOGE("GetDppsInterface not found!, err %s", dlerror());
      goto exit;
    }

    dpps_intf_ = GetDppsInterface();
    if (!dpps_intf_) {
      DLOGE("Failed to get Dpps Interface!");
      goto exit;
    }
  }
  error = dpps_intf_->Init(intf, panel_name, display_intf, prop_intf);
  if (error) {
    DLOGE("DPPS Interface init failure with err %d", error);
    goto exit;
  }

  display_id_.push_back(info_payload.display_id);
  DLOGI("Registered display %d-%d successfully", info_payload.display_id,
        info_payload.display_type);
  return;

exit:
  Deinit_nolock();
  if (!dpps_intf_) {
    dpps_intf_ = new DppsDummyImpl();
    display_id_.push_back(info_payload.display_id);
  }
}

void DppsInfo::Deinit_nolock() {
  if (dpps_intf_) {
    dpps_intf_->Deinit();
    dpps_intf_ = NULL;
  }
  dpps_impl_lib_.~DynLib();
  DLOGI("Dpps info deinit done");
}

void DppsInfo::Deinit() {
  std::lock_guard<std::mutex> guard(lock_);
  Deinit_nolock();
}

void DppsInfo::DppsNotifyOps(enum DppsNotifyOps op, void *payload, size_t size) {
  int ret = 0;
  if (!dpps_intf_) {
    DLOGW("Dpps intf nullptr");
    return;
  }
  ret = dpps_intf_->DppsNotifyOps(op, payload, size);
  if (ret) {
    DLOGE("DppsNotifyOps op %d error %d", op, ret);
  }
}

}  // namespace sdm