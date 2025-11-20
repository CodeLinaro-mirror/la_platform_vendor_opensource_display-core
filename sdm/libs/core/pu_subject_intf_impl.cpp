/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "pu_subject_intf_impl.h"
#include "display_builtin.h"

#define __CLASS__ "PuSubjectIntfImpl"

namespace sdm {

PuSubjectIntfImpl::PuSubjectIntfImpl(DisplayBuiltIn *display_intf) : display_intf_(display_intf) {}

PuSubjectIntfImpl::~PuSubjectIntfImpl() {
  DeInit();
}

int PuSubjectIntfImpl::Init() {
  std::lock_guard<std::mutex> mtx(api_mutex_);
  return 0;
}

int PuSubjectIntfImpl::DeInit() {
  std::lock_guard<std::mutex> mtx(api_mutex_);
  observer_list_.clear();
  return 0;
}

int PuSubjectIntfImpl::Register(std::string &observer, SdmDisplayCbInterface<int> *notifier) {
  std::lock_guard<std::mutex> mtx(api_mutex_);
  int ret = 0;

  if (!display_intf_ || !observer.size()) {
    DLOGW("Invalid input params, display_intf_ is %s, observer size: %d",
          display_intf_ ? "set" : "null", observer.size());
    return -EINVAL;
  }

  if (std::any_of(observer_list_.begin(), observer_list_.end(),
                  [&observer](std::pair<std::string, SdmDisplayCbInterface<int> *> &p) {
                    return p.first == observer;
                  })) {
    DLOGI("observer already registered: %s notifier: %p", observer.c_str(), notifier);
    return 0;
  }

  if (observer_list_.empty()) {
    DLOGI("set partial update to disable");
    ret = display_intf_->SetPartialUpdateControl(false);
    if (ret != kErrorNone) {
      DLOGE("SetPartialUpdateControl to disable failed, ret=%d", ret);
    }
  }

  observer_list_.push_back(std::make_pair(observer, notifier));
  DLOGI_IF(kTagDisplay, "add %s to list, size:%zu", observer.c_str(), observer_list_.size());
  return 0;
}

int PuSubjectIntfImpl::DeRegister(std::string &observer) {
  std::lock_guard<std::mutex> mtx(api_mutex_);
  int ret = 0;
  int i = 0;

  if (!display_intf_ || !observer.size()) {
    DLOGW("Invalid input params, display_intf_ is %s, observer size: %d",
          display_intf_ ? "set" : "null", observer.size());
    return -EINVAL;
  }

  if (std::any_of(observer_list_.begin(), observer_list_.end(),
                  [&observer, &i](std::pair<std::string, SdmDisplayCbInterface<int> *> &p) {
                    i++;
                    return p.first == observer;
                  })) {
    observer_list_.erase(observer_list_.begin() + i - 1);
    DLOGI_IF(kTagDisplay, "erase %s from list, size:%zu", observer.c_str(), observer_list_.size());

    if (observer_list_.empty()) {
      DLOGI("set partial update to enable");
      ret = display_intf_->SetPartialUpdateControl(true);
      if (ret != kErrorNone) {
        DLOGE("SetPartialUpdateControl to enable failed, ret=%d", ret);
      }
    }
  } else {
    DLOGW("observer: %s not found", observer.c_str());
  }

  return 0;
}

int PuSubjectIntfImpl::GetVal(int &val) {
  (void)val;
  return -EINVAL;
}

int PuSubjectIntfImpl::SetVal(int &val) {
  (void)val;
  return -EINVAL;
}

}  // namespace sdm
