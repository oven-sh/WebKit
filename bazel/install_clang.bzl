load("//bazel:install_clang_on_macos.bzl", "install_clang_on_macos")
load("//bazel:install_clang_on_ubuntu.bzl", "install_clang_on_ubuntu")

LLVM_VERSION = "16"
CLANG_EXE = "clang-%s" % LLVM_VERSION
CLANGPP_EXE = "clang++-%s" % LLVM_VERSION
LLVM_AR_EXE = "llvm-ar-%s" % LLVM_VERSION
LLVM_RANLIB_EXE = "llvm-ranlib-%s" % LLVM_VERSION
LLVM_LINKER_EXE = "lld-link-%s" % LLVM_VERSION
LLVM_LINKER_MACOS_EXE = "ld64.lld-%s" % LLVM_VERSION

def install_clang():
    install_clang_on_macos(
        name = "clang_on_macos",
        llvm_version = LLVM_VERSION,
        clang_exe = CLANG_EXE,
        clangpp_exe = CLANGPP_EXE,
        llvm_ar_exe = LLVM_AR_EXE,
        llvm_ranlib_exe = LLVM_RANLIB_EXE,
        llvm_linker_exe = LLVM_LINKER_MACOS_EXE,
    )
    install_clang_on_ubuntu(
        name = "clang_on_ubuntu",
        llvm_version = LLVM_VERSION,
        clang_exe = CLANG_EXE,
        clangpp_exe = CLANGPP_EXE,
        llvm_ar_exe = LLVM_AR_EXE,
        llvm_ranlib_exe = LLVM_RANLIB_EXE,
        llvm_linker_exe = LLVM_LINKER_EXE,
    )
