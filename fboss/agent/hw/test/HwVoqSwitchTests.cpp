// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <fb303/ServiceData.h>
#include "fboss/agent/SwitchStats.h"
#include "fboss/agent/Utils.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/HwResourceStatsPublisher.h"
#include "fboss/agent/hw/HwSwitchFb303Stats.h"
#include "fboss/agent/hw/switch_asics/EbroAsic.h"
#include "fboss/agent/hw/switch_asics/Jericho2Asic.h"
#include "fboss/agent/hw/switch_asics/Jericho3Asic.h"
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwLinkStateDependentTest.h"
#include "fboss/agent/hw/test/HwTest.h"
#include "fboss/agent/hw/test/HwTestAclUtils.h"
#include "fboss/agent/hw/test/HwTestCoppUtils.h"
#include "fboss/agent/hw/test/HwTestFabricUtils.h"
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/hw/test/HwTestPortUtils.h"
#include "fboss/agent/hw/test/LoadBalancerUtils.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/test/EcmpSetupHelper.h"
#include "fboss/agent/test/utils/FabricTestUtils.h"
#include "fboss/agent/test/utils/OlympicTestUtils.h"
#include "fboss/agent/test/utils/QueuePerHostTestUtils.h"
#include "fboss/agent/test/utils/TrapPacketUtils.h"
#include "fboss/agent/test/utils/VoqTestUtils.h"
#include "fboss/lib/CommonUtils.h"

namespace {
constexpr uint8_t kDefaultQueue = 0;
} // namespace

using namespace facebook::fb303;
namespace facebook::fboss {
class HwVoqSwitchTest : public HwLinkStateDependentTest {
 public:
  cfg::SwitchConfig initialConfig() const override {
    auto cfg = utility::onePortPerInterfaceConfig(
        getHwSwitch(),
        masterLogicalPortIds(),
        getAsic()->desiredLoopbackModes(),
        true /*interfaceHasSubnet*/);
    // Add ACL Table group before adding any ACLs
    utility::addAclTableGroup(
        &cfg, cfg::AclStage::INGRESS, utility::getAclTableGroupName());
    utility::addDefaultAclTable(cfg);
    const auto& cpuStreamTypes =
        getAsic()->getQueueStreamTypes(cfg::PortType::CPU_PORT);
    for (const auto& cpuStreamType : cpuStreamTypes) {
      if (getAsic()->getDefaultNumPortQueues(
              cpuStreamType, cfg::PortType::CPU_PORT)) {
        // cpu queues supported
        utility::setDefaultCpuTrafficPolicyConfig(
            cfg,
            getHwSwitchEnsemble()->getL3Asics(),
            getHwSwitchEnsemble()->isSai());
        utility::addCpuQueueConfig(
            cfg,
            getHwSwitchEnsemble()->getL3Asics(),
            getHwSwitchEnsemble()->isSai());
        break;
      }
    }
    return cfg;
  }
  void SetUp() override {
    // VOQ switches will run SAI from day 1. so enable Multi acl for VOQ tests
    FLAGS_enable_acl_table_group = true;
    HwLinkStateDependentTest::SetUp();
    ASSERT_EQ(getHwSwitch()->getSwitchType(), cfg::SwitchType::VOQ);
    ASSERT_TRUE(getHwSwitch()->getSwitchId().has_value());
  }

 protected:
  void addRemoveNeighbor(
      PortDescriptor port,
      bool add,
      std::optional<int64_t> encapIdx = std::nullopt) {
    utility::EcmpSetupAnyNPorts6 ecmpHelper(getProgrammedState());
    if (add) {
      applyNewState(ecmpHelper.resolveNextHops(
          getProgrammedState(), {port}, false /*use link local*/, encapIdx));
    } else {
      applyNewState(ecmpHelper.unresolveNextHops(getProgrammedState(), {port}));
    }
  }

