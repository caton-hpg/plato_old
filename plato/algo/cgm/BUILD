load("//build_tools/rules:variables.bzl", "PLATO_OPTS")

cc_library (
  name = "cgm",
  hdrs = [
    "connected_component.hpp",   
    "cgm.hpp",
  ],
  srcs = [],
  includes = [ ".", ],
  deps = [
    "//3rd/glog:glog",

    "//plato/util:perf",
    "//plato/util:nebula_writer",
    "//plato/util:atomic",
    "//plato/graph:graph",
    "//plato/engine:dualmode",
  ],
  defines = [
    "__DUALMODE_DEBUG__",
  ],
  copts = [ '-O2', ] + PLATO_OPTS,
  linkopts = [ ] + PLATO_OPTS,
  visibility = ["//visibility:public"],
)

