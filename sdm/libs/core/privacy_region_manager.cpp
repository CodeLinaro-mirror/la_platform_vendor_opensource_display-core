/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <inttypes.h>
#include "privacy_region_manager.h"

#include <utils/debug.h>
#include <utils/utils.h>
#include <utils/soc_info.h>
#include <tinyxml2.h>

#define __CLASS__ "PrivacyRegionMgr"
#define PRIVACY_REGIONS_XML_FILE "/vendor/etc/display/privacy_regions_offsets.xml"

using tinyxml2::XML_SUCCESS;
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

namespace sdm {

PrivacyRegionManager::PrivacyRegionManager(uint32_t max_privacy_regions, PrivacyRegionMode mode)
    : max_privacy_regions_(max_privacy_regions), mode_(mode) {
  if (LoadPrivacyRegionsOffsetsFromFile() != kErrorNone) {
    DLOGW("Failed to load privacy region offsets from %s", PRIVACY_REGIONS_XML_FILE);
  }
}

DisplayError PrivacyRegionManager::LoadPrivacyRegionsOffsetsFromFile() {
  DisplayError error = kErrorNone;
  const char *soc_name = GetSocName();
  string offsets_xml_path = string(PRIVACY_REGIONS_XML_FILE);

#ifndef SDM_VIRTUAL_DRIVER
  if ((soc_name == NULL) || (soc_name[0] == '\0')) {
    DLOGW("Unable to retrieve SOC name");
    return kErrorParameters;
  }
#else
  soc_name = kCanoeSocName;
  const char *kXmlPath = getenv("DISPLAY_CORE_CONFIG_PATH");
  offsets_xml_path = string(kXmlPath) + "privacy_regions_offsets.xml";
#endif

  XMLDocument doc;

  if (doc.LoadFile(offsets_xml_path.c_str()) != XML_SUCCESS) {
    DLOGW("Failed to load xml file: %s", offsets_xml_path.c_str());
    return kErrorParameters;
  }

  XMLElement *root = doc.RootElement();
  if (root == nullptr) {
    DLOGW("Root from %s is not found", offsets_xml_path.c_str());
    return kErrorParameters;
  }

  XMLElement *target = root->FirstChildElement("Target");
  if (target == nullptr) {
    DLOGW("No target specified in %s", offsets_xml_path.c_str());
    return kErrorParameters;
  }

  while (target != nullptr) {
    const char *target_name = target->Attribute("name");
    if (target_name == nullptr) {
      continue;
    }

    if (!strcmp(target_name, soc_name)) {
      XMLElement *panel_resolution_node = target->FirstChildElement("MixerResolution");
      while (panel_resolution_node != nullptr) {
        const char *str_width = panel_resolution_node->Attribute("width");
        const char *str_height = panel_resolution_node->Attribute("height");
        if (str_width == nullptr || str_height == nullptr) {
          continue;
        }

        const uint32_t width = std::atoi(str_width);
        const uint32_t height = std::atoi(str_height);

        XMLElement *offsets_node = panel_resolution_node->FirstChildElement("Offsets");
        if (offsets_node != nullptr) {
          const char *str_corner_radius = offsets_node->Attribute("corner_radius");
          const char *str_left = offsets_node->Attribute("left");
          const char *str_top = offsets_node->Attribute("top");
          const char *str_right = offsets_node->Attribute("right");
          const char *str_bottom = offsets_node->Attribute("bottom");

          if (str_corner_radius == nullptr || str_left == nullptr || str_top == nullptr ||
              str_right == nullptr || str_bottom == nullptr) {
            continue;
          }

          const float corner_radius = std::atof(str_corner_radius);
          const int left = std::atoi(str_left);
          const int top = std::atoi(str_top);
          const int right = std::atoi(str_right);
          const int bottom = std::atoi(str_bottom);

          DLOGI_IF(kTagDisplay, "Mixer Resolution:%dx%d Offsets:%f %d %d %d %d", width, height,
                   corner_radius, left, top, right, bottom);

          Resolution res = {width, height};
          SDMRect rect_offsets = {left, top, right, bottom};
          PrivacyRegion region_offsets = {corner_radius, rect_offsets};
          privacy_regions_offsets_.emplace(res, region_offsets);
        }
        panel_resolution_node = panel_resolution_node->NextSiblingElement("MixerResolution");
      }

      XMLElement *spatial_dimming_node = target->FirstChildElement("DimmingWidth");
      if (spatial_dimming_node != nullptr &&
          spatial_dimming_node->QueryIntText(&spatial_dimming_width_) == XML_SUCCESS) {
        DLOGI_IF(kTagDisplay, "Spatial dimming:%d", spatial_dimming_width_);
      }
    }
    target = target->NextSiblingElement("Target");
  }
  return error;
}

SDMRect PrivacyRegionManager::unionRect(SDMRect a, SDMRect b) {
  return SDMRect{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right),
                 std::max(a.bottom, b.bottom)};
}