  int sendPacket(
      const folly::IPAddressV6& dstIp,
      std::optional<PortID> frontPanelPort,
      std::optional<std::vector<uint8_t>> payload =
          std::optional<std::vector<uint8_t>>(),
      int dscp = 0x24) {
    folly::IPAddressV6 kSrcIp("1::1");
    const auto srcMac = utility::kLocalCpuMac();
    const auto dstMac = utility::kLocalCpuMac();

    auto txPacket = utility::makeUDPTxPacket(
        getHwSwitch(),
        std::nullopt, // vlanID
        srcMac,
        dstMac,
        kSrcIp,
        dstIp,
        8000, // l4 src port
        8001, // l4 dst port
        dscp << 2, // dscp
        255, // hopLimit
        payload);
    size_t txPacketSize = txPacket->buf()->length();

    XLOG(DBG5) << "\n"
               << folly::hexDump(
                      txPacket->buf()->data(), txPacket->buf()->length());

    if (frontPanelPort.has_value()) {
      getHwSwitch()->sendPacketOutOfPortAsync(
          std::move(txPacket), *frontPanelPort);
    } else {
      getHwSwitch()->sendPacketSwitchedAsync(std::move(txPacket));
    }
    return txPacketSize;
  }

 private:
  HwSwitchEnsemble::Features featuresDesired() const override {
    return {
        HwSwitchEnsemble::LINKSCAN,
        HwSwitchEnsemble::PACKET_RX,
        HwSwitchEnsemble::TAM_NOTIFY};
  }
};

class HwVoqSwitchWithMultipleDsfNodesTest : public HwVoqSwitchTest {
 public:
  cfg::SwitchConfig initialConfig() const override {
    auto cfg = HwVoqSwitchTest::initialConfig();
    cfg.dsfNodes() = *overrideDsfNodes(*cfg.dsfNodes());
    return cfg;
  }

  std::optional<std::map<int64_t, cfg::DsfNode>> overrideDsfNodes(
      const std::map<int64_t, cfg::DsfNode>& curDsfNodes) const override {
    return utility::addRemoteDsfNodeCfg(curDsfNodes, 1);
  }

