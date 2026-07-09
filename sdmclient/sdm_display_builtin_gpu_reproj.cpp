/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

// GPU Late Stage Reprojection (seraph/GPU LSR path) — SDMDisplayBuiltInGpuReproj implementation.
//
// All GPU reproj state and logic live directly in SDMDisplayBuiltInGpuReproj.
// GpuReproj.h is included here (not in the header) so that sdmclient headers
// remain free of Vulkan/libgpu_reproj dependencies.

#include <dlfcn.h>
#include <sys/mman.h>
#include <cstring>
#include <cmath>
#include <vector>

#include <utils/constants.h>
#include <utils/debug.h>
#include <utils/fence.h>

#include "sdm_display_builtin_gpu_reproj.h"
#include "sdm_debugger.h"

#include <utils/utils.h>  // IsGpuLsrVariant(), GetSocName()

// GpuReproj.h provides the full GpuReproj abstract interface and all
// associated plain-old-data structs.  libgpu_reproj.so is loaded at
// runtime via dlopen/dlsym — no static link.
#include "GpuReproj.h"

#define __CLASS__ "SDMDisplayBuiltInGpuReproj"

namespace sdm {

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------

SDMDisplayBuiltInGpuReproj::SDMDisplayBuiltInGpuReproj(CoreInterface *core_intf,
                                                       BufferAllocator *buffer_allocator,
                                                       SDMCompositorCallbacks *callbacks,
                                                       SDMDisplayEventHandler *event_handler,
                                                       Display id, int32_t sdm_id)
    : SDMDisplayBuiltIn(core_intf, buffer_allocator, callbacks, event_handler, id, sdm_id) {}

// -----------------------------------------------------------------------
// PostInit — initialize GPU reproj after display_intf_ is ready
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::PostInit() {
  // Initialize base class (layer stitch, etc.) first.
  DisplayError status = SDMDisplayBuiltIn::PostInit();
  if (status != kErrorNone) {
    return status;
  }

  // Initialize GPU reprojection.  display_intf_ is available here
  // (set during SDMDisplay::Init()).
  if (!InitGpuReproj()) {
    DLOGW("GPU reprojection init failed — LSR GPU path disabled");
  }

  return kErrorNone;
}

// -----------------------------------------------------------------------
// Deinit — clean up GPU reproj before base class teardown
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::Deinit(bool deinit_layer_builder) {
  DeinitGpuReproj();
  return SDMDisplayBuiltIn::Deinit(deinit_layer_builder);
}

// -----------------------------------------------------------------------
// InitGpuReproj — load libgpu_reproj.so and set up the engine
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::InitGpuReproj() {
  // GPU LSR is only supported on seraph SoC (IDs 736/737).
  if (!IsGpuLsrVariant()) {
    DLOGI("GPU reprojection not supported on SoC '%s' — seraph (GPU LSR) only", GetSocName());
    return true;  // Not an error — just not applicable on this target
  }

  int gpu_reproj_enabled = 1;
  SDMDebugHandler::Get()->GetProperty("vendor.display.gpu_reproj.enable", &gpu_reproj_enabled);
  if (!gpu_reproj_enabled) {
    DLOGI("GPU reprojection disabled by vendor.display.gpu_reproj.enable=0");
    return true;  // Not an error — just disabled
  }

  gpu_reproj_lib_ = ::dlopen("libgpu_reproj.so", RTLD_NOW);
  if (!gpu_reproj_lib_) {
    DLOGW("libgpu_reproj.so not found: %s — GPU reproj disabled", dlerror());
    return true;  // not a hard error; reproj simply stays disabled
  }

  GpuReproj *(*factory_fn)(bool) = nullptr;
  *reinterpret_cast<void **>(&factory_fn) = ::dlsym(gpu_reproj_lib_, "GpuReprojImpl_GetInstance");
  if (!factory_fn) {
    DLOGE("dlsym GpuReprojImpl_GetInstance failed: %s", dlerror());
    ::dlclose(gpu_reproj_lib_);
    gpu_reproj_lib_ = nullptr;
    return false;
  }

  gpu_reproj_ = factory_fn(false /* isSecure */);
  if (!gpu_reproj_) {
    DLOGE("GpuReprojImpl_GetInstance returned null");
    ::dlclose(gpu_reproj_lib_);
    gpu_reproj_lib_ = nullptr;
    return false;
  }

  // Upload passthrough static config; real calibration arrives later via
  // SetDisplayDeviceConfig().  Buffer dimensions are not yet known so
  // SetStaticConfig will query display_intf_ directly.
  SetStaticConfig(nullptr);

  DLOGI("GPU reprojection initialized (display id=%" PRIu64 ")", id_);
  return true;
}

// -----------------------------------------------------------------------
// DeinitGpuReproj — release engine and buffers
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::DeinitGpuReproj() {
  if (!gpu_reproj_)
    return;

  if (reproj_out_fence_fd_ >= 0) {
    close(reproj_out_fence_fd_);
    reproj_out_fence_fd_ = -1;
  }

  FreeReprojBuffers();

  delete gpu_reproj_;
  gpu_reproj_ = nullptr;

  if (gpu_reproj_lib_) {
    ::dlclose(gpu_reproj_lib_);
    gpu_reproj_lib_ = nullptr;
  }

  DLOGI("GPU reprojection deinitialized (display id=%" PRIu64 ")", id_);
}

// -----------------------------------------------------------------------
// AllocateReprojBuffers
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::AllocateReprojBuffers() {
  DisplayConfigVariableInfo fb_cfg = {};
  DisplayError err = display_intf_->GetFrameBufferConfig(&fb_cfg);
  if (err != kErrorNone) {
    DLOGE("GetFrameBufferConfig failed: %d", err);
    return false;
  }

  const uint32_t buf_w = fb_cfg.x_pixels / 2;  // per-eye width
  const uint32_t buf_h = fb_cfg.y_pixels;

  for (int i = 0; i < kReprojSlotCount; i++) {
    for (int b = 0; b < kReprojBufsPerSlot; b++) {
      BufferConfig &cfg = reproj_out_[i][b].buffer_config;
      cfg.width = buf_w;
      cfg.height = buf_h;
      // C8 UBWC allocation requires that GPU_TEXTURE is NOT set.
      // When gfx_client=true, GPU_TEXTURE triggers IsUBWCSupportedByGPU(C_8)
      // which returns false → UBWC disabled.  Without GPU_TEXTURE the GPU
      // check is bypassed and snapalloc allocates C_8 UBWC via
      // MmmColorFormatMapper.
      cfg.format = kFormatC8Ubwc;
      cfg.gfx_client = false;
      cfg.secure = false;
      cfg.cache = true;

      int ret = buffer_allocator_->AllocateBuffer(&reproj_out_[i][b]);
      if (ret != 0) {
        DLOGE("AllocateBuffer failed for reproj slot %d buf %d: %d", i, b, ret);
        for (int si = 0; si <= i; si++) {
          int blim = (si < i) ? kReprojBufsPerSlot : b;
          for (int bi = 0; bi < blim; bi++) {
            buffer_allocator_->FreeBuffer(&reproj_out_[si][bi]);
          }
        }
        return false;
      }
    }
  }

  reproj_bufs_allocated_ = true;
  DLOGI("Reproj buffers allocated: %ux%u C8 UBWC x%d (slots=%d bufs/slot=%d)", buf_w, buf_h,
        kReprojSlotCount * kReprojBufsPerSlot, kReprojSlotCount, kReprojBufsPerSlot);
  return true;
}

// -----------------------------------------------------------------------
// AllocateCoordBuffer
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::AllocateCoordBuffer() {
  // 1024×1 RGBA8888 = 4096 bytes (4K DMA-BUF) for the DCP↔GPU coord buffer.
  static constexpr uint32_t kCoordBufWidth = 1024;
  static constexpr uint32_t kCoordBufSize = kCoordBufWidth * 4;

  BufferConfig &cfg = reproj_coord_buf_info_.buffer_config;
  cfg.width = kCoordBufWidth;
  cfg.height = 1;
  cfg.format = kFormatRGBA8888;
  cfg.gfx_client = false;
  cfg.secure = false;
  cfg.cache = true;  // CPU-accessible for initialization

  int ret = buffer_allocator_->AllocateBuffer(&reproj_coord_buf_info_);
  if (ret != 0) {
    DLOGE("AllocateBuffer for coord buffer failed: %d", ret);
    return false;
  }

  reproj_coord_buf_fd_ = reproj_coord_buf_info_.alloc_buffer_info.fd;

  // Zero-initialise the entire 4K buffer.
  void *mapped =
      mmap(nullptr, kCoordBufSize, PROT_READ | PROT_WRITE, MAP_SHARED, reproj_coord_buf_fd_, 0);
  if (mapped != MAP_FAILED) {
    memset(mapped, 0, kCoordBufSize);
    munmap(mapped, kCoordBufSize);
  } else {
    DLOGW("AllocateCoordBuffer: mmap failed — buffer not zeroed");
  }

  DLOGI("Coord buffer allocated: fd=%d (1024x1 RGBA8888 = 4096 bytes / 4K)", reproj_coord_buf_fd_);
  return true;
}

// -----------------------------------------------------------------------
// RegisterCoordBufferWithDpu (stub — kernel property not yet defined)
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::RegisterCoordBufferWithDpu() {
  // TODO: Once the kernel DRM property for the coord buffer is defined,
  // replace the stub below with the appropriate display_intf_ call, e.g.:
  //   display_intf_->SetReprojCoordBuffer(reproj_coord_buf_fd_);
  if (reproj_coord_buf_fd_ >= 0) {
    DLOGW(
        "RegisterCoordBufferWithDpu: coord buffer fd=%d ready but DRM "
        "property not yet defined — skipping",
        reproj_coord_buf_fd_);
  }
}

// -----------------------------------------------------------------------
// SetStaticConfig
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::SetStaticConfig(const SDMDisplayDeviceConfig *device_cfg) {
  if (!gpu_reproj_)
    return;

  if (device_cfg) {
    reproj_device_cfg_ = *device_cfg;
    reproj_device_cfg_set_ = true;
  }

  // Passthrough sparse warp grid (35×27, 6 channels).
  static constexpr int kGridW = 35, kGridH = 27, kGridCh = 6;
  static float sparse_grid[kGridW * kGridH * kGridCh * 2];
  for (int ch = 0; ch < kGridCh; ch++) {
    for (int row = 0; row < kGridH; row++) {
      for (int col = 0; col < kGridW; col++) {
        int idx = (ch * kGridH * kGridW + row * kGridW + col) * 2;
        sparse_grid[idx + 0] = static_cast<float>(col) / (kGridW - 1);
        sparse_grid[idx + 1] = static_cast<float>(row) / (kGridH - 1);
      }
    }
  }

  // Passthrough radial distortion LUT (257 samples, 6 channels).
  static constexpr int kDistRes = 257;
  static float radial_dis_grid[kDistRes * kGridCh];
  for (int i = 0; i < kDistRes * kGridCh; i++)
    radial_dis_grid[i] = 1.0f;

  // Passthrough gamma LUT (256 entries): linear ramp.
  static constexpr int kGammaSize = 256;
  static float gamma_lut[kGammaSize];
  for (int i = 0; i < kGammaSize; i++)
    gamma_lut[i] = static_cast<float>(i) / 255.0f;

  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  GpuReprojStaticConfig cfg = {};
  cfg.sparse_grid = sparse_grid;
  cfg.grid_w = kGridW;
  cfg.grid_h = kGridH;
  cfg.radial_dis_grid = radial_dis_grid;
  cfg.distort_resolution = kDistRes;
  cfg.r_max = 1.5f;
  cfg.to_lrgb_left = 1.0f;
  cfg.to_lrgb_right = 1.0f;
  cfg.gamma_lut = gamma_lut;
  cfg.gamma_size = kGammaSize;
  cfg.optical_axis_offset[0] = 0.5f;
  cfg.optical_axis_offset[1] = 0.5f;
  cfg.optical_axis_offset[2] = 0.5f;
  cfg.optical_axis_offset[3] = 0.5f;

  // Gauss-Jordan 4×4 matrix inverse.
  auto mat4Inverse = [](const float *m, float *inv) -> bool {
    float a[4][8];
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        a[r][c] = m[r * 4 + c];
        a[r][c + 4] = (r == c) ? 1.0f : 0.0f;
      }
    }
    for (int col = 0; col < 4; col++) {
      int pivot = col;
      for (int row = col + 1; row < 4; row++) {
        if (fabsf(a[row][col]) > fabsf(a[pivot][col]))
          pivot = row;
      }
      if (pivot != col) {
        for (int k = 0; k < 8; k++) {
          float t = a[col][k];
          a[col][k] = a[pivot][k];
          a[pivot][k] = t;
        }
      }
      float diag = a[col][col];
      if (fabsf(diag) < 1e-7f)
        return false;
      for (int k = 0; k < 8; k++)
        a[col][k] /= diag;
      for (int row = 0; row < 4; row++) {
        if (row == col)
          continue;
        float factor = a[row][col];
        for (int k = 0; k < 8; k++)
          a[row][k] -= factor * a[col][k];
      }
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        inv[r * 4 + c] = a[r][c + 4];
    return true;
  };