bool PrivacyRegionManager::isEmptyRegion(SDMRect rect) {
  return (rect.left <= 0 && rect.top <= 0 && rect.right <= 0 && rect.bottom <= 0);
}

DisplayError PrivacyRegionManager::ConfigurePrivacyRegions(DispLayerStack *disp_layer_stack,
                                                           const DisplayClientContext &client_ctx,
                                                           bool mixer_resolution_updated,
                                                           std::vector<PrivacyRegion> *regions) {
  DTRACE_SCOPED();
  if (max_privacy_regions_ <= 0) {
    return kErrorNotSupported;
  }

  bool regions_updated = disp_layer_stack->stack->flags.privacy_regions_updated;
  bool privacy_filter_active = (num_privacy_regions_ > 0);

  if (!regions_updated && !privacy_filter_active) {
    // Keep privacy filter disabled
    return kErrorNone;
  }

  // If the PRs aren't updated but privacy filter is active and LM is updated, reconfigure the PRs
  if (!regions_updated && privacy_filter_active && !mixer_resolution_updated) {
    return kErrorNone;
  }

  for (auto &[id, info] : disp_layer_stack->info) {
    info.privacy_region_mode = mode_;
  }

  uint32_t display_width = client_ctx.display_attributes.x_pixels;
  uint32_t display_height = client_ctx.display_attributes.y_pixels;
  uint32_t mixer_width = client_ctx.mixer_attributes.width;
  uint32_t mixer_height = client_ctx.mixer_attributes.height;
  Resolution panel_res = Resolution{display_width, display_height};
  Resolution mixer_res = Resolution{mixer_width, mixer_height};

  std::vector<PrivacyRegion> consolidated_regions = {};

  ApplyCollapsing(disp_layer_stack, consolidated_regions, mixer_res);
  ApplyScaling(consolidated_regions, panel_res, mixer_res);
  ApplyOffsets(consolidated_regions, panel_res, mixer_res);

  *regions = consolidated_regions;
  num_privacy_regions_ = consolidated_regions.size();
  return kErrorNeedsCommit;
}

void PrivacyRegionManager::ApplyCollapsing(const DispLayerStack *disp_layer_stack,
                                           std::vector<PrivacyRegion> &consolidated_regions,
                                           Resolution &mixer_res) {
  if (mode_ == PrivacyRegionMode::LAYER) {
    LayerModeCollapsing(disp_layer_stack, consolidated_regions, mixer_res);
  } else if (mode_ == PrivacyRegionMode::AREA) {
    AreaModeCollapsing(disp_layer_stack, consolidated_regions, mixer_res);
  }
}

