#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "envoy/common/callback.h"
#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/stats/scope.h"
#include "envoy/upstream/health_check_host_monitor.h"

#include "source/common/network/utility.h"
#include "source/common/singleton/manager_impl.h"
#include "source/extensions/clusters/common/dns_cluster_backcompat.h"
#include "source/extensions/clusters/dns/dns_cluster.h"
#include "source/extensions/clusters/logical_dns/logical_dns_cluster.h"
#include "source/server/transport_socket_config_impl.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/protobuf/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/server/admin.h"
#include "test/mocks/server/instance.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/cluster_manager.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::AnyNumber;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Upstream {

class LogicalDnsClusterTest : public Event::TestUsingSimulatedTime, public testing::Test {
protected:
  LogicalDnsClusterTest() : api_(Api::createApiForTest(stats_store_, random_)) {
    ON_CALL(server_context_, api()).WillByDefault(ReturnRef(*api_));
  }

  void setupFromV3Yaml(const std::string& yaml, bool expect_success = true) {
    ON_CALL(server_context_, api()).WillByDefault(ReturnRef(*api_));
    if (expect_success) {
      resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
    }
    NiceMock<MockClusterManager> cm;
    envoy::config::cluster::v3::Cluster cluster_config = parseClusterFromV3Yaml(yaml);
    Envoy::Upstream::ClusterFactoryContextImpl factory_context(server_context_, nullptr, nullptr,
                                                               false);
    absl::StatusOr<std::unique_ptr<LogicalDnsCluster>> status_or_cluster;

    envoy::extensions::clusters::dns::v3::DnsCluster dns_cluster{};
    if (cluster_config.has_cluster_type()) {
      ProtobufTypes::MessagePtr dns_cluster_msg =
          std::make_unique<envoy::extensions::clusters::dns::v3::DnsCluster>();
      ASSERT_TRUE(Config::Utility::translateOpaqueConfig(
                      cluster_config.cluster_type().typed_config(),
                      factory_context.messageValidationVisitor(), *dns_cluster_msg)
                      .ok());
      dns_cluster =
          MessageUtil::downcastAndValidate<const envoy::extensions::clusters::dns::v3::DnsCluster&>(
              *dns_cluster_msg, factory_context.messageValidationVisitor());
    } else {
      createDnsClusterFromLegacyFields(cluster_config, dns_cluster);
    }

    status_or_cluster =
        LogicalDnsCluster::create(cluster_config, dns_cluster, factory_context, dns_resolver_);
    THROW_IF_NOT_OK_REF(status_or_cluster.status());
    cluster_ = std::move(*status_or_cluster);
    priority_update_cb_ = cluster_->prioritySet().addPriorityUpdateCb(
        [&](uint32_t, const HostVector&, const HostVector&) {
          membership_updated_.ready();
          return absl::OkStatus();
        });
    cluster_->initialize([&]() {
      initialized_.ready();
      return absl::OkStatus();
    });
  }

  absl::Status factorySetupFromV3Yaml(const std::string& yaml) {
    ON_CALL(server_context_, api()).WillByDefault(ReturnRef(*api_));
    resolve_timer_ = new Event::MockTimer(&server_context_.dispatcher_);
    NiceMock<MockClusterManager> cm;
    envoy::config::cluster::v3::Cluster cluster_config = parseClusterFromV3Yaml(yaml);
    ClusterFactoryContextImpl::LazyCreateDnsResolver resolver_fn = [&]() { return dns_resolver_; };
    auto status_or_cluster = ClusterFactoryImplBase::create(cluster_config, server_context_,
                                                            resolver_fn, nullptr, false);
    if (status_or_cluster.ok()) {
      cluster_ = std::dynamic_pointer_cast<LogicalDnsCluster>(status_or_cluster->first);
      priority_update_cb_ = cluster_->prioritySet().addPriorityUpdateCb(
          [&](uint32_t, const HostVector&, const HostVector&) {
            membership_updated_.ready();
            return absl::OkStatus();
          });
      cluster_->initialize([&]() {
        initialized_.ready();
        return absl::OkStatus();
      });
    } else {
      // the Event::MockTimer constructor creates EXPECT_CALL for the dispatcher.
      // If we want cluster creation to fail, there won't be a cluster to create the timer,
      // so we need to clear the expectation manually.
      server_context_.dispatcher_.createTimer([]() -> void {});
    }
    return status_or_cluster.status();
  }

