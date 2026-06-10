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
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
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

  // Load libLayerConversion.so for LDC calibration (best-effort; not fatal).
  // LDC calibration (ResolveLDCCACGrid_C) + static config upload happen only when
  // SetDisplayDeviceConfig() arrives — mirrors Balsam iwe_repro_impl.cpp.
  LoadLdcLib();

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

  if (ldc_lib_) {
    ::dlclose(ldc_lib_);
    ldc_lib_ = nullptr;
    resolve_ldc_cac_grid_fn_ = nullptr;
    convert_proj_layer_fn_ = nullptr;
    convert_quad_info_fn_ = nullptr;
    convert_roi_layer_fn_ = nullptr;
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
// LoadLdcLib — dlopen libLayerConversion.so, resolve ResolveLDCCACGrid_C
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::LoadLdcLib() {
  ldc_lib_ = ::dlopen("liblayerconversion.so", RTLD_NOW);
  if (!ldc_lib_) {
    DLOGW("liblayerconversion.so not found: %s — LDC calibration disabled", dlerror());
    return false;
  }

  // All four symbols are extern "C" wrappers added in LayerConversion.cpp
  // specifically for dlopen/dlsym access from open-source components.
  // LC_API is empty on Linux/Android so C++ name mangling would prevent
  // finding the symbols without the extern "C" wrappers.

  resolve_ldc_cac_grid_fn_ = ::dlsym(ldc_lib_, "ResolveLDCCACGrid_C");
  if (!resolve_ldc_cac_grid_fn_) {
    DLOGE("dlsym ResolveLDCCACGrid_C failed: %s", dlerror());
    ::dlclose(ldc_lib_);
    ldc_lib_ = nullptr;
    return false;
  }

  // Layer conversion symbols — best-effort; not fatal if missing.
  convert_proj_layer_fn_ = ::dlsym(ldc_lib_, "ConvertProjectionLayerParameters_C");
  if (!convert_proj_layer_fn_)
    DLOGW(
        "dlsym ConvertProjectionLayerParameters_C failed: %s — projection layer conversion "
        "disabled",
        dlerror());

  convert_quad_info_fn_ = ::dlsym(ldc_lib_, "ConvertQuadInfoToProjectionInfo_C");
  if (!convert_quad_info_fn_)
    DLOGW("dlsym ConvertQuadInfoToProjectionInfo_C failed: %s — quad layer conversion disabled",
          dlerror());

  convert_roi_layer_fn_ = ::dlsym(ldc_lib_, "ConvertROILayerToProjectionLayer_C");
  if (!convert_roi_layer_fn_)
    DLOGW("dlsym ConvertROILayerToProjectionLayer_C failed: %s — ROI layer conversion disabled",
          dlerror());

  DLOGI(
      "libLayerConversion.so loaded — LDC calibration enabled; "
      "layer conversion: proj=%s quad=%s roi=%s",
      convert_proj_layer_fn_ ? "OK" : "N/A", convert_quad_info_fn_ ? "OK" : "N/A",
      convert_roi_layer_fn_ ? "OK" : "N/A");
  return true;
}

// -----------------------------------------------------------------------
// ParseLDCFile — parse LDC CSV file into LdcMetadata
// (adapted from iwe_repro_impl.cpp::ParseLDCFile)
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::ParseLDCFile(const std::string &path, LdcMetadata *out) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    DLOGE("ParseLDCFile: failed to open %s", path.c_str());
    return false;
  }

  auto split = [](const std::string &line, char delim = ',') -> std::vector<std::string> {
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string token;
    while (std::getline(ss, token, delim)) {
      if (!token.empty())
        tokens.push_back(token);
    }
    return tokens;
  };

  auto to_float = [](std::string s, float &v) -> bool {
    s.erase(
        std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(),
        s.end());
    errno = 0;
    char *end = nullptr;
    v = std::strtof(s.c_str(), &end);
    return (end != s.c_str() && *end == '\0' && errno != ERANGE);
  };

  auto to_uint = [](std::string s, uint32_t &v) -> bool {
    s.erase(
        std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(),
        s.end());
    errno = 0;
    char *end = nullptr;
    unsigned long tmp = std::strtoul(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE || tmp > UINT32_MAX)
      return false;
    v = static_cast<uint32_t>(tmp);
    return true;
  };

  auto parse_u32 = [&](const char *name, uint32_t &val) -> bool {
    std::string line;
    if (!std::getline(file, line)) {
      DLOGE("ParseLDCFile: EOF reading %s in %s", name, path.c_str());
      return false;
    }
    auto fields = split(line);
    if (fields.size() < 2 || !to_uint(fields[1], val)) {
      DLOGE("ParseLDCFile: invalid value for %s in %s", name, path.c_str());
      return false;
    }
    return true;
  };

  if (!parse_u32("Version", out->version))
    return false;
  if (!parse_u32("Distortion Method", out->distortion))
    return false;
  if (!parse_u32("Width", out->eye_width))
    return false;
  if (!parse_u32("Height", out->eye_height))
    return false;
  if (!parse_u32("NumCols", out->num_cols))
    return false;
  if (!parse_u32("NumRows", out->num_rows))
    return false;
  if (!parse_u32("Center X", out->center_x))
    return false;
  if (!parse_u32("Center Y", out->center_y))
    return false;

  // Skip FOV line (XR lib doesn't need it)
  std::string line;
  if (!std::getline(file, line)) {
    DLOGE("ParseLDCFile: EOF reading FOV in %s", path.c_str());
    return false;
  }

  // LDCData: 10 floats per grid entry (Index_X/Y, RefLoc_X/Y, R/G/B_Dist_X/Y)
  struct LdcRawData {
    float Index_X, Index_Y;
    float RefLoc_X, RefLoc_Y;
    float R_Dist_X, R_Dist_Y;
    float G_Dist_X, G_Dist_Y;
    float B_Dist_X, B_Dist_Y;
  };

  out->data_width = out->num_cols * static_cast<uint32_t>(sizeof(LdcRawData));
  out->data_height = out->num_rows;

  // Find data header row
  while (std::getline(file, line)) {
    if (line.find("Index X") != std::string::npos)
      break;
  }
  if (file.eof()) {
    DLOGE("ParseLDCFile: missing LDC table header in %s", path.c_str());
    return false;
  }

  const uint64_t num_entries = static_cast<uint64_t>(out->num_rows) * out->num_cols;
  if (num_entries == 0) {
    DLOGE("ParseLDCFile: invalid grid dimensions in %s", path.c_str());
    return false;
  }

  out->data = static_cast<float *>(malloc(num_entries * sizeof(LdcRawData)));
  if (!out->data) {
    DLOGE("ParseLDCFile: malloc failed for %s", path.c_str());
    return false;
  }

  LdcRawData *ldc_array = reinterpret_cast<LdcRawData *>(out->data);
  for (uint64_t i = 0; i < num_entries; i++) {
    if (!std::getline(file, line)) {
      DLOGE("ParseLDCFile: EOF at row %" PRIu64 " in %s", i, path.c_str());
      free(out->data);
      out->data = nullptr;
      return false;
    }
    auto cols = split(line);
    if (cols.size() < 10 || !to_float(cols[0], ldc_array[i].Index_X) ||
        !to_float(cols[1], ldc_array[i].Index_Y) || !to_float(cols[2], ldc_array[i].RefLoc_X) ||
        !to_float(cols[3], ldc_array[i].RefLoc_Y) || !to_float(cols[4], ldc_array[i].R_Dist_X) ||
        !to_float(cols[5], ldc_array[i].R_Dist_Y) || !to_float(cols[6], ldc_array[i].G_Dist_X) ||
        !to_float(cols[7], ldc_array[i].G_Dist_Y) || !to_float(cols[8], ldc_array[i].B_Dist_X) ||
        !to_float(cols[9], ldc_array[i].B_Dist_Y)) {
      DLOGE("ParseLDCFile: parse error at row %" PRIu64 " in %s", i, path.c_str());
      free(out->data);
      out->data = nullptr;
      return false;
    }
  }

  DLOGI("ParseLDCFile: %s parsed (grid=%ux%u)", path.c_str(), out->num_cols, out->num_rows);
  return true;
}