void PrivacyRegionManager::ApplyScaling(std::vector<PrivacyRegion> &consolidated_regions,
                                        Resolution &panel_res, Resolution &mixer_res) {
  if (panel_res.x_pixels < mixer_res.x_pixels || panel_res.y_pixels < mixer_res.y_pixels) {
    DLOGI_IF(kTagDisplay, "LM resolution exceeds panel's resolution");
    return;
  }

  // Adjust the privacy regions to match the panel resolution
  if (panel_res.x_pixels != mixer_res.x_pixels && panel_res.y_pixels != mixer_res.y_pixels) {
    DLOGI_IF(kTagDisplay, "Display %dx%d and LM Resolutions %dx%d are different",
             panel_res.x_pixels, panel_res.y_pixels, mixer_res.x_pixels, mixer_res.y_pixels);
    for (size_t i = 0; i < consolidated_regions.size(); i++) {
      PrivacyRegion region = consolidated_regions.at(i);
      LayerRect src_domain = {0.0f, 0.0f, FLOAT(mixer_res.x_pixels), FLOAT(mixer_res.y_pixels)};
      LayerRect dst_domain = {0.0f, 0.0f, FLOAT(panel_res.x_pixels), FLOAT(panel_res.y_pixels)};
      LayerRect privacy_rect = {FLOAT(region.rect.left), FLOAT(region.rect.top),
                                FLOAT(region.rect.right), FLOAT(region.rect.bottom)};
      LayerRect privacy_dst_rect = {};
      float corner_radius =
          region.corner_radius * (FLOAT(panel_res.x_pixels) / FLOAT(mixer_res.x_pixels));
      MapRect(src_domain, dst_domain, privacy_rect, &privacy_dst_rect);

      DLOGI_IF(kTagDisplay, "PrivacyRegions with scaling [radius:%.2f rect:%f %f %f %f]",
               corner_radius, privacy_dst_rect.left, privacy_dst_rect.top, privacy_dst_rect.right,
               privacy_dst_rect.bottom);

      SDMRect rect = {INT(privacy_dst_rect.left), INT(privacy_dst_rect.top),
                      INT(privacy_dst_rect.right), INT(privacy_dst_rect.bottom)};

      consolidated_regions.at(i) = PrivacyRegion{corner_radius, rect, region.index};
    }
  }
}

void PrivacyRegionManager::ApplyOffsets(std::vector<PrivacyRegion> &consolidated_regions,
                                        Resolution &panel_res, Resolution &mixer_res) {
  if (mode_ == PrivacyRegionMode::LAYER) {
    LayerModeBounds(consolidated_regions, panel_res, mixer_res);
  } else if (mode_ == PrivacyRegionMode::AREA) {
    AreaModeBounds(consolidated_regions, panel_res, mixer_res);
  }
}

void PrivacyRegionManager::LayerModeCollapsing(const DispLayerStack *disp_layer_stack,
                                               std::vector<PrivacyRegion> &consolidated_regions,
                                               Resolution &mixer_res) {
  DTRACE_SCOPED();
  // Consolidate the privacy regions for each layer
  for (auto layer : disp_layer_stack->stack->layers) {
    // If a layer's privacy regions exceeds max allowed, use layer's destination rectangle as roi
    if (layer->privacy_regions.size() > max_privacy_regions_) {
      float radius = std::min(layer->corner_radius.x, layer->corner_radius.y);
      consolidated_regions.push_back(PrivacyRegion{
          radius,
          SDMRect{static_cast<int>(layer->dst_rect.left), static_cast<int>(layer->dst_rect.top),
                  static_cast<int>(layer->dst_rect.right),
                  static_cast<int>(layer->dst_rect.bottom)}});

      DLOGI_IF(kTagDisplay,
               "Layer's %" PRIu64
               " %s privacy regions %zu exceeds limit [radius:%.2f rect:%.2f %.2f %.2f %.2f]",
               layer->layer_id, layer->layer_name.c_str(), layer->privacy_regions.size(), radius,
               layer->dst_rect.left, layer->dst_rect.top, layer->dst_rect.right,
               layer->dst_rect.bottom);
    } else {
      for (auto region : layer->privacy_regions) {
        DLOGI_IF(kTagDisplay, "Layer %" PRIu64 " %s [radius:%.2f rect:%d %d %d %d]",
                 layer->layer_id, layer->layer_name.c_str(), region.corner_radius, region.rect.left,
                 region.rect.top, region.rect.right, region.rect.bottom);
        consolidated_regions.push_back(region);
      }
    }
  }

  // If total privacy regions exceeds the max allowed, use full frame as roi
  if (consolidated_regions.size() > max_privacy_regions_) {
    consolidated_regions.clear();
    consolidated_regions.push_back(
        PrivacyRegion{0.0f, SDMRect{0, 0, INT(mixer_res.x_pixels), INT(mixer_res.y_pixels)}});

    DLOGI_IF(kTagDisplay, "Frame's privacy regions %d exceeds limit [radius:%.2f rect:%d %d %d %d]",
             consolidated_regions.size(), 0.0f, 0, 0, INT(mixer_res.x_pixels),
             INT(mixer_res.y_pixels));
  }
}

