/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

// image_algo_adapter_factory.cpp
//
// Implements GetImageAlgoAdapterFactory() — the public entry point for clients.
// Corresponds to GetSDMInterfaceFactory() in the SDM architecture.
//
// Design: Parameterized Abstract Factory (Generic Template Factory + Traits).
//   - GetImageAlgoAdapterFactory() returns a process-lifetime singleton factory.
//   - PluginAdapterFactory::CreateAdapterImpl():
//       * Checks a type_index-keyed shared_ptr cache for an existing instance.
//       * If found: returns the cached shared_ptr (ref count incremented).
//       * If not found: dlopen .so → dlsym CreateImageAlgoAdapter /
//         DestroyImageAlgoAdapter → wrap raw pointer in shared_ptr<void>
//         with a custom deleter (DestroyImageAlgoAdapter + dlclose).
//       * Stores shared_ptr<void> in cache keyed by std::type_index.
//   - Adapter instances are owned by the factory (process lifetime).
//   - No explicit Destroy() call needed — shared_ptr handles cleanup.
//   - Adding a new adapter type requires NO changes to this file.
//     (Add interface header + AdapterTraits specialization + build new .so.)

#include "image_algo_interface.h"

#include <dlfcn.h>
#include <mutex>
#include <string>
#include <unordered_map>

#include "debug_handler.h"
#define __CLASS__ "ImageAlgoAdapterFactory"

// ---------------------------------------------------------------------------
// PluginAdapterFactory — concrete ImageAlgoAdapterFactory.
//
// Implements CreateAdapterImpl() — the single virtual method.
// All adapter types share the same dlopen/dlsym/cache logic via type erasure:
//   - Cache key:   std::string (lib_path)
//   - Cache value: std::shared_ptr<void>
//   - shared_ptr<void> deleter calls DestroyImageAlgoAdapter() + dlclose()
//
// Lifetime: adapter instances are owned by the factory (process lifetime).
// The factory holds a shared_ptr, so the adapter stays alive as long as the
// factory exists. Clients receive a copy of the same shared_ptr.
//
// Thread-safe: double-checked locking (dlopen called WITHOUT holding mutex_).
//
// Adding a new adapter type requires NO changes here.
// ---------------------------------------------------------------------------
class PluginAdapterFactory : public imagealgo::ImageAlgoAdapterFactory {
public:
    PluginAdapterFactory()  = default;
    ~PluginAdapterFactory() override = default;

protected:
    // -----------------------------------------------------------------------
    // CreateAdapterImpl — dlopen + dlsym + string-keyed shared_ptr cache.
    //
    // lib_path: path to the adapter plugin .so (from AdapterTraits or caller).
    //           Also used as the cache key, one per adapter type.
    //
    // Double-checked locking: dlopen() is called WITHOUT holding mutex_ to
    // avoid blocking other threads for the duration of library loading
    // (dlopen may run .so constructors and take significant time).
    // -----------------------------------------------------------------------
    std::shared_ptr<void> CreateAdapterImpl(const std::string& lib_path) override {

        // Step 1: fast path — check cache under lock.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(lib_path);
            if (it != cache_.end()) {
                DLOGD("CreateAdapterImpl: returning cached instance for '%s'",
                      lib_path.c_str());
                return it->second;
            }
        }

        // Step 2: slow path — load the plugin without holding mutex_.
        // Two threads may race here and both call LoadAdapterVoid(); that is safe:
        // the second thread's instance will be discarded in step 3.
        auto sp = LoadAdapterVoid(lib_path);
        if (!sp) return nullptr;

        // Step 3: re-acquire lock, check cache again (another thread may have
        // loaded it while we were in step 2), then store our instance.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(lib_path);
            if (it != cache_.end()) {
                // Another thread won the race — discard our instance and return
                // the one already in the cache (shared_ptr destructor cleans up,
                // triggering DestroyImageAlgoAdapter + dlclose for our duplicate).
                DLOGD("CreateAdapterImpl: using instance loaded by another thread for '%s'",
                      lib_path.c_str());
                return it->second;
            }
            cache_[lib_path] = sp;
        }
        return sp;
    }

