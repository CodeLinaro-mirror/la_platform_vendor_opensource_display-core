/*
* Copyright (c) 2019, 2021, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above
*      copyright notice, this list of conditions and the following
*      disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its
*      contributors may be used to endorse or promote products derived
*      from this software without specific prior written permission.

* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
* Changes from Qualcomm Innovation Center are provided under the following license:
* Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <drm/drm_fourcc.h>
#include <drm_utils.h>
#include <regex>
#include <sstream>
#include <sstream>
#include <string>
#include <string>
#include <utility>
#include <vector>

using std::string;
using std::stringstream;
using std::regex;
using std::pair;
using std::vector;

namespace sde_drm {

void ParseFormats(const string &line, vector<pair<uint32_t, uint64_t>> *formats) {
  // Match fourcc strings like RA24 or C8<space><space> or those with modifier like
  // RA24/5/1 or C8<space><space>/5/1. The digit after first / is vendor code, the digit
  // after second / is modifier code.
  regex exp_base("[[:alnum:]]{2}([[:alnum:]]{2}|[[:space:]]{2})(/[[:digit:]]/([[:digit:]]){1,3})?");
  regex exp_mod("[[:alnum:]]{2}([[:alnum:]]{2}|[[:space:]]{2})(/[[:digit:]]/([[:digit:]]){1,3})");
  string tmp_line = line;
  std::smatch str_match;  // Resultant match
  while (std::regex_search(tmp_line, str_match, exp_base)) { //clang_sa_ignore[core.CallAndMessage]
    string matched_sub_str = str_match.str();
    string final_format_str = {};
    uint64_t modifier = 0;

    if (std::regex_match(matched_sub_str, exp_mod)) {  //clang_sa_ignore[core.CallAndMessage]
      // Here we try to parse formats with vendor code and modifier like
      // RA24/5/1

      // Extract base format string
      final_format_str = matched_sub_str.substr(0, matched_sub_str.find("/"));

      // Match vendor code
      string vendor_sub_str = matched_sub_str.substr(matched_sub_str.find("/") + 1);
      uint64_t vendor_code = std::stoi(vendor_sub_str, 0, 16);

      // Match modifier
      uint64_t fmt_modifier = std::stoi(vendor_sub_str.substr(vendor_sub_str.find("/") + 1), 0, 16);
      if (vendor_code == DRM_FORMAT_MOD_VENDOR_QCOM) {
        // Macro from drm_fourcc.h to form modifier
        modifier = fourcc_mod_code(QCOM, fmt_modifier);
      }

    } else {
      final_format_str = matched_sub_str.c_str();
    }

    // fourcc_code is a macro from drm_fourcc.h to form the format from 4 characters (thus fourcc)
    formats->push_back(std::make_pair(fourcc_code(final_format_str.at(0), final_format_str.at(1),
                                                  final_format_str.at(2), final_format_str.at(3)),
                                      modifier));
    tmp_line = str_match.suffix();
  }
}

void Tokenize(const std::string &str, std::vector<std::string> *tokens, char delim) {
  size_t pos = 0;
  std::string str_temp(str);

  while ((pos = str_temp.find(delim)) != std::string::npos && delim != ' ') {
    str_temp.replace(pos, 1, 1, ' ');
  }

  std::stringstream ss(str_temp);
  while (ss >> str_temp) {
    tokens->push_back(str_temp);
  }
}

void AddProperty(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value,
                 bool cache, std::unordered_map<uint32_t, uint64_t> &prop_val_map) {
#ifndef SDM_VIRTUAL_DRIVER
  auto it = prop_val_map.find(property_id);
  if (it == prop_val_map.end() || it->second != value)
#endif
    drmModeAtomicAddProperty(req, object_id, property_id, value);
#ifndef SDM_VIRTUAL_DRIVER
  if (cache)
    prop_val_map[property_id] = value;
#endif
}

// TODO(user): Use FP16 library instead for conversions
uint16_t DRM_float_2_FP16(const float in) {
  if (in == 0) {
    return static_cast<uint16_t>(in);
  }

  float f = 0;
  float *pf = nullptr;
  uint32_t *pu = nullptr;
  uint32_t b = 0;
  uint32_t mi = 0;
  uint32_t mo = 0;
  int32_t ei = 0;
  int32_t eo = 0;
  uint32_t si = 0;
  uint16_t out = 0;

  f = in;
  pf = &f;
  // this is to obtain the uint32_t representation
  // of floating-point input
  pu = reinterpret_cast<uint32_t *>(pf);
  b = *pu;
  // this is to find the uint16_t representation
  // of the uint32_t type
  mi = (b >> 13) & 0x3FF;
  ei = (b >> 23) & 0xFF;
  si = b >> 31;
  eo = ei - 127 + 15;
  mo = mi;
  if (eo <= 0) {
    mo = (mi | 0x400) >> (1 - eo);
    eo = 0;
  }
  out = (uint16_t)((si << 15) | ((uint16_t)eo << 10) | mo);

  return out;
}

// TODO(user): Use FP16 library instead for conversions
float DRM_FP16_2_float(const uint16_t in) {
  if (in == 0) {
    return static_cast<float>(in);
  }

  float *pf = nullptr;
  uint32_t *pu = nullptr;
  uint32_t b = 0;
  uint32_t mi = 0;
  uint32_t mo = 0;
  int32_t ei = 0;
  int32_t eo = 0;
  uint32_t si = 0;
  float out = 0;

  // this is to find the uint32_t representation
  // of the uint16_t type
  mi = in & 0x3FF;
  ei = (in >> 10) & 0x1F;
  si = in >> 15;
  eo = ei - 15 + 127;
  mo = mi;

  b = (uint32_t)((si << 31) | ((uint32_t)eo << 23) | (mo << 13));

  pu = &b;
  // this is to obtain the floating_point representation
  // of uint32_t input
  pf = reinterpret_cast<float *>(pu);
  out = *pf;

  return out;
}

}  // namespace sde_drm