  const SDMDisplayDeviceConfig *dcfg =
      device_cfg ? device_cfg : (reproj_device_cfg_set_ ? &reproj_device_cfg_ : nullptr);

  if (dcfg) {
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        cfg.proj_matrix_left[r * 4 + c] = dcfg->projectionMatrix[0].prjMatrix[r][c];
        cfg.proj_matrix_right[r * 4 + c] = dcfg->projectionMatrix[1].prjMatrix[r][c];
      }
    }
    if (!mat4Inverse(cfg.proj_matrix_left, cfg.inv_proj_matrix_left)) {
      DLOGW("proj_matrix_left is singular — falling back to identity inverse");
      memcpy(cfg.inv_proj_matrix_left, kIdentity, sizeof(kIdentity));
    }
    if (!mat4Inverse(cfg.proj_matrix_right, cfg.inv_proj_matrix_right)) {
      DLOGW("proj_matrix_right is singular — falling back to identity inverse");
      memcpy(cfg.inv_proj_matrix_right, kIdentity, sizeof(kIdentity));
    }
  } else {
    memcpy(cfg.proj_matrix_left, kIdentity, sizeof(kIdentity));
    memcpy(cfg.proj_matrix_right, kIdentity, sizeof(kIdentity));
    memcpy(cfg.inv_proj_matrix_left, kIdentity, sizeof(kIdentity));
    memcpy(cfg.inv_proj_matrix_right, kIdentity, sizeof(kIdentity));
  }

  if (reproj_bufs_allocated_) {
    cfg.display_width = reproj_out_[0][0].alloc_buffer_info.aligned_width;
    cfg.display_height = reproj_out_[0][0].alloc_buffer_info.aligned_height;
  } else {
    DisplayConfigVariableInfo fb_cfg = {};
    if (display_intf_->GetFrameBufferConfig(&fb_cfg) == kErrorNone) {
      cfg.display_width = fb_cfg.x_pixels / 2;
      cfg.display_height = fb_cfg.y_pixels;
    }
  }

  int ret = gpu_reproj_->setStaticConfig(cfg);
  if (ret != 0)
    DLOGE("GpuReproj::setStaticConfig failed: %d", ret);
}

