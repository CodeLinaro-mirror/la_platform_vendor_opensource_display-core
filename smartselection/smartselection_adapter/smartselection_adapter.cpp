/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

// smartselection_adapter.cpp
// SmartSelectionAdapter implements SmartSelectionIntf
// = GenericIntf<int, SmartSelectionOp, GenericPayload>.
// Abstract Factory pattern: typed interface, no uint32_t op codes.
#include "smartselection_adapter.h"
#include "image_algo_common.h"

#include <dlfcn.h>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>
#include <fstream>
#include <chrono>
#include <cutils/native_handle.h>  // native_handle_t
#include <unistd.h>                // dup, close
#include <vndk/hardware_buffer.h>  // AHardwareBuffer — VNDK vendor API (gralloc4 HAL path)

#include "debug_handler.h"
#define __CLASS__ "SmartSelectionAdapter"

namespace imagealgo {

// ---------------------------------------------------------------------------
// HAL pixel format constants — avoids a dependency on <hardware/gralloc.h>
// while keeping the intent clear. Values match the Android HAL specification.
// ---------------------------------------------------------------------------
static constexpr int kHalPixelFormatRGBA8888 = 1;        // HAL_PIXEL_FORMAT_RGBA_8888
static constexpr int kHalPixelFormatRGB888 = 3;          // HAL_PIXEL_FORMAT_RGB_888
static constexpr int kHalPixelFormatRGBA1010102 = 0x2B;  // HAL_PIXEL_FORMAT_RGBA_1010102

// Helper macro: resolve a symbol via dlsym and store in function pointer member.
// On failure: calls UnloadLibrary() to dlclose and null lib_handle_ before returning,
// preventing a subsequent Init() from seeing a non-null lib_handle_ with null fn_* pointers.
#define DLSYM_OR_FAIL(handle, fn_ptr, symbol)                  \
  do {                                                         \
    *(void **)(&(fn_ptr)) = dlsym((handle), (symbol));         \
    if (!(fn_ptr)) {                                           \
      DLOGE("dlsym failed for '%s': %s", (symbol), dlerror()); \
      UnloadLibrary();                                         \
      return -1;                                               \
    }                                                          \
  } while (0)

//=============================================================================
// Constructor / Destructor
//=============================================================================

SmartSelectionAdapter::SmartSelectionAdapter()
    : init_done_(false),
      lib_handle_(nullptr),
      pipeline_(nullptr),
      context_(nullptr),
      client_callback_fn_(nullptr),
      client_callback_cookie_(nullptr) {}

SmartSelectionAdapter::~SmartSelectionAdapter() {
  Deinit();
}

//=============================================================================
// dlopen / dlsym
//=============================================================================

int SmartSelectionAdapter::LoadLibrary() {
  if (lib_handle_) {
    DLOGD("Library already loaded");
    return 0;
  }

  // Clear any previous dlerror
  dlerror();

  lib_handle_ = dlopen(kSmartSelectionLibPath, RTLD_NOW | RTLD_LOCAL);
  if (!lib_handle_) {
    DLOGE("dlopen('%s') failed: %s", kSmartSelectionLibPath, dlerror());
    return -1;
  }
  DLOGI("Loaded %s successfully", kSmartSelectionLibPath);

  // Resolve all required C API symbols
  DLSYM_OR_FAIL(lib_handle_, fn_createPipeline_, "QaiorSS_createPipeline");
  DLSYM_OR_FAIL(lib_handle_, fn_destroyPipeline_, "QaiorSS_destroyPipeline");
  DLSYM_OR_FAIL(lib_handle_, fn_init_, "QaiorSS_init");
  DLSYM_OR_FAIL(lib_handle_, fn_deinit_, "QaiorSS_deinit");
  DLSYM_OR_FAIL(lib_handle_, fn_reconfigure_, "QaiorSS_reconfigure");
  DLSYM_OR_FAIL(lib_handle_, fn_enqueue_, "QaiorSS_enqueue");
  DLSYM_OR_FAIL(lib_handle_, fn_flushByConfig_, "QaiorSS_flushByConfig");
  DLSYM_OR_FAIL(lib_handle_, fn_flushAll_, "QaiorSS_flushAll");
  DLSYM_OR_FAIL(lib_handle_, fn_flushSelected_, "QaiorSS_flushSelected");
  DLSYM_OR_FAIL(lib_handle_, fn_deleteByConfig_, "QaiorSS_deleteByConfig");
  DLSYM_OR_FAIL(lib_handle_, fn_deleteAll_, "QaiorSS_deleteAll");
  DLSYM_OR_FAIL(lib_handle_, fn_freeFrames_, "QaiorSS_freeFrames");
  DLSYM_OR_FAIL(lib_handle_, fn_waitUntilIdle_, "QaiorSS_waitUntilIdle");
  DLSYM_OR_FAIL(lib_handle_, fn_querySelectorStatus_, "QaiorSS_querySelectorStatus");

  DLOGD("All C API symbols resolved successfully");
  return 0;
}

void SmartSelectionAdapter::UnloadLibrary() {
  // Null out all function pointers before closing
  fn_createPipeline_ = nullptr;
  fn_destroyPipeline_ = nullptr;
  fn_init_ = nullptr;
  fn_deinit_ = nullptr;
  fn_reconfigure_ = nullptr;
  fn_enqueue_ = nullptr;
  fn_flushByConfig_ = nullptr;
  fn_flushAll_ = nullptr;
  fn_flushSelected_ = nullptr;
  fn_deleteByConfig_ = nullptr;
  fn_deleteAll_ = nullptr;
  fn_freeFrames_ = nullptr;
  fn_waitUntilIdle_ = nullptr;
  fn_querySelectorStatus_ = nullptr;

  if (lib_handle_) {
    dlclose(lib_handle_);
    lib_handle_ = nullptr;
    DLOGD("Unloaded %s", kSmartSelectionLibPath);
  }
}

//=============================================================================
// ImageAlgoInterface Implementation
//=============================================================================

int SmartSelectionAdapter::Init() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (init_done_) {
    DLOGD("SmartSelectionAdapter already initialized");
    return 0;
  }

  // 1. Load libQaiorSmartSelection.so and resolve symbols
  int ret = LoadLibrary();
  if (ret != 0) {
    DLOGE("Failed to load SmartSelection library");
    return ret;
  }

  // 2. Initialize ISnapMapper independently (adapter owns its own instance)
  int snap_ret = ImageAlgoCommon::InitSnapMapper(snapmapper_);
  if (snap_ret != 0 || !snapmapper_) {
    DLOGW("Init(): failed to initialize ISnapMapper (ret=%d); DoEnqueue() will fail", snap_ret);
  }

  // 3. Load configuration JSON from default vendor path
  ret = LoadConfigurationJson("/vendor/etc/smartselection_config.json");
  if (ret != 0) {
    DLOGW("LoadConfigurationJson failed (%d); config must be provided via ProcessOps INIT", ret);
  }

  init_done_ = true;
  DLOGI("SmartSelectionAdapter initialized");
  return 0;
}

int SmartSelectionAdapter::Deinit() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!init_done_) {
    return 0;
  }

  // Deinit pipeline if active
  if (pipeline_ && context_ && fn_deinit_) {
    fn_deinit_(pipeline_, context_);
    context_ = nullptr;
  }

  // Destroy pipeline
  if (pipeline_ && fn_destroyPipeline_) {
    fn_destroyPipeline_(pipeline_);
    pipeline_ = nullptr;
  }

  // Unload the shared library
  UnloadLibrary();

  // Release any pending AHardwareBuffers.
  // Safe with CLONE: AHardwareBuffer_release() frees only the cloned handle,
  // not the original native_handle_t* owned by BufferAllocator.
  {
    std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
    for (AHardwareBuffer *ahb : pending_ahbs_) {
      DLOGW("Deinit: releasing pending AHB %p (cloned handle)", ahb);
      AHardwareBuffer_release(ahb);
    }
    pending_ahbs_.clear();
    ahb_to_native_handle_map_.clear();
  }

  // Release snapmapper_ (snapalloc lib stays resident — no dlclose needed)
  snapmapper_.reset();

  init_done_ = false;
  DLOGD("SmartSelectionAdapter deinitialized");
  return 0;
}

