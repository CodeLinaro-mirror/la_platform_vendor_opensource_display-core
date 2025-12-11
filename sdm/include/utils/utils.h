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

#define EMERGENCY_INTEGER_BANDWIDTH_FOR_ID 8

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
    if (next_to_max) {
      auto max_id = GetMaxId();
      if (max_id == UINT64_MAX) {
        // Either wrap to 0, or reuse GetNonConflictingIdToIncrementalPath(),
        // depending on expected behavior.
        return GetNonConflictingIdToIncrementalPath();
      }
      return max_id + 1;
    }
    return GetNonConflictingIdToIncrementalPath();
  }

 private:
  inline uint64_t GetMaxId() { return (active_ids_.empty() ? 0 : *(active_ids_.rbegin())); }
  /*
  * GetNonConflictingIdToIncrementalPath function finds and returns a unique ID that won't
  * conflict with existing incremental path IDs. It follows a multi-step strategy:
  *
  * 1. If no active IDs exist, returns 0 as starting ID as incremental ID always starts with 1.
  * 2. Attempts to reuse a disposed ID if exists, which ensures no conflict with incremental IDs.
  * 3. If no disposed IDs are available, falls back to using an emergency integer bandwidth from
  *    upper end of uint64 range (UINT64_MAX - EMERGENCY_INTEGER_BANDWIDTH_FOR_ID, UINT64_MAX).
  * 4. If the current max ID is already within the emergency bandwidth range,
  *    increments it by 1 (with overflow protection).
  */
  uint64_t GetNonConflictingIdToIncrementalPath() {
    if (active_ids_.empty()) {
      // No active IDs; start from 0
      return 0;
    }
    uint64_t max_id = GetMaxId();
    // Try to get disposed id, which will never conflict with incremental id.
    if (GetDisposedId(max_id)) {
      return max_id;  // As max_id is replaced with disposed id.
    }
    // No disposed IDs available; all IDs in [0, max_id] are in use. Fall back to emergency
    // bandwidth at upper end of uint64 range
    // [UINT64_MAX - EMERGENCY_INTEGER_BANDWIDTH_FOR_ID, UINT64_MAX].
    if (max_id < UINT64_MAX - EMERGENCY_INTEGER_BANDWIDTH_FOR_ID) {
      // Reserve a set of emergency integers for ID allocation at the upper end
      // of the 64-bit integer range.
      return UINT64_MAX - EMERGENCY_INTEGER_BANDWIDTH_FOR_ID;
    }
    // Edge case: prevent overflow if max_id reaches UINT64_MAX (extremely unlikely)
    if (max_id == UINT64_MAX) {
      return 0;
    }
    // Validated integer from range [UINT64_MAX - EMERGENCY_INTEGER_BANDWIDTH_FOR_ID, UINT64_MAX].
    return max_id + 1;
  }

  bool GetDisposedId(uint64_t &id) {
    // std::set is ordered; begin() points to the minimum element.
    uint64_t min_id = *active_ids_.begin();
    if (min_id > 0) {
      // Safe: min_id is at least 1, so min_id - 1 will not underflow.
      id = min_id - 1;
      return true;
    }
    // min_id == 0 case
    // Find an ID not in the set between min_id (0) and max_id, inclusive.
    // Since active_ids_ is sorted, walk from the beginning and look for a gap.
    uint64_t prev = *active_ids_.begin();  // this is 0
    for (auto it = std::next(active_ids_.begin()); it != active_ids_.end(); ++it) {
      uint64_t cur = *it;
      // If there is a gap (non-consecutive numbers), return the missing ID.
      if (cur > prev + 1) {
        id = prev + 1;
        return true;
      }
      prev = cur;
    }
    return false;
  }

  std::mutex id_mutex_;
  std::set<uint64_t> active_ids_;
};

float gcd(float a, float b);
float lcm(float a, float b);
void CloseFd(int *fd);
uint64_t GetSystemTimeInNs();
void SetRealTimePriority();

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