void PrivacyRegionManager::AreaModeCollapsing(const DispLayerStack *disp_layer_stack,
                                              std::vector<PrivacyRegion> &consolidated_regions,
                                              Resolution &mixer_res) {
  DTRACE_SCOPED();
  int region_to_reset = 3;
  std::map<int, std::vector<PrivacyRegion>> regions_by_index = {};
  // Group the privacy regions based on index
  for (auto layer : disp_layer_stack->stack->layers) {
    for (auto region : layer->privacy_regions) {
      regions_by_index[region.index].push_back(region);
      DLOGI_IF(kTagDisplay, "Layer %" PRIu64 " on index:%d radius:%.2f rect:%d %d %d %d",
               layer->layer_id, region.index, region.corner_radius, region.rect.left,
               region.rect.top, region.rect.right, region.rect.bottom);
    }
  }

  // Combine all privacy regions for each index into a single region
  for (auto &[index, list_of_regions] : regions_by_index) {
    if (!list_of_regions.empty()) {
      float final_corner_radius = list_of_regions.at(0).corner_radius;
      SDMRect final_consolidated_rect = list_of_regions.at(0).rect;
      for (int j = 1; j < list_of_regions.size(); j++) {
        final_corner_radius = std::min(final_corner_radius, list_of_regions[j].corner_radius);
        final_consolidated_rect = unionRect(final_consolidated_rect, list_of_regions[j].rect);
      }
      consolidated_regions.push_back(
          PrivacyRegion{final_corner_radius, final_consolidated_rect, index});
      region_to_reset -= index;
      DLOGI_IF(kTagDisplay, "Collapsed PrivacyRegions [index:%d radius:%.2f rect:%d %d %d %d]",
               index, final_corner_radius, final_consolidated_rect.left,
               final_consolidated_rect.top, final_consolidated_rect.right,
               final_consolidated_rect.bottom);
    }
  }

  // Clear privacy regions for index that was active in previous round, but is now inactive
  if (region_to_reset < 3 && region_to_reset > 0 && prev_region_state_[region_to_reset - 1]) {
    PrivacyRegion region = PrivacyRegion();
    region.index = region_to_reset;
    consolidated_regions.push_back(region);
    DLOGI_IF(kTagDisplay, "Reset region index %d", region.index);
  }
}

void PrivacyRegionManager::LayerModeBounds(std::vector<PrivacyRegion> &consolidated_regions,
                                           Resolution &panel_res, Resolution &mixer_res) {
  if (privacy_regions_offsets_.find(mixer_res) != privacy_regions_offsets_.end()) {
    PrivacyRegion offsets = privacy_regions_offsets_.at(mixer_res);
    for (size_t i = 0; i < consolidated_regions.size(); i++) {
      PrivacyRegion region = consolidated_regions.at(i);

      SDMRect rect = {};
      // Avoid applying offset if L is at the edge of panel resolution, then clip L
      rect.left =
          (region.rect.left > 1) ? (region.rect.left + offsets.rect.left) : region.rect.left;
      if (rect.left <= 0) {
        rect.left = 1;
      }

      // Avoid applying offset if T is at the edge of panel resolution, then clip T
      rect.top = (region.rect.top > 1) ? (region.rect.top + offsets.rect.top) : region.rect.top;
      if (rect.top <= 0) {
        rect.top = 1;
      }

      // Avoid applying offset if R is at the edge of panel resolution, then clip R
      rect.right = (region.rect.right != panel_res.x_pixels)
                       ? (region.rect.right + offsets.rect.right)
                       : region.rect.right;
      if (rect.right > panel_res.x_pixels) {
        rect.right = panel_res.x_pixels;
      } else if (rect.right <= 0) {
        rect.right = region.rect.right;
      }

      // Avoid applying offset if B is at the edge of panel resolution, then clip B
      rect.bottom = (region.rect.bottom != panel_res.y_pixels)
                        ? (region.rect.bottom + offsets.rect.bottom)
                        : region.rect.bottom;
      if (rect.bottom > panel_res.y_pixels) {
        rect.bottom = panel_res.y_pixels;
      } else if (rect.bottom <= 0) {
        rect.bottom = region.rect.bottom;
      }

      region.corner_radius = (region.corner_radius + offsets.corner_radius);
      region.rect = rect;

      DLOGI_IF(kTagDisplay, "PrivacyRegions with offsets [radius:%.2f rect:%d %d %d %d]",
               region.corner_radius, rect.left, rect.top, rect.right, rect.bottom);

      consolidated_regions.at(i) = PrivacyRegion{region.corner_radius, rect, region.index};
    }
  }
}

