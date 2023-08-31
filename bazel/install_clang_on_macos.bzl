"""
Installing clang on macOS using homebrew.
"""

LLVM_VERSION = "16"
LLVM_EXE = "clang-%s" % LLVM_VERSION
CLANG_EXE = "clang-%s" % LLVM_VERSION
CLANGPP_EXE = "clang++-%s" % LLVM_VERSION
LLVM_AR_EXE = "llvm-ar-%s" % LLVM_VERSION
LLVM_RANLIB_EXE = "llvm-ranlib-%s" % LLVM_VERSION

def _install_home_brew(ctx):
    return ctx.execute([
        "curl",
        "-s",
        "https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh",
        "|",
        "bash -s",
    ])

def _install_clang_on_macos(ctx):
    """
    MacOS only rule to install clang on macOS using homebrew.
    """
    if ctx.os.name != "macos" or \
       (ctx.which("clang-%s" % LLVM_VERSION) == None) or \
       (ctx.which("clang++-%s" % LLVM_VERSION) == None) or \
       (ctx.which("llvm-%s" % LLVM_VERSION) == None) or \
       (ctx.which("llvm-ar-%s" % LLVM_VERSION) == None) or \
       (ctx.which("llvm-ranlib-%s" % LLVM_VERSION) == None):
        return

    if ctx.which("brew") == None:
        what = _install_home_brew(ctx)
        if what.return_code == 0:
            ctx.execute(["brew", "install", "llvm@%s" % LLVM_VERSION])
    else:
        ctx.execute(["brew", "install", "llvm@%s" % LLVM_VERSION])

install_clang_on_macos = repository_rule(
    implementation = _install_clang_on_macos,
    attrs = {},
    doc = "Installing clang on macOS",
)
