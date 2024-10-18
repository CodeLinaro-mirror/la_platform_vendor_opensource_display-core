// Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "Debug.h"

#include <cstdarg>
#include <log/log.h>

namespace snapalloc {

Debug *Debug::GetInstance() {
  static Debug *debug_ = new Debug();
  return debug_;
}

void Debug::RegisterDebugCallback(DebugCallbackIntf *dbg) {
  // Check that debug intf isn't registered already
  if (!debug_callback_ && dbg) {
    debug_callback_ = dbg;
  }
}
int Debug::GetProperty(const char *property_name, int *value) {
  if (!debug_callback_ || debug_callback_->GetProperty(property_name, value)) {
    return -ENOTSUP;
  }
  return 0;
}
int Debug::GetProperty(const char *property_name, char *value) {
  if (!debug_callback_ || debug_callback_->GetProperty(property_name, value)) {
    return -ENOTSUP;
  }
  return 0;
}
bool Debug::IsAhardwareBufferDisabled() {
  int value = 0;
  GetProperty(DISABLE_AHARDWARE_BUFFER_PROP, &value);
  return (value == 1);
}
bool Debug::IsUBWCDisabled() {
  int value = 0;
  GetProperty(DISABLE_UBWC_PROP, &value);
  return (value == 1);
}
bool Debug::IsSecurePreviewBufferFormatEnabled(std::string *secure_preview_buffer_format) {
  char value[PROPERTY_VALUE_MAX] = "0";
  int error = GetProperty(SECURE_PREVIEW_BUFFER_FORMAT_PROP, value);
  if (error != 0) {
    return -ENOTSUP;
  }
  *secure_preview_buffer_format = value;
  return 0;
}
bool Debug::IsSecurePreviewOnlyEnabled() {
  int value = 0;
  GetProperty(SECURE_PREVIEW_ONLY_PROP, &value);
  return (value == 1);
}
bool Debug::UseDMABufHeaps() {
  int value = 0;
  GetProperty(USE_DMA_BUF_HEAPS_PROP, &value);
  return (value == 1);
}
bool Debug::HwSupportsUBWCP() {
  int value = 0;
  GetProperty(HW_SUPPORTS_UBWCP, &value);
  return (value == 1);
}

void Debug::Log(sdm::DebugLogType type, const char *fmt, ...) {
  if (debug_callback_) {
    std::va_list args;
    va_start(args, fmt);
    debug_callback_->Log(type, LOG_TAG, fmt, args);
  }
}

}  // namespace snapalloc