int SmartSelectionAdapter::SetParameter(int param, const sdm::GenericPayload &in) {
  // Reserved for future use (e.g. runtime configuration updates).
  // Currently returns -ENOTSUP; add cases here as needed.
  (void)in;
  DLOGW("SetParameter: param=%d not implemented (reserved for future use)", param);
  return -ENOTSUP;
}

int SmartSelectionAdapter::GetParameter(int param, sdm::GenericPayload *out) {
  // Reserved for future use (e.g. querying pipeline status or capabilities).
  // Currently returns -ENOTSUP; add cases here as needed.
  (void)out;
  DLOGW("GetParameter: param=%d not implemented (reserved for future use)", param);
  return -ENOTSUP;
}

int SmartSelectionAdapter::ProcessOps(SmartSelectionOp ops, const sdm::GenericPayload &input,
                                      sdm::GenericPayload *output) {
  // Validate state under lock, then release before dispatching to external library.
  // This prevents a deadlock if the library calls the emit callback synchronously
  // from within fn_enqueue_() / fn_flushAll_() etc., and the callback tries to
  // call ProcessOps() again (mutex_ is non-recursive).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!init_done_) {
      DLOGE("Adapter not initialized");
      return -EINVAL;
    }
    // output may be null for ops that produce no output (kSSFlushAll, kSSDeinit, etc.)
    // Each Do* handler is responsible for checking output only when it actually needs it.
  }

  // SmartSelectionOp is a typed enum — the compiler rejects SuperResolution op codes here.
  switch (ops) {
    case kSSInit:
      return DoInit(input, output);
    case kSSReconfigure:
      return DoReconfigure(input, output);
    case kSSEnqueue:
      return DoEnqueue(input, output);
    case kSSFlushByConfig:
      return DoFlushByConfig(input, output);
    case kSSFlushAll:
      return DoFlushAll(input, output);
    case kSSFlushSelected:
      return DoFlushSelected(input, output);
    case kSSDeleteByConfig:
      return DoDeleteByConfig(input, output);
    case kSSDeleteAll:
      return DoDeleteAll(input, output);
    case kSSDeinit:
      return DoDeinit(input, output);
    case kSSWaitUntilIdle:
      return DoWaitUntilIdle(input, output);
    case kSSQuerySelectorStatus:
      return DoQuerySelectorStatus(input, output);
    default:
      DLOGW("Unsupported operation: %d", static_cast<int>(ops));
      return -ENOTSUP;
  }
}

//=============================================================================
// Emit Callback (static trampoline → member function)
//=============================================================================

// static
void SmartSelectionAdapter::EmitCallbackTrampoline(const QaiorSS_EmitResult_t *result,
                                                   void *cookie) {
  if (!cookie)
    return;
  static_cast<SmartSelectionAdapter *>(cookie)->OnEmitResult(result);
}

void SmartSelectionAdapter::OnEmitResult(const QaiorSS_EmitResult_t *result) {
  if (!result)
    return;

  DLOGD("OnEmitResult: selected=%d rejected=%d", result->selectedCount, result->rejectedCount);

  // Copy callback and cookie under callback_mutex_ before use.
  SmartSelectionEmitCallbackFn cb = nullptr;
  void *cb_cookie = nullptr;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    cb = client_callback_fn_;
    cb_cookie = client_callback_cookie_;
  }

  SmartSelectionEmitResult emit_result;

  // Helper lambda: look up and remove an AHB from the pending set.
  //
  // Returns {found, native_hdl}:
  //   found      = true  → AHB was in pending set and has been removed.
  //   native_hdl = non-null → frame was originally enqueued as kSSFrameNativeHandle;
  //                           the adapter will release the AHB internally and return
  //                           native_hdl to the caller (buffer type stays consistent
  //                           with what was enqueued).
  //   native_hdl = nullptr → frame was enqueued as kSSFrameAHardwareBuffer directly;
  //                           AHB ownership is transferred to the caller.
  //   found      = false → AHB not in pending set (concurrent Deinit); skip frame.
  //
  // ONLY removes if found in pending_ahbs_ — prevents double-removal in:
  //   1. Concurrent Deinit/DoDeinit already released and cleared pending_ahbs_.
  //   2. Same AHB pointer in both selectedFrames and rejectedFrames (library bug).
  auto lookup_ahb = [this](AHardwareBuffer *ahb,
                           const char *tag) -> std::pair<bool, native_handle_t *> {
    if (!ahb)
      return {false, nullptr};
    std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
    auto it = pending_ahbs_.find(ahb);
    if (it == pending_ahbs_.end()) {
      DLOGW("OnEmitResult: AHB %p (%s) not in pending set — already released, skipping", ahb, tag);
      return {false, nullptr};
    }
    pending_ahbs_.erase(it);
    // Retrieve and erase the native_handle mapping.
    // nullptr means the AHB was enqueued directly (kSSFrameAHardwareBuffer path).
    native_handle_t *nh = nullptr;
    auto map_it = ahb_to_native_handle_map_.find(ahb);
    if (map_it != ahb_to_native_handle_map_.end()) {
      nh = map_it->second;
      ahb_to_native_handle_map_.erase(map_it);
    }
    DLOGD("OnEmitResult: AHB %p (%s) removed from pending set, native_handle=%p", ahb, tag, nh);
    return {true, nh};
  };

  // Helper lambda: convert QaiorSS_FrameHandle_t → SmartSelectionFrame.
  // For ParcelFd: dups the fd (caller must close).
  // For AHardwareBuffer: transfers ownership from pending set to caller
  //   (caller MUST call AHardwareBuffer_release(buffer) when done).
  // For GraphicBuffer: passes pointer through (caller manages lifetime).
  // For Other: passes otherHandle through.
  // Returns false if the frame should be skipped (e.g. dup failed).
  auto convert_frame = [&](const QaiorSS_FrameHandle_t &src, int idx, const char *tag,
                           SmartSelectionFrame &dst) -> bool {
    dst.metadata_json = src.metadataJson ? src.metadataJson : "";
    dst.cookie = src.cookie;  // pass through per-frame OEM cookie
    switch (src.type) {
      case QAIOR_SS_FRAME_PARCELFD:
        dst.type = kSSFrameParcelFd;
        if (src.parcelFd >= 0) {
          dst.parcel_fd = dup(src.parcelFd);
          if (dst.parcel_fd < 0) {
            DLOGW("OnEmitResult: dup() failed for %s frame %d: %s", tag, idx, strerror(errno));
            return false;
          }
        }
        break;
      case QAIOR_SS_FRAME_AHARDWAREBUFFER: {
        AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(src.buffer);
        auto [found, native_hdl] = lookup_ahb(ahb, tag);
        if (!found)
          return false;  // concurrent Deinit released it; skip frame

        if (native_hdl) {
          // Frame was originally enqueued as kSSFrameNativeHandle.
          // Return the original native_handle_t* to the caller so the emit
          // result type matches the enqueue input type.
          // The AHB (CLONE'd handle) is released here — caller never sees it.
          dst.type = kSSFrameNativeHandle;
          dst.buffer = native_hdl;
          AHardwareBuffer_release(ahb);
          DLOGD("OnEmitResult: AHB %p → native_handle %p (%s)", ahb, native_hdl, tag);
        } else {
          // Frame was enqueued directly as kSSFrameAHardwareBuffer.
          // Transfer AHB ownership to caller; caller MUST call AHardwareBuffer_release().
          dst.type = kSSFrameAHardwareBuffer;
          dst.buffer = ahb;
          DLOGD("OnEmitResult: transferring AHB %p (%s) ownership to caller", ahb, tag);
        }
        break;
      }
      case QAIOR_SS_FRAME_GRAPHICBUFFER:
        dst.type = kSSFrameGraphicBuffer;
        dst.buffer = src.buffer;  // caller manages GraphicBuffer lifetime
        break;
      default:
        dst.type = kSSFrameNativeHandle;
        dst.other_handle = src.otherHandle;
        break;
    }
    return true;
  };

  // Guard: check cb BEFORE any ownership-changing conversion.
  // convert_frame() for kSSFrameParcelFd calls dup() and for kSSFrameAHardwareBuffer
  // transfers ownership out of pending_ahbs_/ahb_to_native_handle_map_.
  // If cb is null those resources would be leaked when the unconverted frame is discarded.
  if (!cb) {
    DLOGW("OnEmitResult: no client callback registered — releasing resources");
    // Release AHB frames: remove from pending set and release the cloned/acquired AHB.
    // ParcelFd frames need no action: library owns the fd; we never dup'd it here.
    auto release_ahb_frame = [&](const QaiorSS_FrameHandle_t &src, const char *tag) {
      if (src.type == QAIOR_SS_FRAME_AHARDWAREBUFFER && src.buffer) {
        AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(src.buffer);
        auto [found, native_hdl] = lookup_ahb(ahb, tag);
        if (found) {
          // Release the AHB (CLONE'd or acquire'd in PrepareFrame*).
          // native_hdl (if non-null) is the original handle owned by the caller;
          // no ownership action needed for it.
          AHardwareBuffer_release(ahb);
        }
      }
    };
    for (int32_t i = 0; i < result->selectedCount; ++i)
      release_ahb_frame(result->selectedFrames[i], "selected");
    for (int32_t i = 0; i < result->rejectedCount; ++i)
      release_ahb_frame(result->rejectedFrames[i], "rejected");
    return;
  }

  // Build per-frame selected results.
  // IMPORTANT: metadataJson is a raw pointer valid only for this callback duration.
  for (int32_t i = 0; i < result->selectedCount; ++i) {
    SmartSelectionFrame fr;
    if (!convert_frame(result->selectedFrames[i], i, "selected", fr))
      continue;
    emit_result.selected_frames.push_back(std::move(fr));
  }

  // Build per-frame rejected results.
  for (int32_t i = 0; i < result->rejectedCount; ++i) {
    SmartSelectionFrame fr;
    if (!convert_frame(result->rejectedFrames[i], i, "rejected", fr))
      continue;
    emit_result.rejected_frames.push_back(std::move(fr));
  }

  // Call client callback with the locally-copied cb/cookie.
  cb(&emit_result, cb_cookie);
}

