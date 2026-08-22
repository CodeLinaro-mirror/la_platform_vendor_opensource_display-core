/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __IMAGE_ALGO_SMARTSELECTION_INTF_H__
#define __IMAGE_ALGO_SMARTSELECTION_INTF_H__

#include <string>
#include <vector>
#include <memory>

// GenericIntf<ParamEnum, OpsEnum, PayloadType> — same base template used by ClstcInterface.
#include <private/generic_intf.h>
// sdm::GenericPayload — same payload type used by ClstcInterface.
#include <private/generic_payload.h>

namespace imagealgo {

// ---------------------------------------------------------------------------
// SmartSelectionOp — operation IDs for ProcessOps().
// Typed enum (not uint32_t) — prevents passing SuperResolution ops to this adapter.
//
// All operations return an int:
//   0          — success
//   -EINVAL    — invalid argument or pipeline not initialized
//   -ENOMEM    — allocation failure (kSSInit only)
//   -ETIMEDOUT — timeout expired (kSSWaitUntilIdle only)
//   -ENOTSUP   — operation not supported
//
// The primary result delivery channel is the emit callback registered via
// SmartSelectionInitInput::callback in kSSInit. Selected and rejected frames
// are delivered asynchronously through SmartSelectionEmitResult.
//
// output (GenericPayload*) is unused (pass nullptr) for all current operations.
// ---------------------------------------------------------------------------
enum SmartSelectionOp {
  // kSSInit
  //   input:  SmartSelectionInitInput*
  //             .config_json  — optional JSON config string; if empty, loads from
  //                             /vendor/etc/smartselection_config.json in adapter
  //             .callback     — SmartSelectionEmitCallbackFn invoked when frames are emitted
  //             .cookie       — opaque pointer passed unchanged to callback
  //   output: nullptr
  //   return: 0 on success, -EINVAL if pipeline already initialized or config missing,
  //           -ENOMEM if pipeline allocation fails
  kSSInit = 0,

  // kSSReconfigure
  //   input:  std::string* — new CaptureConfig JSON; replaces the active config
  //   output: nullptr
  //   return: 0 on success, -EINVAL on failure
  kSSReconfigure,

  // kSSEnqueue
  //   input:  SmartSelectionEnqueueInput*
  //             .input_type    — selects buffer representation (default: kSSFrameNativeHandle)
  //             .buffer        — native_handle_t*, AHardwareBuffer*, or GraphicBuffer*
  //             .parcel_fd     — file descriptor (kSSFrameParcelFd only)
  //             .metadata_json — JSON string passed directly to QaiorSS_enqueue() as metadataJson
  //                              Required fields: appName, width, height, stride,
  //                              alignedHeight, format (see SmartSelectionEnqueueInput)
  //   output: nullptr
  //   return: 0 on success, -EINVAL on failure
  //   note:   results are delivered asynchronously via the emit callback
  kSSEnqueue,

  // kSSFlushByConfig
  //   input:  std::string* — JSON filter identifying which app's frames to flush
  //   output: nullptr
  //   return: 0 on success, -EINVAL on failure
  //   note:   triggers emit callback with selected/rejected frames matching the filter
  kSSFlushByConfig,

  // kSSFlushAll
  //   input:  nullptr (no input payload needed)
  //   output: nullptr
  //   return: 0 on success, -EINVAL on failure
  //   note:   triggers emit callback with all buffered frames across all queues
  kSSFlushAll,

  // kSSDeleteByConfig
  //   input:  std::string* — JSON filter identifying which app's frames to delete
  //   output: SmartSelectionDeleteResult* — deleted frames returned to caller
  //   return: 0 on success, -EINVAL on failure
  //   note:   deleted frames are returned via output; no emit callback fired.
  //           For kSSFrameParcelFd frames: caller MUST close() parcel_fd.
  //           For kSSFrameAHardwareBuffer frames: caller MUST call AHardwareBuffer_release().
  //           Pass output=nullptr to discard deleted frames (adapter closes fds internally).
  kSSDeleteByConfig,

  // kSSDeleteAll
  //   input:  nullptr (no input payload needed)
  //   output: SmartSelectionDeleteResult* — deleted frames returned to caller
  //   return: 0 on success, -EINVAL on failure
  //   note:   deleted frames are returned via output; no emit callback fired.
  //           For kSSFrameParcelFd frames: caller MUST close() parcel_fd.
  //           For kSSFrameAHardwareBuffer frames: caller MUST call AHardwareBuffer_release().
  //           Pass output=nullptr to discard deleted frames (adapter closes fds internally).
  kSSDeleteAll,

