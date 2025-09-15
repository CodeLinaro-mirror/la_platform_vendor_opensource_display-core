/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DEMURATN_VALIDATOR_INTF_H__
#define __DEMURATN_VALIDATOR_INTF_H__

#include <private/generic_intf.h>
#include <private/generic_payload.h>

namespace sdm {

enum DemuraTnValidatorParams {
  /* setter */
  /* Clean up corrupted and old files, input: empty */
  kDemuraTnValidatorCleanupFiles,
  /* Delete the files, input: struct DemuraPanelInfo */
  kDemuraTnValidatorDeleteFiles,
  /* Double buffer codebook flags, input: bool */
  kDemuraTnValidatorDoubleBufferCodebookFlags,
  kDemuraTnValidatorParamsMax = 0xff,
};

enum DemuraTnValidatorOps {
  /* Verify the config files, input: struct DemuraPanelInfo, output: bool */
  kDemuraTnValidatorVerifyFiles,
  kDemuraTnValidatorOpsMax = 0xff,
};

using DemuraTnValidatorIntf =
    GenericIntf<DemuraTnValidatorParams, DemuraTnValidatorOps, GenericPayload>;
}  // namespace sdm
#endif  // __DEMURATN_VALIDATOR_INTF_H__
