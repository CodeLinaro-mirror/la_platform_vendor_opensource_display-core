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
* Changes from Qualcomm Innovation Center are provided under the following license:
* Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause-Clear
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
  uint64_t CreateId(uint64_t new_id = 0) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    uint64_t id = (!new_id) ? (1 + GetMaxId()) : new_id;
    active_ids_.insert(id);
    return id;
  }
  void DestroyId(uint64_t id) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    active_ids_.erase(id);
  }
  bool IsIdExisting(uint64_t id) {
    std::lock_guard<std::mutex> lock(id_mutex_);
    return !(active_ids_.empty() || (*(active_ids_.rbegin()) < id) ||
             active_ids_.find(id) == active_ids_.end());
  }
  uint64_t GetNextPossibleId() {
    std::lock_guard<std::mutex> lock(id_mutex_);
    return 1 + GetMaxId();
  }

 private:
  inline uint64_t GetMaxId() { return (active_ids_.empty() ? 0 : *(active_ids_.rbegin())); }

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
}  // namespace sdm

#endif  // __UTILS_H__