private:
    // -----------------------------------------------------------------------
    // LoadAdapterVoid — dlopen + dlsym + shared_ptr<void> wrapper.
    //
    // The plugin .so must export:
    //   void* CreateImageAlgoAdapter()
    //   void  DestroyImageAlgoAdapter(void*)
    //
    // The shared_ptr<void> custom deleter calls DestroyImageAlgoAdapter() +
    // dlclose() when the last reference is released (factory destruction).
    //
    // Called WITHOUT mutex_ held (dlopen must not block under lock).
    // -----------------------------------------------------------------------
    std::shared_ptr<void> LoadAdapterVoid(const std::string& lib_path) {
        // Clear any previous dlerror.
        dlerror();
        void* lib_handle = dlopen(lib_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!lib_handle) {
            DLOGE("LoadAdapterVoid: dlopen(%s) failed: %s", lib_path.c_str(), dlerror());
            return nullptr;
        }

        using CreateFn  = void*(*)();
        using DestroyFn = void(*)(void*);

        auto create_fn  = reinterpret_cast<CreateFn> (
            dlsym(lib_handle, "CreateImageAlgoAdapter"));
        auto destroy_fn = reinterpret_cast<DestroyFn>(
            dlsym(lib_handle, "DestroyImageAlgoAdapter"));

        if (!create_fn || !destroy_fn) {
            DLOGE("LoadAdapterVoid: dlsym failed in '%s': %s",
                  lib_path.c_str(), dlerror());
            dlclose(lib_handle);
            return nullptr;
        }

        void* raw = create_fn();
        if (!raw) {
            DLOGE("LoadAdapterVoid: CreateImageAlgoAdapter() returned null for '%s'",
                  lib_path.c_str());
            dlclose(lib_handle);
            return nullptr;
        }

        // Wrap in shared_ptr<void> with custom deleter.
        // The deleter is called when the factory is destroyed (process exit):
        //   1. destroy_fn(p) → DestroyImageAlgoAdapter() → delete adapter
        //      (destructor calls Deinit() which unloads the algorithm .so)
        //   2. dlclose(lib_handle) → unloads libImageAlgoAdapter_<name>.so
        auto sp = std::shared_ptr<void>(
            raw,
            [destroy_fn, lib_handle, lib_path](void* p) {
                destroy_fn(p);
                dlclose(lib_handle);
                DLOGI("PluginAdapterFactory: adapter '%s' destroyed and plugin unloaded",
                      lib_path.c_str());
            });

        DLOGI("LoadAdapterVoid: loaded plugin '%s'", lib_path.c_str());
        return sp;
    }

    std::mutex mutex_;

    // Unified string-keyed shared_ptr cache.
    // Key:   std::string (lib_path) — one entry per adapter type.
    // Value: std::shared_ptr<void> — factory owns the adapter for its lifetime.
    //        All clients receive a copy of this shared_ptr (ref count shared).
    std::unordered_map<std::string, std::shared_ptr<void>> cache_;
};

// ---------------------------------------------------------------------------
// GetImageAlgoAdapterFactory — returns the singleton ImageAlgoAdapterFactory.
//
// Corresponds to GetSDMInterfaceFactory() in the SDM architecture.
// The factory lives for the process lifetime; do NOT delete the returned pointer.
//
// To add a new adapter type (this file needs NO modification):
//   1. Create include/image_algo_<name>_intf.h:
//      - Define XxxIntf = GenericIntf<XxxParam, XxxOp, GenericPayload>
//      - Add AdapterTraits<XxxIntf> specialization with kLibPath
//   2. Add #include "image_algo_<name>_intf.h" in image_algo_interface.h.
//   3. Build libImageAlgoAdapter_<name>.so exporting
//      CreateImageAlgoAdapter() / DestroyImageAlgoAdapter().
//   4. Client calls: factory->CreateAdapter<XxxIntf>()
// ---------------------------------------------------------------------------
imagealgo::ImageAlgoAdapterFactory* GetImageAlgoAdapterFactory() {
    static PluginAdapterFactory factory;
    return &factory;
}
