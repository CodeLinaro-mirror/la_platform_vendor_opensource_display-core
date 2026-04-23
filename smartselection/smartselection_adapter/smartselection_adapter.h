/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __SMARTSELECTION_ADAPTER_H__
#define __SMARTSELECTION_ADAPTER_H__

// image_algo_interface.h includes image_algo_smartselection_intf.h which defines
// SmartSelectionIntf = GenericIntf<int, SmartSelectionOp, GenericPayload>.
// SmartSelectionAdapter implements SmartSelectionIntf — typed, no uint32_t op codes.
#include "image_algo_interface.h"
#include "QaiorSmartSelectionApi.h"
#include <ISnapMapper.h>
#include <SnapHandle.h>
#include <vndk/hardware_buffer.h>  // AHardwareBuffer — VNDK vendor API (gralloc4 HAL path)

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Path to the SmartSelection shared library in the vendor partition.
// Defined here so both SmartSelectionAdapter and unit tests can reference it.
// inline constexpr ensures a single definition across all translation units (C++17).
inline constexpr const char* kSmartSelectionLibPath = "/vendor/lib64/libQaiorSmartSelection.so";

namespace imagealgo {

/**
 * @brief Adapter that bridges SmartSelectionIntf to the SmartSelection C API.
 *
 * Implements SmartSelectionIntf = GenericIntf<int, SmartSelectionOp, GenericPayload>.
 * This is the Abstract Factory pattern: SmartSelectionAdapter is the concrete product
 * for the SmartSelection algorithm, with its own typed interface (not shared with other adapters).
 *
 * Loads libQaiorSmartSelection.so at runtime via dlopen() and resolves all
 * C API symbols via dlsym(). The library path is kSmartSelectionLibPath.
 *
 * Lifecycle (managed by PluginAdapterFactory in image_algo_adapter_factory.cpp):
 *   1. CreateImageAlgoAdapter()  — construct (no library loaded yet)
 *   2. Init()                    — dlopen + dlsym + InitSnapMapper + LoadConfig
 *   3. ProcessOps(kSSInit, ...)  — create pipeline/context
 *   4. ProcessOps(kSSDeinit, ...)— destroy pipeline/context
 *   5. Deinit()                  — dlclose + release resources (called by destructor)
 *   6. DestroyImageAlgoAdapter() — delete instance (shared_ptr custom deleter)
 */
class SmartSelectionAdapter : public SmartSelectionIntf {
public:
    SmartSelectionAdapter();
    ~SmartSelectionAdapter() override;

    // -----------------------------------------------------------------------
    // SmartSelectionIntf = GenericIntf<int, SmartSelectionOp, GenericPayload>
    // -----------------------------------------------------------------------

    /** Load libQaiorSmartSelection.so, resolve symbols, load config JSON. */
    int Init() override;

    /** Unload library and release resources. Called by destructor. */
    int Deinit() override;

    /**
     * Set adapter state.
     * Reserved for future use (e.g. runtime configuration updates).
     * Currently returns -ENOTSUP.
     */
    int SetParameter(int param, const sdm::GenericPayload& in) override;

    /**
     * Query adapter state.
     * Reserved for future use (e.g. querying pipeline status).
     * Currently returns -ENOTSUP.
     */
    int GetParameter(int param, sdm::GenericPayload* out) override;

    /**
     * Dispatch a SmartSelection operation.
     * @param op   SmartSelectionOp value (kSSInit, kSSEnqueue, etc.)
     * @param in   Input payload (op-specific struct via sdm::GenericPayload)
     * @param out  Output payload (may be empty for ops with no output)
     * @return 0 on success, negative errno on failure.
     */
    int ProcessOps(SmartSelectionOp op,
                   const sdm::GenericPayload& in,
                   sdm::GenericPayload* out) override;

private:
    // -----------------------------------------------------------------------
    // dlopen / dlsym helpers
    // -----------------------------------------------------------------------
    int  LoadLibrary();
    void UnloadLibrary();

