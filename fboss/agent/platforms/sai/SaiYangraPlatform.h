/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include "fboss/agent/platforms/sai/SaiPlatform.h"

namespace facebook::fboss {

class ChenabAsic;

class SaiYangraPlatform : public SaiPlatform {
 public:
  SaiYangraPlatform(
      std::unique_ptr<PlatformProductInfo> productInfo,
      folly::MacAddress localMac,
      const std::string& platformMappingStr);
  ~SaiYangraPlatform() override;

  std::optional<SaiSwitchTraits::Attributes::AclFieldList> getAclFieldList()
      const override;

  HwAsic* getAsic() const override;
  bool isSerdesApiSupported() const override;
  std::vector<PortID> getAllPortsInGroup(PortID /*portID*/) const override;
  std::vector<FlexPortMode> getSupportedFlexPortModes() const override;
  std::optional<sai_port_interface_type_t> getInterfaceType(
      TransmitterTechnology /*transmitterTech*/,
      cfg::PortSpeed /*speed*/) const override;
  bool supportInterfaceType() const override;
  void initLEDs() override;

  const std::set<sai_api_t>& getSupportedApiList() const override;

  const std::unordered_map<std::string, std::string>
  getSaiProfileVendorExtensionValues() const override;

  std::string getHwConfig() override;

 private:
  void setupAsic(
      cfg::SwitchType switchType,
      std::optional<int64_t> switchId,
      int16_t switchIndex,
      std::optional<cfg::Range64> systemPortRange,
      folly::MacAddress& mac,
      std::optional<HwAsic::FabricNodeRole> role) override;
  std::unique_ptr<ChenabAsic> asic_;
};

} // namespace facebook::fboss
