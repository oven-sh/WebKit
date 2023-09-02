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


# install clang for macOS or Ubuntu
load("//bazel:install_clang.bzl", "install_clang")
install_clang()

# register clang toolchains
register_toolchains(
    "//bazel/toolchains:clang_linux_toolchain",
    "//bazel/toolchains:clang_darwin_toolchain",
)

# llvm start
# BAZEL_TOOLCHAIN_TAG = "0.8.2"
# BAZEL_TOOLCHAIN_SHA = "0fc3a2b0c9c929920f4bed8f2b446a8274cad41f5ee823fd3faa0d7641f20db0"

# http_archive(
#     name = "com_grail_bazel_toolchain",
#     sha256 = BAZEL_TOOLCHAIN_SHA,
#     strip_prefix = "bazel-toolchain-{tag}".format(tag = BAZEL_TOOLCHAIN_TAG),
#     canonical_id = BAZEL_TOOLCHAIN_TAG,
#     url = "https://github.com/grailbio/bazel-toolchain/archive/refs/tags/{tag}.tar.gz".format(tag = BAZEL_TOOLCHAIN_TAG),
# )

# load("@com_grail_bazel_toolchain//toolchain:deps.bzl", "bazel_toolchain_dependencies")
# bazel_toolchain_dependencies()

# load("@com_grail_bazel_toolchain//toolchain:rules.bzl", "llvm_toolchain")
# llvm_toolchain(
#     name = "llvm_toolchain",
#     llvm_version = "16.0.0",
# )

# load("@llvm_toolchain//:toolchains.bzl", "llvm_register_toolchains")
# llvm_register_toolchains()

### llvm end
