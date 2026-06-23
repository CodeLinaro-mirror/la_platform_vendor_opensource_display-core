/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __IMAGE_ALGO_INTERFACE_H__
#define __IMAGE_ALGO_INTERFACE_H__

#include <memory>
#include <string>

// Each adapter has its own typed interface header.
// Clients include image_algo_interface.h and get all adapter interfaces transitively.
// Each intf header also defines AdapterTraits<XxxIntf>::kLibPath (the plugin .so path).
#include "image_algo_smartselection_intf.h"
// #include "image_algo_superresolution_intf.h"  // future: add here + define AdapterTraits there
// #include "image_algo_denoise_intf.h"           // future: add here + define AdapterTraits there

namespace imagealgo {

// ---------------------------------------------------------------------------
// AdapterTraits<IntfType> — binds an adapter interface type to its plugin .so path.
//
// Primary template is intentionally undefined.
// Each adapter's interface header (image_algo_<name>_intf.h) provides a
// specialization, e.g.:
//
//   template <>
//   struct AdapterTraits<SmartSelectionIntf> {
//       static constexpr const char* kLibPath =
//           "/vendor/lib64/libImageAlgoAdapter_smartselection.so";
//   };
//
// If CreateAdapter<T>() is called for a type with no AdapterTraits specialization,
// the compiler reports an error: "incomplete type 'imagealgo::AdapterTraits<T>'".
//
// To add a new adapter type (factory code needs NO modification):
//   1. Create include/image_algo_<name>_intf.h:
//      - Define the typed interface (XxxIntf = GenericIntf<...>)
//      - Add AdapterTraits<XxxIntf> specialization with kLibPath
//   2. Add #include "image_algo_<name>_intf.h" above (one-line change here)
//   3. Build libImageAlgoAdapter_<name>.so exporting
//      CreateImageAlgoAdapter() / DestroyImageAlgoAdapter()
//   4. Client calls: factory->CreateAdapter<XxxIntf>()
//      image_algo_adapter_factory.cpp needs NO modification.
// ---------------------------------------------------------------------------
template <typename IntfType>
struct AdapterTraits;  // Intentionally undefined — specialization required per adapter

// ---------------------------------------------------------------------------
// ImageAlgoAdapterFactory — Parameterized Abstract Factory for algorithm adapters.
//
// Design: Generic Template Factory + Traits (replaces per-adapter virtual methods).
//   - CreateAdapter<IntfType>() uses AdapterTraits<IntfType>::kLibPath to locate
//     the plugin .so. No factory code change needed when adding new adapters.
//   - CreateAdapter<IntfType>(lib_path) overload for testing/mock scenarios.
//   - CreateAdapterImpl() is the single virtual method implemented by
//     PluginAdapterFactory in image_algo_adapter_factory.cpp.
//
// Lifecycle:
//   1. GetImageAlgoAdapterFactory()                  — returns process-lifetime singleton
//   2. factory->CreateAdapter<SmartSelectionIntf>()  — dlopen + CreateImageAlgoAdapter()
//                                                       returns shared_ptr with custom deleter
//   3. adapter->Init()                               — load algorithm .so, resolve symbols
//   4. adapter->ProcessOps(kSSInit, ...)             — create pipeline/context
//   5. adapter->ProcessOps(kSSDeinit, ...)           — destroy pipeline/context
//   6. adapter.reset()                               — shared_ptr deleter: Destroy + dlclose
//
// Do NOT delete the ImageAlgoAdapterFactory* returned by GetImageAlgoAdapterFactory().
// ---------------------------------------------------------------------------
class ImageAlgoAdapterFactory {
public:
    virtual ~ImageAlgoAdapterFactory() = default;

    // -----------------------------------------------------------------------
    // CreateAdapter<IntfType>() — primary interface for creating adapters.
    //
    // Uses AdapterTraits<IntfType>::kLibPath to locate the plugin .so.
    // Compilation fails with "incomplete type AdapterTraits<T>" if IntfType
    // has no AdapterTraits specialization — caught at compile time.
    //
    // On first call: dlopen the plugin .so and call CreateImageAlgoAdapter().
    // Subsequent calls return the same instance (weak_ptr cache in factory).
    // When the last shared_ptr is released, the custom deleter calls
    // DestroyImageAlgoAdapter() + dlclose.
    //
    // The caller must call adapter->Init() before use.
    // Returns nullptr if the plugin could not be loaded.
    // -----------------------------------------------------------------------
    template <typename IntfType>
    std::shared_ptr<IntfType> CreateAdapter() {
        // AdapterTraits<IntfType>::kLibPath — compile error if no specialization exists.
        return CreateAdapter<IntfType>(AdapterTraits<IntfType>::kLibPath);
    }

    // -----------------------------------------------------------------------
    // CreateAdapter<IntfType>(lib_path) — override lib path (for testing/mock).
    //
    // Bypasses AdapterTraits; uses the provided lib_path directly.
    // Useful for unit tests that inject a mock .so.
    // -----------------------------------------------------------------------
    template <typename IntfType>
    std::shared_ptr<IntfType> CreateAdapter(const std::string& lib_path) {
        return std::static_pointer_cast<IntfType>(CreateAdapterImpl(lib_path));
    }

protected:
    // -----------------------------------------------------------------------
    // CreateAdapterImpl — the single virtual method for the concrete factory.
    //
    // Implemented by PluginAdapterFactory in image_algo_adapter_factory.cpp.
    // Handles dlopen, dlsym, string-keyed shared_ptr caching, and thread safety.
    //
    // lib_path: path to the adapter plugin .so, used as the cache key.
    //
    // Returns shared_ptr<void> (type-erased); CreateAdapter<T> casts it back
    // to shared_ptr<IntfType> via static_pointer_cast.
    // -----------------------------------------------------------------------
    virtual std::shared_ptr<void> CreateAdapterImpl(const std::string& lib_path) = 0;
};

}  // namespace imagealgo

// ---------------------------------------------------------------------------
// GetImageAlgoAdapterFactory — returns the singleton ImageAlgoAdapterFactory.
// Implemented in libImageAlgoIntegration.so (image_algo_adapter_factory.cpp).
//
// Corresponds to GetSDMInterfaceFactory() in the SDM architecture.
//
// To add a new adapter type (image_algo_adapter_factory.cpp needs NO modification):
//   1. Create include/image_algo_<name>_intf.h:
//      - Define XxxIntf = GenericIntf<XxxParam, XxxOp, GenericPayload>
//      - Add AdapterTraits<XxxIntf> specialization with kLibPath
//   2. Add #include "image_algo_<name>_intf.h" in image_algo_interface.h above.
//   3. Build libImageAlgoAdapter_<name>.so exporting
//      CreateImageAlgoAdapter() / DestroyImageAlgoAdapter().
//   4. Client calls: factory->CreateAdapter<XxxIntf>()
//
// Do NOT delete the returned ImageAlgoAdapterFactory* (process-lifetime singleton).
// ---------------------------------------------------------------------------
imagealgo::ImageAlgoAdapterFactory* GetImageAlgoAdapterFactory();

#endif  // __IMAGE_ALGO_INTERFACE_H__