// -----------------------------------------------------------------------
// SetPoseBuffer
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::SetPoseBuffer(int fd) {
  if (!gpu_reproj_ || fd < 0)
    return;
  gpu_reproj_->setPoseBuffer(fd);
  reproj_pose_set_ = true;
}

// -----------------------------------------------------------------------
// AppendReprojOutputLayer
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::AppendReprojOutputLayer(LayerStack *layer_stack) {
  if (!gpu_reproj_ || !layer_stack)
    return;

  // Lazy allocation on the first LSR frame.
  if (!reproj_bufs_allocated_) {
    if (!AllocateReprojBuffers()) {
      DLOGE("AppendReprojOutputLayer: lazy buffer allocation failed");
      return;
    }
    if (reproj_coord_buf_fd_ < 0 && AllocateCoordBuffer()) {
      if (gpu_reproj_->setCoordBuffer(reproj_coord_buf_fd_) != 0)
        DLOGW("GpuReproj::setCoordBuffer failed");
      RegisterCoordBufferWithDpu();
    }
    SetStaticConfig(reproj_device_cfg_set_ ? &reproj_device_cfg_ : nullptr);
    DLOGI("Reproj buffers lazily allocated on first LSR frame (display id=%" PRIu64 ")", id_);
  }

  // Set GPU LSR init commit batch params for the first 2 commits.
  if (reproj_dpu_init_count_ < kReprojSlotCount) {
    layer_stack->gpu_reproj_batch_size = static_cast<uint32_t>(kReprojSlotCount);
    layer_stack->gpu_reproj_batch_index = static_cast<uint32_t>(reproj_dpu_init_count_ + 1);

    // On the first init commit (index=1) also pass the HFI coordination buffer.
    if (reproj_dpu_init_count_ == 0 && reproj_coord_buf_fd_ >= 0) {
      auto shared_buf = std::make_shared<LayerBuffer>();
      shared_buf->planes[0].fd = reproj_coord_buf_fd_;
      shared_buf->width = reproj_coord_buf_info_.alloc_buffer_info.aligned_width;
      shared_buf->height = reproj_coord_buf_info_.alloc_buffer_info.aligned_height;
      shared_buf->format = reproj_coord_buf_info_.alloc_buffer_info.format;
      shared_buf->handle_id = reproj_coord_buf_info_.alloc_buffer_info.id;
      layer_stack->gpu_reproj_shared_buffer = shared_buf;
      DLOGI("AppendReprojOutputLayer: coord shared_buf fd=%d wxh=%ux%u fmt=%d handle_id=%" PRIu64,
            shared_buf->planes[0].fd, shared_buf->width, shared_buf->height, shared_buf->format,
            shared_buf->handle_id);
    }

    layer_stack->gpu_reproj_batch_type = 1;  // MSM_MDP_BATCH_TYPE_LSR

    reproj_dpu_init_count_++;
    DLOGI("GPU reproj: DPU init commit %d/%d (slot=%d, batch_index=%u batch_type=%u)",
          reproj_dpu_init_count_, kReprojSlotCount, reproj_slot_,
          layer_stack->gpu_reproj_batch_index, layer_stack->gpu_reproj_batch_type);
  }

  if (reproj_out_[reproj_slot_][0].alloc_buffer_info.fd < 0)
    return;

  // Save FBT/stitch-target layers, clear app layers, append reproj eye layers.
  std::vector<Layer *> special_layers;
  for (auto *layer : layer_stack->layers) {
    if (layer && (layer->composition == kCompositionGPUTarget ||
                  layer->composition == kCompositionStitchTarget)) {
      special_layers.push_back(layer);
    }
  }
  layer_stack->layers.clear();

  for (int eye = 0; eye < kReprojEyeCount; eye++) {
    const int base = eye * kReprojFieldsPerEye;
    const AllocatedBufferInfo &alloc_r = reproj_out_[reproj_slot_][base + 0].alloc_buffer_info;
    const uint32_t unaligned_w = reproj_out_[reproj_slot_][base + 0].buffer_config.width;

    reproj_out_layers_[eye] = {};
    LayerBuffer &lb = reproj_out_layers_[eye].input_buffer;

    lb.width = alloc_r.aligned_width;
    lb.height = alloc_r.aligned_height;
    lb.unaligned_width = unaligned_w;
    lb.unaligned_height = alloc_r.aligned_height;
    lb.format = alloc_r.format;
    lb.size = alloc_r.size;
    lb.handle_id = alloc_r.id;
    lb.buffer_id = reinterpret_cast<uint64_t>(reproj_out_[reproj_slot_][base + 0].private_data);
    lb.acquire_fence = nullptr;

    for (int f = 0; f < kReprojFieldsPerEye; f++) {
      const AllocatedBufferInfo &af = reproj_out_[reproj_slot_][base + f].alloc_buffer_info;
      lb.planes[f].fd = af.fd;
      lb.planes[f].offset = 0;
      lb.planes[f].stride = af.stride;
      lb.planes[f].buffer_id =
          reinterpret_cast<uint64_t>(reproj_out_[reproj_slot_][base + f].private_data);
      lb.planes[f].handle_id = af.id;
      lb.planes[f].color = static_cast<ColorComponent>(f + 1);  // R=1, G=2, B=3
    }

    reproj_out_layers_[eye].composition = kCompositionSDE;
    reproj_out_layers_[eye].src_rect = {0.0f, 0.0f, FLOAT(unaligned_w),
                                        FLOAT(alloc_r.aligned_height)};
    const float dst_left = eye * FLOAT(unaligned_w);
    reproj_out_layers_[eye].dst_rect = {dst_left, 0.0f, dst_left + FLOAT(unaligned_w),
                                        FLOAT(alloc_r.aligned_height)};
    reproj_out_layers_[eye].blending = kBlendingOpaque;
    reproj_out_layers_[eye].plane_alpha = 0xffff;
    reproj_out_layers_[eye].flags.updating = 1;

    layer_stack->layers.push_back(&reproj_out_layers_[eye]);
  }

  for (auto *layer : special_layers) {
    layer_stack->layers.push_back(layer);
  }

  DLOGV_IF(kTagClient, "Reproj output layers appended: slot=%d bufs=%d %ux%u", reproj_slot_,
           kReprojBufsPerSlot, reproj_out_[reproj_slot_][0].alloc_buffer_info.aligned_width,
           reproj_out_[reproj_slot_][0].alloc_buffer_info.aligned_height);

  reproj_slot_ = 1 - reproj_slot_;
}

