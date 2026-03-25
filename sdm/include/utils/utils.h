/*
* Copyright (c) 2016 - 2018, 2020 - 2021 The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of The Linux Foundation nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
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
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __UTILS_H__
#define __UTILS_H__

#include <utils/rect.h>
#include <stdint.h>
#include <cstring>
#include <mutex>
#include <set>

namespace sdm {

constexpr size_t get_page_size() {
#if defined(PAGE_SIZE)
  return PAGE_SIZE;
#else
  return 65536;
#endif
}

class IdManager {
 public:
  IdManager() {}
  ~IdManager() {
    std::lock_guard<std::mutex> lock(id_mutex_);
    active_ids_.clear();
  }
  uint64_t LogId(uint64_t new_id) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    active_ids_.insert(new_id);
    return new_id;
  }
  void EraseId(uint64_t id) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    active_ids_.erase(id);
  }
  bool IsIdExisting(uint64_t id) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    return !(active_ids_.empty() || (*(active_ids_.rbegin()) < id) ||
             active_ids_.find(id) == active_ids_.end());
  }
  uint64_t GetNextPossibleId(bool next_to_max) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    return next_to_max ? GetNextAfterMax() : GetFirstFreeId();
  }

 private:
  static constexpr uint64_t EMERGENCY_INTEGER_BANDWIDTH_FOR_ID = 32;
  static constexpr uint64_t EMERGENCY_INTEGER_START_FOR_ID =
      UINT64_MAX - EMERGENCY_INTEGER_BANDWIDTH_FOR_ID + 1;

  inline uint64_t GetMaxId() { return active_ids_.empty() ? 0 : *active_ids_.rbegin(); }
  uint64_t GetNextAfterMax() {
    uint64_t max_id = GetMaxId();
    if (max_id == UINT64_MAX) {
      return GetFirstFreeId();
    }

    return max_id + 1;
  }

  uint64_t GetFirstFreeId() {
    if (active_ids_.empty()) {
      return 0;
    }

    // Find first disposed ID in the active IDs set.
    uint64_t possible_id = 0;
    for (uint64_t id : active_ids_) {
      // If we have reached the emergency integer band, stop searching.
      if (id == EMERGENCY_INTEGER_START_FOR_ID) {
        break;
      }
      if (possible_id < id) {
        return possible_id;
      }
      ++possible_id;
    }

    // Check for emergency integer band.
    // If no disposed ID was found before, allocate ID from emergency band.
    for (uint64_t id = EMERGENCY_INTEGER_START_FOR_ID;; ++id) {
      if (active_ids_.find(id) == active_ids_.end()) {
        return id;
      }
      if (id == UINT64_MAX) {
        break;
      }
    }

    return 0;
  }

  std::mutex id_mutex_;
  std::set<uint64_t> active_ids_;
};

float gcd(float a, float b);
float lcm(float a, float b);
void CloseFd(int *fd);
uint64_t GetSystemTimeInNs();
void SetRealTimePriority();

template <typename T1, typename T2>
void CopyColorMetadata(T1 &input, T2 &output) {
  output.dataspace = input.dataspace;
  output.matrixCoefficients = input.matrixCoefficients;
  output.masteringDisplayInfo = input.masteringDisplayInfo;
  output.contentLightLevel = input.contentLightLevel;
  output.cRI = input.cRI;
  output.dynamicMetadata = input.dynamicMetadata;
}

template<class T>
bool SameConfig(T *t1, T *t2, unsigned int size) {
  return !(std::memcmp(t1, t2, size));
}
void AdjustSize(const int min_size, const int bound_start, const int bound_end, int *input_start,
                int *input_end);
void ApplyCwbRoiRestrictions(LayerRect &roi, const LayerRect &cwb_full_frame,
                             const int cwb_alignment_factor,
                             LayerBufferFormat format);
uint32_t GetCwbRequestedMixerCount(CwbConfig *config, uint32_t num_split, uint32_t display_width,
                                   uint32_t mixer_width, bool &roi_block_partial);
const char *GetCompositionName(const LayerComposition &composition);
const char* GetSocName();
bool IsXRVariant();
uint16_t float_2_FP16(const float in);
float FP16_2_float(const uint16_t in);
}  // namespace sdm

#endif  // __UTILS_H__