// -----------------------------------------------------------------------
// LoadLdcCalibration — parse identity CSV files and call ResolveLDCCACGrid_C
// Uses identity CSV files (svrapi_lens_*_identity.csv) as the default LDC
// configuration.  Real calibration files (svrapi_lens_*.csv) are used if
// present on the device.
// (mirrors iwe_repro_impl.cpp::SetDisplayDeviceConfig LDC section)
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::LoadLdcCalibration() {
  if (!resolve_ldc_cac_grid_fn_) {
    DLOGW("LoadLdcCalibration: libLayerConversion not loaded — using dummy calibration");
    return;
  }

  const std::string ldc_base_path = "/vendor/etc/display/";
  std::string left_path = ldc_base_path + "svrapi_lens_left.csv";
  std::string right_path = ldc_base_path + "svrapi_lens_right.csv";

  auto FileExists = [](const std::string &p) -> bool {
    std::ifstream f(p);
    return f.good();
  };

  // use same identity csv for both left and right eye.
  if (!FileExists(left_path) || !FileExists(right_path)) {
    DLOGW("LoadLdcCalibration: LDC files not found, falling back to identity");
    left_path = ldc_base_path + "svrapi_lens_left_identity_380_500.csv";
    right_path = ldc_base_path + "svrapi_lens_left_identity_380_500.csv";
  }

  LdcMetadata left_md = {}, right_md = {};
  if (!ParseLDCFile(left_path, &left_md) || !ParseLDCFile(right_path, &right_md)) {
    DLOGE("LoadLdcCalibration: failed to parse LDC CSV files");
    if (left_md.data) {
      free(left_md.data);
      left_md.data = nullptr;
    }
    if (right_md.data) {
      free(right_md.data);
      right_md.data = nullptr;
    }
    return;
  }

  // ResolveLDCCACGrid_C(void*, void*, void*) — extern "C" wrapper in LayerConversion.cpp.
  // Parameters are void* to satisfy Android CFI (Control Flow Integrity): CFI validates
  // indirect calls against the declared function type, so the function-pointer type here
  // must exactly match the extern "C" declaration in LayerConversion.h.
  // The wrapper casts back to Metadata* and LCGridResult* internally.
  // ldc_grid_result_ is a local mirror of LCGridResult (same memory layout).
  using ResolveLDCCACGridFn = int (*)(void *, void *, void *);
  auto resolve_fn = reinterpret_cast<ResolveLDCCACGridFn>(resolve_ldc_cac_grid_fn_);
  int ldc_result = resolve_fn(&left_md, &right_md, &ldc_grid_result_);

  if (left_md.data) {
    free(left_md.data);
    left_md.data = nullptr;
  }
  if (right_md.data) {
    free(right_md.data);
    right_md.data = nullptr;
  }

  if (ldc_result != 0) {
    DLOGE("LoadLdcCalibration: ResolveLDCCACGrid failed rc=%d", ldc_result);
    return;
  }

  ldc_calibration_valid_ = true;
  DLOGI(
      "LoadLdcCalibration: success — grid=%ux%u distort_res=%u r_max=%.4f "
      "optical_offset L=(%.1f,%.1f) R=(%.1f,%.1f)",
      ldc_grid_result_.float_grid.grid_w, ldc_grid_result_.float_grid.grid_h,
      ldc_grid_result_.bbox_params.distor_res, ldc_grid_result_.bbox_params.reprojection_r_max,
      ldc_grid_result_.bbox_params.point[0].x, ldc_grid_result_.bbox_params.point[0].y,
      ldc_grid_result_.bbox_params.point[1].x, ldc_grid_result_.bbox_params.point[1].y);
}