  // kSSDeinit
  //   input:  nullptr (no input payload needed)
  //   output: nullptr
  //   return: 0 on success, -EINVAL if pipeline not initialized
  //   note:   destroys pipeline/context; adapter remains loaded for re-initialization
  //           via a subsequent kSSInit call without reloading the library
  kSSDeinit,

  // kSSWaitUntilIdle
  //   input:  SmartSelectionWaitInput*
  //             .timeout_ms — maximum wait time in milliseconds (default: 5000)
  //   output: nullptr
  //   return: 0 if pipeline became idle within timeout
  //           -ETIMEDOUT if timeout expired before pipeline became idle
  //           -EINVAL if pipeline not initialized or timeout_ms is negative
  //   note:   intended for test/debug use; call before kSSFlushAll to ensure all
  //           enqueued frames have been processed
  kSSWaitUntilIdle,

  // kSSFlushSelected
  //   input:  SmartSelectionFlushSelectedInput*
  //             .app_name — null-terminated UTF-8 app name (must not be empty)
  //             .user_id  — optional user-id filter; empty string to ignore
  //   output: nullptr
  //   return: 0 on success,
  //           -EINVAL if pipeline not initialized, app_name is empty, or
  //                   app/user_id not found
  //   note:   emits only screenshots already selected for the given app via the
  //           emit callback; clears the selector's internal queue for that app.
  //           Pending (not-yet-selected) tasks are left untouched.
  kSSFlushSelected,

  // kSSQuerySelectorStatus
  //   input:  SmartSelectionQuerySelectorInput*
  //             .app_name — null-terminated UTF-8 app name to query
  //   output: SmartSelectionQuerySelectorOutput*
  //             .selected_count — number of selected screenshots (>=0), or -1 on error
  //   return: 0 on success, -EINVAL if pipeline not initialized or app_name is empty
  //   note:   does not modify any internal state; safe to call at any time after kSSInit
  kSSQuerySelectorStatus,

  kSSOpMax = 0xFF,  ///< Reserved; used for bounds checking
};

// ---------------------------------------------------------------------------
// SmartSelectionIntf — the SmartSelection adapter interface.
//
// Follows the CLSTC pattern: each independent adapter has its own typed interface.
// Structurally identical to ClstcInterface but with SmartSelection-specific enums:
//
//   ClstcInterface = GenericIntf<uint32_t, uint32_t,        GenericPayload>
//   SmartSelectionIntf = GenericIntf<int,  SmartSelectionOp, GenericPayload>
//
// Methods (from GenericIntf):
//   Init()                                    — load library, resolve symbols, load config
//   Deinit()                                  — unload library, release resources
//   SetParameter(int param, in)               — reserved for future use; currently -ENOTSUP
//   GetParameter(int param, out)              — reserved for future use; currently -ENOTSUP
//   ProcessOps(SmartSelectionOp op, in, out)  — dispatch an operation (primary call path)
//
// Do NOT delete SmartSelectionIntf* directly; use shared_ptr (returned by CreateAdapter).
// ---------------------------------------------------------------------------
typedef sdm::GenericIntf<int, SmartSelectionOp, sdm::GenericPayload> SmartSelectionIntf;

// ---------------------------------------------------------------------------
// Payload Structures
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SmartSelectionFrameInputType — selects how the adapter interprets the
// buffer/parcel_fd fields of SmartSelectionEnqueueInput.
//
// Mirrors QaiorSS_FrameType_t (QaiorSmartSelectionApi.h) but is defined here
// in the public interface so clients do not need to include internal headers.
// ---------------------------------------------------------------------------
enum SmartSelectionFrameInputType {
  /// buffer = native_handle_t* from CWB/BufferAllocator.
  /// The adapter converts it to AHardwareBuffer via
  /// AHardwareBuffer_createFromHandle(CLONE) internally.
  /// This is the default (backward-compatible) mode.
  kSSFrameNativeHandle = 0,

  /// buffer = AHardwareBuffer* (NDK).
  /// The adapter calls AHardwareBuffer_acquire() and passes it directly to
  /// QaiorSS_enqueue() as QAIOR_SS_FRAME_AHARDWAREBUFFER.
  /// AHardwareBuffer_release() is called in the emit callback.
  kSSFrameAHardwareBuffer = 1,

  /// buffer = android::GraphicBuffer* (platform).
  /// Passed directly to QaiorSS_enqueue() as QAIOR_SS_FRAME_GRAPHICBUFFER.
  /// Caller must keep the GraphicBuffer alive until the emit callback fires.
  kSSFrameGraphicBuffer = 2,

