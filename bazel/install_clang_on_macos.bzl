"""
This file contains rules that are specific to the repository.
"""

def _install_home_brew(ctx):
    ctx.run('/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"')


def _install_clang_on_macos(ctx):
    """
    MacOS only rule to install clang on macOS using homebrew.
    """
    if ctx.os.name != 'macos':
        return

    _install_home_brew(ctx)
    ctx.run('brew install llvm@16')


install_clang_on_macos = repository_rule(
    implementation = _install_clang_on_macos,
    attrs = {},
    doc = 'Installing clang on macOS',
)