 protected:
  void assertVoqTailDrops(
      const folly::IPAddressV6& nbrIp,
      const SystemPortID& sysPortId) {
    auto sendPkts = [=, this]() {
      for (auto i = 0; i < 1000; ++i) {
        sendPacket(nbrIp, std::nullopt);
      }
    };
    auto voqDiscardBytes = 0;
    WITH_RETRIES_N(100, {
      sendPkts();
      getHwSwitch()->updateStats();
      voqDiscardBytes =
          getLatestSysPortStats(sysPortId).get_queueOutDiscardBytes_().at(
              kDefaultQueue);
      XLOG(INFO) << " VOQ discard bytes: " << voqDiscardBytes;
      EXPECT_EVENTUALLY_GT(voqDiscardBytes, 0);
    });
    if (getAsic()->getAsicType() == cfg::AsicType::ASIC_TYPE_JERICHO3) {
      auto switchDropStats = getHwSwitch()->getSwitchDropStats();
      CHECK(switchDropStats.voqResourceExhaustionDrops().has_value());
      XLOG(INFO) << " Voq resource exhaustion drops: "
                 << *switchDropStats.voqResourceExhaustionDrops();
      EXPECT_GT(*switchDropStats.voqResourceExhaustionDrops(), 0);
    }
    checkNoStatsChange();
  }
};

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, remoteSystemPort) {
  auto setup = [this]() {
    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    auto getStats = [] {
      return std::make_tuple(
          fbData->getCounter(kSystemPortsFree), fbData->getCounter(kVoqsFree));
    };
    getHwSwitchEnsemble()->getLatestPortStats(masterLogicalPortIds());
    auto [beforeSysPortsFree, beforeVoqsFree] = getStats();
    applyNewState(utility::addRemoteSysPort(
        getProgrammedState(),
        scopeResolver(),
        SystemPortID(401),
        static_cast<SwitchID>(numCores)));
    getHwSwitchEnsemble()->getLatestPortStats(masterLogicalPortIds());
    auto [afterSysPortsFree, afterVoqsFree] = getStats();
    XLOG(INFO) << " Before sysPortsFree: " << beforeSysPortsFree
               << " voqsFree: " << beforeVoqsFree
               << " after sysPortsFree: " << afterSysPortsFree
               << " voqsFree: " << afterVoqsFree;
    EXPECT_EQ(beforeSysPortsFree - 1, afterSysPortsFree);
    // 8 VOQs allocated per sys port
    EXPECT_EQ(beforeVoqsFree - 8, afterVoqsFree);
  };
  verifyAcrossWarmBoots(setup, [] {});
}

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, remoteRouterInterface) {
  auto setup = [this]() {
    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    auto constexpr remotePortId = 401;
    applyNewState(utility::addRemoteSysPort(
        getProgrammedState(),
        scopeResolver(),
        SystemPortID(remotePortId),
        static_cast<SwitchID>(numCores)));
    applyNewState(utility::addRemoteInterface(
        getProgrammedState(),
        scopeResolver(),
        InterfaceID(remotePortId),
        // TODO - following assumes we haven't
        // already used up the subnets below for
        // local interfaces. In that sense it
        // has a implicit coupling with how ConfigFactory
        // generates subnets for local interfaces
        {
            {folly::IPAddress("100::1"), 64},
            {folly::IPAddress("100.0.0.1"), 24},
        }));
  };
  verifyAcrossWarmBoots(setup, [] {});
}

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, addRemoveRemoteNeighbor) {
  auto setup = [this]() {
    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    auto constexpr remotePortId = 401;
    const SystemPortID kRemoteSysPortId(remotePortId);
    applyNewState(utility::addRemoteSysPort(
        getProgrammedState(),
        scopeResolver(),
        kRemoteSysPortId,
        static_cast<SwitchID>(numCores)));
    const InterfaceID kIntfId(remotePortId);
    applyNewState(utility::addRemoteInterface(
        getProgrammedState(),
        scopeResolver(),
        kIntfId,
        // TODO - following assumes we haven't
        // already used up the subnets below for
        // local interfaces. In that sense it
        // has a implicit coupling with how ConfigFactory
        // generates subnets for local interfaces
        {
            {folly::IPAddress("100::1"), 64},
            {folly::IPAddress("100.0.0.1"), 24},
        }));
    folly::IPAddressV6 kNeighborIp("100::2");
    PortDescriptor kPort(kRemoteSysPortId);
    // Add neighbor
    applyNewState(utility::addRemoveRemoteNeighbor(
        getProgrammedState(),
        scopeResolver(),
        kNeighborIp,
        kIntfId,
        kPort,
        true,
        utility::getDummyEncapIndex(getHwSwitchEnsemble())));
    // Remove neighbor
    applyNewState(utility::addRemoveRemoteNeighbor(
        getProgrammedState(),
        scopeResolver(),
        kNeighborIp,
        kIntfId,
        kPort,
        false,
        utility::getDummyEncapIndex(getHwSwitchEnsemble())));
  };
  verifyAcrossWarmBoots(setup, [] {});
}

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, voqDelete) {
  auto constexpr remotePortId = 401;
  const SystemPortID kRemoteSysPortId(remotePortId);
  folly::IPAddressV6 kNeighborIp("100::2");
  auto setup = [=, this]() {
    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    applyNewState(utility::addRemoteSysPort(
        getProgrammedState(),
        scopeResolver(),
        kRemoteSysPortId,
        static_cast<SwitchID>(numCores)));
    const InterfaceID kIntfId(remotePortId);
    applyNewState(utility::addRemoteInterface(
        getProgrammedState(),
        scopeResolver(),
        kIntfId,
        // TODO - following assumes we haven't
        // already used up the subnets below for
        // local interfaces. In that sense it
        // has a implicit coupling with how ConfigFactory
        // generates subnets for local interfaces
        {
            {folly::IPAddress("100::1"), 64},
            {folly::IPAddress("100.0.0.1"), 24},
        }));
    PortDescriptor kPort(kRemoteSysPortId);
    // Add neighbor
    applyNewState(utility::addRemoveRemoteNeighbor(
        getProgrammedState(),
        scopeResolver(),
        kNeighborIp,
        kIntfId,
        kPort,
        true,
        utility::getDummyEncapIndex(getHwSwitchEnsemble())));
  };
  auto verify = [=, this]() {
    auto getVoQDeletedPkts = [=, this]() {
      if (!getAsic()->isSupported(HwAsic::Feature::VOQ_DELETE_COUNTER)) {
        return 0L;
      }
      return getLatestSysPortStats(kRemoteSysPortId)
          .get_queueCreditWatchdogDeletedPackets_()
          .at(kDefaultQueue);
    };

    auto voqDeletedPktsBefore = getVoQDeletedPkts();
    utility::EcmpSetupAnyNPorts6 ecmpHelper(getProgrammedState());
    auto frontPanelPort = ecmpHelper.ecmpPortDescriptorAt(1).phyPortID();
    for (auto i = 0; i < 100; ++i) {
      // Send pkts via front panel
      sendPacket(kNeighborIp, frontPanelPort, std::vector<uint8_t>(1024, 0xff));
    }
    WITH_RETRIES({
      auto voqDeletedPktsAfter = getVoQDeletedPkts();
      XLOG(INFO) << "Voq deleted pkts, before: " << voqDeletedPktsBefore
                 << " after: " << voqDeletedPktsAfter;
      EXPECT_EVENTUALLY_EQ(voqDeletedPktsBefore + 100, voqDeletedPktsAfter);
    });
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, stressAddRemoveObjects) {
  auto setup = [=, this]() {
    // Disable credit watchdog
    utility::enableCreditWatchdog(getHwSwitch(), false);
  };
  auto verify = [this]() {
    auto numIterations = 500;
    auto constexpr remotePortId = 401;
    const SystemPortID kRemoteSysPortId(remotePortId);
    folly::IPAddressV6 kNeighborIp("100::2");
    utility::EcmpSetupAnyNPorts6 ecmpHelper(getProgrammedState());
    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    const auto kPort = ecmpHelper.ecmpPortDescriptorAt(0);
    const InterfaceID kIntfId(remotePortId);
    PortDescriptor kRemotePort(kRemoteSysPortId);
    auto addObjects = [&]() {
      // add local neighbor
      addRemoveNeighbor(kPort, true /* add neighbor*/);
      // Remote objs
      applyNewState(utility::addRemoteSysPort(
          getProgrammedState(),
          scopeResolver(),
          kRemoteSysPortId,
          static_cast<SwitchID>(numCores)));
      applyNewState(utility::addRemoteInterface(
          getProgrammedState(),
          scopeResolver(),
          kIntfId,
          // TODO - following assumes we haven't
          // already used up the subnets below for
          // local interfaces. In that sense it
          // has a implicit coupling with how ConfigFactory
          // generates subnets for local interfaces
          {
              {folly::IPAddress("100::1"), 64},
              {folly::IPAddress("100.0.0.1"), 24},
          }));
      // Add neighbor
      applyNewState(utility::addRemoveRemoteNeighbor(
          getProgrammedState(),
          scopeResolver(),
          kNeighborIp,
          kIntfId,
          kRemotePort,
          true,
          utility::getDummyEncapIndex(getHwSwitchEnsemble())));
    };
    auto removeObjects = [&]() {
      addRemoveNeighbor(kPort, false /* remove neighbor*/);
      // Remove neighbor
      applyNewState(utility::addRemoveRemoteNeighbor(
          getProgrammedState(),
          scopeResolver(),
          kNeighborIp,
          kIntfId,
          kRemotePort,
          false,
          utility::getDummyEncapIndex(getHwSwitchEnsemble())));
      // Remove rif
      applyNewState(
          utility::removeRemoteInterface(getProgrammedState(), kIntfId));
      // Remove sys port
      applyNewState(
          utility::removeRemoteSysPort(getProgrammedState(), kRemoteSysPortId));
    };
    for (auto i = 0; i < numIterations; ++i) {
      addObjects();
      // Delete on all but the last iteration. In the last iteration
      // we will leave the entries intact and then forward pkts
      // to this VOQ
      if (i < numIterations - 1) {
        removeObjects();
      }
    }
    assertVoqTailDrops(kNeighborIp, kRemoteSysPortId);
    auto beforePkts =
        getLatestPortStats(kPort.phyPortID()).get_outUnicastPkts_();
    // CPU send
    sendPacket(ecmpHelper.ip(kPort), std::nullopt);
    auto frontPanelPort = ecmpHelper.ecmpPortDescriptorAt(1).phyPortID();
    sendPacket(ecmpHelper.ip(kPort), frontPanelPort);
    WITH_RETRIES({
      auto afterPkts =
          getLatestPortStats(kPort.phyPortID()).get_outUnicastPkts_();
      EXPECT_EVENTUALLY_EQ(afterPkts, beforePkts + 2);
    });
    // removeObjects before exiting for WB
    removeObjects();
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, voqTailDropCounter) {
  folly::IPAddressV6 kNeighborIp("100::2");
  auto constexpr remotePortId = 401;
  const SystemPortID kRemoteSysPortId(remotePortId);
  auto setup = [=, this]() {
    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    // Disable credit watchdog
    utility::enableCreditWatchdog(getHwSwitch(), false);
    applyNewState(utility::addRemoteSysPort(
        getProgrammedState(),
        scopeResolver(),
        kRemoteSysPortId,
        static_cast<SwitchID>(numCores)));
    const InterfaceID kIntfId(remotePortId);
    applyNewState(utility::addRemoteInterface(
        getProgrammedState(),
        scopeResolver(),
        kIntfId,
        {
            {folly::IPAddress("100::1"), 64},
            {folly::IPAddress("100.0.0.1"), 24},
        }));
    PortDescriptor kPort(kRemoteSysPortId);
    // Add neighbor
    applyNewState(utility::addRemoveRemoteNeighbor(
        getProgrammedState(),
        scopeResolver(),
        kNeighborIp,
        kIntfId,
        kPort,
        true,
        utility::getDummyEncapIndex(getHwSwitchEnsemble())));
  };

  auto verify = [=, this]() {
    assertVoqTailDrops(kNeighborIp, kRemoteSysPortId);
  };
  verifyAcrossWarmBoots(setup, verify);
};

TEST_F(HwVoqSwitchWithMultipleDsfNodesTest, verifyDscpToVoqMapping) {
  folly::IPAddressV6 kNeighborIp("100::2");
  auto constexpr remotePortId = 401;
  const SystemPortID kRemoteSysPortId(remotePortId);
  auto setup = [=, this]() {
    auto newCfg{initialConfig()};
    utility::addOlympicQosMaps(newCfg, getHwSwitchEnsemble()->getL3Asics());
    applyNewConfig(newCfg);

    // in addRemoteDsfNodeCfg, we use numCores to calculate the remoteSwitchId
    // keeping remote switch id passed below in sync with it
    int numCores = getAsic()->getNumCores();
    applyNewState(utility::addRemoteSysPort(
        getProgrammedState(),
        scopeResolver(),
        kRemoteSysPortId,
        static_cast<SwitchID>(numCores)));
    const InterfaceID kIntfId(remotePortId);
    applyNewState(utility::addRemoteInterface(
        getProgrammedState(),
        scopeResolver(),
        kIntfId,
        {
            {folly::IPAddress("100::1"), 64},
            {folly::IPAddress("100.0.0.1"), 24},
        }));
    PortDescriptor kPort(kRemoteSysPortId);
    // Add neighbor
    applyNewState(utility::addRemoveRemoteNeighbor(
        getProgrammedState(),
        scopeResolver(),
        kNeighborIp,
        kIntfId,
        kPort,
        true,
        utility::getDummyEncapIndex(getHwSwitchEnsemble())));
  };

  auto verify = [=, this]() {
    for (const auto& q2dscps : utility::kOlympicQueueToDscp()) {
      auto queueId = q2dscps.first;
      for (auto dscp : q2dscps.second) {
        XLOG(DBG2) << "verify packet with dscp " << dscp << " goes to queue "
                   << queueId;
        auto statsBefore = getLatestSysPortStats(kRemoteSysPortId);
        auto queueBytesBefore = statsBefore.queueOutBytes_()->at(queueId) +
            statsBefore.queueOutDiscardBytes_()->at(queueId);
        sendPacket(
            kNeighborIp,
            std::nullopt,
            std::optional<std::vector<uint8_t>>(),
            dscp);
        WITH_RETRIES_N(10, {
          auto statsAfter = getLatestSysPortStats(kRemoteSysPortId);
          auto queueBytesAfter = statsAfter.queueOutBytes_()->at(queueId) +
              statsAfter.queueOutDiscardBytes_()->at(queueId);
          XLOG(DBG2) << "queue " << queueId
                     << " stats before: " << queueBytesBefore
                     << " stats after: " << queueBytesAfter;
          EXPECT_EVENTUALLY_GT(queueBytesAfter, queueBytesBefore);
        });
      }
    }
  };
  verifyAcrossWarmBoots(setup, verify);
};

// FullScaleDsfNode Test sets up 128 remote DSF nodes for J2 and 512 for J3.
class HwVoqSwitchFullScaleDsfNodesTest
    : public HwVoqSwitchWithMultipleDsfNodesTest {
 public:
  cfg::SwitchConfig initialConfig() const override {
    auto cfg = HwVoqSwitchTest::initialConfig();
    cfg.dsfNodes() = *overrideDsfNodes(*cfg.dsfNodes());
    cfg.loadBalancers()->push_back(utility::getEcmpFullHashConfig({getAsic()}));
    return cfg;
  }

  std::optional<std::map<int64_t, cfg::DsfNode>> overrideDsfNodes(
      const std::map<int64_t, cfg::DsfNode>& curDsfNodes) const override {
    return utility::addRemoteDsfNodeCfg(curDsfNodes);
  }

 protected:
  int getMaxEcmpWidth(const HwAsic* asic) const {
    // J2 and J3 only supports variable width
    return asic->getMaxVariableWidthEcmpSize();
  }

  int getMaxEcmpGroup() const {
    return 64;
  }

  // Resolve and return list of local nhops (excluding recycle port)
  std::vector<PortDescriptor> resolveLocalNhops(
      utility::EcmpSetupTargetedPorts6& ecmpHelper) {
    auto ports = getProgrammedState()->getSystemPorts()->getAllNodes();
    std::vector<PortDescriptor> portDescs;
    std::for_each(
        ports->begin(), ports->end(), [&portDescs](const auto& idAndPort) {
          if (idAndPort.second->getCorePortIndex() != 1) {
            portDescs.push_back(
                PortDescriptor(static_cast<SystemPortID>(idAndPort.first)));
          }
        });
    auto currState = getProgrammedState();
    for (const auto& portDesc : portDescs) {
      currState = ecmpHelper.resolveNextHops(currState, {portDesc});
    }
    applyNewState(currState);
    return portDescs;
  }
};

TEST_F(HwVoqSwitchFullScaleDsfNodesTest, systemPortScaleTest) {
  auto setup = [this]() {
    applyNewState(utility::setupRemoteIntfAndSysPorts(
        getProgrammedState(),
        scopeResolver(),
        initialConfig(),
        getAsic()->isSupported(HwAsic::Feature::RESERVED_ENCAP_INDEX_RANGE)));
  };
  verifyAcrossWarmBoots(setup, [] {});
}

TEST_F(HwVoqSwitchFullScaleDsfNodesTest, remoteNeighborWithEcmpGroup) {
  const auto kEcmpWidth = getMaxEcmpWidth(getAsic());
  const auto kMaxDeviation = 25;
  FLAGS_ecmp_width = kEcmpWidth;
  boost::container::flat_set<PortDescriptor> sysPortDescs;
  auto setup = [&]() {
    applyNewState(utility::setupRemoteIntfAndSysPorts(
        getProgrammedState(),
        scopeResolver(),
        initialConfig(),
        getAsic()->isSupported(HwAsic::Feature::RESERVED_ENCAP_INDEX_RANGE)));
    utility::EcmpSetupTargetedPorts6 ecmpHelper(getProgrammedState());
    // Trigger config apply to add remote interface routes as directly connected
    // in RIB. This is to resolve ECMP members pointing to remote nexthops.
    applyNewConfig(initialConfig());

    // Resolve remote nhops and get a list of remote sysPort descriptors
    sysPortDescs =
        utility::resolveRemoteNhops(getHwSwitchEnsemble(), ecmpHelper);

    for (int i = 0; i < getMaxEcmpGroup(); i++) {
      auto prefix = RoutePrefixV6{
          folly::IPAddressV6(folly::to<std::string>(i, "::", i)),
          static_cast<uint8_t>(i == 0 ? 0 : 128)};
      ecmpHelper.programRoutes(
          getRouteUpdater(),
          flat_set<PortDescriptor>(
              std::make_move_iterator(sysPortDescs.begin() + i),
              std::make_move_iterator(sysPortDescs.begin() + i + kEcmpWidth)),
          {prefix});
    }
  };
  auto verify = [&]() {
    // Send and verify packets across voq drops.
    auto defaultRouteSysPorts = std::vector<PortDescriptor>(
        sysPortDescs.begin(), sysPortDescs.begin() + kEcmpWidth);
    std::function<std::map<SystemPortID, HwSysPortStats>(
        const std::vector<SystemPortID>&)>
        getSysPortStatsFn = [&](const std::vector<SystemPortID>& portIds) {
          return getLatestSysPortStats(portIds);
        };
    utility::pumpTrafficAndVerifyLoadBalanced(
        [&]() {
          utility::pumpTraffic(
              true, /* isV6 */
              utility::getAllocatePktFn(getHwSwitchEnsemble()),
              utility::getSendPktFunc(getHwSwitchEnsemble()),
              utility::kLocalCpuMac(), /* dstMac */
              std::nullopt, /* vlan */
              std::nullopt, /* frontPanelPortToLoopTraffic */
              255, /* hopLimit */
              1000000 /* numPackets */);
        },
        [&]() {
          auto ports = std::make_unique<std::vector<int32_t>>();
          for (auto sysPortDecs : defaultRouteSysPorts) {
            ports->push_back(static_cast<int32_t>(sysPortDecs.sysPortID()));
          }
          getHwSwitch()->clearPortStats(ports);
        },
        [&]() {
          WITH_RETRIES(EXPECT_EVENTUALLY_TRUE(utility::isLoadBalanced(
              defaultRouteSysPorts,
              {},
              getSysPortStatsFn,
              kMaxDeviation,
              false)));
          return true;
        });
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(HwVoqSwitchFullScaleDsfNodesTest, remoteAndLocalLoadBalance) {
  const auto kEcmpWidth = 16;
  const auto kMaxDeviation = 25;
  FLAGS_ecmp_width = kEcmpWidth;
  std::vector<PortDescriptor> sysPortDescs;
  auto setup = [&]() {
    applyNewState(utility::setupRemoteIntfAndSysPorts(
        getProgrammedState(),
        scopeResolver(),
        initialConfig(),
        getAsic()->isSupported(HwAsic::Feature::RESERVED_ENCAP_INDEX_RANGE)));
    utility::EcmpSetupTargetedPorts6 ecmpHelper(getProgrammedState());
    // Trigger config apply to add remote interface routes as directly connected
    // in RIB. This is to resolve ECMP members pointing to remote nexthops.
    applyNewConfig(initialConfig());

    // Resolve remote and local nhops and get a list of sysPort descriptors
    auto remoteSysPortDescs =
        utility::resolveRemoteNhops(getHwSwitchEnsemble(), ecmpHelper);
    auto localSysPortDescs = resolveLocalNhops(ecmpHelper);

    sysPortDescs.insert(
        sysPortDescs.end(),
        remoteSysPortDescs.begin(),
        remoteSysPortDescs.begin() + kEcmpWidth / 2);
    sysPortDescs.insert(
        sysPortDescs.end(),
        localSysPortDescs.begin(),
        localSysPortDescs.begin() + kEcmpWidth / 2);

    auto prefix = RoutePrefixV6{folly::IPAddressV6("0::0"), 0};
    ecmpHelper.programRoutes(
        getRouteUpdater(),
        flat_set<PortDescriptor>(
            std::make_move_iterator(sysPortDescs.begin()),
            std::make_move_iterator(sysPortDescs.end())),
        {prefix});
  };
  auto verify = [&]() {
    // Send and verify packets across voq drops.
    std::function<std::map<SystemPortID, HwSysPortStats>(
        const std::vector<SystemPortID>&)>
        getSysPortStatsFn = [&](const std::vector<SystemPortID>& portIds) {
          return getLatestSysPortStats(portIds);
        };
    utility::pumpTrafficAndVerifyLoadBalanced(
        [&]() {
          utility::pumpTraffic(
              true, /* isV6 */
              utility::getAllocatePktFn(getHwSwitchEnsemble()),
              utility::getSendPktFunc(getHwSwitchEnsemble()),
              utility::kLocalCpuMac(), /* dstMac */
              std::nullopt, /* vlan */
              std::nullopt, /* frontPanelPortToLoopTraffic */
              255, /* hopLimit */
              10000 /* numPackets */);
        },
        [&]() {
          auto ports = std::make_unique<std::vector<int32_t>>();
          for (auto sysPortDecs : sysPortDescs) {
            ports->push_back(static_cast<int32_t>(sysPortDecs.sysPortID()));
          }
          getHwSwitch()->clearPortStats(ports);
        },
        [&]() {
          WITH_RETRIES(EXPECT_EVENTUALLY_TRUE(utility::isLoadBalanced(
              sysPortDescs, {}, getSysPortStatsFn, kMaxDeviation, false)));
          return true;
        });
  };
  verifyAcrossWarmBoots(setup, verify);
};

TEST_F(HwVoqSwitchFullScaleDsfNodesTest, stressProgramEcmpRoutes) {
  auto kEcmpWidth = getMaxEcmpWidth(getAsic());
  FLAGS_ecmp_width = kEcmpWidth;
  // Stress add/delete 40 iterations of 5 routes with ECMP width.
  // 40 iterations take ~17 mins on j3.
  const auto routeScale = 5;
  const auto numIterations = 40;
  auto setup = [&]() {
    applyNewState(utility::setupRemoteIntfAndSysPorts(
        getProgrammedState(),
        scopeResolver(),
        initialConfig(),
        getAsic()->isSupported(HwAsic::Feature::RESERVED_ENCAP_INDEX_RANGE)));
    utility::EcmpSetupTargetedPorts6 ecmpHelper(getProgrammedState());
    // Trigger config apply to add remote interface routes as directly connected
    // in RIB. This is to resolve ECMP members pointing to remote nexthops.
    applyNewConfig(initialConfig());

    // Resolve remote nhops and get a list of remote sysPort descriptors
    auto sysPortDescs =
        utility::resolveRemoteNhops(getHwSwitchEnsemble(), ecmpHelper);

    for (int iter = 0; iter < numIterations; iter++) {
      std::vector<RoutePrefixV6> routes;
      for (int i = 0; i < routeScale; i++) {
        auto prefix = RoutePrefixV6{
            folly::IPAddressV6(folly::to<std::string>(i + 1, "::", i + 1)),
            128};
        ecmpHelper.programRoutes(
            getRouteUpdater(),
            flat_set<PortDescriptor>(
                std::make_move_iterator(sysPortDescs.begin() + i),
                std::make_move_iterator(sysPortDescs.begin() + i + kEcmpWidth)),
            {prefix});
        routes.push_back(prefix);
      }
      ecmpHelper.unprogramRoutes(getRouteUpdater(), routes);
    }
  };
  verifyAcrossWarmBoots(setup, [] {});
}
} // namespace facebook::fboss
