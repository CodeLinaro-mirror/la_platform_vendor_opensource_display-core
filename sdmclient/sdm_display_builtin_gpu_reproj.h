/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __SDM_DISPLAY_BUILTIN_GPU_REPROJ_H__
#define __SDM_DISPLAY_BUILTIN_GPU_REPROJ_H__

#include <stdint.h>

#include "core/layer_stack.h"
#include "sdm_display.h"
#include "sdm_display_builtin.h"
#include "sdm_layers.h"
#include "utils/fence.h"

// Forward declaration — avoids pulling Vulkan/GpuReproj headers into sdmclient.
// The full definition is included only in sdm_display_builtin_gpu_reproj.cpp.
class GpuReproj;

namespace sdm {

// ---- Coordination buffer layout (must match DCP firmware) ----
// 64-byte DMA-BUF shared between SDM, GPU shader, and DCP firmware.
// DCP writes active_slot (offset 0); GPU shader reads it to determine
// which output buffer to render into.  GMU writes error_type on hang.
struct ReprojCoordBuffer {
  uint32_t active_slot;   // 0 or 1 — DCP writes; GPU shader reads
  uint32_t error_type;    // 0=none, 1=GPU_HANG (written by GMU)
  uint32_t done_counter;  // incremented by GPU each frame
  uint8_t pad[52];        // total = 64 bytes
};
static_assert(sizeof(ReprojCoordBuffer) == 64, "ReprojCoordBuffer must be 64 bytes");

// ---- Pose data cached from the most recent SetPoseConfig() call ----
// Layout mirrors the head of the pose DMA-BUF: pos[3] + orient[4].
struct ReprojPoseData {
  float pos[3] = {};
  float orient[4] = {0.f, 0.f, 0.f, 1.f};  // identity quaternion
};

// ============================================================
// SDMDisplayBuiltInGpuReproj
//
// Derived class of SDMDisplayBuiltIn that adds GPU Late Stage
// Reprojection (seraph/GPU LSR path) support.
//
// Instantiated by SDMDisplayBuiltIn::Create() when
// IsGpuLsrVariant() returns true (SoC IDs 736/737).
//
// All reproj state and logic live directly in this class as
// private members — no separate controller class is needed.
// SDMDisplayBuiltIn is completely unaware of GPU reproj.
// ============================================================
class SDMDisplayBuiltInGpuReproj : public SDMDisplayBuiltIn {
 public:
  SDMDisplayBuiltInGpuReproj(CoreInterface *core_intf, BufferAllocator *buffer_allocator,
                             SDMCompositorCallbacks *callbacks,
                             SDMDisplayEventHandler *event_handler, Display id, int32_t sdm_id);

  // Override only the methods that need GPU reproj hooks.
  DisplayError PreValidateDisplay(bool *exit_validate) override;
  DisplayError CommitLayerStack() override;
  DisplayError CommitOrPrepare(bool validate_only, shared_ptr<Fence> *out_retire_fence,
                               uint32_t *out_num_types, uint32_t *out_num_requests,
                               bool *needs_commit) override;
  DisplayError PostInit() override;
  DisplayError Deinit(bool deinit_layer_builder = true) override;
  DisplayError SetPoseConfig(void *buffer) override;
  DisplayError SetDisplayDeviceConfig(SDMDisplayDeviceConfig sdm_display_device_config) override;

 private:
  // -----------------------------------------------------------------------
  // GPU reproj engine lifecycle
  // -----------------------------------------------------------------------
  bool InitGpuReproj();
  void DeinitGpuReproj();

  // -----------------------------------------------------------------------
  // Buffer management
  // -----------------------------------------------------------------------
  bool AllocateReprojBuffers();
  bool AllocateCoordBuffer();
  void RegisterCoordBufferWithDpu();
  // Frees reproj output buffers and coord buffer; resets allocation flags.
  // Called by both DeinitGpuReproj() and OnReprojInactive().
  void FreeReprojBuffers();

