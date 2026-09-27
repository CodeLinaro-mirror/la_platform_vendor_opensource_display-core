/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#ifndef __FRAME_CAPTURE_INTF_H__
#define __FRAME_CAPTURE_INTF_H__

#include <core/buffer_allocator.h>
#include <core/display_interface.h>

namespace sdm {
enum CWBEyeIndex {
  kLeftEye = 0,  // Dump for left eye
  kRightEye,     // Dump for right eye
  kBothEyes,     // Dump both eyes (full display width, binocular mode only)
};

enum CWBField {
  kAllFSCField = 0,  // Dump all the FSC field or RGB display
  kRedFscField,      // Only dump red FSC field
  kGreenFscField,    // Only dump green FSC field
  kBlueFscField,     // Only dump blue FSC field
};

struct CWBPacketData {
  bool stop_cwb = false;
  CWBEyeIndex eye_index = kLeftEye;
  CWBField field_index = kAllFSCField;
  int32_t frame_dump_count = 0;
  CwbConfig cwb_config = {};
  uint32_t buffer_depth = 5;       //!< Total number of output buffers to allocate in the pool.
                                   //!< Optional, defaults to 20. Controls memory usage.
                                   //!< The number of in-flight slots is derived automatically:
                                   //!< 2 if cwb_config.num_parallel_buffers > 1, else 1.
  bool enable_compression = true;  //!< Enable LZ4 compression on captured frames.
                                   //!< Optional, defaults to true. Set to false to write raw
                                   //!< uncompressed BGR888 frames directly to the output file.
};

class FrameCaptureIntf {
 public:
  virtual ~FrameCaptureIntf() = default;
  static int Create(DisplayInterface *display_intf, BufferAllocator *buffer_allocator,
                    FrameCaptureIntf **intf);
  static int Destroy(FrameCaptureIntf *intf);
  virtual void NotifyCwbDone(int32_t status, const LayerBuffer &buffer) = 0;
  virtual int ConfigureFCM(CWBPacketData &data) = 0;
  virtual bool IsCwbActive() const = 0;  // true while FCM capture is in progress

 private:
  virtual int Init() = 0;
  virtual int DeInit() = 0;
};

}  // namespace sdm
#endif  // __FRAME_CAPTURE_INTF_H__
