/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __SDM_DISPLAY_BUILTIN_GPU_REPROJ_H__
#define __SDM_DISPLAY_BUILTIN_GPU_REPROJ_H__

#include <stdint.h>
#include <string>
#include <vector>

#include "core/layer_stack.h"
#include "sdm_display.h"
#include "sdm_display_builtin.h"
#include "sdm_layers.h"
#include "utils/fence.h"

// Forward declarations — avoids pulling Vulkan/GpuReproj headers into sdmclient.
// The full definitions are included only in sdm_display_builtin_gpu_reproj.cpp.
class GpuReproj;
struct GpuReprojStaticConfig;
struct GpuReprojLayerParams;

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
  // LDC calibration data structures — defined locally to mirror DisplaySW_DS.h
  // from LayerConversion without including proprietary headers.
  // Memory layout must match the corresponding types in DisplaySW_DS.h so that
  // the void* cast in ResolveLDCCACGrid_C works correctly.
  // -----------------------------------------------------------------------
  static constexpr int kLdcDistRes = 257;  // radial distortion LUT columns (CURVE_ESTIMATION_RES)

  // Sparse grid dimensions — must match SPARSE_GRID_WIDTH/HEIGHT in LayerConversion.h
  static constexpr int kLdcSparseGridWidth = 35;
  static constexpr int kLdcSparseGridHeight = 27;
  static constexpr int kLdcSparseGridCells = kLdcSparseGridWidth * kLdcSparseGridHeight;  // 945
  static constexpr int kLdcNumViews = 2;
  static constexpr int kLdcNumColorFields = 3;
  static constexpr int kLdcNumPlanes = kLdcNumViews * kLdcNumColorFields;  // 6

  // Mirrors Metadata from DisplaySW_DS.h
  struct LdcMetadata {
    uint32_t version = 0, distortion = 0;
    uint32_t eye_width = 0, eye_height = 0;
    uint32_t num_rows = 0, num_cols = 0;
    uint32_t center_x = 0, center_y = 0;
    float *data = nullptr;
    uint32_t data_width = 0, data_height = 0;
  };

  // Mirrors FloatLensOut from DisplaySW_DS.h — used on Bonsai (GPU path).
  // dist_grid: flat float buffer, plane p at offset p * (grid_w * grid_h * 2).
  struct LdcFloatLensOut {
    std::vector<float> ref_grid;
    std::vector<float> dist_grid;
    uint32_t grid_w = 0;
    uint32_t grid_h = 0;
  };

  // Mirrors LensOut from DisplaySW_DS.h — NOT used on Bonsai (GPU-only).
  // Kept here solely to preserve the memory layout of LdcGridResult so that
  // the void* cast in ResolveLDCCACGrid_C matches LCGridResult exactly.
  // Uses a fixed-size array (not std::vector) to match LensOut::sparse_grid[6][945].
  struct LdcLensOut {
    uint32_t sparse_grid[kLdcNumPlanes][kLdcSparseGridCells] = {};  // [6][945]
    uint32_t sparse_grid_size = 0;
    uint32_t grid_w = 0;
    uint32_t grid_h = 0;
  };

  // Mirrors BBOxOut from DisplaySW_DS.h — used on Bonsai (GPU path).
  struct LdcBBoxOut {
    struct {
      float x, y;
    } point[2] = {};
    uint32_t distor_res = 0;
    float radial_distortion_grid[kLdcNumPlanes][kLdcDistRes] = {};
    uint32_t errortol = 0;
    float reprojection_r_max = 0.0f;
    float reproj_tol_rgb_left = 0.0f;
    float reproj_tol_rgb_right = 0.0f;
  };

  // Mirrors LCExecutionPath from DisplaySW_DS.h
  enum class LdcExecutionPath { kGpu = 0, kHwPath = 1 };

  // Mirrors LCGridResult from DisplaySW_DS.h.
  // Field order and types must exactly match LCGridResult so that the void*
  // cast in ResolveLDCCACGrid_C is valid across the open-source/proprietary
  // boundary.  On Bonsai only float_grid and bbox_params are populated;
  // quantized_grid is present for layout compatibility only.
  struct LdcGridResult {
    LdcExecutionPath execution_path = LdcExecutionPath::kGpu;
    LdcFloatLensOut float_grid;  // populated by ResolveLDCCACGrid (GPU path)
    LdcLensOut quantized_grid;   // layout placeholder — not used on Bonsai
    LdcBBoxOut bbox_params;      // populated by ResolveLDCCACGrid (both paths)
  };

  // -----------------------------------------------------------------------
  // Local mirrors of LayerConversion conversion input/output types.
  // Memory layout must exactly match the corresponding types in LayerConversion.h
  // so that void* casts in the extern "C" wrappers work correctly.
  // Used by ConvertLayerParams() to call LC conversion functions via dlsym
  // without including proprietary LayerConversion.h.
  // -----------------------------------------------------------------------

  // Mirrors Data::FVec3 (3 floats: x, y, z)
  struct LcVec3 {
    float x, y, z;
  };

  // Mirrors Data::FQuat (4 floats in order: w, x, y, z)
  struct LcQuat {
    float w, x, y, z;
  };

  // Mirrors Data::FPose — member names follow SDM snake_case convention.
  struct LcPose {
    LcVec3 position;
    LcQuat orientation;
  };

  // Mirrors Data::FVec4 / FFoV / FBBox / FImage / FPlaneParams.
  // All are the same underlying type (FVec4 with union aliases):
  //   x = Left = LocX = a
  //   y = Right = LocY = b
  //   z = Bottom = Width = c
  //   w = Top = Height = d
  struct LcVec4 {
    float x, y, z, w;
  };

  // Mirrors glm::mat4 (column-major 4x4 float matrix).
  // col[c][r] = element at column c, row r.
  // Matches glm::mat4 in-memory layout: 4 columns of 4 floats each.
  struct LcMat4 {
    float col[4][4];
  };

  // Mirrors ProjectionLayerInfo from LayerConversion.h
  struct LcProjectionLayerInfo {
    LcPose pose;
    LcVec4 fov;        // Left=x, Right=y, Bottom=z, Top=w  (radians)
    LcVec4 img;        // LocX=x, LocY=y, Width=z, Height=w (swapchain dims)
    LcVec4 sub_img;    // LocX=x, LocY=y, Width=z, Height=w (sub-image crop)
    LcVec4 plane_eqn;  // a=x, b=y, c=z, d=w
  };

  // Mirrors QuadLayerInfo from LayerConversion.h
  struct LcQuadLayerInfo {
    LcPose pose;
    struct {
      float width, height;
    } quad_size;     // physical size in world units (metres)
    LcVec4 img;      // LocX=x, LocY=y, Width=z, Height=w (swapchain dims)
    LcVec4 sub_img;  // LocX=x, LocY=y, Width=z, Height=w (sub-image crop)
  };

  // Mirrors ROILayerInfo from LayerConversion.h
  struct LcROILayerInfo {
    LcVec4 disp_image;     // LocX=x, LocY=y, Width=z, Height=w (display buffer dims)
    LcVec4 disp_roi;       // LocX=x, LocY=y, Width=z, Height=w (dst ROI on display)
    LcVec4 rendr_image;    // LocX=x, LocY=y, Width=z, Height=w (render image dims)
    LcVec4 rendr_roi;      // LocX=x, LocY=y, Width=z, Height=w (src ROI in render image)
    LcMat4 disp_proj_mat;  // display projection matrix (column-major, matches glm::mat4)
  };

  // Mirrors DCPProjectionLayer from LayerConversion.h
  struct LcDCPProjectionLayer {
    LcVec4 plane_eqn;  // a=x, b=y, c=z, d=w
    LcPose pose;
    LcVec4 fov;     // Left=x, Right=y, Bottom=z, Top=w  (radians)
    LcVec4 bbox;    // LocX=x, LocY=y, Width=z, Height=w (bounding box)
    int32_t space;  // Data::XrSpace enum: 0=WorldLocked, 1=HeadLocked, 2=None
  };

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

  // Upload static per-session configuration.
  // Loads LDC CSV files via libLayerConversion.so and populates GpuReprojStaticConfig.
  void SetStaticConfig(const SDMDisplayDeviceConfig *cfg);

  // Load libLayerConversion.so and resolve all LC dlsym symbols.
  bool LoadLdcLib();
  // Parse LDC CSV file into LdcMetadata (caller must free out->data).
  bool ParseLDCFile(const std::string &path, LdcMetadata *out);
  // Parse CSV files, call ResolveLDCCACGrid_C, cache result in ldc_grid_result_.
  void LoadLdcCalibration();

  // ---- SetStaticConfig sub-functions (mirrors hw_virtual_drm.cpp modular pattern) ----
  // Gauss-Jordan 4×4 matrix inverse (mirrors HWVirtualDRM::InvertMatrix).
  bool InvertMatrix(const float input_mat[4][4], float inv_mat[4][4]);
  // Populate proj/inv-proj matrices, gamma LUT, and rotation from SDMDisplayDeviceConfig.
  void PopulateProjMatrices(GpuReprojStaticConfig &cfg, const SDMDisplayDeviceConfig *dcfg);
  // Populate LDC calibration fields from cached ldc_lens_out_/ldc_bbox_out_.
  // (mirrors iwe_repro_impl.cpp::SetDisplayDeviceConfig LDC section)
  void PopulateLdcCalibration(GpuReprojStaticConfig &cfg);
  // Populate per-eye display dimensions from allocated buffers or GetFrameBufferConfig.
  void PopulateDisplayDimensions(GpuReprojStaticConfig &cfg);

  // Pass the render-time pose DMA-BUF fd directly to the GPU reproj library.
  void SetPoseBuffer(int fd);

  // Per-frame: append the reproj output buffer as a kCompositionSDE layer so
  // the resource manager assigns a DPU pipe to it before Prepare() runs.
  void AppendReprojOutputLayer(LayerStack *layer_stack);

  // Per-frame: warp app layers into the reproj output buffer.
  void Apply(SDMLayerStack *layer_stack);

  // Per-layer: convert layer geometry through LayerConversion lib (if loaded).
  // Calls ConvertProjectionLayerParameters_C / ConvertQuadInfoToProjectionInfo_C /
  // ConvertROILayerToProjectionLayer_C depending on comp_layer_type, then updates
  // p with the converted pose, FoV, plane equation, and bounding box.
  // Returns true if conversion was performed; false if LC lib is unavailable
  // (caller should use raw layer params as-is).
  // Mirrors iwe_repro_impl.cpp::GetProjectionLayers() logic.
  bool ConvertLayerParams(const Layer *layer, GpuReprojLayerParams *p);

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

  // True when a CWB frame dump is pending (normal path) or FCM capture is active.
  bool IsCwbCommit() const { return (dump_frame_count_ > 0) || IsFcmCwbActive(); }

  // -----------------------------------------------------------------------
  // GPU reproj library (loaded via dlopen)
  // -----------------------------------------------------------------------
  GpuReproj *gpu_reproj_ = nullptr;
  void *gpu_reproj_lib_ = nullptr;

  // -----------------------------------------------------------------------
  // LDC calibration library and cached result
  // -----------------------------------------------------------------------
  void *ldc_lib_ = nullptr;
  void *resolve_ldc_cac_grid_fn_ = nullptr;  // dlsym("ResolveLDCCACGrid_C")
  void *convert_proj_layer_fn_ = nullptr;    // dlsym("ConvertProjectionLayerParameters_C")
  void *convert_quad_info_fn_ = nullptr;     // dlsym("ConvertQuadInfoToProjectionInfo_C")
  void *convert_roi_layer_fn_ = nullptr;     // dlsym("ConvertROILayerToProjectionLayer_C")
  bool ldc_calibration_valid_ = false;
  LdcGridResult ldc_grid_result_ = {};

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
  int reproj_cwb_batch_index_ = kReprojSlotCount;  // CWB batch index; init uses 1,2; CWB from 3+

  // Persistent reproj output layers — layer_stack_.layers holds raw pointers,
  // so these must outlive each frame's layer stack.
  // One Layer per eye; each carries kReprojFieldsPerEye planes (R/G/B FSC).
  Layer reproj_out_layers_[kReprojEyeCount] = {};
};

}  // namespace sdm

#endif  // __SDM_DISPLAY_BUILTIN_GPU_REPROJ_H__