  void expectResolve(Network::DnsLookupFamily dns_lookup_family,
                     const std::string& expected_address) {
    EXPECT_CALL(*dns_resolver_, resolve(expected_address, dns_lookup_family, _))
        .WillOnce(Invoke([&](const std::string&, Network::DnsLookupFamily,
                             Network::DnsResolver::ResolveCb cb) -> Network::ActiveDnsQuery* {
          dns_callback_ = cb;
          return &active_dns_query_;
        }));
  }

  void testBasicSetup(const std::string& config, const std::string& expected_address,
                      uint32_t expected_port, uint32_t expected_hc_port) {
    EXPECT_CALL(server_context_.dispatcher_, createTimer_(_)).Times(AnyNumber());
    expectResolve(Network::DnsLookupFamily::V4Only, expected_address);
    setupFromV3Yaml(config);

    EXPECT_CALL(membership_updated_, ready());
    EXPECT_CALL(initialized_, ready());
    EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));
    dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                  TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}));

    EXPECT_EQ(1UL, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts().size());
    EXPECT_EQ(1UL, cluster_->prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
    EXPECT_EQ(1UL,
              cluster_->prioritySet().hostSetsPerPriority()[0]->hostsPerLocality().get().size());
    EXPECT_EQ(
        1UL,
        cluster_->prioritySet().hostSetsPerPriority()[0]->healthyHostsPerLocality().get().size());
    EXPECT_EQ(cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0],
              cluster_->prioritySet().hostSetsPerPriority()[0]->healthyHosts()[0]);
    HostSharedPtr logical_host = cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0];

    EXPECT_EQ("127.0.0.1:" + std::to_string(expected_hc_port),
              logical_host->healthCheckAddress()->asString());
    EXPECT_EQ("127.0.0.1:" + std::to_string(expected_port), logical_host->address()->asString());

    EXPECT_CALL(server_context_.dispatcher_,
                createClientConnection_(
                    PointeesEq(*Network::Utility::resolveUrl("tcp://127.0.0.1:443")), _, _, _))
        .WillOnce(Return(new NiceMock<Network::MockClientConnection>()));
    logical_host->createConnection(server_context_.dispatcher_, nullptr, nullptr);
    logical_host->outlierDetector().putResult(Outlier::Result::ExtOriginRequestSuccess,
                                              absl::optional<uint64_t>(200));

    expectResolve(Network::DnsLookupFamily::V4Only, expected_address);
    resolve_timer_->invokeCallback();

    // Should not cause any changes.
    EXPECT_CALL(*resolve_timer_, enableTimer(_, _));
    dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                  TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2", "127.0.0.3"}));

    EXPECT_EQ("127.0.0.1:" + std::to_string(expected_hc_port),
              logical_host->healthCheckAddress()->asString());
    EXPECT_EQ("127.0.0.1:" + std::to_string(expected_port), logical_host->address()->asString());

    EXPECT_EQ(logical_host, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
    EXPECT_CALL(server_context_.dispatcher_,
                createClientConnection_(
                    PointeesEq(*Network::Utility::resolveUrl("tcp://127.0.0.1:443")), _, _, _))
        .WillOnce(Return(new NiceMock<Network::MockClientConnection>()));
    Host::CreateConnectionData data =
        logical_host->createConnection(server_context_.dispatcher_, nullptr, nullptr);
    EXPECT_FALSE(data.host_description_->canary());
    EXPECT_EQ(&cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]->cluster(),
              &data.host_description_->cluster());
    EXPECT_EQ(&cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]->stats(),
              &data.host_description_->stats());
    EXPECT_EQ("127.0.0.1:443", data.host_description_->address()->asString());
    EXPECT_EQ("", data.host_description_->locality().region());
    EXPECT_EQ("", data.host_description_->locality().zone());
    EXPECT_EQ("", data.host_description_->locality().sub_zone());
    EXPECT_EQ("foo.bar.com", data.host_description_->hostname());
    EXPECT_FALSE(data.host_description_->lastHcPassTime());
    EXPECT_EQ("", data.host_description_->hostnameForHealthChecks());
    EXPECT_EQ(0, data.host_description_->priority());
    EXPECT_TRUE(TestUtility::protoEqual(envoy::config::core::v3::Metadata::default_instance(),
                                        *data.host_description_->metadata()));
    data.host_description_->outlierDetector().putResult(Outlier::Result::ExtOriginRequestSuccess,
                                                        absl::optional<uint64_t>(200));
    data.host_description_->healthChecker().setUnhealthy(
        HealthCheckHostMonitor::UnhealthyType::ImmediateHealthCheckFail);

    expectResolve(Network::DnsLookupFamily::V4Only, expected_address);
    resolve_timer_->invokeCallback();

    // Should cause a change.
    EXPECT_CALL(*resolve_timer_, enableTimer(_, _));
    dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                  TestUtility::makeDnsResponse({"127.0.0.3", "127.0.0.1", "127.0.0.2"}));

    EXPECT_EQ("127.0.0.3:" + std::to_string(expected_hc_port),
              logical_host->healthCheckAddress()->asString());
    EXPECT_EQ("127.0.0.3:" + std::to_string(expected_port), logical_host->address()->asString());

    EXPECT_EQ(logical_host, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
    EXPECT_CALL(server_context_.dispatcher_,
                createClientConnection_(
                    PointeesEq(*Network::Utility::resolveUrl("tcp://127.0.0.3:443")), _, _, _))
        .WillOnce(Return(new NiceMock<Network::MockClientConnection>()));
    logical_host->createConnection(server_context_.dispatcher_, nullptr, nullptr);

    expectResolve(Network::DnsLookupFamily::V4Only, expected_address);
    resolve_timer_->invokeCallback();

    // Failure should not cause any change.
    ON_CALL(random_, random()).WillByDefault(Return(6000));
    EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(6000), _));
    dns_callback_(Network::DnsResolver::ResolutionStatus::Failure, "", {});

    EXPECT_EQ(logical_host, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
    EXPECT_CALL(server_context_.dispatcher_,
                createClientConnection_(
                    PointeesEq(*Network::Utility::resolveUrl("tcp://127.0.0.3:443")), _, _, _))
        .WillOnce(Return(new NiceMock<Network::MockClientConnection>()));
    logical_host->createConnection(server_context_.dispatcher_, nullptr, nullptr);

    // Empty Completed should not cause any change.
    ON_CALL(random_, random()).WillByDefault(Return(6000));
    EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(6000), _));
    dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "", {});

    EXPECT_EQ(logical_host, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]);
    EXPECT_CALL(server_context_.dispatcher_,
                createClientConnection_(
                    PointeesEq(*Network::Utility::resolveUrl("tcp://127.0.0.3:443")), _, _, _))
        .WillOnce(Return(new NiceMock<Network::MockClientConnection>()));
    logical_host->createConnection(server_context_.dispatcher_, nullptr, nullptr);

    // Make sure we cancel.
    EXPECT_CALL(active_dns_query_, cancel(Network::ActiveDnsQuery::CancelReason::QueryAbandoned));
    expectResolve(Network::DnsLookupFamily::V4Only, expected_address);
    resolve_timer_->invokeCallback();
  }

  NiceMock<Server::Configuration::MockServerFactoryContext> server_context_;
  Stats::TestUtil::TestStore& stats_store_ = server_context_.store_;
  NiceMock<Random::MockRandomGenerator> random_;
  Api::ApiPtr api_;

  std::shared_ptr<NiceMock<Network::MockDnsResolver>> dns_resolver_{
      new NiceMock<Network::MockDnsResolver>};
  Network::MockActiveDnsQuery active_dns_query_;
  Network::DnsResolver::ResolveCb dns_callback_;
  Event::MockTimer* resolve_timer_;
  ReadyWatcher membership_updated_;
  ReadyWatcher initialized_;
  std::shared_ptr<LogicalDnsCluster> cluster_;
  NiceMock<ProtobufMessage::MockValidationVisitor> validation_visitor_;
  Common::CallbackHandlePtr priority_update_cb_;
  NiceMock<AccessLog::MockAccessLogManager> access_log_manager_;
};

