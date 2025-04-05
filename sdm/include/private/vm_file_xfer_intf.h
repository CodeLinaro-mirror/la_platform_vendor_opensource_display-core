/*
 * Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __VM_FILE_XFER_INTF_H__
#define __VM_FILE_XFER_INTF_H__

#include <private/generic_intf.h>
#include <private/generic_payload.h>

namespace sdm {

enum VMFileXferOps { kVMFileTransferOpsQueryFile, kVMFileTransferOpsMax = 0xff };

enum VMFileXferParams {
  kVMFileTransferParamsSetDirectory,
  kVMFileTransferParamRetrieve,
  kVMFileTransferParamsStore,
  kVMFileTransferParamsListFiles,
  kVMFileTransferParamsDeleteFiles,
  kVMFileTransferParamsMax = 0xff
};

struct VmFile {
  std::string file = "";
};

// Pass Remote file path
struct VMFileXferRetrieveInput {
  std::string remote_file_path = "";
  std::string local_file_path = "";
  int remote_file_size;
};

// Pass Local file path
struct VMFileXferStoreInput {
  std::string local_file_path = "";
  bool overwrite = false;
  bool size_check = false;
};

// Query remote file
struct VMFileXferQueryInput {
  std::string remote_file_path = "";
};

// Get Remote file size
struct VMFileXferQueryOutput {
  int size;
};

struct VMFileXferDeleteInput {
  uint64_t panel_id;
  bool delete_tn;
  bool delete_t0;
};

using VMFileXferIntf = sdm::GenericIntf<VMFileXferParams, VMFileXferOps, sdm::GenericPayload>;

}  // namespace sdm
#endif  // __VM_FILE_XFER_INTF_H__
