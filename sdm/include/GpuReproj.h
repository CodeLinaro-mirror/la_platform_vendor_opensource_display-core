/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __GPU_REPROJ_H__
#define __GPU_REPROJ_H__

#include <stdint.h>

// ---------------------------------------------------------------------------
// Public data structures — no Vulkan types exposed here.
// ---------------------------------------------------------------------------

// Opaque data blob (mirrors kernel struct drm_msm_opaque_config).
// Used to pass sparse_grid and radial_dis_grid to libgpu_reproj.so.
struct GpuReprojOpaqueConfig {
  uint64_t size;  // size of data buffer in bytes
  uint64_t flags;
  uint32_t crc;
  uint64_t data;  // pointer to data buffer cast to uint64_t
};

// Per-eye point pair (mirrors kernel struct sde_drm_lsr_point).
// points[0] = left eye, points[1] = right eye.
struct GpuReprojLsrPoint {
  struct {
    uint32_t x, y;
  } points[2];
};

// ---------------------------------------------------------------------------
// GpuReprojStaticConfig — unified one-time calibration config.
// Populated by SDMDisplayBuiltInGpuReproj::SetStaticConfig() from:
//   • SDMDisplayDeviceConfig (SFLUA)          — proj matrices, gamma, rotation
//   • libLayerConversion.so::GetLDCCACParams  — sparse grid, radial LUT, optical params
//   • display_intf_->GetFrameBufferConfig()   — display dimensions
// ---------------------------------------------------------------------------
struct GpuReprojStaticConfig {
  // ---- From SDMDisplayDeviceConfig (SFLUA) ----
  float proj_matrix_left[16], proj_matrix_right[16];
  float inv_proj_matrix_left[16], inv_proj_matrix_right[16];  // computed by SDM
  float gamma_lut[256];
  float rotation_left[4], rotation_right[4];  // per-eye orientation {x,y,z,w}

  // ---- From libLayerConversion.so::GetLDCCACParams (LDC CSV files) ----
  GpuReprojOpaqueConfig reproj_sparse_grid;  // LensOut::sparse_grid (RG32F warp mesh)
  uint32_t reproj_grid_w;
  uint32_t reproj_grid_h;
  GpuReprojOpaqueConfig reproj_radial_dis_grid;  // BBOxOut::radial_distortion_grid (R32F LUT)
  uint32_t distort_resolution;                   // BBOxOut::distor_res (257)
  GpuReprojLsrPoint reproj_optical_axis_offset;  // BBOxOut::point[2]
  float reproj_r_max;                            // BBOxOut::reprojection_r_max
  uint32_t reproj_error_tol;                     // BBOxOut::errortol
  float reproj_tol_rgb_left;                     // BBOxOut::reproj_tol_rgb_left
  float reproj_tol_rgb_right;                    // BBOxOut::reproj_tol_rgb_right

  // ---- From hardware (GetFrameBufferConfig) ----
  uint32_t reproj_disp_im_width;   // per-eye buffer width  (x_pixels / 2)
  uint32_t reproj_disp_im_height;  // per-eye buffer height (y_pixels)
};

// Plain-old-data descriptor for a DMA-BUF-backed image buffer.
// Replaces sdm::VkImageWrapper::BufferInfo in the public API so that callers
// (e.g. sdmclient) do not need Vulkan headers in their include path.
struct GpuReprojBufferInfo {
  int fd;           // DMA-BUF file descriptor
  uint32_t width;   // buffer width in pixels
  uint32_t height;  // buffer height in pixels
  int format;       // LayerBufferFormat cast to int
  uint64_t usage;   // gralloc usage flags
};

struct GpuReprojLayerParams {
  int src_fd;           // DMA-BUF fd for the layer's input buffer
  uint32_t src_width;   // buffer width in pixels
  uint32_t src_height;  // buffer height in pixels
  int src_format;       // LayerBufferFormat cast to int
  uint64_t src_usage;   // gralloc usage flags
  int src_fence_fd;     // acquire fence fd (-1 if none; ownership transferred to blit())
  float layer_pos[3];
  float layer_orient[4];
  float frustum[4];
  float quad_size[2];
  float plane_eq[4];
  int comp_layer_type;
  int ref_space_type;
  int visibility_type;