namespace {
using LogicalDnsConfigTuple =
    std::tuple<std::string, Network::DnsLookupFamily, std::list<std::string>>;
std::vector<LogicalDnsConfigTuple> generateLogicalDnsParams() {
  std::vector<LogicalDnsConfigTuple> dns_config;
  {
    std::string family_yaml("");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::Auto);
    std::list<std::string> dns_response{"127.0.0.1", "127.0.0.2"};
    dns_config.push_back(std::make_tuple(family_yaml, family, dns_response));
  }
  {
    std::string family_yaml(R"EOF(dns_lookup_family: v4_only
                            )EOF");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::V4Only);
    std::list<std::string> dns_response{"127.0.0.1", "127.0.0.2"};
    dns_config.push_back(std::make_tuple(family_yaml, family, dns_response));
  }
  {
    std::string family_yaml(R"EOF(dns_lookup_family: v6_only
                            )EOF");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::V6Only);
    std::list<std::string> dns_response{"::1", "::2"};
    dns_config.push_back(std::make_tuple(family_yaml, family, dns_response));
  }
  {
    std::string family_yaml(R"EOF(dns_lookup_family: auto
                            )EOF");
    Network::DnsLookupFamily family(Network::DnsLookupFamily::Auto);
    std::list<std::string> dns_response{"::1"};
    dns_config.push_back(std::make_tuple(family_yaml, family, dns_response));
  }
  return dns_config;
}

