// Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#ifndef __DEBUG_H__
#define __DEBUG_H__

#include "display_properties.h"
#include <errno.h>
#include <string>

#include "debug_callback_intf.h"

#define PROPERTY_VALUE_MAX 255

using ::sdm::DebugCallbackIntf;

// TODO: add macro that can be used to simplify log function

namespace snapalloc {
class Debug {
 public:
  static Debug *GetInstance();
  void RegisterDebugCallback(DebugCallbackIntf *cb);
  int GetProperty(const char *property_name, char *value);
  int GetProperty(const char *property_name, int *value);
  bool IsAhardwareBufferDisabled();
  bool IsUBWCDisabled();
  bool IsSecurePreviewBufferFormatEnabled(std::string *secure_preview_buffer_format);
  bool IsSecurePreviewOnlyEnabled();
  bool UseDMABufHeaps();
  bool UseSystemHeapForSensors();
  bool HwSupportsUBWCP();
  void Log(sdm::DebugLogType type, const char *fmt, ...);

 private:
  DebugCallbackIntf *debug_callback_ = nullptr;
};
}  // namespace snapalloc
#endif  // __DEBUG_H__
