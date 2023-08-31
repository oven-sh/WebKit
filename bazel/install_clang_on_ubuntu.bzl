"""
Install LLVM using official script

"""

LLVM_VERSION = "16"
LLVM_EXE = "clang-%s" % LLVM_VERSION
CLANG_EXE = "clang-%s" % LLVM_VERSION
CLANGPP_EXE = "clang++-%s" % LLVM_VERSION
LLVM_AR_EXE = "llvm-ar-%s" % LLVM_VERSION
LLVM_RANLIB_EXE = "llvm-ranlib-%s" % LLVM_VERSION

def _download_and_run_llvm_script(ctx):
    return ctx.execute(["sudo", "curl", "-s", "https://apt.llvm.org/llvm.sh", "|", "bash -s", LLVM_VERSION])

def _install_clang_on_ubuntu(ctx):
    """
    Linux only rule to install LLVM using official script
    """

    # hack to prevent ERROR: install_clang_on_ubuntu rule //external:clang_on_ubuntu must create a directory
    ctx.file(".ignores_me", "")

    if ctx.os.name != "linux" or \
       (ctx.which("clang-%s" % LLVM_VERSION) == None) or \
       (ctx.which("clang++-%s" % LLVM_VERSION) == None) or \
       (ctx.which("llvm-%s" % LLVM_VERSION) == None) or \
       (ctx.which("llvm-ar-%s" % LLVM_VERSION) == None) or \
       (ctx.which("llvm-ranlib-%s" % LLVM_VERSION) == None):
        return
    what = _download_and_run_llvm_script(ctx)
    if what.return_code == 0:
        ctx.execute([
            "sudo",
            "apt-get",
            "install",
            "-y",
            "clang-%s" % LLVM_VERSION,
            "clang++-%s" % LLVM_VERSION,
            "llvm-%s" % LLVM_VERSION,  # includs ar and ranlib
        ])

install_clang_on_ubuntu = repository_rule(
    implementation = _install_clang_on_ubuntu,
    attrs = {},
    doc = "Install LLVM using official script",
)