class LogicalDnsParamTest : public LogicalDnsClusterTest,
                            public testing::WithParamInterface<LogicalDnsConfigTuple> {};

INSTANTIATE_TEST_SUITE_P(DnsParam, LogicalDnsParamTest,
                         testing::ValuesIn(generateLogicalDnsParams()));

// Validate that if the DNS resolves immediately, during the LogicalDnsCluster
// constructor, we have the expected host state and initialization callback
// invocation.
TEST_P(LogicalDnsParamTest, ImmediateResolve) {
  const std::string yaml = R"EOF(
  name: name
  connect_timeout: 0.25s
  type: logical_dns
  lb_policy: round_robin
  )EOF" + std::get<0>(GetParam()) +
                           R"EOF(
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(initialized_, ready());
  EXPECT_CALL(*dns_resolver_, resolve("foo.bar.com", std::get<1>(GetParam()), _))
      .WillOnce(Invoke([&](const std::string&, Network::DnsLookupFamily,
                           Network::DnsResolver::ResolveCb cb) -> Network::ActiveDnsQuery* {
        EXPECT_CALL(*resolve_timer_, enableTimer(_, _));
        cb(Network::DnsResolver::ResolutionStatus::Completed, "",
           TestUtility::makeDnsResponse(std::get<2>(GetParam())));
        return nullptr;
      }));
  setupFromV3Yaml(yaml);
  EXPECT_EQ(1UL, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts().size());
  EXPECT_EQ(1UL, cluster_->prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ("foo.bar.com",
            cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]->hostname());
  cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]->healthChecker().setUnhealthy(
      HealthCheckHostMonitor::UnhealthyType::ImmediateHealthCheckFail);
}

TEST_F(LogicalDnsParamTest, FailureRefreshRateBackoffResetsWhenSuccessHappens) {
  const std::string yaml = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  dns_failure_refresh_rate:
    base_interval: 7s
    max_interval: 10s
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                hostname: foo
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  setupFromV3Yaml(yaml);

  // Failing response kicks the failure refresh backoff strategy.
  ON_CALL(random_, random()).WillByDefault(Return(8000));
  EXPECT_CALL(initialized_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(1000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Failure, "", {});

  // Successful call should reset the failure backoff strategy.
  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}));
  EXPECT_EQ(1UL, cluster_->prioritySet().hostSetsPerPriority()[0]->hosts().size());
  EXPECT_EQ(1UL, cluster_->prioritySet().hostSetsPerPriority()[0]->healthyHosts().size());
  EXPECT_EQ("foo", cluster_->prioritySet().hostSetsPerPriority()[0]->hosts()[0]->hostname());

  // Therefore, a subsequent failure should get a [0,base * 1] refresh.
  ON_CALL(random_, random()).WillByDefault(Return(8000));
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(1000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Failure, "", {});
}

