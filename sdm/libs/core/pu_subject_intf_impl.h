/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __PU_SUBJECT_INTF_IMPL_H__
#define __PU_SUBJECT_INTF_IMPL_H__

#include <vector>
#include <string>

#include <private/display_subject_intf.h>
#include <core/display_interface.h>

namespace sdm {

class DisplayBuiltIn;
using PuSubjectIntf = DisplaySubjectIntf<std::string, int>;

class PuSubjectIntfImpl : public PuSubjectIntf {
 public:
  PuSubjectIntfImpl(DisplayBuiltIn *display_intf);
  int Init();
  int DeInit();
  int Register(std::string &observer, SdmDisplayCbInterface<int> *notifier);
  int DeRegister(std::string &observer);
  int SetVal(int &val);
  int GetVal(int &val);
  ~PuSubjectIntfImpl();

 private:
  std::mutex api_mutex_;
  std::vector<std::pair<std::string, SdmDisplayCbInterface<int> *>> observer_list_;
  DisplayBuiltIn *display_intf_ = nullptr;
};

}  // namespace sdm

#endif  // __PU_SUBJECT_INTF_IMPL_H__
