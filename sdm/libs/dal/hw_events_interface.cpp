/*
* Copyright (c) 2017-2018, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
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

#include <utils/utils.h>
#include <private/hw_events_interface.h>

#include <vector>

#ifndef TARGET_HEADLESS
#include "hw_events_drm.h"
#endif

#define __CLASS__ "HWEventsInterface"

namespace sdm {

DisplayError HWEventsInterface::Create(DisplayId display_id, SDMDisplayType display_type,
                                       HWEventHandler *event_handler,
                                       const std::map<uint32_t, std::vector<HWEvent>> &event_list,
                                       std::vector<HWEventsInterface *> *intf) {
  DisplayError error = kErrorNone;
#ifndef TARGET_HEADLESS
  for (auto map_entry : event_list) {
    HWEventsInterface *hw_events = new HWEventsDRM();
    error =
        hw_events->Init(display_id, map_entry.first, display_type, event_handler, map_entry.second);
    if (error != kErrorNone) {
      delete hw_events;
    } else {
      intf->push_back(hw_events);
    }
  }
#endif

  return error;
}

DisplayError HWEventsInterface::Destroy(std::vector<HWEventsInterface *> *intf) {
  if (!intf) {
    return kErrorParameters;
  }

  for (int i = 0; i < intf->size(); i++) {
    if (intf->at(i)) {
      intf->at(i)->Deinit();
      delete intf->at(i);
    }
  }
  intf->clear();

  return kErrorNone;
}

}  // namespace sdm
