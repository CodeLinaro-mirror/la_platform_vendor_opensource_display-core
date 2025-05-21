// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear


#include "ISnapMemAllocBackend.h"

#include "SnapDMAAllocator.h"
#include "SnapDMABufHeapAllocator.h"
#include "SnapTestAllocator.h"

#define DMABUF_HEAP_ALLOCATE

namespace snapalloc {

ISnapMemAllocBackend *ISnapMemAllocBackend::GetInstance() {
#ifdef SHM_ALLOCATE
  return SnapTestAllocator::GetInstance();
#elif defined(DMABUF_HEAP_ALLOCATE)
  return SnapDMABufHeapAllocator::GetInstance();
#else
  return SnapDMAAllocator::GetInstance();
#endif
}

}  // namespace snapalloc
