/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <tinyxml2.h>
#include "display_properties.h"
#include "perf_hint_parser.h"
#include "sdm_debugger.h"

#define __CLASS__ "PerfHintParser"

using std::make_pair;
using tinyxml2::XML_SUCCESS;
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

namespace sdm {

DisplayError PerfHintParser::Init() {
  if (!LoadPerfHintThresholdFromFile(kPerfThresholdXmlPath)) {
    DLOGW("Could not load Perf hint threshold XML.");
    return kErrorParameters;
  }
  return kErrorNone;
}

bool PerfHintParser::LoadPerfHintThresholdFromFile(const char *file_name) {
  XMLDocument doc;
  const char *retStr = NULL;
  int perf_version = 0;
  SDMDebugHandler::Get()->GetProperty(PERF_VERSION_PROP, &perf_version);

  if (doc.LoadFile(file_name) != XML_SUCCESS) {
    DLOGW("Failed to load perf hint threshold xml file!");
    return false;
  }

  XMLElement *root = doc.RootElement();
  if (root == nullptr) {
    DLOGW("Xml root not found");
    return false;
  }

  XMLElement *threshold_setting_root = root->FirstChildElement("DeviceThresholdList");
  if (threshold_setting_root == nullptr) {
    DLOGW("No Perf hint threshold specified in xml file!");
    return false;
  }

  int version = perf_version;
  XMLElement *default_device_node = threshold_setting_root->FirstChildElement("DefaultDevice");
  if ((default_device_node != nullptr) && (perf_version == 0)) {
    retStr = default_device_node->Attribute("version");
    if (retStr != NULL) {
      version = std::atoi(retStr);
    }
  }

  XMLElement *threshold_node = threshold_setting_root->FirstChildElement("Device");

  while (threshold_node != nullptr) {
    int current_version = 0;
    retStr = threshold_node->Attribute("version");
    if (retStr != NULL) {
      current_version = std::atoi(retStr);
    }

    if (current_version == version) {
      XMLElement *fps_threshold_map_node = threshold_node->FirstChildElement("FpsThresholdMap");

      while (fps_threshold_map_node != nullptr) {
        int32_t fps = 0;
        int32_t threshold = 0;

        // Retrieve fps
        retStr = fps_threshold_map_node->Attribute("fps");
        if (retStr != NULL) {
          fps = std::atoi(retStr);
        }

        // Retrieve threshold
        retStr = fps_threshold_map_node->Attribute("PerfHintThreshold");
        if (retStr != NULL) {
          threshold = std::atoi(retStr);
        }

        // Populate the fps to threshold map
        if ((fps >= 0) && (threshold >= 0)) {
          fps_to_threshold_map_.insert(make_pair(fps, threshold));
        }

        fps_threshold_map_node = fps_threshold_map_node->NextSiblingElement("FpsThresholdMap");
      }
      break;
    }

    threshold_node = threshold_node->NextSiblingElement("Device");
  }

  if (!fps_to_threshold_map_.size()) {
    DLOGW("No perf hint threshold in xml file!");
    return false;
  }

  return true;
}

DisplayError PerfHintParser::GetPerfHintThresholds(
    std::unordered_map<int32_t, int32_t> *fps_to_threshold) {
  if (!fps_to_threshold) {
    DLOGE("Invalid paramater");
    return kErrorParameters;
  }

  for (auto &item : fps_to_threshold_map_) {
    int32_t fps = item.first;
    int32_t threshold = item.second;

    if ((fps > 0) && (threshold >= 0)) {
      fps_to_threshold->insert(make_pair(fps, threshold));
    }
  }
  return kErrorNone;
}

}  // namespace sdm
