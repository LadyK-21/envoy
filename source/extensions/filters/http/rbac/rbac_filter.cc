#include "rbac_filter.h"

#include "envoy/stats/scope.h"

#include "source/common/http/matching/inputs.h"
#include "source/common/http/utility.h"
#include "source/common/network/matching/inputs.h"
#include "source/common/ssl/matching/inputs.h"

#include "absl/strings/str_join.h"
#include "xds/type/matcher/v3/http_inputs.pb.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace RBACFilter {

// Define the static allowed_inputs_set
const absl::flat_hash_set<std::string> ActionValidationVisitor::allowed_inputs_set_{
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::DestinationIPInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::DestinationPortInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::SourceIPInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::SourcePortInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::DirectSourceIPInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::ServerNameInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::type::matcher::v3::HttpRequestHeaderMatchInput::descriptor()->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::ssl::v3::UriSanInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::ssl::v3::DnsSanInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::ssl::v3::SubjectInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::DynamicMetadataInput::descriptor()
            ->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        xds::type::matcher::v3::HttpAttributesCelMatchInput::descriptor()->full_name())},
    {TypeUtil::descriptorFullNameToTypeUrl(
        envoy::extensions::matching::common_inputs::network::v3::FilterStateInput::descriptor()
            ->full_name())}};

absl::Status ActionValidationVisitor::performDataInputValidation(
    const Matcher::DataInputFactory<Http::HttpMatchingData>&, absl::string_view type_url) {
  if (allowed_inputs_set_.contains(type_url)) {
    return absl::OkStatus();
  }

  return absl::InvalidArgumentError(fmt::format("RBAC HTTP filter cannot match on '{}'", type_url));
}

RoleBasedAccessControlFilterConfig::RoleBasedAccessControlFilterConfig(
    const envoy::extensions::filters::http::rbac::v3::RBAC& proto_config,
    const std::string& stats_prefix, Stats::Scope& scope,
    Server::Configuration::ServerFactoryContext& context,
    ProtobufMessage::ValidationVisitor& validation_visitor)
    : stats_(Filters::Common::RBAC::generateStats(stats_prefix, proto_config.rules_stat_prefix(),
                                                  proto_config.shadow_rules_stat_prefix(), scope)),
      rules_stat_prefix_(proto_config.rules_stat_prefix()),
      shadow_rules_stat_prefix_(proto_config.shadow_rules_stat_prefix()),
      per_rule_stats_(proto_config.track_per_rule_stats()),
      engine_(Filters::Common::RBAC::createEngine(proto_config, context, validation_visitor,
                                                  action_validation_visitor_)),
      shadow_engine_(Filters::Common::RBAC::createShadowEngine(
          proto_config, context, validation_visitor, action_validation_visitor_)) {}

#define DEFINE_DYNAMIC_METADATA_STAT_KEY_GETTER(GETTER_NAME, PREFIX, ROUTE_LOCAL_PREFIX_OVERRIDE,  \
                                                DYNAMIC_METADATA_KEY)                              \
  std::string RoleBasedAccessControlFilterConfig::GETTER_NAME(                                     \
      const Http::StreamFilterCallbacks* callbacks) const {                                        \
    const auto* route_local = Http::Utility::resolveMostSpecificPerFilterConfig<                   \
        RoleBasedAccessControlRouteSpecificFilterConfig>(callbacks);                               \
    absl::string_view prefix = PREFIX;                                                             \
    if (route_local && !route_local->ROUTE_LOCAL_PREFIX_OVERRIDE().empty()) {                      \
      prefix = route_local->ROUTE_LOCAL_PREFIX_OVERRIDE();                                         \
    }                                                                                              \
    return absl::StrCat(                                                                           \
        prefix, Filters::Common::RBAC::DynamicMetadataKeysSingleton::get().DYNAMIC_METADATA_KEY);  \
  }

DEFINE_DYNAMIC_METADATA_STAT_KEY_GETTER(shadowEffectivePolicyIdField, shadow_rules_stat_prefix_,
                                        shadowRulesStatPrefix, ShadowEffectivePolicyIdField)
DEFINE_DYNAMIC_METADATA_STAT_KEY_GETTER(shadowEngineResultField, shadow_rules_stat_prefix_,
                                        shadowRulesStatPrefix, ShadowEngineResultField)
DEFINE_DYNAMIC_METADATA_STAT_KEY_GETTER(enforcedEffectivePolicyIdField, rules_stat_prefix_,
                                        rulesStatPrefix, EnforcedEffectivePolicyIdField)
