load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_extension_package",
)

licenses(["notice"])  # Apache 2

envoy_extension_package()

envoy_cc_library(
    name = "utility_lib",
    srcs = ["utility.cc"],
    hdrs = ["utility.h"],
    tags = ["skip_on_windows"],
    deps = [
        ":engine_lib",
        "//envoy/stats:stats_macros",
        "//source/common/singleton:const_singleton",
    ],
)

envoy_cc_library(
    name = "matchers_interface",
    hdrs = [
        "matcher_extension.h",
        "matcher_interface.h",
        "principal_extension.h",
    ],
    deps = [
        "//envoy/http:header_map_interface",
        "//envoy/network:connection_interface",
        "//envoy/server:factory_context_interface",
        "//source/common/config:utility_lib",
        "@envoy_api//envoy/config/rbac/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "matchers_lib",
    srcs = ["matchers.cc"],
    hdrs = [
        "matchers.h",
    ],
    tags = ["skip_on_windows"],
    deps = [
        ":matchers_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:matchers_lib",
        "//source/common/http:header_utility_lib",
        "//source/common/network:cidr_range_lib",
        "//source/common/network:lc_trie_lib",
        "//source/extensions/filters/common/expr:evaluator_lib",
        "//source/extensions/path/match/uri_template:uri_template_match_lib",
        "@com_google_absl//absl/types:optional",
        "@envoy_api//envoy/config/core/v3:pkg_cc_proto",
        "@envoy_api//envoy/config/rbac/v3:pkg_cc_proto",
        "@envoy_api//envoy/config/route/v3:pkg_cc_proto",
        "@envoy_api//envoy/type/matcher/v3:pkg_cc_proto",
    ],
)

envoy_cc_library(
    name = "engine_interface",
    hdrs = ["engine.h"],
    tags = ["skip_on_windows"],
    deps = [
        "//envoy/http:filter_interface",
        "//envoy/http:header_map_interface",
        "//envoy/network:connection_interface",
    ],
)

envoy_cc_library(
    name = "engine_lib",
    srcs = ["engine_impl.cc"],
    hdrs = ["engine_impl.h"],
    tags = ["skip_on_windows"],
    deps = [
        "//source/common/http/matching:data_impl_lib",
        "//source/common/http/matching:inputs_lib",
        "//source/common/matcher:matcher_lib",
        "//source/common/network/matching:inputs_lib",
        "//source/common/ssl/matching:inputs_lib",
        "//source/extensions/filters/common/rbac:engine_interface",
        "//source/extensions/filters/common/rbac:matchers_lib",
        "@envoy_api//envoy/config/rbac/v3:pkg_cc_proto",
    ],
)
