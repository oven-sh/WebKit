"""
Install LLVM using official script

"""

def _download_and_run_llvm_script(ctx):
    LLVM_VERSION = ctx.attr.llvm_version
    return ctx.execute(["sudo", "curl", "-s", "https://apt.llvm.org/llvm.sh", "|", "bash -s", LLVM_VERSION])

def _install_clang_on_ubuntu(ctx):
    """
    Linux only rule to install LLVM using official script
    """

    # hack to prevent ERROR: install_clang_on_ubuntu rule //external:clang_on_ubuntu must create a directory
    ctx.file(".ignores_me", "")

    LLVM_VERSION = ctx.attr.llvm_version
    CLANG_EXE = ctx.attr.clang_exe
    CLANGPP_EXE = ctx.attr.clangpp_exe
    LLVM_AR_EXE = ctx.attr.llvm_ar_exe
    LLVM_RANLIB_EXE = ctx.attr.llvm_ranlib_exe

    if ctx.os.name != "linux" or \
       (ctx.which(CLANG_EXE) == None) or \
       (ctx.which(CLANGPP_EXE) == None) or \
       (ctx.which(LLVM_AR_EXE) == None) or \
       (ctx.which(LLVM_RANLIB_EXE) == None):
        return
    what = _download_and_run_llvm_script(ctx)
    if what.return_code == 0:
        ctx.execute([
            "sudo",
            "apt-get",
            "install",
            "-y",
            CLANG_EXE,
            CLANGPP_EXE,
            "llvm-%s" % LLVM_VERSION,  # includs ar and ranlib
        ])

install_clang_on_ubuntu = repository_rule(
    implementation = _install_clang_on_ubuntu,
    attrs = {
        "llvm_version": attr.string(mandatory=True),
        "clang_exe": attr.string(mandatory=True),
        "clangpp_exe": attr.string(mandatory=True),
        "llvm_ar_exe": attr.string(mandatory=True),
        "llvm_ranlib_exe": attr.string(mandatory=True),
    },
    doc = "Install LLVM using official script",
)