TEST_F(LogicalDnsParamTest, TtlAsDnsRefreshRate) {
  const std::string yaml = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  respect_dns_ttl: true
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                     address: foo.bar.com
                     port_value: 443
  )EOF";

  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  setupFromV3Yaml(yaml);

  // TTL is recorded when the DNS response is successful and not empty
  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(initialized_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(5000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}, std::chrono::seconds(5)));

  // If the response is successful but empty, the cluster uses the cluster configured refresh rate.
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                TestUtility::makeDnsResponse({}, std::chrono::seconds(5)));

  // On failure, the cluster uses the cluster configured refresh rate.
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Failure, "",
                TestUtility::makeDnsResponse({}, std::chrono::seconds(5)));
}

TEST_F(LogicalDnsClusterTest, BadConfig) {
  const std::string multiple_hosts_yaml = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  load_assignment:
        cluster_name: name
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
            - endpoint:
                address:
                  socket_address:
                    address: foo2.bar.com
                    port_value: 443
  )EOF";

  EXPECT_EQ(
      factorySetupFromV3Yaml(multiple_hosts_yaml).message(),
      "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");

  const std::string multiple_hosts_cluster_type_yaml = R"EOF(
  name: name
  cluster_type:
    name: envoy.cluster.strict_dns # (this is right, name shouldnt matter)
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_refresh_rate: 4s
      all_addresses_in_single_endpoint: true
      dns_lookup_family: V4_ONLY
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  load_assignment:
    cluster_name: name
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
            health_check_config:
              port_value: 8000
        - endpoint:
            address:
              socket_address:
                address: hello.world.com
                port_value: 443
            health_check_config:
              port_value: 8000
  )EOF";

  EXPECT_EQ(
      factorySetupFromV3Yaml(multiple_hosts_cluster_type_yaml).message(),
      "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");

  const std::string multiple_lb_endpoints_yaml = R"EOF(
    name: name
    type: LOGICAL_DNS
    dns_refresh_rate: 4s
    connect_timeout: 0.25s
    lb_policy: ROUND_ROBIN
    dns_lookup_family: V4_ONLY
    load_assignment:
      cluster_name: name
      endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: foo.bar.com
                  port_value: 443
              health_check_config:
                port_value: 8000
          - endpoint:
              address:
                socket_address:
                  address: hello.world.com
                  port_value: 443
              health_check_config:
                port_value: 8000
    )EOF";

  EXPECT_EQ(
      factorySetupFromV3Yaml(multiple_lb_endpoints_yaml).message(),
      "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");

  const std::string multiple_lb_endpoints_cluster_type_yaml = R"EOF(
    name: name
    cluster_type:
      name: envoy.cluster.logical_dns
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
        dns_refresh_rate: 4s
        all_addresses_in_single_endpoint: true
    connect_timeout: 0.25s
    lb_policy: ROUND_ROBIN
    dns_lookup_family: V4_ONLY
    load_assignment:
      cluster_name: name
      endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: foo.bar.com
                  port_value: 443
              health_check_config:
                port_value: 8000
          - endpoint:
              address:
                socket_address:
                  address: hello.world.com
                  port_value: 443
              health_check_config:
                port_value: 8000
    )EOF";

  EXPECT_EQ(
      factorySetupFromV3Yaml(multiple_lb_endpoints_cluster_type_yaml).message(),
      "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");

  const std::string multiple_endpoints_yaml = R"EOF(
    name: name
    type: LOGICAL_DNS
    dns_refresh_rate: 4s
    connect_timeout: 0.25s
    lb_policy: ROUND_ROBIN
    dns_lookup_family: V4_ONLY
    load_assignment:
      cluster_name: name
      endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: foo.bar.com
                  port_value: 443
              health_check_config:
                port_value: 8000

        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: hello.world.com
                  port_value: 443
              health_check_config:
                port_value: 8000
    )EOF";

  EXPECT_EQ(
      factorySetupFromV3Yaml(multiple_endpoints_yaml).message(),
      "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");

  const std::string multiple_endpoints_cluster_type_yaml = R"EOF(
    name: name
    cluster_type:
      name: abc
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
        dns_lookup_family: V4_ONLY
        dns_refresh_rate: 4s
        all_addresses_in_single_endpoint: true
    connect_timeout: 0.25s
    lb_policy: ROUND_ROBIN
    load_assignment:
      cluster_name: name
      endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: foo.bar.com
                  port_value: 443
              health_check_config:
                port_value: 8000
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: hello.world.com
                  port_value: 443
              health_check_config:
                port_value: 8000
    )EOF";

  EXPECT_EQ(
      factorySetupFromV3Yaml(multiple_endpoints_cluster_type_yaml).message(),
      "LOGICAL_DNS clusters must have a single locality_lb_endpoint and a single lb_endpoint");

  const std::string custom_resolver_yaml = R"EOF(
    name: name
    type: LOGICAL_DNS
    dns_refresh_rate: 4s
    connect_timeout: 0.25s
    lb_policy: ROUND_ROBIN
    dns_lookup_family: V4_ONLY
    load_assignment:
      cluster_name: name
      endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: hello.world.com
                  port_value: 443
                  resolver_name: customresolver
              health_check_config:
                port_value: 8000
    )EOF";

  EXPECT_EQ(factorySetupFromV3Yaml(custom_resolver_yaml).message(),
            "LOGICAL_DNS clusters must NOT have a custom resolver name set");

  const std::string custom_resolver_cluster_type_yaml = R"EOF(
    name: name
    cluster_type:
      name: abc
      typed_config:
        "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
        dns_lookup_family: V4_ONLY
        dns_refresh_rate: 4s
        all_addresses_in_single_endpoint: true
    connect_timeout: 0.25s
    lb_policy: ROUND_ROBIN
    load_assignment:
      cluster_name: name
      endpoints:
        - lb_endpoints:
          - endpoint:
              address:
                socket_address:
                  address: hello.world.com
                  port_value: 443
                  resolver_name: customresolver
              health_check_config:
                port_value: 8000
    )EOF";

  EXPECT_EQ(factorySetupFromV3Yaml(custom_resolver_cluster_type_yaml).message(),
            "LOGICAL_DNS clusters must NOT have a custom resolver name set");
}