//=============================================================================
// 9 Operations
//=============================================================================

int SmartSelectionAdapter::DoInit(const sdm::GenericPayload &input, sdm::GenericPayload *output) {
  DLOGD("DoInit: Initializing SmartSelection pipeline via C API");

  // P1 fix: extract all needed data under lock, then release before calling library.
  // If fn_init_() calls the emit callback synchronously, the callback can call
  // ProcessOps() without deadlocking on mutex_ (which is non-recursive).
  SmartSelectionEmitCallbackFn cb = nullptr;
  void *cb_cookie = nullptr;
  std::string config_to_use;
  decltype(fn_createPipeline_) create_fn = nullptr;
  decltype(fn_init_) init_fn = nullptr;
  decltype(fn_destroyPipeline_) destroy_fn = nullptr;
  decltype(fn_deinit_) deinit_fn = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Re-check init_done_ after acquiring the lock: ProcessOps() releases
    // mutex_ before calling DoInit(). In the window between ProcessOps()
    // releasing the lock and DoInit() acquiring it, Deinit() could have been
    // called, clearing fn_createPipeline_ to nullptr.
    if (!init_done_) {
      DLOGE("DoInit: adapter was deinitialized concurrently; aborting");
      return -EINVAL;
    }
    if (pipeline_ || context_) {
      DLOGE("Pipeline already initialized");
      return -EINVAL;
    }

    SmartSelectionInitInput *init_input = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(init_input, &size) != 0 || !init_input || size != 1) {
      DLOGE("DoInit: invalid SmartSelectionInitInput payload");
      return -EINVAL;
    }

    cb = init_input->callback;
    cb_cookie = init_input->cookie;

    if (!init_input->config_json.empty()) {
      config_to_use = init_input->config_json;
      DLOGD("DoInit: using JSON config from input (%zu bytes)", config_to_use.size());
    } else {
      if (current_config_json_.empty()) {
        DLOGE("DoInit: no configuration available (neither runtime nor file config)");
        return -EINVAL;
      }
      config_to_use = current_config_json_;
      DLOGD("DoInit: using JSON config from file (%zu bytes)", config_to_use.size());
    }

    // Copy function pointers under lock (cleared by UnloadLibrary under mutex_)
    create_fn = fn_createPipeline_;
    init_fn = fn_init_;
    destroy_fn = fn_destroyPipeline_;
    deinit_fn = fn_deinit_;
  }
  // mutex_ released — library calls below will not deadlock on mutex_

  if (!cb) {
    DLOGW("DoInit: no callback provided");
  }

  // P2 fix: store callback under callback_mutex_ (separate from mutex_).
  // OnEmitResult() reads these under callback_mutex_ from the library's callback thread.
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    client_callback_fn_ = cb;
    client_callback_cookie_ = cb_cookie;
  }

  // Create pipeline (no lock held)
  QaiorSS_Pipeline_t *new_pipeline = create_fn();
  if (!new_pipeline) {
    DLOGE("DoInit: QaiorSS_createPipeline() returned null");
    // Clear callback: pipeline creation failed, no emit will ever fire.
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      client_callback_fn_ = nullptr;
      client_callback_cookie_ = nullptr;
    }
    return -ENOMEM;
  }

  // Initialize context (no lock held — emit callback can call ProcessOps() safely)
  QaiorSS_Context_t *new_context =
      init_fn(new_pipeline, config_to_use.c_str(), EmitCallbackTrampoline, this);
  if (!new_context) {
    DLOGE("DoInit: QaiorSS_init() failed");
    destroy_fn(new_pipeline);
    // Clear callback: context init failed, no emit will ever fire.
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      client_callback_fn_ = nullptr;
      client_callback_cookie_ = nullptr;
    }
    return -EINVAL;
  }

  // Store results under lock; check for concurrent Deinit() or concurrent DoInit().
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!init_done_) {
      // Deinit() was called concurrently — tear down what we just created
      DLOGE("DoInit: adapter was deinitialized concurrently; tearing down new pipeline");
      if (deinit_fn)
        deinit_fn(new_pipeline, new_context);
      destroy_fn(new_pipeline);
      return -EINVAL;
    }
    if (pipeline_) {
      // Another concurrent DoInit() won the race — tear down our duplicate instance.
      // This prevents the first thread's pipeline/context from being leaked.
      DLOGE(
          "DoInit: pipeline already set by concurrent DoInit(); "
          "tearing down duplicate");
      if (deinit_fn)
        deinit_fn(new_pipeline, new_context);
      destroy_fn(new_pipeline);
      return -EINVAL;
    }
    pipeline_ = new_pipeline;
    context_ = new_context;
  }

  DLOGI("SmartSelection pipeline initialized successfully (C API)");
  return 0;
}

int SmartSelectionAdapter::DoReconfigure(const sdm::GenericPayload &input,
                                         sdm::GenericPayload *output) {
  DLOGD("DoReconfigure");

  // P1 fix: copy state under lock, call library without lock.
  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  std::string json_copy;
  decltype(fn_reconfigure_) fn_reconfigure = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    std::string *json_config = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(json_config, &size) != 0 || !json_config || size != 1) {
      DLOGE("DoReconfigure: failed to get JSON config from input");
      return -EINVAL;
    }
    json_copy = *json_config;
    pipeline = pipeline_;
    context = context_;
    fn_reconfigure = fn_reconfigure_;
  }

  QaiorSS_Status_t status = fn_reconfigure(pipeline, context, json_copy.c_str());
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoReconfigure: QaiorSS_reconfigure() failed: %d", status);
    return -EINVAL;
  }

  DLOGD("Pipeline reconfigured successfully");
  return 0;
}