    // -----------------------------------------------------------------------
    // 9 operations corresponding to SmartSelection C API
    // -----------------------------------------------------------------------
    int DoInit(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoReconfigure(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoEnqueue(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoFlushByConfig(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoFlushAll(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoDeleteByConfig(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoDeleteAll(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoDeinit(const sdm::GenericPayload& in, sdm::GenericPayload* out);
    int DoWaitUntilIdle(const sdm::GenericPayload& in, sdm::GenericPayload* out);

    // -----------------------------------------------------------------------
    // Per-input-type frame preparation helpers (called by DoEnqueue).
    //
    // Each function fills 'frame'. metadata_json is the caller-provided JSON string
    // (from SmartSelectionEnqueueInput::metadata_json) and is passed directly to
    // QaiorSS_enqueue() as metadataJson. frame.metadataJson points into
    // metadata_json.c_str(), so the caller MUST keep metadata_json alive until
    // after fn_enqueue() returns.
    //
    // On success returns 0 and frame is ready to pass to fn_enqueue().
    // On failure returns negative errno; no cleanup needed by caller.
    // -----------------------------------------------------------------------

    int PrepareFrameParcelFd(int parcel_fd, const std::string& metadata_json,
                              QaiorSS_FrameHandle_t& frame);

    int PrepareFrameGraphicBuffer(void* buffer, const std::string& metadata_json,
                                   QaiorSS_FrameHandle_t& frame);

    int PrepareFrameAHardwareBuffer(void* ahb_ptr, const std::string& metadata_json,
                                     QaiorSS_FrameHandle_t& frame);

    /**
     * Gralloc query + AHardwareBuffer_createFromHandle(CLONE) + verify + cache invalidation.
     * Uses gralloc metadata (via snapmapper) to build AHardwareBuffer_Desc.
     * metadata_json is passed directly to QaiorSS_enqueue() as metadataJson.
     */
    int PrepareFrameNativeHandle(
        void* native_hdl, const std::string& metadata_json,
        const std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper>& snapmapper,
        QaiorSS_FrameHandle_t& frame);

    /** Release AHardwareBuffer resources on fn_enqueue() failure. */
    void CleanupFrameOnFailure(const QaiorSS_FrameHandle_t& frame);

    // -----------------------------------------------------------------------
    // snap_handle → QaiorSS_FrameHandle_t conversion.
    //
    // @param snap_handle   Opaque snap handle from the CWB buffer.
    // @param app_name      Application identifier to embed in metadata JSON.
    // @param out_metadata  Output: receives the metadata JSON string.
    //                      The caller MUST keep this string alive until after
    //                      QaiorSS_enqueue() returns, since frame.metadataJson
    //                      points into it.
    // @param snapmapper    Caller-provided ISnapMapper (copied from snapmapper_
    //                      under mutex_ before releasing the lock, so this
    //                      function can be called without holding mutex_).
    // -----------------------------------------------------------------------
    QaiorSS_FrameHandle_t BuildFrameHandleFromSnapHandle(
        void* snap_handle,
        const std::string& app_name,
        std::string& out_metadata,
        const std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper>& snapmapper);

    // -----------------------------------------------------------------------
    // Emit callback (static C-linkage trampoline → OnEmitResult)
    // -----------------------------------------------------------------------
    static void EmitCallbackTrampoline(const QaiorSS_EmitResult_t* result, void* cookie);
    void OnEmitResult(const QaiorSS_EmitResult_t* result);

    int LoadConfigurationJson(const std::string& config_path);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    // mutex_ — guards all adapter state (init_done_, pipeline_, context_,
    //           fn_* pointers, snapmapper_, current_config_json_).
    // callback_mutex_ — guards client_callback_fn_ and client_callback_cookie_
    //   separately from mutex_ so that OnEmitResult() (called from the library's
    //   callback thread) can safely read the callback without holding mutex_.
    //   This prevents a deadlock where DoEnqueue() holds mutex_, the library
    //   calls the emit callback synchronously, and the callback tries to call
    //   ProcessOps() which would re-acquire mutex_.
    std::mutex mutex_;
    std::mutex callback_mutex_;
    bool init_done_ = false;

    // dlopen handle
    void* lib_handle_ = nullptr;

    // C API opaque handles
    QaiorSS_Pipeline_t* pipeline_ = nullptr;
    QaiorSS_Context_t*  context_  = nullptr;

    // Configuration JSON (loaded from file or provided at runtime via ProcessOps INIT)
    std::string current_config_json_;

    // Client callback (C-style function pointer + cookie)
    // Passed in via ProcessOps(kSSInit) input payload
    SmartSelectionEmitCallbackFn client_callback_fn_    = nullptr;
    void*                        client_callback_cookie_ = nullptr;

    // ISnapMapper for extracting buffer metadata from snap handles
    std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper> snapmapper_;

    // Pending AHardwareBuffers allocated in DoEnqueue and waiting for emit callback.
    // SmartSelection processes frames asynchronously; we must keep the AHB alive
    // until OnEmitResult() is called (SmartSelection does not acquire its own reference).
    // Released in OnEmitResult() when SmartSelection emits the frame.
    // Also released in DoDeinit() / Deinit() to prevent leaks on teardown.
    std::mutex pending_ahb_mutex_;
    std::unordered_set<AHardwareBuffer*> pending_ahbs_;

    // Maps AHardwareBuffer* (created via CLONE in PrepareFrameNativeHandle) back to
    // the original native_handle_t* supplied by the caller at enqueue time.
    // Populated in PrepareFrameNativeHandle; consumed (and erased) in OnEmitResult,
    // CleanupFrameOnFailure, DoDeinit, and Deinit.
    // Protected by pending_ahb_mutex_ (same lock as pending_ahbs_).
    std::unordered_map<AHardwareBuffer*, native_handle_t*> ahb_to_native_handle_map_;

    // -----------------------------------------------------------------------
    // C API function pointers (resolved via dlsym)
    // -----------------------------------------------------------------------
    QaiorSS_Pipeline_t* (*fn_createPipeline_)(void)                                    = nullptr;
    void                (*fn_destroyPipeline_)(QaiorSS_Pipeline_t*)                    = nullptr;
    QaiorSS_Context_t*  (*fn_init_)(QaiorSS_Pipeline_t*, const char*,
                                    QaiorSS_emitCallback, void*)                       = nullptr;
    void                (*fn_deinit_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*)         = nullptr;
    QaiorSS_Status_t    (*fn_reconfigure_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*,
                                           const char*)                                = nullptr;
    QaiorSS_Status_t    (*fn_enqueue_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*,
                                       const QaiorSS_FrameHandle_t*, void*)            = nullptr;
    QaiorSS_Status_t    (*fn_flushByConfig_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*,
                                             const char*)                              = nullptr;
    QaiorSS_Status_t    (*fn_flushAll_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*)       = nullptr;
    QaiorSS_Status_t    (*fn_deleteByConfig_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*,
                                              const char*, QaiorSS_FrameHandle_t**,
                                              int32_t*)                                = nullptr;
    QaiorSS_Status_t    (*fn_deleteAll_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*,
                                         QaiorSS_FrameHandle_t**, int32_t*)            = nullptr;
    void                (*fn_freeFrames_)(QaiorSS_FrameHandle_t*)                      = nullptr;
    QaiorSS_Status_t    (*fn_waitUntilIdle_)(QaiorSS_Pipeline_t*, QaiorSS_Context_t*,
                                             int32_t)                                  = nullptr;
};

}  // namespace imagealgo

#endif  // __SMARTSELECTION_ADAPTER_H__
