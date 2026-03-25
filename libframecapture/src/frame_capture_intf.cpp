/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <frame_capture_intf.h>
#include "frame_capture_impl.h"

#define __CLASS__ "FrameCaptureIntf"

namespace sdm {

int FrameCaptureIntf::Create(DisplayInterface *display_intf, BufferAllocator *buffer_allocator,
                             FrameCaptureIntf **intf) {
  if (!display_intf || !buffer_allocator || !intf) {
    DLOGE("Invalid parameters");
    return -1;
  }

  FrameCaptureIntf *fcm = new FrameCaptureImpl(display_intf, buffer_allocator);
  if (!fcm) {
    DLOGE("Failed to create FCM");
    return -1;
  }

  int error = fcm->Init();
  if (error != 0) {
    DLOGE("FCM init failed");
    delete fcm;
    return -1;
  }

  *intf = fcm;

  return 0;
}

int FrameCaptureIntf::Destroy(FrameCaptureIntf *intf) {
  if (intf) {
    intf->DeInit();
    delete intf;
  }

  return 0;
}
}  // namespace sdm