int SmartSelectionAdapter::DoEnqueue(const sdm::GenericPayload &input,
                                     sdm::GenericPayload *output) {
  DLOGD("DoEnqueue");

  // Extract all inputs under mutex_, then release before calling library.
  // (Emit callback may fire synchronously from fn_enqueue_ and call ProcessOps,
  //  which would deadlock on the non-recursive mutex_ if still held here.)
  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  SmartSelectionFrameInputType input_type = kSSFrameNativeHandle;
  void *buffer = nullptr;
  int parcel_fd = -1;
  void *frame_cookie = nullptr;
  std::string metadata_json;
  decltype(fn_enqueue_) fn_enqueue = nullptr;
  std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper> snapmapper;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    SmartSelectionEnqueueInput *enqueue_input = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(enqueue_input, &size) != 0 || !enqueue_input || size != 1) {
      DLOGE("DoEnqueue: invalid SmartSelectionEnqueueInput payload");
      return -EINVAL;
    }
    if (enqueue_input->metadata_json.empty()) {
      DLOGE("DoEnqueue: metadata_json is required");
      return -EINVAL;
    }

    input_type = enqueue_input->input_type;
    metadata_json = enqueue_input->metadata_json;

    if (input_type == kSSFrameParcelFd) {
      if (enqueue_input->parcel_fd < 0) {
        DLOGE("DoEnqueue: parcel_fd is required for kSSFrameParcelFd");
        return -EINVAL;
      }
      parcel_fd = enqueue_input->parcel_fd;
    } else {
      if (!enqueue_input->buffer) {
        DLOGE("DoEnqueue: buffer is required for input_type=%d", input_type);
        return -EINVAL;
      }
      buffer = enqueue_input->buffer;
    }

    frame_cookie = enqueue_input->cookie;
    pipeline = pipeline_;
    context = context_;
    fn_enqueue = fn_enqueue_;
    snapmapper = snapmapper_;

    DLOGD("DoEnqueue: input_type=%d metadata_json=%s", input_type, metadata_json.c_str());
  }
  // mutex_ released — Prepare* and fn_enqueue calls below will not deadlock.

  // Prepare QaiorSS_FrameHandle_t based on input_type.
  // metadata_json must outlive fn_enqueue() since frame.metadataJson points into it.
  QaiorSS_FrameHandle_t frame{};
  int ret = 0;

  switch (input_type) {
    case kSSFrameParcelFd:
      ret = PrepareFrameParcelFd(parcel_fd, metadata_json, frame);
      break;
    case kSSFrameGraphicBuffer:
      ret = PrepareFrameGraphicBuffer(buffer, metadata_json, frame);
      break;
    case kSSFrameAHardwareBuffer:
      ret = PrepareFrameAHardwareBuffer(buffer, metadata_json, frame);
      break;
    case kSSFrameNativeHandle:
      ret = PrepareFrameNativeHandle(buffer, metadata_json, snapmapper, frame);
      break;
    default:
      DLOGE("DoEnqueue: unsupported input_type=%d", static_cast<int>(input_type));
      return -ENOTSUP;
  }
  if (ret != 0)
    return ret;

  // Set the per-frame OEM cookie from the enqueue input.
  // QaiorSS_FrameHandle_t::cookie is passed through by the library and returned
  // in QaiorSS_EmitResult_t::selectedFrames[i].cookie / rejectedFrames[i].cookie,
  // allowing callers to correlate emit results back to specific enqueued frames.
  // The initCookie (= this) passed to QaiorSS_init() drives EmitCallbackTrampoline.
  frame.cookie = frame_cookie;

  QaiorSS_Status_t status = fn_enqueue(pipeline, context, &frame);
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoEnqueue: QaiorSS_enqueue() failed: %d (input_type=%d)", status, input_type);
    CleanupFrameOnFailure(frame);
    return -EINVAL;
  }

  DLOGD("Frame enqueued successfully (input_type=%d)", input_type);
  return 0;
}

//=============================================================================
// Frame Preparation Helpers
//=============================================================================

// ---------------------------------------------------------------------------
// Simple JSON uint32 field extractor — no library dependency.
// Searches for "key":<digits> and returns the integer value, or 0 if not found.
// Used by PrepareFrameNativeHandle to extract width/height/stride from metadata_json
// for AHardwareBuffer_Desc when gralloc metadata query fails.
// ---------------------------------------------------------------------------
static uint32_t ExtractUint32FromJson(const std::string &json, const char *key) {
  // Build search pattern: "key":
  std::string pattern = "\"";
  pattern += key;
  pattern += "\":";
  auto pos = json.find(pattern);
  if (pos == std::string::npos)
    return 0;
  pos += pattern.size();
  // skip optional whitespace
  while (pos < json.size() && json[pos] == ' ')
    ++pos;
  uint32_t val = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    val = val * 10 + static_cast<uint32_t>(json[pos] - '0');
    ++pos;
  }
  return val;
}

int SmartSelectionAdapter::PrepareFrameParcelFd(int parcel_fd, const std::string &metadata_json,
                                                QaiorSS_FrameHandle_t &frame) {
  frame = {};
  frame.type = QAIOR_SS_FRAME_PARCELFD;
  frame.buffer = nullptr;
  frame.parcelFd = parcel_fd;
  frame.otherHandle = nullptr;
  frame.metadataJson = metadata_json.c_str();
  DLOGD("PrepareFrameParcelFd: fd=%d", parcel_fd);
  return 0;
}

int SmartSelectionAdapter::PrepareFrameGraphicBuffer(void *buffer, const std::string &metadata_json,
                                                     QaiorSS_FrameHandle_t &frame) {
  frame = {};
  frame.type = QAIOR_SS_FRAME_GRAPHICBUFFER;
  frame.buffer = buffer;
  frame.parcelFd = -1;
  frame.otherHandle = nullptr;
  frame.metadataJson = metadata_json.c_str();
  DLOGD("PrepareFrameGraphicBuffer: buf=%p", buffer);
  return 0;
}

int SmartSelectionAdapter::PrepareFrameAHardwareBuffer(void *ahb_ptr,
                                                       const std::string &metadata_json,
                                                       QaiorSS_FrameHandle_t &frame) {
  AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(ahb_ptr);
  AHardwareBuffer_acquire(ahb);
  {
    std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
    pending_ahbs_.insert(ahb);
  }
  frame = {};
  frame.type = QAIOR_SS_FRAME_AHARDWAREBUFFER;
  frame.buffer = ahb;
  frame.parcelFd = -1;
  frame.otherHandle = nullptr;
  frame.metadataJson = metadata_json.c_str();
  DLOGD("PrepareFrameAHardwareBuffer: ahb=%p", ahb);
  return 0;
}

