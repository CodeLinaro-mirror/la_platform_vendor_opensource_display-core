/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __VM_FILE_XFER_FACT_INTF_EXTN_H__
#define __VM_FILE_XFER_FACT_INTF_EXTN_H__

#include <private/tvm_service_manager_intf.h>
#include <private/cb_intf.h>
#include <private/vm_file_xfer_intf.h>
#include <core/buffer_allocator.h>

#define GET_VM_FILE_XFER_CLIENT_FACT_INTF_EXTN "GetVmFileXferClientFactIntfExtn"

namespace sdm {

class VmFileXferClientFactIntfExtn {
 public:
  virtual ~VmFileXferClientFactIntfExtn() {}
  virtual std::shared_ptr<VMFileXferIntf> CreateVmFileXferClient(
      SdmDisplayCbInterface<TvmServiceCbEvent> *cb_intf, BufferAllocator *buffer_allocator) = 0;
};

extern "C" VmFileXferClientFactIntfExtn *GetVmFileXferClientFactIntfExtn();

}  // namespace sdm
#endif  // __VM_FILE_XFER_FACT_INTF_EXTN_H__
