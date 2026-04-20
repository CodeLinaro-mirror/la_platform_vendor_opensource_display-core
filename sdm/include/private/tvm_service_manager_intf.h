/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __TVM_SERVICE_MANAGER_INTF_H___
#define __TVM_SERVICE_MANAGER_INTF_H___

#include <private/generic_payload.h>
#include <private/generic_intf.h>
#include <private/display_cb_intf.h>
#include <string>

namespace sdm {

enum TvmDispServiceManagerParams {
  kStartVmFileTransferService,
  kStartDemuraTnService,
  kCheckMinkQrtrConnection,
  kRegisterAvfCallback,
  kDeRegisterAvfCallback,
  kTvmDispServiceManagerParamMax,
};

enum TvmDispServiceManagerOps {
  kTvmDispServiceManagerOpsMax,
};

enum TvmServiceCbEvent {
  kVmFileTransferServiceDead,
  kDemuraTnServiceDead,
  kVmUserspaceReady,
  kVmStopped,
  kTvmServiceEventsMax = 0xff
};

struct AvfCbInfo {
  std::string observer;
  SdmDisplayCbInterface<TvmServiceCbEvent> *cb;
};

using TvmDispServiceManagerIntf =
    GenericIntf<TvmDispServiceManagerParams, TvmDispServiceManagerOps, GenericPayload>;

}  // namespace sdm

#endif  // __TVM_SERVICE_MANAGER_INTF_H___
