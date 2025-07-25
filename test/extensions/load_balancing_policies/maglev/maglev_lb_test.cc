#include <memory>

#include "envoy/config/cluster/v3/cluster.pb.h"

#include "source/extensions/load_balancing_policies/maglev/maglev_lb.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/server/server_factory_context.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/host_set.h"
#include "test/mocks/upstream/load_balancer_context.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_runtime.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Upstream {
namespace {

using testing::Return;

class TestLoadBalancerContext : public LoadBalancerContextBase {
public:
  using HostPredicate = std::function<bool(const Host&)>;

  TestLoadBalancerContext(uint64_t hash_key)
      : TestLoadBalancerContext(hash_key, 0, [](const Host&) { return false; }) {}
  TestLoadBalancerContext(uint64_t hash_key, uint32_t retry_count,
                          HostPredicate should_select_another_host)
      : hash_key_(hash_key), retry_count_(retry_count),
        should_select_another_host_(should_select_another_host) {}

  // Upstream::LoadBalancerContext
  absl::optional<uint64_t> computeHashKey() override { return hash_key_; }
  uint32_t hostSelectionRetryCount() const override { return retry_count_; };
  bool shouldSelectAnotherHost(const Host& host) override {
    return should_select_another_host_(host);
  }

  absl::optional<uint64_t> hash_key_;
  uint32_t retry_count_;
  HostPredicate should_select_another_host_;
};

TEST(MaglevTableLogMaglevTableTest, MaglevTableLogMaglevTableTest) {
  Stats::IsolatedStoreImpl stats_store;
  MaglevLoadBalancerStats stats = MaglevLoadBalancer::generateStats(*stats_store.rootScope());

  auto host1 = std::make_shared<NiceMock<MockHost>>();
  const std::string hostname1 = "host1";
  ON_CALL(*host1, hostname()).WillByDefault(testing::ReturnRef(hostname1));

  NormalizedHostWeightVector normalized_host_weights = {{host1, 1}};

  {
    CompactMaglevTable table(normalized_host_weights, 1, 2, true, stats);
    table.logMaglevTable(true);
  }

  {
    CompactMaglevTable table(normalized_host_weights, 1, 2, true, stats);
    table.logMaglevTable(true);
  }
}

// Note: ThreadAwareLoadBalancer base is heavily tested by RingHashLoadBalancerTest. Only basic
//       functionality is covered here.
class MaglevLoadBalancerTest : public Event::TestUsingSimulatedTime, public testing::Test {
public:
  MaglevLoadBalancerTest()
      : stat_names_(stats_store_.symbolTable()), stats_(stat_names_, *stats_store_.rootScope()) {}

  void createLb() {
    absl::Status creation_status;
    TypedMaglevLbConfig typed_config(config_, context_.regex_engine_, creation_status);
    ASSERT(creation_status.ok());

    lb_ = std::make_unique<MaglevLoadBalancer>(priority_set_, stats_, *stats_store_.rootScope(),
                                               context_.runtime_loader_, context_.api_.random_, 50,
                                               typed_config.lb_config_, typed_config.hash_policy_);
  }

  void init(uint64_t table_size, bool locality_weighted_balancing = false) {
    config_.mutable_table_size()->set_value(table_size);
    if (locality_weighted_balancing) {
      config_.mutable_locality_weighted_lb_config();
    }

    createLb();
    EXPECT_TRUE(lb_->initialize().ok());
  }

  NiceMock<MockPrioritySet> priority_set_;

  // Just use this as parameters of create() method but thread aware load balancer will not use it.
  NiceMock<MockPrioritySet> worker_priority_set_;
  LoadBalancerParams lb_params_{worker_priority_set_, {}};

  MockHostSet& host_set_ = *priority_set_.getMockHostSet(0);
  std::shared_ptr<MockClusterInfo> info_{new NiceMock<MockClusterInfo>()};
  Stats::IsolatedStoreImpl stats_store_;
  ClusterLbStatNames stat_names_;
  ClusterLbStats stats_;
  envoy::extensions::load_balancing_policies::maglev::v3::Maglev config_;
  NiceMock<Server::Configuration::MockServerFactoryContext> context_;