// Test using both types of names in the cluster type.
TEST_F(LogicalDnsClusterTest, UseDnsExtension) {
  const std::string config = R"EOF(
  name: name
  cluster_type:
    name: arglegarble
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.clusters.dns.v3.DnsCluster
      dns_refresh_rate: 4s
      dns_lookup_family: V4_ONLY
      all_addresses_in_single_endpoint: true
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  wait_for_warm_on_init: false
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  EXPECT_CALL(initialized_, ready());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  ASSERT_TRUE(factorySetupFromV3Yaml(config).ok());

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));

  dns_callback_(
      Network::DnsResolver::ResolutionStatus::Completed, "",
      TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}, std::chrono::seconds(3000)));
}

TEST_F(LogicalDnsClusterTest, TypedConfigBackcompat) {
  const std::string config = R"EOF(
  name: name
  cluster_type:
    name: envoy.cluster.logical_dns
  dns_refresh_rate: 4s
  dns_lookup_family: V4_ONLY
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  wait_for_warm_on_init: false
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  EXPECT_CALL(initialized_, ready());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  ASSERT_TRUE(factorySetupFromV3Yaml(config).ok());

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));

  dns_callback_(
      Network::DnsResolver::ResolutionStatus::Completed, "",
      TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}, std::chrono::seconds(3000)));
}

