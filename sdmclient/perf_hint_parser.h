/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __PERF_HINT_PARSER_H__
#define __PERF_HINT_PARSER_H__

#include "core/sdm_types.h"
#include <unordered_map>

using std::pair;
using std::unordered_map;

namespace sdm {

class PerfHintParser {
 public:
  DisplayError GetPerfHintThresholds(unordered_map<int32_t, int32_t> *fps_to_threshold);
  DisplayError Init();

 private:
  const char *kPerfThresholdXmlPath = "/vendor/etc/display/perf_hint_threshold.xml";
  unordered_map<int32_t, int32_t> fps_to_threshold_map_;

  bool LoadPerfHintThresholdFromFile(const char *file_name);
};

}  // namespace sdm

#endif  // __PERF_HINT_PARSER_H__