// -----------------------------------------------------------------------
// Apply
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::Apply(SDMLayerStack *layer_stack) {
  if (!gpu_reproj_) {
    DLOGW("Apply: gpu_reproj_ not initialized");
    return;
  }

  // Lazy allocation before the blit so slot=0 is blitted on LSR commit 1.
  if (!reproj_bufs_allocated_) {
    if (!AllocateReprojBuffers()) {
      DLOGE("Apply: lazy buffer allocation failed");
      return;
    }
    if (reproj_coord_buf_fd_ < 0 && AllocateCoordBuffer()) {
      if (gpu_reproj_->setCoordBuffer(reproj_coord_buf_fd_) != 0)
        DLOGW("GpuReproj::setCoordBuffer failed");
      RegisterCoordBufferWithDpu();
    }
    SetStaticConfig(reproj_device_cfg_set_ ? &reproj_device_cfg_ : nullptr);
    DLOGI("Apply: reproj buffers lazily allocated on first LSR frame (display id=%" PRIu64 ")",
          id_);
  }

  reproj_frame_count_++;
  if (reproj_frame_count_ % 60 == 0) {
    DLOGD_IF(kTagClient, "GPU reproj: %" PRIu64 " frames processed (display id=%" PRIu64 ")",
             reproj_frame_count_, id_);
  }

  // Init frame: pass all 12 output buffers to the GPU for registration.
  // Steady-state: pass input layers only (buffers already registered).
  bool is_init_frame = (reproj_dpu_init_count_ == 0);

  constexpr int kTotalBufs = kReprojBufsPerSlot * kReprojSlotCount;
  GpuReprojBufferInfo dst_buf_infos[kTotalBufs] = {};
  int num_dst_bufs = 0;

  if (is_init_frame) {
    if (reproj_out_[0][0].alloc_buffer_info.fd < 0) {
      DLOGW("Apply: reproj output fd is invalid — skipping");
      return;
    }
    num_dst_bufs = kTotalBufs;
    DLOGI("Apply: INIT FRAME — passing all %d output buffers (both slots) to GPU", num_dst_bufs);
    for (int slot = 0; slot < kReprojSlotCount; slot++) {
      for (int b = 0; b < kReprojBufsPerSlot; b++) {
        const AllocatedBufferInfo &a = reproj_out_[slot][b].alloc_buffer_info;
        int idx = slot * kReprojBufsPerSlot + b;
        dst_buf_infos[idx] = {a.fd, a.aligned_width, a.aligned_height, static_cast<int>(a.format),
                              a.usage};
      }
    }
  }

  if (reproj_out_fence_fd_ >= 0) {
    close(reproj_out_fence_fd_);
    reproj_out_fence_fd_ = -1;
  }

  // Build per-layer params.
  std::vector<GpuReprojLayerParams> layer_params;
  for (auto sdm_layer : layer_stack->layer_set_) {
    Layer *layer = sdm_layer->GetSDMLayer();
    if (!layer)
      continue;
    GpuReprojLayerParams p = {};

    p.src_fd = layer->input_buffer.planes[0].fd;
    p.src_width = layer->input_buffer.width;
    p.src_height = layer->input_buffer.height;
    p.src_format = static_cast<int>(layer->input_buffer.format);
    p.src_usage = layer->input_buffer.usage;
    p.src_fence_fd = Fence::Dup(layer->input_buffer.acquire_fence);

    p.layer_pos[0] = layer->layer_pose.pos.x;
    p.layer_pos[1] = layer->layer_pose.pos.y;
    p.layer_pos[2] = layer->layer_pose.pos.z;
    p.layer_orient[0] = layer->layer_pose.orientation.x;
    p.layer_orient[1] = layer->layer_pose.orientation.y;
    p.layer_orient[2] = layer->layer_pose.orientation.z;
    p.layer_orient[3] = layer->layer_pose.orientation.w;
    p.frustum[0] = layer->layer_frustum.angleLeft;
    p.frustum[1] = layer->layer_frustum.angleRight;
    p.frustum[2] = layer->layer_frustum.angleUp;
    p.frustum[3] = layer->layer_frustum.angleDown;
    p.quad_size[0] = layer->layer_quad_size.width;
    p.quad_size[1] = layer->layer_quad_size.height;
    p.plane_eq[0] = layer->plane_equation.a;
    p.plane_eq[1] = layer->plane_equation.b;
    p.plane_eq[2] = layer->plane_equation.c;
    p.plane_eq[3] = layer->plane_equation.d;
    p.comp_layer_type = static_cast<int>(layer->comp_layer_type);
    p.ref_space_type = static_cast<int>(layer->reference_space_type);
    p.visibility_type = static_cast<int>(layer->layer_visibility_type);
    p.src_rect[0] = layer->src_rect.left;
    p.src_rect[1] = layer->src_rect.top;
    p.src_rect[2] = layer->src_rect.right;
    p.src_rect[3] = layer->src_rect.bottom;
    p.dst_rect[0] = layer->dst_rect.left;
    p.dst_rect[1] = layer->dst_rect.top;
    p.dst_rect[2] = layer->dst_rect.right;
    p.dst_rect[3] = layer->dst_rect.bottom;
    {
      int xform = 0;
      if (layer->transform.rotation == 90.0f)
        xform = 1;
      else if (layer->transform.rotation == 180.0f)
        xform = 2;
      else if (layer->transform.rotation == 270.0f)
        xform = 3;
      else if (layer->transform.flip_horizontal)
        xform = 4;
      else if (layer->transform.flip_vertical)
        xform = 5;
      p.transform = xform;
    }
    p.plane_alpha = layer->plane_alpha / 65535.0f;
    p.blending = static_cast<int>(layer->blending);
    layer_params.push_back(p);
  }

  if (layer_params.empty()) {
    DLOGW("Apply: no app layers to reproject — skipping");
    return;
  }

  GpuReprojPoseParams pose = {};
  if (reproj_pose_set_) {
    pose.render_pos[0] = reproj_pose_.pos[0];
    pose.render_pos[1] = reproj_pose_.pos[1];
    pose.render_pos[2] = reproj_pose_.pos[2];
    pose.render_orient[0] = reproj_pose_.orient[0];
    pose.render_orient[1] = reproj_pose_.orient[1];
    pose.render_orient[2] = reproj_pose_.orient[2];
    pose.render_orient[3] = reproj_pose_.orient[3];
  } else {
    pose.render_orient[3] = 1.0f;  // identity quaternion
  }
  pose.pose_delta_orient[3] = 1.0f;

  GpuReprojBlitParams blit_params = {};
  blit_params.dst_bufs = dst_buf_infos;
  blit_params.num_dst_bufs = num_dst_bufs;
  blit_params.src_buf = nullptr;
  blit_params.src_fence_fd = -1;
  blit_params.layers = layer_params.data();
  blit_params.num_layers = static_cast<int>(layer_params.size());
  blit_params.pose = &pose;

  reproj_out_fence_fd_ = gpu_reproj_->blit(blit_params);

  if (reproj_out_fence_fd_ < 0) {
    DLOGE("GpuReproj::blit failed to create output fence");
    return;
  }

  // Set release fences on all input app layers.
  size_t layer_idx = 0;
  for (auto sdm_layer : layer_stack->layer_set_) {
    Layer *layer = sdm_layer->GetSDMLayer();
    if (!layer)
      continue;
    int release_fd = dup(reproj_out_fence_fd_);
    if (release_fd >= 0) {
      layer->input_buffer.release_fence = Fence::Create(release_fd, "gpu_reproj_layer_release");
    }
    layer_idx++;
  }

  if (reproj_out_fence_fd_ >= 0) {
    close(reproj_out_fence_fd_);
    reproj_out_fence_fd_ = -1;
  }
}

