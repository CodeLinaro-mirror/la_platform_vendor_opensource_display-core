// Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "SnapUtils.h"

uint64_t GetPixelFormatModifier(BufferDescriptor desc) {
  for (auto type : desc.additionalOptions) {
    // TODO: use a versioned string
    if (std::strcmp(type.key, "pixel_format_modifier") == 0) {
      return type.value;
    }
  }
  return 0;
}

bool CpuCanRead(SnapUsage usage) {
  return usage & SnapUsage::CPU_READ_MASK;
}

bool CpuCanWrite(SnapUsage usage) {
  return usage & SnapUsage::CPU_WRITE_MASK;
}

bool CpuCanAccess(SnapUsage usage) {
  return CpuCanRead(usage) || CpuCanWrite(usage);
}

// TODO: read this from formats.json

[[clang::no_destroy]] static std::unordered_map<SnapPixelFormat, FormatTraits> format_traits_map{
    // {{Format},{rgb,yuv,tile rendered, gpu depth stencil, astc,
    // ubwc_supported, width_even, height_even}}
    {{SnapPixelFormat::RGBA_8888}, {true, false, false, false, false, true, false, false}},
    {{SnapPixelFormat::RGBX_8888}, {true, false, false, false, false, true, false, false}},
    {{SnapPixelFormat::RGBA_FP16}, {true, false, false, false, false, true, false, false}},
    {{SnapPixelFormat::YCBCR_P010}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::BGRA_8888}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RGB_888}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::YCbCr_420_SP}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::NV21_ZSL}, {false, true, false, false, false, false, false, false}},
    {{SnapPixelFormat::YCrCb_420_SP}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::TP10}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::RGB_565}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::YV12}, {false, true, false, false, false, false, true, true}},
    {{SnapPixelFormat::R_8}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RGBA_1010102}, {true, false, false, false, false, true, false, false}},
    {{SnapPixelFormat::BGR_565}, {true, false, false, false, false, true, false, false}},
    {{SnapPixelFormat::RG_88}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RAW8}, {false, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RAW10}, {false, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RAW12}, {false, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RAW14}, {false, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RAW16}, {false, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::DEPTH_16}, {false, false, true, true, false, true, false, false}},
    {{SnapPixelFormat::DEPTH_24}, {false, false, true, true, false, true, false, false}},
    {{SnapPixelFormat::DEPTH_24_STENCIL_8}, {false, false, true, true, false, false, false}},
    {{SnapPixelFormat::DEPTH_32F}, {false, false, true, true, false, true, false, false}},
    {{SnapPixelFormat::DEPTH_32F_STENCIL_8}, {false, false, true, true, false, true, false, false}},
    {{SnapPixelFormat::STENCIL_8}, {false, false, true, true, false, true, false, false}},
    {{SnapPixelFormat::BLOB}, {false, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::YCBCR_422_SP}, {false, true, false, false, false, false, true, false}},
    {{SnapPixelFormat::YCBCR_422_I}, {false, true, false, false, false, false, true, false}},
    {{SnapPixelFormat::CbYCrY_422_I}, {false, true, false, false, false, false, true, false}},
    {{SnapPixelFormat::YCrCb_422_SP}, {false, true, false, false, false, false, true, false}},
    {{SnapPixelFormat::YCBCR_420_888}, {false, true, false, false, false, false, false, false}},
    {{SnapPixelFormat::YCrCb_422_I}, {false, true, false, false, false, false, true, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_4x4_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_5x4_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_5x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_6x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_6x6_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_8x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_8x6_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_8x8_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_10x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_10x6_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_10x8_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_10x10_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_12x10_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_RGBA_ASTC_12x12_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_5x4_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_5x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_6x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_6x6_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_8x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_8x6_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_8x8_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_10x5_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_10x6_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_10x8_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_10x10_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_12x10_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::COMPRESSED_SRGB8_ALPHA8_ASTC_12x12_KHR},
     {true, false, false, false, true, false, false, false}},
    {{SnapPixelFormat::RGBA_4444}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::RGBA_5551}, {true, false, false, false, false, false, false, false}},
    {{SnapPixelFormat::Y16}, {false, true, false, false, false, false, false, false}},
    {{SnapPixelFormat::YCBCR_P210}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::NV12_UBWC_MIPMAP}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::NV12_MIPMAP}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::TP10_UBWC_MIPMAP}, {false, true, false, false, false, true, false, false}},
    {{SnapPixelFormat::P010_MIPMAP}, {false, true, false, false, false, true, false, false}},
};

bool IsUbwcSupported(SnapPixelFormat format) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.ubwc_supported) {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool IsTileRendered(SnapPixelFormat format) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.tile_rendered) {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool IsAstc(SnapPixelFormat format) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.astc) {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool IsRgb(SnapPixelFormat format) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.rgb) {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool IsYuv(SnapPixelFormat format) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.yuv) {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool IsGpuDepthStencil(SnapPixelFormat format) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.gpu_depth_stencil) {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool CheckWidthConstraints(SnapPixelFormat format, int width) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.width_even) {
      if (width & 1) {
        DLOGE("Width is odd for format %lu", static_cast<uint64_t>(format));
        return false;
      } else {
        return true;
      }
    } else {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool CheckHeightConstraints(SnapPixelFormat format, int height) {
  auto format_traits = format_traits_map.find(format);
  if (format_traits != format_traits_map.end()) {
    if (format_traits->second.height_even) {
      if (height & 1) {
        DLOGE("Height is odd for format %lu", static_cast<uint64_t>(format));
        return false;
      } else {
        return true;
      }
    } else {
      return true;
    }
  } else {
    DLOGW("Format %lu not found in format traits map", static_cast<uint64_t>(format));
  }
  return false;
}

bool IsCameraCustomFormat(SnapPixelFormat format, SnapPixelFormatModifier modifier) {
  if ((modifier == SnapPixelFormatModifier::PIXEL_FORMAT_MODIFIER_MIPMAP) ||
      (modifier == SnapPixelFormatModifier::PIXEL_FORMAT_MODIFIER_UBWC_MIPMAP)) {
    return true;
  }
  return false;
}

int GetBatchSize(vendor_qti_hardware_display_common_PixelFormatModifier modifier) {
  int batchsize = 1;
  switch (modifier) {
    case PIXEL_FORMAT_MODIFIER_UBWC_FLEX:
      batchsize = 16;
      break;
    case PIXEL_FORMAT_MODIFIER_UBWC_FLEX_2_BATCH:
      batchsize = 2;
      break;
    case PIXEL_FORMAT_MODIFIER_UBWC_FLEX_4_BATCH:
      batchsize = 4;
      break;
    case PIXEL_FORMAT_MODIFIER_UBWC_FLEX_8_BATCH:
      batchsize = 8;
      break;
    default:
      break;
  }
  return batchsize;
}