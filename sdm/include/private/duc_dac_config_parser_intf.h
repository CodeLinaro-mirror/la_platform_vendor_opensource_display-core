/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DUC_DAC_CONFIG_PARSER_INTF_H__
#define __DUC_DAC_CONFIG_PARSER_INTF_H__

#include <chrono>
#include <private/generic_intf.h>
#include <private/generic_payload.h>

#define DEFAULT_FEATURE_CONFIG_LOCATION "/vendor/etc/display/duc_dac_config.xml"

namespace sdm {

enum DucDacConfigParserParams {
  kDucDisable,                    // Getter: int
  kDucDisableSecondary,           // Getter: int
  kDucOverridePanelId,            // Getter: uint64_t
  kDucOverridePanelIdSecondary,   // Getter: uint64_t
  kDucMultiConfigCount,           // Getter: int
  kDucMultiConfigCountSecondary,  // Getter: int
  kDucFileRenameAllowed,          // Getter: int
  kDucEnableBLScreenRefresh,      // Getter: int
  kDucDisablePanelReplacement,    // Getter: int
  kDucDisableOptSingleLM,         // Getter: int
  kDucBypassConfigCopyToQMCS,     // Getter: int

  kDacPanelTimerInfo,                // Getter: DemuraTnPropertyInfo
  kDacPanelTimerInfoSecondary,       // DemuraTnPropertyInfo
  kDacServiceRetryWaitTime,          // Getter: int
  kDacDisableAODHandle,              // Getter: int
  kDacDisableCWBDownScale,           // Getter: int
  kDacDisableCWBDownScaleSecondary,  // Getter: int

  kDucDacParamsMax
};

enum DucDacConfigParserOps { kDucDacOpsMax };

struct DemuraTnTimers {
  std::chrono::milliseconds short_timer;
  std::chrono::milliseconds long_timer;
  std::chrono::milliseconds recalib_timer;
  std::chrono::milliseconds record_timer;
  std::chrono::milliseconds idle_timer;
  int recalib_timer_divider;
};

using DucDacConfigParserIntf =
    GenericIntf<DucDacConfigParserParams, DucDacConfigParserOps, GenericPayload>;
}  // namespace sdm
#endif  // __DUC_DAC_CONFIG_PARSER_INTF_H__