// -----------------------------------------------------------------------
// IsReprojNeeded
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::IsReprojNeeded(SDMLayerStack *layer_stack) const {
  if (!layer_stack || layer_stack->layer_set_.empty())
    return false;

  bool reproj_needed = false;
  int idx = 0;
  for (auto sdm_layer : layer_stack->layer_set_) {
    Layer *layer = sdm_layer->GetSDMLayer();
    if (!layer) {
      idx++;
      continue;
    }
    bool this_layer_reproj = (layer->comp_layer_type == COMPOSITION_LAYER_PROJECTION);
    reproj_needed |= this_layer_reproj;
    idx++;
  }
  DLOGD_IF(kTagClient, "IsReprojNeeded: total layers=%d result=%d", idx,
           static_cast<int>(reproj_needed));
  return reproj_needed;
}

// -----------------------------------------------------------------------
// FreeReprojBuffers
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::FreeReprojBuffers() {
  if (reproj_coord_buf_fd_ >= 0) {
    buffer_allocator_->FreeBuffer(&reproj_coord_buf_info_);
    reproj_coord_buf_fd_ = -1;
  }
  if (reproj_bufs_allocated_) {
    for (int i = 0; i < kReprojSlotCount; i++) {
      for (int b = 0; b < kReprojBufsPerSlot; b++) {
        buffer_allocator_->FreeBuffer(&reproj_out_[i][b]);
      }
    }
    reproj_bufs_allocated_ = false;
  }
  reproj_dpu_init_count_ = 0;
}