int SmartSelectionAdapter::PrepareFrameNativeHandle(
    void *native_hdl_ptr, const std::string &metadata_json,
    const std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper> &snapmapper,
    QaiorSS_FrameHandle_t &frame) {
  using namespace vendor::qti::hardware::display::snapalloc;
  using MetadataType = vendor_qti_hardware_display_common_MetadataType;

  native_handle_t *native_hdl = static_cast<native_handle_t *>(native_hdl_ptr);

  // -----------------------------------------------------------------------
  // Step 1: Query gralloc metadata (format, UBWC, dimensions).
  // Primary source for AHardwareBuffer_Desc; falls back to metadata_json fields
  // if snapmapper is unavailable or query fails.
  // -----------------------------------------------------------------------
  int gralloc_format = kHalPixelFormatRGBA8888;
  bool gralloc_format_valid = false;
  int64_t is_ubwc = 0;
  uint32_t gralloc_w = 0, gralloc_h = 0, gralloc_stride = 0;

  if (snapmapper) {
    SnapHandle *snap_hdl = reinterpret_cast<SnapHandle *>(native_hdl);
    int queried_format = 0;
    int64_t queried_ubwc = 0;
    uint64_t qw = 0, qh = 0;

    auto ret_fmt =
        snapmapper->GetMetadata(*snap_hdl, MetadataType::PIXEL_FORMAT_ALLOCATED, &queried_format);
    auto ret_ubwc = snapmapper->GetMetadata(*snap_hdl, MetadataType::IS_UBWC, &queried_ubwc);
    snapmapper->GetMetadata(*snap_hdl, MetadataType::WIDTH, &qw);
    snapmapper->GetMetadata(*snap_hdl, MetadataType::HEIGHT, &qh);
    snapmapper->GetMetadata(*snap_hdl, MetadataType::STRIDE, &gralloc_stride);

    if (ret_fmt == 0 && queried_format != 0) {
      gralloc_format = queried_format;
      gralloc_format_valid = true;
      DLOGI("PrepareFrameNativeHandle: gralloc format=0x%x", gralloc_format);
    } else {
      DLOGW("PrepareFrameNativeHandle: PIXEL_FORMAT_ALLOCATED query failed (ret=%d)",
            static_cast<int>(ret_fmt));
    }
    if (ret_ubwc == 0) {
      is_ubwc = queried_ubwc;
    }
    gralloc_w = static_cast<uint32_t>(qw);
    gralloc_h = static_cast<uint32_t>(qh);

    if (gralloc_w != 0 && gralloc_h != 0) {
      DLOGI("PrepareFrameNativeHandle: gralloc dims=%ux%u stride=%u", gralloc_w, gralloc_h,
            gralloc_stride);
    } else {
      DLOGW("PrepareFrameNativeHandle: gralloc dims query failed");
    }
  } else {
    DLOGW("PrepareFrameNativeHandle: snapmapper not available");
  }

  // -----------------------------------------------------------------------
  // Step 2: Determine AHardwareBuffer_Desc dimensions.
  // Primary: gralloc metadata. Fallback: parse from metadata_json.
  // -----------------------------------------------------------------------
  uint32_t w = (gralloc_w != 0) ? gralloc_w : ExtractUint32FromJson(metadata_json, "width");
  uint32_t h = (gralloc_h != 0) ? gralloc_h : ExtractUint32FromJson(metadata_json, "height");
  uint32_t stride =
      (gralloc_stride != 0) ? gralloc_stride : ExtractUint32FromJson(metadata_json, "stride");

  if (w == 0 || h == 0) {
    DLOGE(
        "PrepareFrameNativeHandle: cannot determine buffer dimensions "
        "(gralloc failed and metadata_json missing width/height)");
    return -EINVAL;
  }
  DLOGI("PrepareFrameNativeHandle: using dims=%ux%u stride=%u (ubwc=%lld)", w, h, stride,
        static_cast<long long>(is_ubwc));

  // -----------------------------------------------------------------------
  // Step 3: Map format → AHardwareBuffer format.
  // Primary: gralloc format. Fallback: parse "format" field from metadata_json.
  // -----------------------------------------------------------------------
  uint32_t ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  if (gralloc_format_valid) {
    if (gralloc_format == kHalPixelFormatRGBA1010102) {
      ahb_format = AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
    } else if (gralloc_format == kHalPixelFormatRGB888) {
      ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
    } else {
      ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    }
    DLOGI("PrepareFrameNativeHandle: AHB format from gralloc 0x%x → 0x%x", gralloc_format,
          ahb_format);
  } else {
    // Parse "format" string from metadata_json
    auto fmt_pos = metadata_json.find("\"format\":\"");
    if (fmt_pos != std::string::npos) {
      fmt_pos += 10;  // skip "format":"
      if (metadata_json.find("RGBA1010102", fmt_pos) == fmt_pos) {
        ahb_format = AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
      } else if (metadata_json.find("RGB888", fmt_pos) == fmt_pos) {
        ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
      } else {
        ahb_format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
      }
    }
    DLOGI("PrepareFrameNativeHandle: AHB format from metadata_json → 0x%x", ahb_format);
  }

  // -----------------------------------------------------------------------
  // Step 4: Create AHardwareBuffer via CLONE.
  //
  // CLONE dups the fds → new handle H'. importBuffer(H') succeeds (same fds).
  // AHardwareBuffer_release(ahb) frees H' only — original native_handle_t* unaffected.
  // -----------------------------------------------------------------------
  AHardwareBuffer_Desc desc{};
  desc.width = w;
  desc.height = h;
  desc.layers = 1;
  desc.format = ahb_format;
  desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
  desc.stride = stride;

  AHardwareBuffer *ahb = nullptr;
  int ahb_ret = AHardwareBuffer_createFromHandle(
      &desc, native_hdl, AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);
  if (ahb_ret != 0 || !ahb) {
    DLOGE("PrepareFrameNativeHandle: AHardwareBuffer_createFromHandle(CLONE) failed: %d", ahb_ret);
    return -EINVAL;
  }
  DLOGI("PrepareFrameNativeHandle: AHB %p from native_hdl %p (%ux%u stride=%u fmt=0x%x ubwc=%lld)",
        ahb, native_hdl, w, h, stride, gralloc_format, static_cast<long long>(is_ubwc));

  // -----------------------------------------------------------------------
  // Step 5: Verify AHB descriptor after creation.
  // -----------------------------------------------------------------------
  {
    AHardwareBuffer_Desc actual{};
    AHardwareBuffer_describe(ahb, &actual);
    DLOGI("PrepareFrameNativeHandle: AHB actual desc w=%u h=%u fmt=0x%x stride=%u", actual.width,
          actual.height, actual.format, actual.stride);
    bool mismatch =
        (actual.width != desc.width || actual.height != desc.height ||
         actual.format != desc.format || (actual.stride != 0 && actual.stride != desc.stride));
    if (mismatch) {
      DLOGW(
          "PrepareFrameNativeHandle: AHB descriptor mismatch! "
          "req w=%u h=%u fmt=0x%x stride=%u, actual w=%u h=%u fmt=0x%x stride=%u",
          desc.width, desc.height, desc.format, desc.stride, actual.width, actual.height,
          actual.format, actual.stride);
    } else {
      DLOGD("PrepareFrameNativeHandle: AHB descriptor verified OK");
    }
  }

  // -----------------------------------------------------------------------
  // Step 6: Lock/unlock for CPU cache invalidation.
  //
  // CWB buffer is written by display DMA. CPU cache may hold stale data.
  // AHardwareBuffer_lock(CPU_READ_OFTEN) forces cache invalidation so
  // SmartSelection model inference sees the latest pixel data.
  // -----------------------------------------------------------------------
  {
    void *cpu_data = nullptr;
    int lock_ret =
        AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &cpu_data);
    if (lock_ret != 0 || !cpu_data) {
      DLOGW(
          "PrepareFrameNativeHandle: AHardwareBuffer_lock() failed (ret=%d) — "
          "CPU cache invalidation skipped; SmartSelection may read stale data",
          lock_ret);
    } else {
      const uint8_t *px = static_cast<const uint8_t *>(cpu_data);
      bool all_zero = true;
      for (int i = 0; i < 16 && all_zero; i++) {
        if (px[i] != 0)
          all_zero = false;
      }
      DLOGI(
          "PrepareFrameNativeHandle: AHB lock OK cpu_data=%p "
          "first 4px=[%02x%02x%02x%02x %02x%02x%02x%02x "
          "%02x%02x%02x%02x %02x%02x%02x%02x] %s",
          cpu_data, px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7], px[8], px[9], px[10],
          px[11], px[12], px[13], px[14], px[15],
          all_zero ? "WARNING: first 16 bytes all zero — buffer may be uninitialized!"
                   : "content OK");
      AHardwareBuffer_unlock(ahb, nullptr);
    }
  }

  // -----------------------------------------------------------------------
  // Step 7: Add to pending set and fill frame.
  // Store BEFORE fn_enqueue (OnEmitResult may fire before fn_enqueue returns).
  // AHardwareBuffer_createFromHandle already holds one reference — do NOT
  // call AHardwareBuffer_acquire() again.
  // Also record the AHB → native_handle_t* mapping so OnEmitResult can
  // surface the original native handle back to the caller.
  // -----------------------------------------------------------------------
  {
    std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
    pending_ahbs_.insert(ahb);
    ahb_to_native_handle_map_[ahb] = native_hdl;
  }

  frame = {};
  frame.type = QAIOR_SS_FRAME_AHARDWAREBUFFER;
  frame.buffer = ahb;
  frame.parcelFd = -1;
  frame.otherHandle = nullptr;
  frame.metadataJson = metadata_json.c_str();

  DLOGD("PrepareFrameNativeHandle: ahb=%p native_hdl=%p", ahb, native_hdl);
  return 0;
}

void SmartSelectionAdapter::CleanupFrameOnFailure(const QaiorSS_FrameHandle_t &frame) {
  // Called when fn_enqueue() fails AFTER a Prepare* function succeeded.
  // Each case documents the ownership model so the correct cleanup is applied.
  switch (frame.type) {
    case QAIOR_SS_FRAME_AHARDWAREBUFFER:
      // PrepareFrameAHardwareBuffer: AHardwareBuffer_acquire() was called.
      // PrepareFrameNativeHandle:    AHardwareBuffer_createFromHandle(CLONE) was called.
      // Both paths added the AHB to pending_ahbs_ and hold one reference.
      // fn_enqueue failed → library never took ownership → we must release.
      if (frame.buffer) {
        AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(frame.buffer);
        {
          std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
          pending_ahbs_.erase(ahb);
          ahb_to_native_handle_map_.erase(ahb);
        }
        DLOGW("CleanupFrameOnFailure: releasing AHB %p", ahb);
        AHardwareBuffer_release(ahb);
      }
      break;

    case QAIOR_SS_FRAME_PARCELFD:
      // PrepareFrameParcelFd: frame.parcelFd is the caller's original fd (not dup'd).
      // The library was supposed to dup it internally on enqueue.
      // fn_enqueue failed → library never dup'd → caller still owns the fd.
      // No cleanup needed here.
      break;

    case QAIOR_SS_FRAME_GRAPHICBUFFER:
      // PrepareFrameGraphicBuffer: frame.buffer is the caller's GraphicBuffer* (no ref taken).
      // Caller owns the GraphicBuffer lifetime entirely.
      // No cleanup needed here.
      break;

    default:
      DLOGW("CleanupFrameOnFailure: unknown frame type %d — no cleanup performed",
            static_cast<int>(frame.type));
      break;
  }
}

