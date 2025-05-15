// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "DefaultConstraintProvider.h"

#include <dlfcn.h>
#include <fstream>
#include <iostream>

namespace snapalloc {
DefaultConstraintProvider *DefaultConstraintProvider::instance_{nullptr};
std::mutex DefaultConstraintProvider::default_provider_mutex_;

DefaultConstraintProvider *DefaultConstraintProvider::GetInstance(
    std::map<vendor_qti_hardware_display_common_PixelFormat, FormatData> format_data_map) {
  std::lock_guard<std::mutex> lock(default_provider_mutex_);

  if (instance_ == nullptr) {
    instance_ = new DefaultConstraintProvider();
    instance_->Init(format_data_map);
  }
  return instance_;
}

void DefaultConstraintProvider::Init(
    std::map<vendor_qti_hardware_display_common_PixelFormat, FormatData> format_data_map) {
  parser_ = SnapConstraintParser::GetInstance();
  parser_->ParseAlignments("/vendor/etc/display/default_alignments.json", &constraint_set_map_);
}

int DefaultConstraintProvider::GetCapabilities(BufferDescriptor desc, CapabilitySet *out) {
  // Only call default if no others are enabled so that it can always set out->enabled to true
  out->enabled = true;
  // Default constraint provider is not tied to HW, so it does not have a UBWC version
  out->ubwc_caps.version = 0;
  (void)desc;
  DLOGD_IF(enable_logs, "DefaultConstraintProvider is enabled");
  return 0;
}

int DefaultConstraintProvider::GetConstraints(BufferDescriptor desc, BufferConstraints *out) {
  if (constraint_set_map_.empty()) {
    DLOGD_IF(enable_logs, "Default constraint set map is empty");
    return -1;
  }
  if (!(parser_->GetBufferConstraints(constraint_set_map_, desc, out))) {
    DLOGD_IF(enable_logs, "Default could not find entry for format %lu & modifier %d",
             static_cast<uint64_t>(desc.format), GetPixelFormatModifier(desc));
  }
  return 0;
}

}  // namespace snapalloc