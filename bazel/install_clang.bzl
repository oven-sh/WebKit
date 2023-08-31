load(":bazel/install_clang_on_macos.bzl", "install_clang_on_macos")
load(":bazel/install_clang_on_ubuntu.bzl", "install_clang_on_ubuntu")

def install_clang():
    install_clang_on_macos(name = "clang_on_macos")
    install_clang_on_ubuntu(name = "clang_on_ubuntu")