  // Standard layer composition params (from SDM Layer struct).
  // src_rect: source crop in pixels {left, top, right, bottom}.
  //   Normalized to [0,1] by blit() using src_width/src_height.
  // dst_rect: destination rect in pixels {left, top, right, bottom}.
  //   Normalized to [0,1] by blit() using dst buffer dimensions.
  // transform: 0=none, 1=rot90CW, 2=rot180, 3=rot270CW, 4=flipH, 5=flipV.
  // plane_alpha: 0.0–1.0 (from layer->plane_alpha / 65535.0f).
  // blending: 0=premultiplied, 1=opaque, 2=coverage.
  float src_rect[4];  // {left, top, right, bottom} in pixels
  float dst_rect[4];  // {left, top, right, bottom} in pixels
  int transform;      // 0=none,1=rot90CW,2=rot180,3=rot270CW,4=flipH,5=flipV
  float plane_alpha;  // 0.0–1.0
  int blending;       // 0=premultiplied, 1=opaque, 2=coverage
};

struct GpuReprojPoseParams {
  float render_pos[3];
  float render_orient[4];
  float pose_delta_orient[4];
};

// ---------------------------------------------------------------------------
// GpuReprojBlitParams — all per-frame parameters for a single blit() call.
// ---------------------------------------------------------------------------

struct GpuReprojBlitParams {
  const GpuReprojBufferInfo *dst_bufs;  // array of num_dst_bufs output C8Ubwc buffers
  int num_dst_bufs;                     // must be >= 1 (primary target is dst_bufs[0])
  const GpuReprojBufferInfo *src_buf;   // FBT source buffer; nullptr when num_layers > 0
  int src_fence_fd;  // acquire fence for src_buf (-1 if none); ownership transferred
  const GpuReprojLayerParams *layers;  // per-layer params; nullptr when num_layers == 0
  int num_layers;                      // 0 → passthrough (src_buf used), >0 → layer-based
  const GpuReprojPoseParams *pose;     // current head pose; may be nullptr (identity assumed)
};

// ---------------------------------------------------------------------------
// GpuReproj — abstract interface for the Vulkan-backed reprojection engine.
//
// Callers (e.g. sdmclient) include only this header — no Vulkan types are
// exposed here.  The concrete implementation is GpuReprojImpl, defined in
// GpuReprojImpl.cpp (inside libgpu_reproj).
// Instances are obtained via dlsym("GpuReprojImpl_GetInstance").
// ---------------------------------------------------------------------------

class GpuReproj {
 public:
  virtual ~GpuReproj() {}

  // Upload static per-session configuration (projection matrices, LUTs, etc.).
  // Must be called before the first blit().
  virtual int setStaticConfig(const GpuReprojStaticConfig &config) = 0;

  // Set the DCP↔GPU coordination buffer (64-byte DMA-BUF).
  // Layout: { uint32_t active_slot; uint32_t error_type; uint32_t done_counter; uint8_t pad[52]; }
  // Must be called after construction and before the first blit().
  // Returns 0 on success, non-zero on failure.
  virtual int setCoordBuffer(int fd) = 0;

  // Set the render-time pose buffer (DMA-BUF fd).
  // Buffer layout: float pos[3] + float orient[4] = 28 bytes.
  // GpuReprojImpl maps the fd and reads pose each frame in FillPoseSSBO().
  // Returns 0 on success, non-zero on failure.
  virtual int setPoseBuffer(int fd) = 0;

  // Reproject into the destination buffers described by params.
  // Returns an output release fence fd (caller must close), or -1 on error.
  virtual int blit(const GpuReprojBlitParams &params) = 0;

  // Maximum number of layers supported per blit() call.
  static constexpr int kMaxLayers = 16;
};

#endif  // __GPU_REPROJ_H__