DEFINE_DYNAMIC_METADATA_STAT_KEY_GETTER(enforcedEngineResultField, rules_stat_prefix_,
                                        rulesStatPrefix, EnforcedEngineResultField)

const Filters::Common::RBAC::RoleBasedAccessControlEngine*
RoleBasedAccessControlFilterConfig::engine(
    const Http::StreamFilterCallbacks* callbacks,
    const Filters::Common::RBAC::EnforcementMode mode) const {
  const auto* route_local = Http::Utility::resolveMostSpecificPerFilterConfig<
      RoleBasedAccessControlRouteSpecificFilterConfig>(callbacks);

  if (route_local) {
    return route_local->engine(mode);
  }

  return engine(mode);
}

bool RoleBasedAccessControlFilterConfig::perRuleStatsEnabled(
    const Http::StreamFilterCallbacks* callbacks) const {
  const auto* route_local = Http::Utility::resolveMostSpecificPerFilterConfig<
      RoleBasedAccessControlRouteSpecificFilterConfig>(callbacks);

  if (route_local) {
    return route_local->perRuleStatsEnabled();
  }

  return per_rule_stats_;
}

RoleBasedAccessControlRouteSpecificFilterConfig::RoleBasedAccessControlRouteSpecificFilterConfig(
    const envoy::extensions::filters::http::rbac::v3::RBACPerRoute& per_route_config,
    Server::Configuration::ServerFactoryContext& context,
    ProtobufMessage::ValidationVisitor& validation_visitor)
    : rules_stat_prefix_(per_route_config.rbac().rules_stat_prefix()),
      shadow_rules_stat_prefix_(per_route_config.rbac().shadow_rules_stat_prefix()),
      per_rule_stats_(per_route_config.rbac().track_per_rule_stats()) {
  // Moved from member initializer to ctor body to overcome clang false warning about memory
  // leak (clang-analyzer-cplusplus.NewDeleteLeaks,-warnings-as-errors).
  // Potentially https://lists.llvm.org/pipermail/llvm-bugs/2018-July/066769.html
  engine_ = Filters::Common::RBAC::createEngine(per_route_config.rbac(), context,
                                                validation_visitor, action_validation_visitor_);
  shadow_engine_ = Filters::Common::RBAC::createShadowEngine(
      per_route_config.rbac(), context, validation_visitor, action_validation_visitor_);
}

// Evaluates the shadow engine policy and updates metrics accordingly
bool RoleBasedAccessControlFilter::evaluateShadowEngine(const Http::RequestHeaderMap& headers,
                                                        ProtobufWkt::Struct& metrics) const {
  const auto shadow_engine =
      config_->engine(callbacks_, Filters::Common::RBAC::EnforcementMode::Shadow);
  if (shadow_engine == nullptr) {
    return false;
  }

  auto& fields = *metrics.mutable_fields();
  std::string effective_policy_id;
  absl::string_view shadow_response_code =
      Filters::Common::RBAC::DynamicMetadataKeysSingleton::get().EngineResultAllowed;

  const bool per_rule_stats_enabled = config_->perRuleStatsEnabled(callbacks_);

  if (shadow_engine->handleAction(*callbacks_->connection(), headers, callbacks_->streamInfo(),
                                  &effective_policy_id)) {
    ENVOY_LOG(debug, "shadow allowed, matched policy {}",
              effective_policy_id.empty() ? "none" : effective_policy_id);
    config_->stats().shadow_allowed_.inc();
    if (!effective_policy_id.empty() && per_rule_stats_enabled) {
      config_->stats().incPolicyShadowAllowed(effective_policy_id);
    }
  } else {
    ENVOY_LOG(debug, "shadow denied, matched policy {}",
              effective_policy_id.empty() ? "none" : effective_policy_id);
    config_->stats().shadow_denied_.inc();
    if (!effective_policy_id.empty() && per_rule_stats_enabled) {
      config_->stats().incPolicyShadowDenied(effective_policy_id);
    }
    shadow_response_code =
        Filters::Common::RBAC::DynamicMetadataKeysSingleton::get().EngineResultDenied;
  }

  if (!effective_policy_id.empty()) {
    *fields[config_->shadowEffectivePolicyIdField(callbacks_)].mutable_string_value() =
        effective_policy_id;
  }

  *fields[config_->shadowEngineResultField(callbacks_)].mutable_string_value() =
      shadow_response_code;

  return true;
}