int SmartSelectionAdapter::DoFlushByConfig(const sdm::GenericPayload &input,
                                           sdm::GenericPayload *output) {
  DLOGD("DoFlushByConfig");

  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  std::string json_copy;
  decltype(fn_flushByConfig_) fn_flush = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    std::string *flush_config_json = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(flush_config_json, &size) != 0 || !flush_config_json || size != 1) {
      DLOGE("DoFlushByConfig: failed to get flush config from input");
      return -EINVAL;
    }
    json_copy = *flush_config_json;
    pipeline = pipeline_;
    context = context_;
    fn_flush = fn_flushByConfig_;
  }

  QaiorSS_Status_t status = fn_flush(pipeline, context, json_copy.c_str());
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoFlushByConfig: QaiorSS_flushByConfig() failed: %d", status);
    return -EINVAL;
  }

  DLOGD("FlushByConfig succeeded");
  return 0;
}

int SmartSelectionAdapter::DoFlushAll(const sdm::GenericPayload &input,
                                      sdm::GenericPayload *output) {
  DLOGD("DoFlushAll");

  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  decltype(fn_flushAll_) fn_flush = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }
    pipeline = pipeline_;
    context = context_;
    fn_flush = fn_flushAll_;
  }

  QaiorSS_Status_t status = fn_flush(pipeline, context);
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoFlushAll: QaiorSS_flushAll() failed: %d", status);
    return -EINVAL;
  }

  DLOGD("FlushAll succeeded");
  return 0;
}

int SmartSelectionAdapter::DoFlushSelected(const sdm::GenericPayload &input,
                                           sdm::GenericPayload *output) {
  DLOGD("DoFlushSelected");

  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  std::string app_name_copy;
  std::string user_id_copy;
  decltype(fn_flushSelected_) fn_flush = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    SmartSelectionFlushSelectedInput *flush_input = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(flush_input, &size) != 0 || !flush_input || size != 1) {
      DLOGE("DoFlushSelected: failed to get SmartSelectionFlushSelectedInput from input");
      return -EINVAL;
    }
    if (flush_input->app_name.empty()) {
      DLOGE("DoFlushSelected: app_name is required");
      return -EINVAL;
    }
    app_name_copy = flush_input->app_name;
    user_id_copy = flush_input->user_id;
    pipeline = pipeline_;
    context = context_;
    fn_flush = fn_flushSelected_;
  }

  // Pass nullptr for user_id when empty (API treats nullptr as "ignore user_id filter").
  const char *user_id_ptr = user_id_copy.empty() ? nullptr : user_id_copy.c_str();
  QaiorSS_Status_t status = fn_flush(pipeline, context, app_name_copy.c_str(), user_id_ptr);
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoFlushSelected: QaiorSS_flushSelected() failed: %d (app=%s)", status,
          app_name_copy.c_str());
    return -EINVAL;
  }

  DLOGD("FlushSelected succeeded for app=%s", app_name_copy.c_str());
  return 0;
}

int SmartSelectionAdapter::DoDeleteByConfig(const sdm::GenericPayload &input,
                                            sdm::GenericPayload *output) {
  DLOGD("DoDeleteByConfig");

  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  std::string json_copy;
  decltype(fn_deleteByConfig_) fn_delete = nullptr;
  decltype(fn_freeFrames_) fn_free = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    std::string *delete_config_json = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(delete_config_json, &size) != 0 || !delete_config_json || size != 1) {
      DLOGE("DoDeleteByConfig: failed to get delete config from input");
      return -EINVAL;
    }
    json_copy = *delete_config_json;
    pipeline = pipeline_;
    context = context_;
    fn_delete = fn_deleteByConfig_;
    fn_free = fn_freeFrames_;
  }

  QaiorSS_FrameHandle_t *out_frames = nullptr;
  int32_t out_count = 0;

  QaiorSS_Status_t status =
      fn_delete(pipeline, context, json_copy.c_str(), &out_frames, &out_count);
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoDeleteByConfig: QaiorSS_deleteByConfig() failed: %d", status);
    return -EINVAL;
  }

  if (out_frames) {
    // Helper: convert C frame array → SmartSelectionDeleteResult, handling AHB ownership.
    // For ParcelFd: dup the fd for the caller, then close the library's copy.
    // For AHB: remove from pending_ahbs_ (transfer ownership to caller, no release).
    // For GraphicBuffer: pass pointer through.
    SmartSelectionDeleteResult *delete_result = nullptr;
    uint32_t payload_size = 0;
    bool has_output = (output && output->GetPayload(delete_result, &payload_size) == 0 &&
                       delete_result && payload_size == 1);

    for (int32_t i = 0; i < out_count; ++i) {
      const QaiorSS_FrameHandle_t &f = out_frames[i];
      if (has_output) {
        SmartSelectionFrame fr;
        fr.metadata_json = f.metadataJson ? f.metadataJson : "";
        if (f.type == QAIOR_SS_FRAME_PARCELFD && f.parcelFd >= 0) {
          fr.type = kSSFrameParcelFd;
          fr.parcel_fd = dup(f.parcelFd);  // dup for caller; close original below
          close(f.parcelFd);
        } else if (f.type == QAIOR_SS_FRAME_AHARDWAREBUFFER && f.buffer) {
          AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(f.buffer);
          native_handle_t *nh = nullptr;
          {
            std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
            pending_ahbs_.erase(ahb);
            // Must also erase from map to keep the two structures consistent.
            auto map_it = ahb_to_native_handle_map_.find(ahb);
            if (map_it != ahb_to_native_handle_map_.end()) {
              nh = map_it->second;
              ahb_to_native_handle_map_.erase(map_it);
            }
          }
          if (nh) {
            // Originally enqueued as kSSFrameNativeHandle: return original handle.
            // Release the CLONE'd AHB internally (caller never sees it).
            fr.type = kSSFrameNativeHandle;
            fr.buffer = nh;
            AHardwareBuffer_release(ahb);
          } else {
            // Originally enqueued as kSSFrameAHardwareBuffer: transfer ownership.
            fr.type = kSSFrameAHardwareBuffer;
            fr.buffer = ahb;
          }
        } else if (f.type == QAIOR_SS_FRAME_GRAPHICBUFFER) {
          fr.type = kSSFrameGraphicBuffer;
          fr.buffer = f.buffer;
        } else {
          fr.type = kSSFrameNativeHandle;
          fr.other_handle = f.otherHandle;
          if (f.type == QAIOR_SS_FRAME_PARCELFD && f.parcelFd >= 0) {
            close(f.parcelFd);
          }
        }
        delete_result->deleted_frames.push_back(std::move(fr));
      } else {
        // No output requested — release resources internally.
        if (f.type == QAIOR_SS_FRAME_PARCELFD && f.parcelFd >= 0) {
          close(f.parcelFd);
        } else if (f.type == QAIOR_SS_FRAME_AHARDWAREBUFFER && f.buffer) {
          AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(f.buffer);
          bool found = false;
          {
            std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
            auto it = pending_ahbs_.find(ahb);
            if (it != pending_ahbs_.end()) {
              pending_ahbs_.erase(it);
              ahb_to_native_handle_map_.erase(ahb);  // keep structures consistent
              found = true;
            }
          }
          if (found)
            AHardwareBuffer_release(ahb);
        }
      }
    }
    fn_free(out_frames);
  }

  DLOGD("DeleteByConfig succeeded (deleted %d frames)", out_count);
  return 0;
}