TEST_F(LogicalDnsClusterTest, Basic) {
  const std::string basic_yaml_hosts = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  dns_failure_refresh_rate:
    base_interval: 7s
    max_interval: 10s
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  const std::string basic_yaml_load_assignment = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  dns_failure_refresh_rate:
    base_interval: 7s
    max_interval: 10s
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  load_assignment:
    cluster_name: name
    endpoints:
      - lb_endpoints:
        - endpoint:
            address:
              socket_address:
                address: foo.bar.com
                port_value: 443
            health_check_config:
              port_value: 8000
  )EOF";

  testBasicSetup(basic_yaml_hosts, "foo.bar.com", 443, 443);
  // Expect to override the health check address port value.
  testBasicSetup(basic_yaml_load_assignment, "foo.bar.com", 443, 8000);
}

TEST_F(LogicalDnsClusterTest, DontWaitForDNSOnInit) {
  const std::string config = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  dns_failure_refresh_rate:
    base_interval: 7s
    max_interval: 10s
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  wait_for_warm_on_init: false
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  EXPECT_CALL(initialized_, ready());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  setupFromV3Yaml(config);

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000), _));
  dns_callback_(Network::DnsResolver::ResolutionStatus::Completed, "",
                TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}));
}

TEST_F(LogicalDnsClusterTest, DNSRefreshHasJitter) {
  const std::string config = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 4s
  dns_jitter:
    seconds: 0
    nanos: 512000000
  connect_timeout: 0.25s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  wait_for_warm_on_init: false
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  uint64_t random_return = 8000;
  uint64_t jitter_ms = random_return % 512; // default value

  EXPECT_CALL(initialized_, ready());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  setupFromV3Yaml(config);

  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(std::chrono::milliseconds(4000 + jitter_ms), _));
  ON_CALL(random_, random()).WillByDefault(Return(random_return));

  dns_callback_(
      Network::DnsResolver::ResolutionStatus::Completed, "",
      TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}, std::chrono::seconds(3000)));
}

TEST_F(LogicalDnsClusterTest, NegativeDnsJitter) {
  const std::string yaml = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_jitter: -1s
  lb_policy: ROUND_ROBIN
  dns_lookup_family: V4_ONLY
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";
  EXPECT_THROW_WITH_REGEX(setupFromV3Yaml(yaml, false), EnvoyException,
                          "(?s)Invalid duration: Expected positive duration:.*seconds: -1\n");
}

TEST_F(LogicalDnsClusterTest, ExtremeJitter) {
  // When random returns large values, they were being reinterpreted as very negative values causing
  // negative refresh rates.
  const std::string jitter_yaml = R"EOF(
  name: name
  type: LOGICAL_DNS
  dns_refresh_rate: 1s
  dns_failure_refresh_rate:
    base_interval: 7s
    max_interval: 10s
  connect_timeout: 0.25s
  dns_jitter: 1000s
  lb_policy: ROUND_ROBIN
  # Since the following expectResolve() requires Network::DnsLookupFamily::V4Only we need to set
  # dns_lookup_family to V4_ONLY explicitly for v2 .yaml config.
  dns_lookup_family: V4_ONLY
  load_assignment:
        endpoints:
          - lb_endpoints:
            - endpoint:
                address:
                  socket_address:
                    address: foo.bar.com
                    port_value: 443
  )EOF";

  EXPECT_CALL(initialized_, ready());
  expectResolve(Network::DnsLookupFamily::V4Only, "foo.bar.com");
  setupFromV3Yaml(jitter_yaml);
  EXPECT_CALL(membership_updated_, ready());
  EXPECT_CALL(*resolve_timer_, enableTimer(testing::Ge(std::chrono::milliseconds(4000)), _));
  ON_CALL(random_, random()).WillByDefault(Return(std::numeric_limits<int64_t>::min()));
  dns_callback_(
      Network::DnsResolver::ResolutionStatus::Completed, "",
      TestUtility::makeDnsResponse({"127.0.0.1", "127.0.0.2"}, std::chrono::seconds(3000)));
}

} // namespace
} // namespace Upstream
} // namespace Envoy
