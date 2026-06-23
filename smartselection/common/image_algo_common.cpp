/*
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
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
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "image_algo_common.h"

#include <dlfcn.h>
#include <mutex>

#include "debug_handler.h"    // DLOGE / DLOGI macros
#include "debug_callback_intf.h"
#define __CLASS__ "ImageAlgoCommon"

namespace imagealgo {

// ---------------------------------------------------------------------------
// InitSnapMapper — dlopen snapalloc impl and obtain ISnapMapper instance.
// ---------------------------------------------------------------------------
int ImageAlgoCommon::InitSnapMapper(
    std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper>& snapmapper) {

    const char* lib_name = "vendor.qti.hardware.display.snapalloc-impl.so";

    static void*      s_snapalloc_lib = nullptr;
    static std::mutex s_mutex;

    void* lib = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_snapalloc_lib) {
            dlerror();
            s_snapalloc_lib = dlopen(lib_name, RTLD_NOW | RTLD_LOCAL);
            if (!s_snapalloc_lib) {
                DLOGE("InitSnapMapper: dlopen(%s) failed: %s", lib_name, dlerror());
                return -1;
            }
            DLOGI("InitSnapMapper: loaded %s", lib_name);
        }
        lib = s_snapalloc_lib;
    }

    std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper> (*LINK_FETCH_ISnapMapper)(
        sdm::DebugCallbackIntf*) = nullptr;
    *reinterpret_cast<void**>(&LINK_FETCH_ISnapMapper) = dlsym(lib, "FETCH_ISnapMapper");
    if (!LINK_FETCH_ISnapMapper) {
        DLOGE("InitSnapMapper: FETCH_ISnapMapper not found: %s", dlerror());
        return -1;
    }

    snapmapper = LINK_FETCH_ISnapMapper(nullptr);
    if (!snapmapper) {
        DLOGE("InitSnapMapper: FETCH_ISnapMapper returned null");
        return -1;
    }

    DLOGI("InitSnapMapper: snapmapper initialized successfully");
    return 0;
}

}  // namespace imagealgo