  /// parcel_fd = file descriptor.
  /// Passed directly to QaiorSS_enqueue() as QAIOR_SS_FRAME_PARCELFD.
  /// The library dups the fd internally; caller retains ownership of the
  /// original fd.
  kSSFrameParcelFd = 3,
};

// ---------------------------------------------------------------------------
// SmartSelectionFrame — unified per-frame representation.
//
// Mirrors QaiorSS_FrameHandle_t (QaiorSmartSelectionApi.h) but uses C++ types
// and is used consistently across Enqueue input, EmitResult output, and
// DeleteByConfig/DeleteAll output.
//
// Ownership rules (caller of ProcessOps is responsible):
//   kSSFrameParcelFd:
//     - parcel_fd is a dup'd fd; caller MUST close() it when done.
//   kSSFrameAHardwareBuffer:
//     - buffer is an AHardwareBuffer* with one reference held for the caller.
//     - Caller MUST call AHardwareBuffer_release(buffer) when done.
//   kSSFrameGraphicBuffer:
//     - buffer is an android::GraphicBuffer* (no ref taken by adapter).
//     - Caller manages GraphicBuffer lifetime.
//   kSSFrameNativeHandle:
//     - buffer is the original native_handle_t* passed at enqueue time.
//     - The adapter retains ownership; caller MUST NOT close/delete it.
// ---------------------------------------------------------------------------
/// Per-frame result delivered via SmartSelectionEmitResult.
///
/// The adapter guarantees that the buffer type in the emit result matches
/// the input type used at enqueue time:
///
///   Enqueue input_type          → Emit result type / buffer
///   ─────────────────────────────────────────────────────────────────────
///   kSSFrameNativeHandle        → kSSFrameNativeHandle, buffer = native_handle_t*
///                                 (the exact pointer passed at enqueue; adapter
///                                  releases the internal AHardwareBuffer clone)
///   kSSFrameAHardwareBuffer     → kSSFrameAHardwareBuffer, buffer = AHardwareBuffer*
///                                 (caller MUST call AHardwareBuffer_release(buffer))
///   kSSFrameGraphicBuffer       → kSSFrameGraphicBuffer, buffer = GraphicBuffer*
///   kSSFrameParcelFd            → kSSFrameParcelFd, parcel_fd = dup'd fd
///                                 (caller MUST close(parcel_fd))
///
/// cookie: the per-frame OEM cookie set in SmartSelectionEnqueueInput::cookie at
///         enqueue time. Passed through unchanged from QaiorSS_FrameHandle_t::cookie.
///         Useful for correlating emit results back to specific enqueued frames.
///
/// metadata_json format (UTF-8 JSON string, deep-copied from the library):
/// {
///   "appName":      "<string>",   // application identifier from SmartSelectionEnqueueInput
///   "width":        <uint32>,     // true image width  (buffer_config.width)
///   "height":       <uint32>,     // true image height (buffer_config.height)
///   "stride":       <uint32>,     // aligned row width in pixels (aligned_width)
///   "alignedHeight":<uint32>,     // aligned height in pixels (aligned_height)
///   "format":       "<string>"    // pixel format: "RGBA8888" | "RGBA8888_UBWC" |
///                                 //               "RGBA1010102" | "RGBA1010102_UBWC" |
///                                 //               "RGB888"
/// }
struct SmartSelectionFrame {
  SmartSelectionFrameInputType type = kSSFrameNativeHandle;
  void *buffer = nullptr;        ///< native_handle_t*, AHardwareBuffer*, or GraphicBuffer*
  int parcel_fd = -1;            ///< valid when type == kSSFrameParcelFd
  void *other_handle = nullptr;  ///< OEM-defined handle
  void *cookie = nullptr;        ///< per-frame OEM cookie (passed through from enqueue)
  std::string metadata_json;     ///< deep copy of per-frame JSON metadata
};

struct SmartSelectionEmitResult {
  std::vector<SmartSelectionFrame> selected_frames;
  std::vector<SmartSelectionFrame> rejected_frames;
};

/// Output for ProcessOps(kSSDeleteByConfig) and ProcessOps(kSSDeleteAll).
///
/// Ownership: same rules as SmartSelectionFrame above.
/// For kSSFrameParcelFd: caller MUST close() parcel_fd.
/// For kSSFrameAHardwareBuffer: caller MUST call AHardwareBuffer_release(buffer).
struct SmartSelectionDeleteResult {
  std::vector<SmartSelectionFrame> deleted_frames;
};

typedef void (*SmartSelectionEmitCallbackFn)(const SmartSelectionEmitResult *result, void *cookie);

/// Input for ProcessOps(kSSInit)
struct SmartSelectionInitInput {
  std::string config_json;  ///< optional: overrides file config
  SmartSelectionEmitCallbackFn callback = nullptr;
  void *cookie = nullptr;
};

/// Input for ProcessOps(kSSEnqueue)
///
/// Directly mirrors QaiorSS_FrameHandle_t (QaiorSmartSelectionApi.h):
/// the caller provides metadata_json as a pre-built JSON string, which the adapter
/// passes as-is to QaiorSS_enqueue() as metadataJson.
///
/// Required JSON fields in metadata_json:
///   "appName"       — application identifier (must match config appName)
///   "width"         — true image width  (buffer_config.width)
///   "height"        — true image height (buffer_config.height)
///   "stride"        — aligned row width in pixels (alloc_buffer_info.aligned_width)
///   "alignedHeight" — aligned height in pixels   (alloc_buffer_info.aligned_height)
///   "format"        — pixel format: "RGBA8888" | "RGBA8888_UBWC" |
///                     "RGBA1010102" | "RGBA1010102_UBWC" | "RGB888"
///
/// Example:
///   enq_input->metadata_json =
///       "{\"appName\":\"com.test.cwb.imagealgo\","
///       "\"width\":1080,\"height\":2400,"
///       "\"stride\":1088,\"alignedHeight\":2432,"
///       "\"format\":\"RGBA8888_UBWC\"}";
///
/// For kSSFrameNativeHandle: the adapter also uses width/height/stride from
/// metadata_json (or gralloc metadata via ISnapMapper) to create the internal
/// AHardwareBuffer_Desc for AHardwareBuffer_createFromHandle(CLONE).
///
/// NOTE: Both cwb-test and libImageAlgoAdapter_smartselection.so MUST be
/// compiled with the same version of this struct (GenericPayload sizeof check).
struct SmartSelectionEnqueueInput {
  /// Frame input type — controls how buffer/parcel_fd is interpreted.
  SmartSelectionFrameInputType input_type = kSSFrameNativeHandle;