// -----------------------------------------------------------------------
// InvertMatrix — Gauss-Jordan 4×4 matrix inverse
// (mirrors HWVirtualDRM::InvertMatrix in hw_virtual_drm.cpp)
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::InvertMatrix(const float input_mat[4][4], float inv_mat[4][4]) {
  static constexpr int kMatSize = 4;
  float augmented[kMatSize][2 * kMatSize];

  for (int row = 0; row < kMatSize; row++) {
    for (int col = 0; col < kMatSize; col++)
      augmented[row][col] = input_mat[row][col];
    for (int col = kMatSize; col < 2 * kMatSize; col++)
      augmented[row][col] = (row == col - kMatSize) ? 1.0f : 0.0f;
  }

  for (int pivot_row = 0; pivot_row < kMatSize; pivot_row++) {
    float diag_val = augmented[pivot_row][pivot_row];
    if (fabsf(diag_val) < 1e-6f) {
      DLOGW("InvertMatrix: singular matrix");
      return false;
    }
    for (int col = 0; col < 2 * kMatSize; col++)
      augmented[pivot_row][col] /= diag_val;
    for (int row = 0; row < kMatSize; row++) {
      if (row == pivot_row)
        continue;
      float row_factor = augmented[row][pivot_row];
      for (int col = 0; col < 2 * kMatSize; col++)
        augmented[row][col] -= row_factor * augmented[pivot_row][col];
    }
  }

  for (int row = 0; row < kMatSize; row++)
    for (int col = 0; col < kMatSize; col++)
      inv_mat[row][col] = augmented[row][col + kMatSize];
  return true;
}

