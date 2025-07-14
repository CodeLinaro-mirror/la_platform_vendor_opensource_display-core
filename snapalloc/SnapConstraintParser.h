// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifndef __SNAP_CONSTRAINT_PARSER_H__
#define __SNAP_CONSTRAINT_PARSER_H__

#include "SnapConstraintDefs.h"
#include "SnapTypes.h"
#include "SnapUtils.h"

#include <map>
#include <mutex>
#include <vector>

namespace snapalloc {

class SnapConstraintParser {
 public:
  SnapConstraintParser(SnapConstraintParser &other) = delete;
  void operator=(const SnapConstraintParser &) = delete;
  static SnapConstraintParser *GetInstance();

  int ParseFormats(
      std::map<vendor_qti_hardware_display_common_PixelFormat, FormatData> *format_data_map);
  int ParseAlignments(const std::string &json_path,
                      std::unordered_map<SnapFormatDescriptor, BufferConstraints,
                                         SnapFormatDescriptorHash> *constraint_set_map);
  int GetBufferConstraints(std::unordered_map<SnapFormatDescriptor, BufferConstraints,
                                              SnapFormatDescriptorHash> &constraint_set_map_,
                           BufferDescriptor desc, BufferConstraints *out);

 private:
  ~SnapConstraintParser();
  SnapConstraintParser(){};
  static std::mutex constraint_parser_mutex_;

  static SnapConstraintParser *instance_;

  bool StringToEnumType(std::string input, vendor_qti_hardware_display_common_PixelFormat *output);
  bool StringToEnumType(std::string input,
                        vendor_qti_hardware_display_common_PlaneLayoutComponentType *output);
};

}  // namespace snapalloc

#endif  // __SNAP_CONSTRAINT_PARSER_H__