// -----------------------------------------------------------------------
// OnReprojInactive
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::OnReprojInactive() {
  if (!reproj_bufs_allocated_)
    return;
  FreeReprojBuffers();
  DLOGI("Reproj buffers freed — LSR inactive (display id=%" PRIu64 ")", id_);
}

// -----------------------------------------------------------------------
// PreValidateDisplay — append reproj output layer before Prepare()
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::PreValidateDisplay(bool *exit_validate) {
  DisplayError status = SDMDisplayBuiltIn::PreValidateDisplay(exit_validate);
  if (status != kErrorNone || *exit_validate) {
    return status;
  }

  if (IsGpuReprojEnabled() && IsReprojNeeded(sdm_layer_stack_)) {
    AppendReprojOutputLayer(&layer_stack_);
    layer_stack_.gpu_reproj_active = true;
  }

  return status;
}

// -----------------------------------------------------------------------
// CommitLayerStack — GPU blit before DPU commit, buffer lifecycle after
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::CommitLayerStack() {
  // Compute skip_commit_ for THIS frame before the reproj check.
  // SDMDisplayBuiltIn::CommitLayerStack() sets skip_commit_ = CanSkipCommit()
  // internally, but that happens AFTER we need to check it here.
  // CanSkipCommit() side effects (ResetBufferFlip, pending_refresh_=false) are
  // idempotent — the base class calling it again produces the same result.
  skip_commit_ = CanSkipCommit();

  layer_stack_.gpu_reproj_active = false;
  if (!skip_commit_ && IsGpuReprojEnabled() && IsReprojNeeded(sdm_layer_stack_)) {
    Apply(sdm_layer_stack_);
    layer_stack_.gpu_reproj_active = true;
  }

  DisplayError error = SDMDisplayBuiltIn::CommitLayerStack();

  if (IsGpuReprojEnabled() && !IsReprojNeeded(sdm_layer_stack_)) {
    OnReprojInactive();
  }

  return error;
}

