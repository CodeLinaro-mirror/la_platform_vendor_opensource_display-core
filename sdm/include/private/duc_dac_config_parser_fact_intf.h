/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef __DUC_DAC_CONFIG_PARSER_FACT_INTF_H__
#define __DUC_DAC_CONFIG_PARSER_FACT_INTF_H__

#include "duc_dac_config_parser_intf.h"
#include <memory>

namespace sdm {

class DucDacConfigParserFactIntf {
 public:
  virtual ~DucDacConfigParserFactIntf(){};
  virtual std::shared_ptr<DucDacConfigParserIntf> CreateDucDacConfigParserIntf(
      std::string file_path) = 0;
};

extern "C" DucDacConfigParserFactIntf *GetDucDacConfigParserFactIntf();

}  // namespace sdm

#endif  // __DUC_DAC_CONFIG_PARSER_FACT_INTF_H__