  // -----------------------------------------------------------------------
  // Per-frame operations
  // -----------------------------------------------------------------------

  // Upload static per-session configuration (projection matrices, LUTs, etc.).
  // Pass nullptr for a passthrough (identity) configuration.
  void SetStaticConfig(const SDMDisplayDeviceConfig *cfg);

  // Pass the render-time pose DMA-BUF fd directly to the GPU reproj library.
  void SetPoseBuffer(int fd);

  // Per-frame: append the reproj output buffer as a kCompositionSDE layer so
  // the resource manager assigns a DPU pipe to it before Prepare() runs.
  void AppendReprojOutputLayer(LayerStack *layer_stack);

  // Per-frame: warp app layers into the reproj output buffer.
  void Apply(SDMLayerStack *layer_stack);

  // Returns true if the layer stack contains at least one PROJECTION layer.
  bool IsReprojNeeded(SDMLayerStack *layer_stack) const;

  // Called when IsReprojNeeded() returns false after being active.
  // Frees reproj buffers so they are not held after the XR app exits.
  void OnReprojInactive();

  // -----------------------------------------------------------------------
  // State queries
  // -----------------------------------------------------------------------
  bool IsGpuReprojEnabled() const { return gpu_reproj_ != nullptr; }

  // Returns true exactly once per init sequence: after the first DPU init
  // commit (reproj_dpu_init_count_ == 1) and before the second (== 2).
  bool NeedsBatchSecondCommit() const { return reproj_dpu_init_count_ == 1; }

  // Returns true once both DPU init commits are done (>= kReprojSlotCount).
  bool IsReprojInitDone() const { return reproj_dpu_init_count_ >= kReprojSlotCount; }

  // -----------------------------------------------------------------------
  // GPU reproj library (loaded via dlopen)
  // -----------------------------------------------------------------------
  GpuReproj *gpu_reproj_ = nullptr;
  void *gpu_reproj_lib_ = nullptr;

  // -----------------------------------------------------------------------
  // Output buffer pool: 2 slots × 2 eyes × 3 R/G/B fields = 12 buffers
  // -----------------------------------------------------------------------
  static constexpr int kReprojSlotCount = 2;
  static constexpr int kReprojEyeCount = 2;      // left eye + right eye
  static constexpr int kReprojFieldsPerEye = 3;  // R, G, B FSC color fields
  static constexpr int kReprojBufsPerSlot = kReprojEyeCount * kReprojFieldsPerEye;  // = 6

  BufferInfo reproj_out_[kReprojSlotCount][kReprojBufsPerSlot] = {};
  int reproj_slot_ = 0;
  bool reproj_bufs_allocated_ = false;

  // -----------------------------------------------------------------------
  // DCP↔GPU coordination buffer (64-byte DMA-BUF)
  // -----------------------------------------------------------------------
  BufferInfo reproj_coord_buf_info_ = {};
  int reproj_coord_buf_fd_ = -1;

  // -----------------------------------------------------------------------
  // Pose and device config
  // -----------------------------------------------------------------------
  ReprojPoseData reproj_pose_ = {};
  bool reproj_pose_set_ = false;

  SDMDisplayDeviceConfig reproj_device_cfg_ = {};
  bool reproj_device_cfg_set_ = false;

  // -----------------------------------------------------------------------
  // Fences, counters, and persistent output layers
  // -----------------------------------------------------------------------
  int reproj_out_fence_fd_ = -1;
  uint64_t reproj_frame_count_ = 0;
  int reproj_dpu_init_count_ = 0;  // 0→1→2; reset on FreeReprojBuffers()

  // Persistent reproj output layers — layer_stack_.layers holds raw pointers,
  // so these must outlive each frame's layer stack.
  // One Layer per eye; each carries kReprojFieldsPerEye planes (R/G/B FSC).
  Layer reproj_out_layers_[kReprojEyeCount] = {};
};

}  // namespace sdm

#endif  // __SDM_DISPLAY_BUILTIN_GPU_REPROJ_H__
