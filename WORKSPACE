workspace(name = "com_github_oven-sh_webkit")


# Rules ForeignCc  start
# https://bazelbuild.github.io/rules_foreign_cc/main/index.html#setup
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "rules_foreign_cc",
    # TODO: Get the latest sha256 value from a bazel debug message or the latest 
    #       release on the releases page: https://github.com/bazelbuild/rules_foreign_cc/releases
    #
    # sha256 = "...",
    strip_prefix = "rules_foreign_cc-7b673547a3b51febb4e67642bf0cc30c3ba09453",
    url = "https://github.com/bazelbuild/rules_foreign_cc/archive/7b673547a3b51febb4e67642bf0cc30c3ba09453.tar.gz",
)

load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")

rules_foreign_cc_dependencies()

# Rules ForeignCc  end


# Skylib start
load("@bazel_skylib//:workspace.bzl", "bazel_skylib_workspace")

bazel_skylib_workspace()
# Skylib end