// Evaluates the enforced engine policy and returns the appropriate filter status
Http::FilterHeadersStatus
RoleBasedAccessControlFilter::evaluateEnforcedEngine(Http::RequestHeaderMap& headers,
                                                     ProtobufWkt::Struct& metrics) const {
  const auto engine = config_->engine(callbacks_, Filters::Common::RBAC::EnforcementMode::Enforced);
  if (engine == nullptr) {
    return Http::FilterHeadersStatus::Continue;
  }

  auto& fields = *metrics.mutable_fields();
  std::string effective_policy_id;
  const bool per_rule_stats_enabled = config_->perRuleStatsEnabled(callbacks_);

  const bool allowed = engine->handleAction(*callbacks_->connection(), headers,
                                            callbacks_->streamInfo(), &effective_policy_id);

  absl::string_view log_policy_id = effective_policy_id;
  if (!effective_policy_id.empty()) {
    *fields[config_->enforcedEffectivePolicyIdField(callbacks_)].mutable_string_value() =
        effective_policy_id;
  } else {
    log_policy_id = "none";
  }

  if (allowed) {
    ENVOY_LOG(debug, "enforced allowed, matched policy {}", log_policy_id);
    config_->stats().allowed_.inc();
    if (!effective_policy_id.empty() && per_rule_stats_enabled) {
      config_->stats().incPolicyAllowed(effective_policy_id);
    }

    *fields[config_->enforcedEngineResultField(callbacks_)].mutable_string_value() =
        Filters::Common::RBAC::DynamicMetadataKeysSingleton::get().EngineResultAllowed;

    return Http::FilterHeadersStatus::Continue;
  }

  ENVOY_LOG(debug, "enforced denied, matched policy {}", log_policy_id);
  callbacks_->sendLocalReply(Http::Code::Forbidden, "RBAC: access denied", nullptr, absl::nullopt,
                             Filters::Common::RBAC::responseDetail(log_policy_id));
  config_->stats().denied_.inc();
  if (!effective_policy_id.empty() && per_rule_stats_enabled) {
    config_->stats().incPolicyDenied(effective_policy_id);
  }

  *fields[config_->enforcedEngineResultField(callbacks_)].mutable_string_value() =
      Filters::Common::RBAC::DynamicMetadataKeysSingleton::get().EngineResultDenied;

  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterHeadersStatus
RoleBasedAccessControlFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool) {
  ENVOY_LOG(
      debug,
      "checking request: requestedServerName: {}, sourceIP: {}, directRemoteIP: {}, remoteIP: {},"
      "localAddress: {}, ssl: {}, headers: {}, dynamicMetadata: {}",
      callbacks_->connection()->requestedServerName(),
      callbacks_->connection()->connectionInfoProvider().remoteAddress()->asString(),
      callbacks_->streamInfo().downstreamAddressProvider().directRemoteAddress()->asString(),
      callbacks_->streamInfo().downstreamAddressProvider().remoteAddress()->asString(),
      callbacks_->streamInfo().downstreamAddressProvider().localAddress()->asString(),
      callbacks_->connection()->ssl()
          ? "uriSanPeerCertificate: " +
                absl::StrJoin(callbacks_->connection()->ssl()->uriSanPeerCertificate(), ",") +
                ", dnsSanPeerCertificate: " +
                absl::StrJoin(callbacks_->connection()->ssl()->dnsSansPeerCertificate(), ",") +
                ", subjectPeerCertificate: " +
                callbacks_->connection()->ssl()->subjectPeerCertificate()
          : "none",
      headers, callbacks_->streamInfo().dynamicMetadata().DebugString());

  // Create metrics structure to hold results
  ProtobufWkt::Struct metrics;

  // Evaluate shadow engine if it exists
  const bool shadow_engine_evaluated = evaluateShadowEngine(headers, metrics);

  // Evaluate enforced engine if it exists
  const Http::FilterHeadersStatus status = evaluateEnforcedEngine(headers, metrics);

  // Set dynamic metadata if either engine was evaluated
  const bool enforced_engine_evaluated =
      status == Http::FilterHeadersStatus::StopIteration ||
      config_->engine(callbacks_, Filters::Common::RBAC::EnforcementMode::Enforced) != nullptr;

  if (shadow_engine_evaluated || enforced_engine_evaluated) {
    callbacks_->streamInfo().setDynamicMetadata("envoy.filters.http.rbac", metrics);
  }

  if (status == Http::FilterHeadersStatus::StopIteration) {
    return status;
  }

  ENVOY_LOG(debug, "allowed by default or by policy");
  return Http::FilterHeadersStatus::Continue;
}

} // namespace RBACFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
