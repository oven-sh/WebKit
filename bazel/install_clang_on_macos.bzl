"""
Installing clang on macOS using homebrew.
"""

def _install_home_brew(ctx):
    return ctx.execute([
        "curl",
        "-s",
        "https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh",
        "|",
        "bash -s",
    ])

def _install_clang_on_macos_impl(ctx):
    """
    MacOS only rule to install clang on macOS using homebrew.
    """
    LLVM_VERSION = ctx.attr.llvm_version
    CLANG_EXE = ctx.attr.clang_exe
    CLANGPP_EXE = ctx.attr.clangpp_exe
    LLVM_AR_EXE = ctx.attr.llvm_ar_exe
    LLVM_RANLIB_EXE = ctx.attr.llvm_ranlib_exe
    LLVM_LINKER_EXE = ctx.attr.llvm_linker_exe

    # hack to prevent ERROR: install_clang_on_ubuntu rule //external:clang_on_macos must create a directory
    ctx.file(".ignores_me", "")

    if ctx.os.name != "macos" or \
       (ctx.which(CLANG_EXE) == None) or \
       (ctx.which(CLANGPP_EXE) == None) or \
       (ctx.which(LLVM_AR_EXE) == None) or \
       (ctx.which(LLVM_RANLIB_EXE) == None) or \
       (ctx.which(LLVM_LINKER_EXE) == None):
        return

    if ctx.which("brew") == None:
        what = _install_home_brew(ctx)
        if what.return_code == 0:
            ctx.execute(["brew", "install", "llvm@%s" % LLVM_VERSION])
    else:
        ctx.execute(["brew", "install", "llvm@%s" % LLVM_VERSION])

install_clang_on_macos = repository_rule(
    implementation = _install_clang_on_macos_impl,
    attrs = {
        "llvm_version": attr.string(mandatory = True),
        "clang_exe": attr.string(mandatory = True),
        "clangpp_exe": attr.string(mandatory = True),
        "llvm_ar_exe": attr.string(mandatory = True),
        "llvm_ranlib_exe": attr.string(mandatory = True),
        "llvm_linker_exe": attr.string(mandatory = True),
    },
    doc = "Installing clang on macOS",
)