int SmartSelectionAdapter::DoDeleteAll(const sdm::GenericPayload &input,
                                       sdm::GenericPayload *output) {
  DLOGD("DoDeleteAll");

  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  decltype(fn_deleteAll_) fn_delete = nullptr;
  decltype(fn_freeFrames_) fn_free = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }
    pipeline = pipeline_;
    context = context_;
    fn_delete = fn_deleteAll_;
    fn_free = fn_freeFrames_;
  }

  QaiorSS_FrameHandle_t *out_frames = nullptr;
  int32_t out_count = 0;

  QaiorSS_Status_t status = fn_delete(pipeline, context, &out_frames, &out_count);
  if (status != QAIOR_SS_STATUS_OK) {
    DLOGE("DoDeleteAll: QaiorSS_deleteAll() failed: %d", status);
    return -EINVAL;
  }

  if (out_frames) {
    SmartSelectionDeleteResult *delete_result = nullptr;
    uint32_t payload_size = 0;
    bool has_output = (output && output->GetPayload(delete_result, &payload_size) == 0 &&
                       delete_result && payload_size == 1);

    for (int32_t i = 0; i < out_count; ++i) {
      const QaiorSS_FrameHandle_t &f = out_frames[i];
      if (has_output) {
        SmartSelectionFrame fr;
        fr.metadata_json = f.metadataJson ? f.metadataJson : "";
        if (f.type == QAIOR_SS_FRAME_PARCELFD && f.parcelFd >= 0) {
          fr.type = kSSFrameParcelFd;
          fr.parcel_fd = dup(f.parcelFd);
          close(f.parcelFd);
        } else if (f.type == QAIOR_SS_FRAME_AHARDWAREBUFFER && f.buffer) {
          AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(f.buffer);
          native_handle_t *nh = nullptr;
          {
            std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
            pending_ahbs_.erase(ahb);
            // Must also erase from map to keep the two structures consistent.
            auto map_it = ahb_to_native_handle_map_.find(ahb);
            if (map_it != ahb_to_native_handle_map_.end()) {
              nh = map_it->second;
              ahb_to_native_handle_map_.erase(map_it);
            }
          }
          if (nh) {
            // Originally enqueued as kSSFrameNativeHandle: return original handle.
            // Release the CLONE'd AHB internally (caller never sees it).
            fr.type = kSSFrameNativeHandle;
            fr.buffer = nh;
            AHardwareBuffer_release(ahb);
          } else {
            // Originally enqueued as kSSFrameAHardwareBuffer: transfer ownership.
            fr.type = kSSFrameAHardwareBuffer;
            fr.buffer = ahb;
          }
        } else if (f.type == QAIOR_SS_FRAME_GRAPHICBUFFER) {
          fr.type = kSSFrameGraphicBuffer;
          fr.buffer = f.buffer;
        } else {
          fr.type = kSSFrameNativeHandle;
          fr.other_handle = f.otherHandle;
          if (f.type == QAIOR_SS_FRAME_PARCELFD && f.parcelFd >= 0) {
            close(f.parcelFd);
          }
        }
        delete_result->deleted_frames.push_back(std::move(fr));
      } else {
        if (f.type == QAIOR_SS_FRAME_PARCELFD && f.parcelFd >= 0) {
          close(f.parcelFd);
        } else if (f.type == QAIOR_SS_FRAME_AHARDWAREBUFFER && f.buffer) {
          AHardwareBuffer *ahb = static_cast<AHardwareBuffer *>(f.buffer);
          bool found = false;
          {
            std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
            auto it = pending_ahbs_.find(ahb);
            if (it != pending_ahbs_.end()) {
              pending_ahbs_.erase(it);
              ahb_to_native_handle_map_.erase(ahb);  // keep structures consistent
              found = true;
            }
          }
          if (found)
            AHardwareBuffer_release(ahb);
        }
      }
    }
    fn_free(out_frames);
  }

  DLOGD("DeleteAll succeeded (deleted %d frames)", out_count);
  return 0;
}

int SmartSelectionAdapter::DoDeinit(const sdm::GenericPayload &input, sdm::GenericPayload *output) {
  DLOGD("DoDeinit");

  // P1 fix: take ownership of pipeline/context under lock (set members to null),
  // then call library functions without holding mutex_.
  // Setting pipeline_/context_ to null under lock prevents Deinit() from
  // double-freeing them if called concurrently.
  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  decltype(fn_deinit_) fn_deinit = nullptr;
  decltype(fn_destroyPipeline_) fn_destroy = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }
    // Transfer ownership — Deinit() will see null and skip double-free
    pipeline = pipeline_;
    context = context_;
    pipeline_ = nullptr;
    context_ = nullptr;
    fn_deinit = fn_deinit_;
    fn_destroy = fn_destroyPipeline_;
  }

  fn_deinit(pipeline, context);
  fn_destroy(pipeline);

  // Clear callback to prevent stale invocations after pipeline teardown.
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    client_callback_fn_ = nullptr;
    client_callback_cookie_ = nullptr;
  }

  // Release any pending AHardwareBuffers.
  // Safe with CLONE: AHardwareBuffer_release() frees only the cloned handle.
  {
    std::lock_guard<std::mutex> lock(pending_ahb_mutex_);
    for (AHardwareBuffer *ahb : pending_ahbs_) {
      DLOGW("DoDeinit: releasing pending AHB %p (cloned handle)", ahb);
      AHardwareBuffer_release(ahb);
    }
    pending_ahbs_.clear();
    ahb_to_native_handle_map_.clear();
  }

  // Note: lib_handle_ and snapmapper_ are intentionally NOT released here.
  // DoDeinit() only tears down the pipeline/context so the adapter can be
  // re-initialized via ProcessOps(kSSInit) without reloading the library.
  // Full cleanup (UnloadLibrary + snapmapper_.reset) happens in Deinit()
  // which is called from the destructor.
  DLOGD("SmartSelection pipeline deinitialized (C API)");
  return 0;
}

int SmartSelectionAdapter::DoWaitUntilIdle(const sdm::GenericPayload &input,
                                           sdm::GenericPayload *output) {
  DLOGD("DoWaitUntilIdle");

  QaiorSS_Pipeline_t *pipeline = nullptr;
  QaiorSS_Context_t *context = nullptr;
  int32_t timeout_ms = 5000;
  decltype(fn_waitUntilIdle_) fn_wait = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pipeline_ || !context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    SmartSelectionWaitInput *wait_input = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(wait_input, &size) == 0 && wait_input && size == 1) {
      timeout_ms = wait_input->timeout_ms;
      DLOGD("DoWaitUntilIdle: using timeout %d ms", timeout_ms);
    } else {
      DLOGD("DoWaitUntilIdle: using default timeout %d ms", timeout_ms);
    }
    pipeline = pipeline_;
    context = context_;
    fn_wait = fn_waitUntilIdle_;
  }

  QaiorSS_Status_t status = fn_wait(pipeline, context, timeout_ms);

  if (status == QAIOR_SS_STATUS_OK) {
    DLOGD("WaitUntilIdle succeeded (pipeline is idle)");
    return 0;
  } else if (status == QAIOR_SS_STATUS_ERROR_TIMEOUT) {
    DLOGW("WaitUntilIdle timed out after %d ms", timeout_ms);
    return -ETIMEDOUT;
  } else if (status == QAIOR_SS_STATUS_ERROR_INVALID_ARGUMENT) {
    DLOGE("WaitUntilIdle: invalid argument");
    return -EINVAL;
  } else {
    DLOGE("WaitUntilIdle failed with unexpected status: %d", status);
    return -EIO;
  }
}

int SmartSelectionAdapter::DoQuerySelectorStatus(const sdm::GenericPayload &input,
                                                 sdm::GenericPayload *output) {
  DLOGD("DoQuerySelectorStatus");

  QaiorSS_Context_t *context = nullptr;
  std::string app_name_copy;
  decltype(fn_querySelectorStatus_) fn_query = nullptr;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!context_) {
      DLOGE("Pipeline not initialized");
      return -EINVAL;
    }

    SmartSelectionQuerySelectorInput *query_input = nullptr;
    uint32_t size = 0;
    if (input.GetPayload(query_input, &size) != 0 || !query_input || size != 1) {
      DLOGE("DoQuerySelectorStatus: failed to get SmartSelectionQuerySelectorInput");
      return -EINVAL;
    }
    if (query_input->app_name.empty()) {
      DLOGE("DoQuerySelectorStatus: app_name is required");
      return -EINVAL;
    }
    app_name_copy = query_input->app_name;
    context = context_;
    fn_query = fn_querySelectorStatus_;
  }

  int count = fn_query(context, app_name_copy.c_str());
  DLOGD("DoQuerySelectorStatus: app=%s selected_count=%d", app_name_copy.c_str(), count);

  if (output) {
    SmartSelectionQuerySelectorOutput *query_output = nullptr;
    uint32_t size = 0;
    if (output->GetPayload(query_output, &size) == 0 && query_output && size == 1) {
      query_output->selected_count = count;
    }
  }

  return (count >= 0) ? 0 : -EINVAL;
}