// -----------------------------------------------------------------------
// CommitOrPrepare — GPU blit + 2-commit DPU init sequence
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::CommitOrPrepare(bool validate_only,
                                                         shared_ptr<Fence> *out_retire_fence,
                                                         uint32_t *out_num_types,
                                                         uint32_t *out_num_requests,
                                                         bool *needs_commit) {
  layer_stack_.gpu_reproj_active = false;
  if (!validate_only && IsGpuReprojEnabled() && IsReprojNeeded(sdm_layer_stack_)) {
    Apply(sdm_layer_stack_);
    layer_stack_.gpu_reproj_active = true;

    // LSR steady-state: DCP/IPCC drives ping-pong — skip DPU commit.
    if (IsReprojInitDone()) {
      DLOGI("CommitOrPrepare: LSR steady-state — skipping DPU commit (DCP/IPCC drives ping-pong)");
      if (out_num_types)
        *out_num_types = 0;
      if (out_num_requests)
        *out_num_requests = 0;
      if (needs_commit)
        *needs_commit = false;
      prepare_phase_ = false;
      return kErrorNone;
    }
  }

  bool reproj_was_active = layer_stack_.gpu_reproj_active;

  DisplayError status = SDMDisplayBuiltIn::CommitOrPrepare(
      validate_only, out_retire_fence, out_num_types, out_num_requests, needs_commit);

  // Second init commit: register slot 1 buffers with the DPU.
  bool need_second = (!validate_only && reproj_was_active && NeedsBatchSecondCommit());
  if (need_second) {
    DLOGI("CommitOrPrepare: GPU reproj BATCH INIT — issuing second DRM commit (batch_index=2)");
    AppendReprojOutputLayer(&layer_stack_);
    display_intf_->Commit(&layer_stack_);
    DLOGI("CommitOrPrepare: second DRM commit submitted — batch init complete");
  }

  return status;
}

// -----------------------------------------------------------------------
// SetPoseConfig — forward pose fd to GPU reproj library
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::SetPoseConfig(void *buffer_hnd) {
  DisplayError error = SDMDisplayBuiltIn::SetPoseConfig(buffer_hnd);
  if (error != kErrorNone) {
    return error;
  }

  if (IsGpuReprojEnabled()) {
    LayerBuffer pose_buffer = {};
    if (PopulateLayerBuffer(buffer_hnd, &pose_buffer) == kErrorNone) {
      SetPoseBuffer(pose_buffer.planes[0].fd);
    }
  }

  return kErrorNone;
}

// -----------------------------------------------------------------------
// SetDisplayDeviceConfig — re-upload lens calibration to GPU reproj
// -----------------------------------------------------------------------

DisplayError SDMDisplayBuiltInGpuReproj::SetDisplayDeviceConfig(
    SDMDisplayDeviceConfig sdm_display_device_config) {
  if (IsGpuReprojEnabled()) {
    SetStaticConfig(&sdm_display_device_config);
  }
  return display_intf_->SetDisplayDeviceConfig(sdm_display_device_config);
}

}  // namespace sdm