// -----------------------------------------------------------------------
// PopulateProjMatrices — proj/inv-proj matrices, gamma LUT, rotation
// (mirrors hw_virtual_drm.cpp::ProgramDisplayDeviceConfig)
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::PopulateProjMatrices(GpuReprojStaticConfig &cfg,
                                                      const SDMDisplayDeviceConfig *dcfg) {
  static const float kIdentity[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

  if (!dcfg) {
    memcpy(cfg.proj_matrix_left, kIdentity, sizeof(kIdentity));
    memcpy(cfg.proj_matrix_right, kIdentity, sizeof(kIdentity));
    memcpy(cfg.inv_proj_matrix_left, kIdentity, sizeof(kIdentity));
    memcpy(cfg.inv_proj_matrix_right, kIdentity, sizeof(kIdentity));
    for (int i = 0; i < 256; i++)
      cfg.gamma_lut[i] = static_cast<float>(i) / 255.0f;
    return;
  }

  // Forward projection matrices
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      cfg.proj_matrix_left[r * 4 + c] = dcfg->projectionMatrix[0].prjMatrix[r][c];
      cfg.proj_matrix_right[r * 4 + c] = dcfg->projectionMatrix[1].prjMatrix[r][c];
    }

  // Inverse projection matrices
  float inv_mat_left[4][4] = {}, inv_mat_right[4][4] = {};
  if (!InvertMatrix(dcfg->projectionMatrix[0].prjMatrix, inv_mat_left)) {
    DLOGW("PopulateProjMatrices: proj_matrix_left singular — using identity inverse");
    memcpy(inv_mat_left, kIdentity, sizeof(kIdentity));
  }
  if (!InvertMatrix(dcfg->projectionMatrix[1].prjMatrix, inv_mat_right)) {
    DLOGW("PopulateProjMatrices: proj_matrix_right singular — using identity inverse");
    memcpy(inv_mat_right, kIdentity, sizeof(kIdentity));
  }
  memcpy(cfg.inv_proj_matrix_left, inv_mat_left, sizeof(inv_mat_left));
  memcpy(cfg.inv_proj_matrix_right, inv_mat_right, sizeof(inv_mat_right));

  // Gamma LUT: copy gamma[256] from SDMDisplayDeviceConfig
  // (hw_virtual_drm.cpp replicates to float[6][256] for DRM; GPU lib handles FSC internally)
  memcpy(cfg.gamma_lut, dcfg->gamma, sizeof(cfg.gamma_lut));

  // Per-eye orientation quaternions {x,y,z,w}
  cfg.rotation_left[0] = dcfg->rotation[0].x;
  cfg.rotation_left[1] = dcfg->rotation[0].y;
  cfg.rotation_left[2] = dcfg->rotation[0].z;
  cfg.rotation_left[3] = dcfg->rotation[0].w;
  cfg.rotation_right[0] = dcfg->rotation[1].x;
  cfg.rotation_right[1] = dcfg->rotation[1].y;
  cfg.rotation_right[2] = dcfg->rotation[1].z;
  cfg.rotation_right[3] = dcfg->rotation[1].w;
}

