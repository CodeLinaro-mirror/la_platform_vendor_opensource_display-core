/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DISPLAY_CB_INTF_H__
#define __DISPLAY_CB_INTF_H__

namespace sdm {

template <typename X>
class DisplayCbInterface {
 public:
  virtual int Notify(const X &) = 0;
  virtual ~DisplayCbInterface() {}
};

template <typename T>
using SdmDisplayCbInterface = DisplayCbInterface<T>;
}  // namespace sdm
#endif  // __DISPLAY_CB_INTF_H__