  std::unique_ptr<MaglevLoadBalancer> lb_;
};

// Works correctly without any hosts.
TEST_F(MaglevLoadBalancerTest, NoHost) {
  init(7);
  EXPECT_EQ(nullptr, lb_->factory()->create(lb_params_)->chooseHost(nullptr).host);
};

// Test for thread aware load balancer destructed before load balancer factory. After CDS removes a
// cluster, the operation does not immediately reach the worker thread. There may be cases where the
// thread aware load balancer is destructed, but the load balancer factory is still used in the
// worker thread.
TEST_F(MaglevLoadBalancerTest, LbDestructedBeforeFactory) {
  init(7);

  auto factory = lb_->factory();
  lb_.reset();

  EXPECT_NE(nullptr, factory->create(lb_params_));
}

// Throws an exception if table size is not a prime number.
TEST_F(MaglevLoadBalancerTest, NoPrimeNumber) {
  EXPECT_THROW_WITH_MESSAGE(init(8), EnvoyException,
                            "The table size of maglev must be prime number");
};

// Check it has default table size if config is null or table size has invalid value.
TEST_F(MaglevLoadBalancerTest, DefaultMaglevTableSize) {
  const uint64_t defaultValue = MaglevTable::DefaultTableSize;

  createLb();
  EXPECT_EQ(defaultValue, lb_->tableSize());

  createLb();
  EXPECT_EQ(defaultValue, lb_->tableSize());
};

// Basic sanity tests.
TEST_F(MaglevLoadBalancerTest, Basic) {
  host_set_.hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:90"), makeTestHost(info_, "tcp://127.0.0.1:91"),
      makeTestHost(info_, "tcp://127.0.0.1:92"), makeTestHost(info_, "tcp://127.0.0.1:93"),
      makeTestHost(info_, "tcp://127.0.0.1:94"), makeTestHost(info_, "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:92
  // maglev: i=1 host=127.0.0.1:94
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:91
  // maglev: i=4 host=127.0.0.1:95
  // maglev: i=5 host=127.0.0.1:90
  // maglev: i=6 host=127.0.0.1:93
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{2, 4, 0, 1, 5, 0, 3};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Test bounded load. This test only ensures that the
// hash balancer factory won't break the normal load balancer process.
TEST_F(MaglevLoadBalancerTest, BasicWithBoundedLoad) {
  host_set_.hosts_ = {makeTestHost(info_, "90", "tcp://127.0.0.1:90"),
                      makeTestHost(info_, "91", "tcp://127.0.0.1:91"),
                      makeTestHost(info_, "92", "tcp://127.0.0.1:92"),
                      makeTestHost(info_, "93", "tcp://127.0.0.1:93"),
                      makeTestHost(info_, "94", "tcp://127.0.0.1:94"),
                      makeTestHost(info_, "95", "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  config_.mutable_consistent_hashing_lb_config()->set_use_hostname_for_hashing(true);
  config_.mutable_consistent_hashing_lb_config()->mutable_hash_balance_factor()->set_value(200);
  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=92
  // maglev: i=1 host=95
  // maglev: i=2 host=90
  // maglev: i=3 host=93
  // maglev: i=4 host=94
  // maglev: i=5 host=91
  // maglev: i=6 host=90
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{2, 5, 0, 3, 4, 1, 0};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Basic with hostname.
TEST_F(MaglevLoadBalancerTest, BasicWithHostName) {
  host_set_.hosts_ = {makeTestHost(info_, "90", "tcp://127.0.0.1:90"),
                      makeTestHost(info_, "91", "tcp://127.0.0.1:91"),
                      makeTestHost(info_, "92", "tcp://127.0.0.1:92"),
                      makeTestHost(info_, "93", "tcp://127.0.0.1:93"),
                      makeTestHost(info_, "94", "tcp://127.0.0.1:94"),
                      makeTestHost(info_, "95", "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  config_.mutable_consistent_hashing_lb_config()->set_use_hostname_for_hashing(true);
  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=92
  // maglev: i=1 host=95
  // maglev: i=2 host=90
  // maglev: i=3 host=93
  // maglev: i=4 host=94
  // maglev: i=5 host=91
  // maglev: i=6 host=90
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{2, 5, 0, 3, 4, 1, 0};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Basic with metadata hash_key.
TEST_F(MaglevLoadBalancerTest, BasicWithMetadataHashKey) {
  host_set_.hosts_ = {makeTestHostWithHashKey(info_, "90", "tcp://127.0.0.1:90"),
                      makeTestHostWithHashKey(info_, "91", "tcp://127.0.0.1:91"),
                      makeTestHostWithHashKey(info_, "92", "tcp://127.0.0.1:92"),
                      makeTestHostWithHashKey(info_, "93", "tcp://127.0.0.1:93"),
                      makeTestHostWithHashKey(info_, "94", "tcp://127.0.0.1:94"),
                      makeTestHostWithHashKey(info_, "95", "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  config_.mutable_consistent_hashing_lb_config()->set_use_hostname_for_hashing(true);
  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=92
  // maglev: i=1 host=95
  // maglev: i=2 host=90
  // maglev: i=3 host=93
  // maglev: i=4 host=94
  // maglev: i=5 host=91
  // maglev: i=6 host=90
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{2, 5, 0, 3, 4, 1, 0};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

TEST_F(MaglevLoadBalancerTest, MaglevLbWithHashPolicy) {
  host_set_.hosts_ = {makeTestHostWithHashKey(info_, "90", "tcp://127.0.0.1:90"),
                      makeTestHostWithHashKey(info_, "91", "tcp://127.0.0.1:91"),
                      makeTestHostWithHashKey(info_, "92", "tcp://127.0.0.1:92"),
                      makeTestHostWithHashKey(info_, "93", "tcp://127.0.0.1:93"),
                      makeTestHostWithHashKey(info_, "94", "tcp://127.0.0.1:94"),
                      makeTestHostWithHashKey(info_, "95", "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});

  config_.mutable_consistent_hashing_lb_config()->set_use_hostname_for_hashing(true);
  auto* hash_policy = config_.mutable_consistent_hashing_lb_config()->add_hash_policy();
  *hash_policy->mutable_cookie()->mutable_name() = "test-cookie-name";
  *hash_policy->mutable_cookie()->mutable_path() = "/test/path";
  hash_policy->mutable_cookie()->mutable_ttl()->set_seconds(1000);

  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);

  {
    // Cookie exists.
    Http::TestRequestHeaderMapImpl request_headers{{"cookie", "test-cookie-name=1234567890"}};
    NiceMock<StreamInfo::MockStreamInfo> stream_info;

    NiceMock<Upstream::MockLoadBalancerContext> context;
    EXPECT_CALL(context, downstreamHeaders()).Times(2).WillRepeatedly(Return(&request_headers));
    EXPECT_CALL(context, requestStreamInfo()).Times(2).WillRepeatedly(Return(&stream_info));

    EXPECT_CALL(context_.api_.random_, random()).Times(0);

    auto host_1 = lb->chooseHost(&context);
    auto host_2 = lb->chooseHost(&context);
    EXPECT_EQ(host_1.host, host_2.host);
  }

  {
    // Cookie not exists and no stream info is provided.
    Http::TestRequestHeaderMapImpl request_headers{};

    NiceMock<Upstream::MockLoadBalancerContext> context;
    EXPECT_CALL(context, downstreamHeaders()).Times(2).WillRepeatedly(Return(&request_headers));

    // No hash is generated and random will be used.
    EXPECT_CALL(context_.api_.random_, random()).Times(2);
    auto host_1 = lb->chooseHost(&context);
    auto host_2 = lb->chooseHost(&context);
  }

  {
    // Cookie not exists and no valid addresses.
    Http::TestRequestHeaderMapImpl request_headers{};
    NiceMock<StreamInfo::MockStreamInfo> stream_info;
    stream_info.downstream_connection_info_provider_->setRemoteAddress(nullptr);
    stream_info.downstream_connection_info_provider_->setLocalAddress(nullptr);

    NiceMock<Upstream::MockLoadBalancerContext> context;
    EXPECT_CALL(context, downstreamHeaders()).Times(2).WillRepeatedly(Return(&request_headers));
    EXPECT_CALL(context, requestStreamInfo()).Times(4).WillRepeatedly(Return(&stream_info));

    // No hash is generated and random will be used.
    EXPECT_CALL(context_.api_.random_, random()).Times(2);
    auto host_1 = lb->chooseHost(&context);
    auto host_2 = lb->chooseHost(&context);
  }

  {
    // Cookie not exists and has valid addresses.
    Http::TestRequestHeaderMapImpl request_headers{};
    NiceMock<StreamInfo::MockStreamInfo> stream_info;

    NiceMock<Upstream::MockLoadBalancerContext> context;
    EXPECT_CALL(context, downstreamHeaders()).Times(2).WillRepeatedly(Return(&request_headers));
    EXPECT_CALL(context, requestStreamInfo()).Times(4).WillRepeatedly(Return(&stream_info));

    const std::string address_values =
        stream_info.downstream_connection_info_provider_->remoteAddress()->asString() +
        stream_info.downstream_connection_info_provider_->localAddress()->asString();
    std::string new_cookie_value = Hex::uint64ToHex(HashUtil::xxHash64(address_values));

    EXPECT_CALL(context, setHeadersModifier(_))
        .WillRepeatedly(
            testing::Invoke([&](std::function<void(Http::ResponseHeaderMap&)> modifier) {
              Http::TestResponseHeaderMapImpl response;
              modifier(response);
              // Cookie is set.
              EXPECT_TRUE(absl::StrContains(response.get_(Http::Headers::get().SetCookie),
                                            new_cookie_value));
            }));

    // Hash is generated and random will not be used.
    EXPECT_CALL(context_.api_.random_, random()).Times(0);
    auto host_1 = lb->chooseHost(&context);
    auto host_2 = lb->chooseHost(&context);
    EXPECT_EQ(host_1.host, host_2.host);
  }
}

// Same ring as the Basic test, but exercise retry host predicate behavior.
TEST_F(MaglevLoadBalancerTest, BasicWithRetryHostPredicate) {
  host_set_.hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:90"), makeTestHost(info_, "tcp://127.0.0.1:91"),
      makeTestHost(info_, "tcp://127.0.0.1:92"), makeTestHost(info_, "tcp://127.0.0.1:93"),
      makeTestHost(info_, "tcp://127.0.0.1:94"), makeTestHost(info_, "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:92
  // maglev: i=1 host=127.0.0.1:94
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:91
  // maglev: i=4 host=127.0.0.1:95
  // maglev: i=5 host=127.0.0.1:90
  // maglev: i=6 host=127.0.0.1:93
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  {
    // Confirm that i=3 is selected by the hash.
    TestLoadBalancerContext context(10);
    EXPECT_EQ(host_set_.hosts_[1], lb->chooseHost(&context).host);
  }
  {
    // First attempt succeeds even when retry count is > 0.
    TestLoadBalancerContext context(10, 2, [](const Host&) { return false; });
    EXPECT_EQ(host_set_.hosts_[1], lb->chooseHost(&context).host);
  }
  {
    // Second attempt chooses a different host in the ring.
    TestLoadBalancerContext context(
        10, 2, [&](const Host& host) { return &host == host_set_.hosts_[1].get(); });
    EXPECT_EQ(host_set_.hosts_[0], lb->chooseHost(&context).host);
  }
  {
    // Exhausted retries return the last checked host.
    TestLoadBalancerContext context(10, 2, [](const Host&) { return true; });
    EXPECT_EQ(host_set_.hosts_[5], lb->chooseHost(&context).host);
  }
}

// Basic stability test.
TEST_F(MaglevLoadBalancerTest, BasicStability) {
  host_set_.hosts_ = {
      makeTestHost(info_, "tcp://127.0.0.1:90"), makeTestHost(info_, "tcp://127.0.0.1:91"),
      makeTestHost(info_, "tcp://127.0.0.1:92"), makeTestHost(info_, "tcp://127.0.0.1:93"),
      makeTestHost(info_, "tcp://127.0.0.1:94"), makeTestHost(info_, "tcp://127.0.0.1:95")};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  init(7);

  EXPECT_EQ("maglev_lb.min_entries_per_host", lb_->stats().min_entries_per_host_.name());
  EXPECT_EQ("maglev_lb.max_entries_per_host", lb_->stats().max_entries_per_host_.name());
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(2, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:92
  // maglev: i=1 host=127.0.0.1:94
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:91
  // maglev: i=4 host=127.0.0.1:95
  // maglev: i=5 host=127.0.0.1:90
  // maglev: i=6 host=127.0.0.1:93
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{2, 4, 0, 1, 5, 0, 3};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }

  // Shuffle healthy_hosts_ to check stability of assignments
  std::shuffle(host_set_.healthy_hosts_.begin(), host_set_.healthy_hosts_.end(),
               std::default_random_engine());
  host_set_.runCallbacks({}, {});
  lb = lb_->factory()->create(lb_params_);

  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Weighted sanity test.
TEST_F(MaglevLoadBalancerTest, Weighted) {
  host_set_.hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:90", 1),
                      makeTestHost(info_, "tcp://127.0.0.1:91", 2)};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  init(17);
  EXPECT_EQ(6, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(11, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:91
  // maglev: i=1 host=127.0.0.1:90
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:91
  // maglev: i=4 host=127.0.0.1:90
  // maglev: i=5 host=127.0.0.1:91
  // maglev: i=6 host=127.0.0.1:91
  // maglev: i=7 host=127.0.0.1:90
  // maglev: i=8 host=127.0.0.1:91
  // maglev: i=9 host=127.0.0.1:91
  // maglev: i=10 host=127.0.0.1:91
  // maglev: i=11 host=127.0.0.1:91
  // maglev: i=12 host=127.0.0.1:91
  // maglev: i=13 host=127.0.0.1:90
  // maglev: i=14 host=127.0.0.1:91
  // maglev: i=15 host=127.0.0.1:90
  // maglev: i=16 host=127.0.0.1:91
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{1, 0, 0, 1, 0, 1, 1, 0, 1,
                                                   1, 1, 1, 1, 0, 1, 0, 1};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Locality weighted sanity test when localities have the same weights. Host weights for hosts in
// different localities shouldn't matter.
TEST_F(MaglevLoadBalancerTest, LocalityWeightedSameLocalityWeights) {
  envoy::config::core::v3::Locality zone_a;
  zone_a.set_zone("A");
  envoy::config::core::v3::Locality zone_b;
  zone_b.set_zone("B");

  host_set_.hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:90", zone_a, 1),
                      makeTestHost(info_, "tcp://127.0.0.1:91", zone_b, 2)};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.hosts_per_locality_ =
      makeHostsPerLocality({{host_set_.hosts_[0]}, {host_set_.hosts_[1]}});
  host_set_.healthy_hosts_per_locality_ = host_set_.hosts_per_locality_;
  LocalityWeightsConstSharedPtr locality_weights{new LocalityWeights{1, 1}};
  host_set_.locality_weights_ = locality_weights;
  host_set_.runCallbacks({}, {});
  init(17, true);
  EXPECT_EQ(8, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(9, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:91
  // maglev: i=1 host=127.0.0.1:90
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:91
  // maglev: i=4 host=127.0.0.1:90
  // maglev: i=5 host=127.0.0.1:91
  // maglev: i=6 host=127.0.0.1:91
  // maglev: i=7 host=127.0.0.1:90
  // maglev: i=8 host=127.0.0.1:90
  // maglev: i=9 host=127.0.0.1:91
  // maglev: i=10 host=127.0.0.1:90
  // maglev: i=11 host=127.0.0.1:91
  // maglev: i=12 host=127.0.0.1:90
  // maglev: i=13 host=127.0.0.1:90
  // maglev: i=14 host=127.0.0.1:91
  // maglev: i=15 host=127.0.0.1:90
  // maglev: i=16 host=127.0.0.1:91
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{1, 0, 0, 1, 0, 1, 1, 0, 0,
                                                   1, 0, 1, 0, 0, 1, 0, 1};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Locality weighted sanity test when localities have different weights. Host weights for hosts in
// different localities shouldn't matter.
TEST_F(MaglevLoadBalancerTest, LocalityWeightedDifferentLocalityWeights) {
  envoy::config::core::v3::Locality zone_a;
  zone_a.set_zone("A");
  envoy::config::core::v3::Locality zone_b;
  zone_b.set_zone("B");
  envoy::config::core::v3::Locality zone_c;
  zone_c.set_zone("C");

  host_set_.hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:90", zone_a, 1),
                      makeTestHost(info_, "tcp://127.0.0.1:91", zone_c, 2),
                      makeTestHost(info_, "tcp://127.0.0.1:92", zone_b, 3)};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.hosts_per_locality_ =
      makeHostsPerLocality({{host_set_.hosts_[0]}, {host_set_.hosts_[2]}, {host_set_.hosts_[1]}});
  host_set_.healthy_hosts_per_locality_ = host_set_.hosts_per_locality_;
  LocalityWeightsConstSharedPtr locality_weights{new LocalityWeights{8, 0, 2}};
  host_set_.locality_weights_ = locality_weights;
  host_set_.runCallbacks({}, {});
  init(17, true);
  EXPECT_EQ(4, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(13, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:91
  // maglev: i=1 host=127.0.0.1:90
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:90
  // maglev: i=4 host=127.0.0.1:90
  // maglev: i=5 host=127.0.0.1:90
  // maglev: i=6 host=127.0.0.1:91
  // maglev: i=7 host=127.0.0.1:90
  // maglev: i=8 host=127.0.0.1:90
  // maglev: i=9 host=127.0.0.1:91
  // maglev: i=10 host=127.0.0.1:90
  // maglev: i=11 host=127.0.0.1:91
  // maglev: i=12 host=127.0.0.1:90
  // maglev: i=13 host=127.0.0.1:90
  // maglev: i=14 host=127.0.0.1:90
  // maglev: i=15 host=127.0.0.1:90
  // maglev: i=16 host=127.0.0.1:90
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{1, 0, 0, 0, 0, 0, 1, 0, 0,
                                                   1, 0, 1, 0, 0, 0, 0, 0};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Locality weighted with all localities zero weighted.
TEST_F(MaglevLoadBalancerTest, LocalityWeightedAllZeroLocalityWeights) {
  host_set_.hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:90", 1)};
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.hosts_per_locality_ = makeHostsPerLocality({{host_set_.hosts_[0]}});
  host_set_.healthy_hosts_per_locality_ = host_set_.hosts_per_locality_;
  LocalityWeightsConstSharedPtr locality_weights{new LocalityWeights{0}};
  host_set_.locality_weights_ = locality_weights;
  host_set_.runCallbacks({}, {});
  init(17, true);
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  TestLoadBalancerContext context(0);
  EXPECT_EQ(nullptr, lb->chooseHost(&context).host);
}

// Validate that when we are in global panic and have localities, we get sane
// results (fall back to non-healthy hosts).
TEST_F(MaglevLoadBalancerTest, LocalityWeightedGlobalPanic) {
  envoy::config::core::v3::Locality zone_a;
  zone_a.set_zone("A");
  envoy::config::core::v3::Locality zone_b;
  zone_b.set_zone("B");

  host_set_.hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:90", zone_a, 1),
                      makeTestHost(info_, "tcp://127.0.0.1:91", zone_b, 2)};
  host_set_.healthy_hosts_ = {};
  host_set_.hosts_per_locality_ =
      makeHostsPerLocality({{host_set_.hosts_[0]}, {host_set_.hosts_[1]}});
  host_set_.healthy_hosts_per_locality_ = makeHostsPerLocality({{}, {}});
  LocalityWeightsConstSharedPtr locality_weights{new LocalityWeights{1, 1}};
  host_set_.locality_weights_ = locality_weights;
  host_set_.runCallbacks({}, {});
  init(17, true);
  EXPECT_EQ(8, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(9, lb_->stats().max_entries_per_host_.value());

  // maglev: i=0 host=127.0.0.1:91
  // maglev: i=1 host=127.0.0.1:90
  // maglev: i=2 host=127.0.0.1:90
  // maglev: i=3 host=127.0.0.1:91
  // maglev: i=4 host=127.0.0.1:90
  // maglev: i=5 host=127.0.0.1:91
  // maglev: i=6 host=127.0.0.1:91
  // maglev: i=7 host=127.0.0.1:90
  // maglev: i=8 host=127.0.0.1:90
  // maglev: i=9 host=127.0.0.1:91
  // maglev: i=10 host=127.0.0.1:90
  // maglev: i=11 host=127.0.0.1:91
  // maglev: i=12 host=127.0.0.1:90
  // maglev: i=13 host=127.0.0.1:90
  // maglev: i=14 host=127.0.0.1:91
  // maglev: i=15 host=127.0.0.1:90
  // maglev: i=16 host=127.0.0.1:91
  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);
  const std::vector<uint32_t> expected_assignments{1, 0, 0, 1, 0, 1, 1, 0, 0,
                                                   1, 0, 1, 0, 0, 1, 0, 1};
  for (uint32_t i = 0; i < 3 * expected_assignments.size(); ++i) {
    TestLoadBalancerContext context(i);
    EXPECT_EQ(host_set_.hosts_[expected_assignments[i % expected_assignments.size()]],
              lb->chooseHost(&context).host);
  }
}

// Given extremely lopsided locality weights, and a table that isn't large enough to fit all hosts,
// expect that the least-weighted hosts appear once, and the most-weighted host fills the remainder.
TEST_F(MaglevLoadBalancerTest, LocalityWeightedLopsided) {
  envoy::config::core::v3::Locality zone_a;
  zone_a.set_zone("A");
  envoy::config::core::v3::Locality zone_b;
  zone_b.set_zone("B");

  host_set_.hosts_.clear();
  HostVector heavy_but_sparse, light_but_dense;
  for (uint32_t i = 0; i < 1024; ++i) {
    auto host_locality = i == 0 ? zone_a : zone_b;
    auto host(makeTestHost(info_, fmt::format("tcp://127.0.0.1:{}", i), host_locality));
    host_set_.hosts_.push_back(host);
    (i == 0 ? heavy_but_sparse : light_but_dense).push_back(host);
  }
  host_set_.healthy_hosts_ = {};
  host_set_.hosts_per_locality_ = makeHostsPerLocality({heavy_but_sparse, light_but_dense});
  host_set_.healthy_hosts_per_locality_ = host_set_.hosts_per_locality_;
  host_set_.locality_weights_ = makeLocalityWeights({127, 1});
  host_set_.runCallbacks({}, {});
  init(MaglevTable::DefaultTableSize, true);
  EXPECT_EQ(1, lb_->stats().min_entries_per_host_.value());
  EXPECT_EQ(MaglevTable::DefaultTableSize - 1023, lb_->stats().max_entries_per_host_.value());

  LoadBalancerPtr lb = lb_->factory()->create(lb_params_);

  // Populate a histogram of the number of table entries for each host...
  uint32_t counts[1024] = {0};
  for (uint32_t i = 0; i < MaglevTable::DefaultTableSize; ++i) {
    TestLoadBalancerContext context(i);
    uint32_t port = lb->chooseHost(&context).host->address()->ip()->port();
    ++counts[port];
  }

  // Each of the light_but_dense hosts should appear in the table once.
  for (uint32_t i = 1; i < 1024; ++i) {
    EXPECT_EQ(1, counts[i]);
  }

  // The heavy_but_sparse host should occupy the remainder of the table.
  EXPECT_EQ(MaglevTable::DefaultTableSize - 1023, counts[0]);
}

TEST(TypedMaglevLbConfigTest, TypedMaglevLbConfigTest) {
  {
    envoy::config::cluster::v3::Cluster::MaglevLbConfig legacy;
    envoy::config::cluster::v3::Cluster::CommonLbConfig common;
    TypedMaglevLbConfig typed_config(common, legacy);

    EXPECT_FALSE(typed_config.lb_config_.has_locality_weighted_lb_config());
    EXPECT_FALSE(typed_config.lb_config_.has_consistent_hashing_lb_config());
    EXPECT_FALSE(typed_config.lb_config_.has_table_size());
  }

  {
    envoy::config::cluster::v3::Cluster::MaglevLbConfig legacy;
    envoy::config::cluster::v3::Cluster::CommonLbConfig common;

    common.mutable_locality_weighted_lb_config();
    common.mutable_consistent_hashing_lb_config()->set_use_hostname_for_hashing(true);
    common.mutable_consistent_hashing_lb_config()->mutable_hash_balance_factor()->set_value(233);

    legacy.mutable_table_size()->set_value(12);

    TypedMaglevLbConfig typed_config(common, legacy);

    EXPECT_TRUE(typed_config.lb_config_.has_locality_weighted_lb_config());
    EXPECT_TRUE(typed_config.lb_config_.has_consistent_hashing_lb_config());
    EXPECT_TRUE(typed_config.lb_config_.consistent_hashing_lb_config().use_hostname_for_hashing());
    EXPECT_EQ(233,
              typed_config.lb_config_.consistent_hashing_lb_config().hash_balance_factor().value());

    EXPECT_EQ(12, typed_config.lb_config_.table_size().value());
  }
}

} // namespace
} // namespace Upstream
} // namespace Envoy