// -----------------------------------------------------------------------
// PopulateLdcCalibration — sparse grid, radial LUT, optical params
// (mirrors iwe_repro_impl.cpp::SetDisplayDeviceConfig LDC section)
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::PopulateLdcCalibration(GpuReprojStaticConfig &cfg) {
  if (!ldc_calibration_valid_) {
    DLOGW("PopulateLdcCalibration: LDC calibration not available — fields left empty");
    return;
  }

  // Bonsai uses the GPU path exclusively — always use FloatLensOut::dist_grid.
  // dist_grid is a flat contiguous float buffer: plane p at p * (grid_w * grid_h * 2).
  // GpuReprojOpaqueConfig::data is uint64_t (opaque pointer field); the underlying
  // data is float* cast to uint64_t for storage in the opaque config struct.
  const auto &fg = ldc_grid_result_.float_grid;
  cfg.reproj_sparse_grid.size = fg.dist_grid.size() * sizeof(float);
  cfg.reproj_sparse_grid.data = reinterpret_cast<uint64_t>(fg.dist_grid.data());
  cfg.reproj_sparse_grid.crc = 0;
  cfg.reproj_sparse_grid.flags = 0;
  cfg.reproj_grid_w = fg.grid_w;
  cfg.reproj_grid_h = fg.grid_h;

  // Radial distortion LUT and optical params from BBOxOut.
  const auto &bbox = ldc_grid_result_.bbox_params;
  cfg.reproj_radial_dis_grid.size = sizeof(bbox.radial_distortion_grid);
  cfg.reproj_radial_dis_grid.data = reinterpret_cast<uint64_t>(bbox.radial_distortion_grid);
  cfg.reproj_radial_dis_grid.crc = 0;
  cfg.reproj_radial_dis_grid.flags = 0;
  cfg.distort_resolution = bbox.distor_res;

  // Optical axis offset per eye (BBOxOut::point[2])
  cfg.reproj_optical_axis_offset.points[0].x = static_cast<uint32_t>(bbox.point[0].x);
  cfg.reproj_optical_axis_offset.points[0].y = static_cast<uint32_t>(bbox.point[0].y);
  cfg.reproj_optical_axis_offset.points[1].x = static_cast<uint32_t>(bbox.point[1].x);
  cfg.reproj_optical_axis_offset.points[1].y = static_cast<uint32_t>(bbox.point[1].y);

  // Distortion parameters
  cfg.reproj_r_max = bbox.reprojection_r_max;
  cfg.reproj_error_tol = bbox.errortol;
  cfg.reproj_tol_rgb_left = bbox.reproj_tol_rgb_left;
  cfg.reproj_tol_rgb_right = bbox.reproj_tol_rgb_right;

  DLOGI(
      "PopulateLdcCalibration: grid=%ux%u distort_res=%u r_max=%.4f "
      "tol_rgb=(%.4f,%.4f) error_tol=%u",
      cfg.reproj_grid_w, cfg.reproj_grid_h, cfg.distort_resolution, cfg.reproj_r_max,
      cfg.reproj_tol_rgb_left, cfg.reproj_tol_rgb_right, cfg.reproj_error_tol);
}

// -----------------------------------------------------------------------
// PopulateDisplayDimensions — per-eye buffer width/height
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::PopulateDisplayDimensions(GpuReprojStaticConfig &cfg) {
  if (reproj_bufs_allocated_) {
    cfg.reproj_disp_im_width = reproj_out_[0][0].alloc_buffer_info.aligned_width;
    cfg.reproj_disp_im_height = reproj_out_[0][0].alloc_buffer_info.aligned_height;
  } else {
    DisplayConfigVariableInfo fb_cfg = {};
    if (display_intf_->GetFrameBufferConfig(&fb_cfg) == kErrorNone) {
      cfg.reproj_disp_im_width = fb_cfg.x_pixels / 2;
      cfg.reproj_disp_im_height = fb_cfg.y_pixels;
    }
  }
}

// -----------------------------------------------------------------------
// SetStaticConfig — orchestrates PopulateProjMatrices / PopulateLdcCalibration
//                   / PopulateDisplayDimensions then calls setStaticConfig
// -----------------------------------------------------------------------

