/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiSwitch.h"

#include "fboss/agent/FabricConnectivityManager.h"
#include "fboss/agent/hw/HwResourceStatsPublisher.h"
#include "fboss/agent/hw/sai/switch/ConcurrentIndices.h"
#include "fboss/agent/hw/sai/switch/SaiAclTableManager.h"
#include "fboss/agent/hw/sai/switch/SaiBufferManager.h"
#include "fboss/agent/hw/sai/switch/SaiCounterManager.h"
#include "fboss/agent/hw/sai/switch/SaiHostifManager.h"
#include "fboss/agent/hw/sai/switch/SaiLagManager.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/sai/switch/SaiSystemPortManager.h"

namespace facebook::fboss {

void SaiSwitch::updateStatsImpl() {
  if (FLAGS_skip_stats_update_for_debug) {
    // Skip collecting any ASIC stats while debugs are in progress
    return;
  }

  auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  bool updateWatermarks = now - watermarkStatsUpdateTime_ >=
      FLAGS_update_watermark_stats_interval_s;
  if (updateWatermarks) {
    watermarkStatsUpdateTime_ = now;
  }

  int64_t missingCount = 0, mismatchCount = 0;
  auto portsIter = concurrentIndices_->portSaiId2PortInfo.begin();
  std::map<PortID, multiswitch::FabricConnectivityDelta> connectivityDelta;
  while (portsIter != concurrentIndices_->portSaiId2PortInfo.end()) {
    {
      std::lock_guard<std::mutex> locked(saiSwitchMutex_);
      auto endpointOpt = managerTable_->portManager().getFabricConnectivity(
          portsIter->second.portID);
      if (endpointOpt.has_value()) {
        auto delta = fabricConnectivityManager_->processConnectivityInfoForPort(
            portsIter->second.portID, *endpointOpt);
        if (delta) {
          connectivityDelta.insert({portsIter->second.portID, *delta});
        }
        if (fabricConnectivityManager_->isConnectivityInfoMissing(
                portsIter->second.portID)) {
          missingCount++;
        }
        if (fabricConnectivityManager_->isConnectivityInfoMismatch(
                portsIter->second.portID)) {
          mismatchCount++;
        }
      }
      managerTable_->portManager().updateStats(
          portsIter->second.portID, updateWatermarks);
    }
    ++portsIter;
  }

  getSwitchStats()->fabricReachabilityMissingCount(missingCount);
  getSwitchStats()->fabricReachabilityMismatchCount(mismatchCount);

  auto sysPortsIter = concurrentIndices_->sysPortIds.begin();
  while (sysPortsIter != concurrentIndices_->sysPortIds.end()) {
    {
      std::lock_guard<std::mutex> locked(saiSwitchMutex_);
      managerTable_->systemPortManager().updateStats(
          sysPortsIter->second, updateWatermarks);
    }
    ++sysPortsIter;
  }
  auto lagsIter = concurrentIndices_->aggregatePortIds.begin();
  while (lagsIter != concurrentIndices_->aggregatePortIds.end()) {
    {
      std::lock_guard<std::mutex> locked(saiSwitchMutex_);
      managerTable_->lagManager().updateStats(lagsIter->second);
    }
    ++lagsIter;
  }
  if (platform_->getAsic()->isSupported(HwAsic::Feature::CPU_PORT)) {
    std::lock_guard<std::mutex> locked(saiSwitchMutex_);
    managerTable_->hostifManager().updateStats(updateWatermarks);
  }

  {
    std::lock_guard<std::mutex> locked(saiSwitchMutex_);
    managerTable_->bufferManager().updateStats();
  }
  {
    std::lock_guard<std::mutex> locked(saiSwitchMutex_);
    HwResourceStatsPublisher(getPlatform()->getMultiSwitchStatsPrefix())
        .publish(hwResourceStats_);
  }
  {
    std::lock_guard<std::mutex> locked(saiSwitchMutex_);
    managerTable_->aclTableManager().updateStats();
  }
  {
    std::lock_guard<std::mutex> locked(saiSwitchMutex_);
    managerTable_->counterManager().updateStats();
  }
  {
    std::lock_guard<std::mutex> locked(saiSwitchMutex_);
    managerTable_->switchManager().updateStats(updateWatermarks);
  }
  reportAsymmetricTopology();
  linkConnectivityChangeBottomHalfEventBase_.runInEventBaseThread(
      [this, connectivityDelta = std::move(connectivityDelta)] {
        if (connectivityDelta.size()) {
          linkConnectivityChanged(connectivityDelta);
        }
      });
}

} // namespace facebook::fboss
