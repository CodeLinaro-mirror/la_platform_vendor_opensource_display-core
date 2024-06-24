// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "CPUConstraintProvider.h"

#include <dlfcn.h>
#include <fstream>
#include <iostream>


namespace snapalloc {

CPUConstraintProvider *CPUConstraintProvider::instance_{nullptr};
std::mutex CPUConstraintProvider::cpu_provider_mutex_;

CPUConstraintProvider *CPUConstraintProvider::GetInstance(
    std::map<vendor_qti_hardware_display_common_PixelFormat, FormatData> format_data_map) {
  std::lock_guard<std::mutex> lock(cpu_provider_mutex_);

  if (instance_ == nullptr) {
    instance_ = new CPUConstraintProvider();
    instance_->Init(format_data_map);
  }
  return instance_;
}

void CPUConstraintProvider::Init(
    std::map<vendor_qti_hardware_display_common_PixelFormat, FormatData> format_data_map) {
  parser_ = SnapConstraintParser::GetInstance();
  parser_->ParseAlignments("/vendor/etc/display/cpu_alignments.json", &constraint_set_map_);
}

int CPUConstraintProvider::GetCapabilities(BufferDescriptor desc, CapabilitySet *out) {
  if (CpuCanAccess(desc.usage)) {
    DLOGD_IF(enable_logs, "CPUConstraintProvider is enabled");
    out->enabled = true;
  }

  if (IsAstc(desc.format)) {
    out->enabled = false;
  }

  out->ubwc_caps.version = 0;
  return 0;
}

int CPUConstraintProvider::GetConstraints(BufferDescriptor desc, BufferConstraints *out) {
  if (constraint_set_map_.empty()) {
    DLOGD_IF(enable_logs, "CPU constraint set map is empty");
    return -1;
  }
  if (!(parser_->GetBufferConstraints(constraint_set_map_, desc, out))) {
    DLOGD_IF(enable_logs, "CPU could not find entry for format %lu & modifier %d",
             static_cast<uint64_t>(desc.format), GetPixelFormatModifier(desc));
  }
  return 0;
}

}  // namespace snapalloc
