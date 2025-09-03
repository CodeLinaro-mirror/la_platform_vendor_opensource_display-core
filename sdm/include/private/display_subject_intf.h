/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DISPLAY_SUBJECT_INTF_H__
#define __DISPLAY_SUBJECT_INTF_H__

#include "display_cb_intf.h"

namespace sdm {

template <typename X, typename Y>
class DisplaySubjectIntf {
 public:
  virtual ~DisplaySubjectIntf() {}
  virtual int Init() = 0;
  virtual int DeInit() = 0;
  virtual int Register(X &observer, DisplayCbInterface<Y> *notify) = 0;
  virtual int DeRegister(X &observer) = 0;
  virtual int SetVal(Y &val) = 0;
  virtual int GetVal(Y &val) = 0;
};
}  // namespace sdm
#endif  // __DISPLAY_SUBJECT_INTF_H__
