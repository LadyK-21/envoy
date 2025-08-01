#pragma once

#include <memory>

#include "envoy/extensions/filters/http/rbac/v3/rbac.pb.h"
#include "envoy/http/filter.h"
#include "envoy/stats/scope.h"

#include "source/common/common/logger.h"
#include "source/extensions/filters/common/rbac/engine_impl.h"
#include "source/extensions/filters/common/rbac/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RBACFilter {

class ActionValidationVisitor final : public Filters::Common::RBAC::ActionValidationVisitor {
public:
  absl::Status
  performDataInputValidation(const Matcher::DataInputFactory<Http::HttpMatchingData>& data_input,
                             absl::string_view type_url) override;

private:
  static const absl::flat_hash_set<std::string> allowed_inputs_set_;
};

class RoleBasedAccessControlRouteSpecificFilterConfig : public Router::RouteSpecificFilterConfig {
public:
  RoleBasedAccessControlRouteSpecificFilterConfig(
      const envoy::extensions::filters::http::rbac::v3::RBACPerRoute& per_route_config,
      Server::Configuration::ServerFactoryContext& context,
      ProtobufMessage::ValidationVisitor& validation_visitor);

  const Filters::Common::RBAC::RoleBasedAccessControlEngine*
  engine(Filters::Common::RBAC::EnforcementMode mode) const {
    return mode == Filters::Common::RBAC::EnforcementMode::Enforced ? engine_.get()
                                                                    : shadow_engine_.get();
  }
  absl::string_view rulesStatPrefix() const { return rules_stat_prefix_; }

  absl::string_view shadowRulesStatPrefix() const { return shadow_rules_stat_prefix_; }

  bool perRuleStatsEnabled() const { return per_rule_stats_; }

private:
  const std::string rules_stat_prefix_;
  const std::string shadow_rules_stat_prefix_;
  ActionValidationVisitor action_validation_visitor_;
  std::unique_ptr<Filters::Common::RBAC::RoleBasedAccessControlEngine> engine_;
  std::unique_ptr<Filters::Common::RBAC::RoleBasedAccessControlEngine> shadow_engine_;
  const bool per_rule_stats_;
};

/**
 * Configuration for the RBAC filter.
 */
class RoleBasedAccessControlFilterConfig {
public:
  RoleBasedAccessControlFilterConfig(
      const envoy::extensions::filters::http::rbac::v3::RBAC& proto_config,
      const std::string& stats_prefix, Stats::Scope& scope,
      Server::Configuration::ServerFactoryContext& context,
      ProtobufMessage::ValidationVisitor& validation_visitor);

  Filters::Common::RBAC::RoleBasedAccessControlFilterStats& stats() { return stats_; }
  std::string shadowEffectivePolicyIdField(const Http::StreamFilterCallbacks* callbacks) const;
  std::string shadowEngineResultField(const Http::StreamFilterCallbacks* callbacks) const;

  std::string enforcedEffectivePolicyIdField(const Http::StreamFilterCallbacks* callbacks) const;
  std::string enforcedEngineResultField(const Http::StreamFilterCallbacks* callbacks) const;

  const Filters::Common::RBAC::RoleBasedAccessControlEngine*
  engine(const Http::StreamFilterCallbacks* callbacks,
         Filters::Common::RBAC::EnforcementMode mode) const;

  bool perRuleStatsEnabled(const Http::StreamFilterCallbacks* callbacks) const;

private:
  const Filters::Common::RBAC::RoleBasedAccessControlEngine*
  engine(Filters::Common::RBAC::EnforcementMode mode) const {
    return mode == Filters::Common::RBAC::EnforcementMode::Enforced ? engine_.get()
                                                                    : shadow_engine_.get();
  }

  Filters::Common::RBAC::RoleBasedAccessControlFilterStats stats_;
  const std::string rules_stat_prefix_;
  const std::string shadow_rules_stat_prefix_;
  const bool per_rule_stats_;

  ActionValidationVisitor action_validation_visitor_;
  std::unique_ptr<const Filters::Common::RBAC::RoleBasedAccessControlEngine> engine_;
  std::unique_ptr<const Filters::Common::RBAC::RoleBasedAccessControlEngine> shadow_engine_;
};

using RoleBasedAccessControlFilterConfigSharedPtr =
    std::shared_ptr<RoleBasedAccessControlFilterConfig>;

/**
 * A filter that provides role-based access control authorization for HTTP requests.
 */
class RoleBasedAccessControlFilter final : public Http::StreamDecoderFilter,
                                           public Logger::Loggable<Logger::Id::rbac> {
public:
  RoleBasedAccessControlFilter(RoleBasedAccessControlFilterConfigSharedPtr config)
      : config_(std::move(config)) {}

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;

  Http::FilterDataStatus decodeData(Buffer::Instance&, bool) override {
    return Http::FilterDataStatus::Continue;
  }

  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap&) override {
    return Http::FilterTrailersStatus::Continue;
  }

  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override {
    callbacks_ = &callbacks;
  }

  // Http::StreamFilterBase
  void onDestroy() override {}

private:
  // Handles shadow engine evaluation and updates metrics
  bool evaluateShadowEngine(const Http::RequestHeaderMap& headers,
                            ProtobufWkt::Struct& metrics) const;

  // Handles enforced engine evaluation and updates metrics
  Http::FilterHeadersStatus evaluateEnforcedEngine(Http::RequestHeaderMap& headers,
                                                   ProtobufWkt::Struct& metrics) const;

  RoleBasedAccessControlFilterConfigSharedPtr config_;
  Http::StreamDecoderFilterCallbacks* callbacks_{};
};

} // namespace RBACFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