void SDMDisplayBuiltInGpuReproj::SetStaticConfig(const SDMDisplayDeviceConfig *device_cfg) {
  if (!gpu_reproj_)
    return;

  if (device_cfg) {
    reproj_device_cfg_ = *device_cfg;
    reproj_device_cfg_set_ = true;
    DLOGI(
        "SetStaticConfig: projL[0][0]=%.4f projR[0][0]=%.4f "
        "gamma[0]=%.3f gamma[255]=%.3f "
        "rotL=(%.4f,%.4f,%.4f,%.4f) rotR=(%.4f,%.4f,%.4f,%.4f) calib=%s",
        device_cfg->projectionMatrix[0].prjMatrix[0][0],
        device_cfg->projectionMatrix[1].prjMatrix[0][0], device_cfg->gamma[0],
        device_cfg->gamma[255], device_cfg->rotation[0].x, device_cfg->rotation[0].y,
        device_cfg->rotation[0].z, device_cfg->rotation[0].w, device_cfg->rotation[1].x,
        device_cfg->rotation[1].y, device_cfg->rotation[1].z, device_cfg->rotation[1].w,
        device_cfg->calibrationFileStr);

    // Load LDC calibration via ResolveLDCCACGrid_C using identity CSV files
    // (or real calibration files if present on the device).
    LoadLdcCalibration();
  }

  const SDMDisplayDeviceConfig *dcfg =
      device_cfg ? device_cfg : (reproj_device_cfg_set_ ? &reproj_device_cfg_ : nullptr);

  GpuReprojStaticConfig cfg = {};
  PopulateProjMatrices(cfg, dcfg);
  PopulateLdcCalibration(cfg);
  PopulateDisplayDimensions(cfg);

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
// ConvertLayerParams — convert layer geometry via LayerConversion lib.
//
// Mirrors iwe_repro_impl.cpp::GetProjectionLayers():
//   PROJECTION → ConvertProjectionLayerParameters_C  (computes correct BBox)
//   QUAD       → ConvertQuadInfoToProjectionInfo_C   (world-space quad → projection)
//   ROI        → ConvertROILayerToProjectionLayer_C  (2D ROI → projection via proj matrix)
//
// On success, updates p->layer_pos, p->layer_orient, p->frustum, p->plane_eq,
// and p->src_rect with the converted DCP values and returns true.
// Returns false if the LC lib is unavailable or the layer type is unhandled
// (caller keeps the raw params already set).
//
// Unit conventions (mirrors iwe_repro_impl.cpp):
//   SDM layer_frustum  — degrees
//   LC FoV input       — radians  (converted here: degrees × kDegToRad)
//   LC FoV output      — radians  (converted back: radians × kRadToDeg)
//   GpuReprojLayerParams.frustum — degrees (same as SDM interface)
// -----------------------------------------------------------------------

bool SDMDisplayBuiltInGpuReproj::ConvertLayerParams(const Layer *layer, GpuReprojLayerParams *p) {
  if (!layer || !p)
    return false;

  // Function-pointer types — must exactly match the extern "C" declarations
  // in LayerConversion.cpp so that Android CFI validation passes.
  using ConvertLayerFn = void (*)(void *, void *);

  static constexpr float kDegToRad = 0.0174533f;  // π/180
  static constexpr float kRadToDeg = 57.295779f;  // 180/π

  LcDCPProjectionLayer dcp = {};
  bool converted = false;

  // ---- PROJECTION layer ----
  // Mirrors iwe_repro_impl.cpp::GetProjectionLayers() dispatch logic exactly.
  if (layer->comp_layer_type == COMPOSITION_LAYER_PROJECTION && convert_proj_layer_fn_) {
    LcProjectionLayerInfo info = {};

    // Pose: SDMLayerPose → LcPose (snake_case members per SDM style)
    info.pose.position = {layer->layer_pose.pos.x, layer->layer_pose.pos.y,
                          layer->layer_pose.pos.z};
    // SDMLayerOrientation stores {x,y,z,w}; LcQuat is {w,x,y,z}
    info.pose.orientation = {layer->layer_pose.orientation.w, layer->layer_pose.orientation.x,
                             layer->layer_pose.orientation.y, layer->layer_pose.orientation.z};

    // FoV: degrees → radians.  SDMLayerFrustum: angleLeft/Right/Up/Down.
    // LcVec4 aliases: x=Left, y=Right, z=Bottom, w=Top.
    info.fov.x = layer->layer_frustum.angleLeft * kDegToRad;   // Left
    info.fov.y = layer->layer_frustum.angleRight * kDegToRad;  // Right
    info.fov.z = layer->layer_frustum.angleDown * kDegToRad;   // Bottom
    info.fov.w = layer->layer_frustum.angleUp * kDegToRad;     // Top

    // img: full swapchain dimensions (LocX=0, LocY=0, Width, Height)
    info.img.x = 0.0f;
    info.img.y = 0.0f;
    info.img.z = static_cast<float>(layer->input_buffer.unaligned_width);
    info.img.w = static_cast<float>(layer->input_buffer.unaligned_height);

    // sub_img: source crop (left, top, width, height)
    info.sub_img.x = layer->src_rect.left;
    info.sub_img.y = layer->src_rect.top;
    info.sub_img.z = layer->src_rect.right - layer->src_rect.left;
    info.sub_img.w = layer->src_rect.bottom - layer->src_rect.top;

    // plane_eqn: a, b, c, d
    info.plane_eqn.x = layer->plane_equation.a;
    info.plane_eqn.y = layer->plane_equation.b;
    info.plane_eqn.z = layer->plane_equation.c;
    info.plane_eqn.w = layer->plane_equation.d;

    auto fn = reinterpret_cast<ConvertLayerFn>(convert_proj_layer_fn_);
    fn(&info, &dcp);
    converted = true;

    // ---- QUAD layer ----
    // Guard: only convert if quad_size is non-zero (mirrors iwe_repro_impl.cpp condition).
  } else if (layer->comp_layer_type == COMPOSITION_LAYER_QUAD && convert_quad_info_fn_ &&
             layer->layer_quad_size.width != 0 && layer->layer_quad_size.height != 0) {
    LcQuadLayerInfo info = {};

    info.pose.position = {layer->layer_pose.pos.x, layer->layer_pose.pos.y,
                          layer->layer_pose.pos.z};
    info.pose.orientation = {layer->layer_pose.orientation.w, layer->layer_pose.orientation.x,
                             layer->layer_pose.orientation.y, layer->layer_pose.orientation.z};

    info.quad_size.width = layer->layer_quad_size.width;
    info.quad_size.height = layer->layer_quad_size.height;

    info.img.x = 0.0f;
    info.img.y = 0.0f;
    info.img.z = static_cast<float>(layer->input_buffer.unaligned_width);
    info.img.w = static_cast<float>(layer->input_buffer.unaligned_height);

    info.sub_img.x = layer->src_rect.left;
    info.sub_img.y = layer->src_rect.top;
    info.sub_img.z = layer->src_rect.right - layer->src_rect.left;
    info.sub_img.w = layer->src_rect.bottom - layer->src_rect.top;

    auto fn = reinterpret_cast<ConvertLayerFn>(convert_quad_info_fn_);
    fn(&info, &dcp);
    converted = true;

    // ---- ROI layer (fallback) ----
  } else if (convert_roi_layer_fn_ && reproj_device_cfg_set_) {
    LcROILayerInfo info = {};

    // disp_image: per-eye display buffer dimensions
    // Mirrors iwe_repro_impl.cpp: session_->buf_info.buffer_config.width/height
    uint32_t disp_w = 0, disp_h = 0;
    if (reproj_bufs_allocated_) {
      disp_w = reproj_out_[0][0].buffer_config.width;
      disp_h = reproj_out_[0][0].buffer_config.height;
    } else {
      DisplayConfigVariableInfo fb_cfg = {};
      if (display_intf_->GetFrameBufferConfig(&fb_cfg) == kErrorNone) {
        disp_w = fb_cfg.x_pixels / 2;
        disp_h = fb_cfg.y_pixels;
      }
    }
    info.disp_image.x = 0.0f;
    info.disp_image.y = 0.0f;
    info.disp_image.z = static_cast<float>(disp_w);
    info.disp_image.w = static_cast<float>(disp_h);

    // disp_roi: destination ROI on the display (LocX, LocY, Width, Height).
    // Mirrors iwe_repro_impl.cpp::GetDCPForROILayer() exactly:
    // For the right eye, dst_rect.left is offset by per-eye width — subtract it back
    // so DispROI.LocX is relative to the per-eye display origin.
    float disp_roi_loc_x = layer->dst_rect.left;
    if (layer->dst_rect.left >= static_cast<float>(disp_w)) {
      disp_roi_loc_x = layer->dst_rect.left - static_cast<float>(disp_w);
    }
    info.disp_roi.x = disp_roi_loc_x;
    info.disp_roi.y = layer->dst_rect.top;
    info.disp_roi.z = layer->dst_rect.right - layer->dst_rect.left;
    info.disp_roi.w = layer->dst_rect.bottom - layer->dst_rect.top;

    // rendr_image: render image dimensions
    info.rendr_image.x = 0.0f;
    info.rendr_image.y = 0.0f;
    info.rendr_image.z = static_cast<float>(layer->input_buffer.unaligned_width);
    info.rendr_image.w = static_cast<float>(layer->input_buffer.unaligned_height);

    // rendr_roi: source ROI in the render image (LocX, LocY, Width, Height)
    info.rendr_roi.x = layer->src_rect.left;
    info.rendr_roi.y = layer->src_rect.top;
    info.rendr_roi.z = layer->src_rect.right - layer->src_rect.left;
    info.rendr_roi.w = layer->src_rect.bottom - layer->src_rect.top;

    // disp_proj_mat: select eye based on layer visibility.
    // Mirrors iwe_repro_impl.cpp::GetDCPForROILayer() exactly:
    //   LEFT_EYE  → projectionMatrix[0]
    //   RIGHT_EYE → projectionMatrix[1]
    //   other     → invalid, skip conversion
    // Matrix copy: DispProjMat[r][c] = prjMatrix[r][c] (same index convention as IWE).
    // In GLM col-major: col[r][c] = column r, row c — matches DispProjMat[r][c].
    const SDMDisplayProjectionMatrix *proj_mat = nullptr;
    if (layer->layer_visibility_type == LAYER_VISIBILITY_LEFT_EYE) {
      proj_mat = &reproj_device_cfg_.projectionMatrix[0];
    } else if (layer->layer_visibility_type == LAYER_VISIBILITY_RIGHT_EYE) {
      proj_mat = &reproj_device_cfg_.projectionMatrix[1];
    }
    if (!proj_mat) {
      DLOGE("ConvertLayerParams: invalid layer visibility type %d for ROI layer",
            static_cast<int>(layer->layer_visibility_type));
      return false;
    }
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        info.disp_proj_mat.col[r][c] = proj_mat->prjMatrix[r][c];

    auto fn = reinterpret_cast<ConvertLayerFn>(convert_roi_layer_fn_);
    fn(&info, &dcp);
    converted = true;
  }

  if (!converted)
    return false;

  // ---- Apply converted DCP values to GpuReprojLayerParams ----

  // Pose: LcPose → layer_pos / layer_orient
  p->layer_pos[0] = dcp.pose.position.x;
  p->layer_pos[1] = dcp.pose.position.y;
  p->layer_pos[2] = dcp.pose.position.z;
  // LcQuat is {w,x,y,z}; GpuReprojLayerParams.layer_orient is {x,y,z,w}
  p->layer_orient[0] = dcp.pose.orientation.x;
  p->layer_orient[1] = dcp.pose.orientation.y;
  p->layer_orient[2] = dcp.pose.orientation.z;
  p->layer_orient[3] = dcp.pose.orientation.w;

  // FoV: radians → degrees.  LcVec4: x=Left, y=Right, z=Bottom, w=Top.
  // GpuReprojLayerParams.frustum: [0]=Left, [1]=Right, [2]=Up, [3]=Down.
  p->frustum[0] = dcp.fov.x * kRadToDeg;  // Left
  p->frustum[1] = dcp.fov.y * kRadToDeg;  // Right
  p->frustum[2] = dcp.fov.w * kRadToDeg;  // Up  (Top)
  p->frustum[3] = dcp.fov.z * kRadToDeg;  // Down (Bottom)

  // plane_eqn: a=x, b=y, c=z, d=w
  p->plane_eq[0] = dcp.plane_eqn.x;
  p->plane_eq[1] = dcp.plane_eqn.y;
  p->plane_eq[2] = dcp.plane_eqn.z;
  p->plane_eq[3] = dcp.plane_eqn.w;

  // bbox → src_rect: LcVec4 stores (LocX, LocY, Width, Height).
  // GpuReprojLayerParams.src_rect is {left, top, right, bottom}.
  p->src_rect[0] = dcp.bbox.x;               // left  = LocX
  p->src_rect[1] = dcp.bbox.y;               // top   = LocY
  p->src_rect[2] = dcp.bbox.x + dcp.bbox.z;  // right = LocX + Width
  p->src_rect[3] = dcp.bbox.y + dcp.bbox.w;  // bottom= LocY + Height

  return true;
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

    // Apply LayerConversion geometry transformation (mirrors iwe_repro_impl.cpp).
    // ConvertLayerParams() updates pose, FoV, plane_eq, and src_rect in-place.
    // Falls back silently to raw params if LC lib is unavailable.
    ConvertLayerParams(layer, &p);

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
  // On Bonsai the path is SDM → GPU directly (no IWE/virtual display chain).
  // display_intf_->SetDisplayDeviceConfig is not needed here.
  if (IsGpuReprojEnabled()) {
    SetStaticConfig(&sdm_display_device_config);
  }
  return kErrorNone;
}

}  // namespace sdm