  /// Buffer pointer — interpretation depends on input_type:
  ///   kSSFrameNativeHandle:    native_handle_t* (converted to AHB by adapter)
  ///   kSSFrameAHardwareBuffer: AHardwareBuffer*
  ///   kSSFrameGraphicBuffer:   android::GraphicBuffer*
  ///   kSSFrameParcelFd:        unused (set parcel_fd instead)
  void *buffer = nullptr;

  /// File descriptor — used only when input_type == kSSFrameParcelFd.
  /// The library dups it internally; caller retains ownership of the original fd.
  int parcel_fd = -1;

  /// JSON metadata string passed directly to QaiorSS_enqueue() as metadataJson.
  /// Must contain: appName, width, height, stride, alignedHeight, format.
  std::string metadata_json;

  /// OEM-defined per-frame cookie stored in QaiorSS_FrameHandle_t::cookie.
  /// Passed through unchanged to SmartSelectionFrame::cookie in the emit callback.
  /// Use this to correlate emit results back to specific enqueued frames
  /// (e.g. a pointer to a filename string or a frame sequence number).
  /// The adapter does NOT dereference or manage the lifetime of this pointer.
  void *cookie = nullptr;
};

/// Input for ProcessOps(kSSWaitUntilIdle)
struct SmartSelectionWaitInput {
  int32_t timeout_ms = 5000;
};

/// Input for ProcessOps(kSSFlushSelected)
struct SmartSelectionFlushSelectedInput {
  std::string app_name;  ///< app name to flush selected frames for (required, non-empty)
  std::string user_id;   ///< optional user-id filter; empty string to ignore
};

/// Input for ProcessOps(kSSQuerySelectorStatus)
struct SmartSelectionQuerySelectorInput {
  std::string app_name;  ///< app name to query (required, non-empty)
};

/// Output for ProcessOps(kSSQuerySelectorStatus)
struct SmartSelectionQuerySelectorOutput {
  int selected_count = -1;  ///< number of selected screenshots (>=0), or -1 on error
};

}  // namespace imagealgo

// ---------------------------------------------------------------------------
// AdapterTraits<SmartSelectionIntf> — binds this interface to its plugin .so.
//
// Forward-declares AdapterTraits<T> so this header can be included standalone
// (without image_algo_interface.h). The primary template is declared in
// image_algo_interface.h; the two declarations are compatible.
//
// Used by ImageAlgoAdapterFactory::CreateAdapter<SmartSelectionIntf>() to
// locate the plugin .so without the caller needing to know the path.
// ---------------------------------------------------------------------------
namespace imagealgo {

template <typename T>
struct AdapterTraits;  // forward declaration (primary template in image_algo_interface.h)

template <>
struct AdapterTraits<SmartSelectionIntf> {
  static constexpr const char *kLibPath = "/vendor/lib64/libImageAlgoAdapter_smartselection.so";
};

}  // namespace imagealgo

#endif  // __IMAGE_ALGO_SMARTSELECTION_INTF_H__