//=============================================================================
// snap_handle → QaiorSS_FrameHandle_t Conversion
//
// The metadata JSON string is written into out_metadata (caller-owned storage).
// frame.metadataJson points into out_metadata.c_str(), so the caller MUST keep
// out_metadata alive until after QaiorSS_enqueue() returns.
//=============================================================================

QaiorSS_FrameHandle_t SmartSelectionAdapter::BuildFrameHandleFromSnapHandle(
    void *snap_handle, const std::string &app_name, std::string &out_metadata,
    const std::shared_ptr<vendor::qti::hardware::display::snapalloc::ISnapMapper> &snapmapper) {
  QaiorSS_FrameHandle_t frame{};
  frame.type = QAIOR_SS_FRAME_PARCELFD;
  frame.parcelFd = -1;

  if (!snap_handle || !snapmapper) {
    DLOGE("BuildFrameHandleFromSnapHandle: invalid snap_handle or snapmapper");
    return frame;
  }

  using namespace vendor::qti::hardware::display::snapalloc;
  using MetadataType = vendor_qti_hardware_display_common_MetadataType;

  SnapHandle *snap_hdl = reinterpret_cast<SnapHandle *>(snap_handle);

  // 1. Extract fd and dup it
  int fd = -1;
  snapmapper->GetMetadata(*snap_hdl, MetadataType::FD, &fd);
  if (fd < 0) {
    DLOGE("BuildFrameHandleFromSnapHandle: invalid fd");
    return frame;
  }
  frame.parcelFd = dup(fd);
  if (frame.parcelFd < 0) {
    DLOGE("BuildFrameHandleFromSnapHandle: dup() failed: %s", strerror(errno));
    return frame;
  }

  // 2. Extract buffer metadata.
  //    stride / aligned_height: memory layout dimensions (aligned to hardware requirements).
  //    width / height:          true image dimensions (unaligned, as captured).
  uint32_t stride = 0, aligned_height = 0;
  uint64_t width = 0, height = 0;
  int gralloc_format = 0;
  int64_t is_ubwc = 0;
  uint64_t buffer_id = 0;

  snapmapper->GetMetadata(*snap_hdl, MetadataType::STRIDE, &stride);
  snapmapper->GetMetadata(*snap_hdl, MetadataType::ALIGNED_HEIGHT_IN_PIXELS, &aligned_height);
  snapmapper->GetMetadata(*snap_hdl, MetadataType::WIDTH, &width);
  snapmapper->GetMetadata(*snap_hdl, MetadataType::HEIGHT, &height);
  snapmapper->GetMetadata(*snap_hdl, MetadataType::PIXEL_FORMAT_ALLOCATED, &gralloc_format);
  snapmapper->GetMetadata(*snap_hdl, MetadataType::IS_UBWC, &is_ubwc);
  snapmapper->GetMetadata(*snap_hdl, MetadataType::BUFFER_ID, &buffer_id);

  // Warn if critical layout fields are zero (GetMetadata may have failed silently).
  if (stride == 0 || width == 0 || height == 0) {
    DLOGW(
        "BuildFrameHandleFromSnapHandle: critical metadata may be invalid "
        "(stride=%u, width=%llu, height=%llu)",
        stride, static_cast<unsigned long long>(width), static_cast<unsigned long long>(height));
  }

  // 3. Build metadata JSON into caller-provided out_metadata string.
  //    frame.metadataJson will point into out_metadata.c_str().
  //
  //    JSON field semantics (fixed from original):
  //      "width"         — true image width  (MetadataType::WIDTH, unaligned)
  //      "height"        — true image height (MetadataType::HEIGHT, unaligned)
  //      "stride"        — aligned row width  (MetadataType::STRIDE)
  //      "alignedHeight" — aligned height     (MetadataType::ALIGNED_HEIGHT_IN_PIXELS)
  //
  //    Use reserve + append to avoid ostringstream heap allocation per enqueue call.
  out_metadata.clear();
  out_metadata.reserve(256);
  out_metadata += "{\"appName\":\"";
  out_metadata += app_name;
  out_metadata += "\",\"width\":";
  out_metadata += std::to_string(width);
  out_metadata += ",\"height\":";
  out_metadata += std::to_string(height);
  out_metadata += ",\"stride\":";
  out_metadata += std::to_string(stride);
  out_metadata += ",\"alignedHeight\":";
  out_metadata += std::to_string(aligned_height);
  out_metadata += ",\"format\":\"";
  if (gralloc_format == kHalPixelFormatRGBA8888) {
    out_metadata += (is_ubwc ? "RGBA8888_UBWC" : "RGBA8888");
  } else if (gralloc_format == kHalPixelFormatRGBA1010102) {
    out_metadata += (is_ubwc ? "RGBA1010102_UBWC" : "RGBA1010102");
  } else {
    out_metadata += "RGBA8888";  // default
  }
  out_metadata += "\",\"bufferId\":";
  out_metadata += std::to_string(buffer_id);
  out_metadata += ",\"timestamp\":";
  auto now = std::chrono::system_clock::now();
  auto ts_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  out_metadata += std::to_string(ts_ms);
  out_metadata += "}";

  frame.metadataJson = out_metadata.c_str();

  DLOGV("BuildFrameHandleFromSnapHandle: fd=%d size=%llux%llu stride=%u app=%s", frame.parcelFd,
        static_cast<unsigned long long>(width), static_cast<unsigned long long>(height), stride,
        app_name.c_str());

  return frame;
}

//=============================================================================
// Configuration Loading
//=============================================================================

int SmartSelectionAdapter::LoadConfigurationJson(const std::string &config_path) {
  if (config_path.empty())
    return -EINVAL;

  std::ifstream config_file(config_path);
  if (!config_file.is_open()) {
    DLOGE("Failed to open configuration file: %s", config_path.c_str());
    return -ENOENT;
  }

  std::string config_json((std::istreambuf_iterator<char>(config_file)),
                          std::istreambuf_iterator<char>());
  config_file.close();

  if (config_json.empty()) {
    DLOGE("Configuration file is empty: %s", config_path.c_str());
    return -EINVAL;
  }

  current_config_json_ = std::move(config_json);
  DLOGD("Loaded configuration from %s (%zu bytes)", config_path.c_str(),
        current_config_json_.size());
  return 0;
}

}  // namespace imagealgo

// ---------------------------------------------------------------------------
// Plugin entry points — called by PluginAdapterFactory in image_algo_adapter_factory.cpp.
//
// CreateImageAlgoAdapter():
//   Returns void* (matching factory's CreateFn = void*(*)()) to eliminate the
//   function-pointer type mismatch UB that arises when calling a SmartSelectionIntf*(*)()
//   function through a void*(*)() pointer.
//   The returned void* holds the SmartSelectionIntf* value (pointer already adjusted
//   for single inheritance), so PluginAdapterFactory::CreateAdapter<SmartSelectionIntf>()
//   can safely static_pointer_cast it back to SmartSelectionIntf*.
//
// DestroyImageAlgoAdapter():
//   Accepts void* (matching factory's DestroyFn = void(*)(void*)) for the same reason.
//   Casts back to SmartSelectionIntf* — safe because the value was produced by
//   static_cast<SmartSelectionIntf*>(new SmartSelectionAdapter()) above.
//   Destructor calls Deinit() — safe even if Deinit() was already called.
// ---------------------------------------------------------------------------
extern "C" {
void *CreateImageAlgoAdapter() {
  return static_cast<imagealgo::SmartSelectionIntf *>(new imagealgo::SmartSelectionAdapter());
}

void DestroyImageAlgoAdapter(void *adapter) {
  delete static_cast<imagealgo::SmartSelectionIntf *>(adapter);
}
}
