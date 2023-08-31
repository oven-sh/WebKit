"""
Install LLVM using official script

"""

LLVM_VERSION = "16"

def _download_and_run_llvm_script(ctx):
    ctx.run("bash <(curl -s https://apt.llvm.org/llvm.sh) %s" % LLVM_VERSION)

def _install_clang_on_ubuntu(ctx):
    """
    Linux only rule to install LLVM using official script
    """
    print("Installing LLVM using official script %s" % LLVM_VERSION)
    print("os.name: %s" % ctx.os.name)

    if ctx.os.name != 'linux':
        return
    _download_and_run_llvm_script(ctx)
    ctx.run("sudo apt-get install -y clang-%s clang++-%s llvm-%s llvm-ar-%s llvm-ranlib-%s" % (LLVM_VERSION, LLVM_VERSION, LLVM_VERSION, LLVM_VERSION, LLVM_VERSION))

install_clang_on_ubuntu = repository_rule(
    implementation=_install_clang_on_ubuntu,
    attrs={},
    doc="Install LLVM using official script",
)