void PrivacyRegionManager::AreaModeBounds(std::vector<PrivacyRegion> &consolidated_regions,
                                          Resolution &panel_res, Resolution &mixer_res) {
  PrivacyRegion *offsets = nullptr;
  if (privacy_regions_offsets_.find(mixer_res) != privacy_regions_offsets_.end()) {
    offsets = &privacy_regions_offsets_.at(mixer_res);
    DLOGI_IF(kTagDisplay, "Offsets %d %d %d %d", offsets->rect.left, offsets->rect.top,
             offsets->rect.right, offsets->rect.bottom);
  }

  int region_to_remove = 0;  // Track layers previously sent with an empty roi
  for (size_t i = 0; i < consolidated_regions.size(); i++) {
    PrivacyRegion region = consolidated_regions.at(i);
    if (offsets != nullptr) {
      region.corner_radius = (region.corner_radius + offsets->corner_radius);
      region.rect.left += offsets->rect.left;
      region.rect.top += offsets->rect.top;
      region.rect.right += offsets->rect.right;
      region.rect.bottom += offsets->rect.bottom;
    }

    if (static_cast<int>(region.corner_radius) % 2 != 0) {
      region.corner_radius = region.corner_radius - 1.0f;
    }

    // Validate roi with spatial dimming, reset if out of bounds
    if ((region.rect.left - spatial_dimming_width_ < 1 ||
         region.rect.top - spatial_dimming_width_ < 1 ||
         region.rect.right + spatial_dimming_width_ > panel_res.x_pixels ||
         region.rect.bottom + spatial_dimming_width_ > panel_res.y_pixels)) {
      DLOGI_IF(kTagDisplay,
               "PrivacyRegions outside of panel bounds: index:%d rect:%d %d %d %d panel:%dx%d",
               region.index, region.rect.left, region.rect.top, region.rect.right,
               region.rect.bottom, panel_res.x_pixels, panel_res.y_pixels);
      region.corner_radius = 0.0f;
      region.rect.left = 0;
      region.rect.top = 0;
      region.rect.right = 0;
      region.rect.bottom = 0;
      // Remove privacy regions that were previously sent as empty
      if (region.index > 0 && region.index < 3 && !prev_region_state_[region.index - 1]) {
        region_to_remove += region.index;
        DLOGI_IF(kTagDisplay, "Remove region %d", region_to_remove);
      }
    }

    prev_region_state_[region.index - 1] = !isEmptyRegion(region.rect);

    DLOGI_IF(kTagDisplay, "PrivacyRegions with offsets [index:%d radius:%.2f rect:%d %d %d %d]",
             region.index, region.corner_radius, region.rect.left, region.rect.top,
             region.rect.right, region.rect.bottom);

    consolidated_regions.at(i) = region;
  }

  // Remove the privacy regions from the list
  if (region_to_remove >= 3) {
    consolidated_regions.clear();
  } else if (region_to_remove > 0 && region_to_remove < 3) {
    for (auto it = consolidated_regions.begin(); it != consolidated_regions.end();) {
      PrivacyRegion region = *it;
      if (region.index == region_to_remove) {
        it = consolidated_regions.erase(it);
      } else {
        ++it;
      }
    }
  }
}

}  // namespace sdm
