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

#ifndef __IMAGE_ALGO_COMMON_H__
#define __IMAGE_ALGO_COMMON_H__

#include <memory>
#include <ISnapMapper.h>

namespace imagealgo {

/**
 * @brief Static utility helpers shared across ImageAlgo adapters.
 *
 * All methods are static. This class is not meant to be instantiated or inherited.
 */
class ImageAlgoCommon {
public:
    ImageAlgoCommon() = delete;
    ~ImageAlgoCommon() = delete;
    ImageAlgoCommon(const ImageAlgoCommon&) = delete;
    ImageAlgoCommon& operator=(const ImageAlgoCommon&) = delete;

    /**
     * @brief Initialize ISnapMapper by dlopen-ing the snapalloc implementation library.
     *
     * Each adapter calls this in its own Init() to obtain a snapmapper_ instance.
     * The dynamic linker ensures the .so is loaded only once per process regardless
     * of how many adapters call this (reference-counted by the OS loader).
     * The lib handle is not stored — the library stays resident for the process lifetime,
     * consistent with the sdmclient pattern.
     *
     * @param[out] snapmapper  Receives the ISnapMapper shared_ptr on success.
     * @return 0 on success, -1 on failure (dlopen or FETCH_ISnapMapper not found).
     */
    static int InitSnapMapper(
        std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper>& snapmapper);
};

}  // namespace imagealgo

#endif  // __IMAGE_ALGO_COMMON_H__
