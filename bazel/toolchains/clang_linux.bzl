# NEW
load("@bazel_tools//tools/build_defs/cc:action_names.bzl", "ACTION_NAMES")

# NEW
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "feature",
    "flag_group",
    "flag_set",
    "tool_path",
)
load("//bazel:install_clang.bzl", "CLANGPP_EXE", "LLVM_LINKER_EXE", "LLVM_AR_EXE")

all_link_actions = [
    # NEW
    ACTION_NAMES.cpp_link_executable,
    ACTION_NAMES.cpp_link_dynamic_library,
    ACTION_NAMES.cpp_link_nodeps_dynamic_library,
]

def _cc_toolchain_config_impl(ctx):
    tool_paths = [
        tool_path(
            name = "gcc",
            # path = "/usr/bin/clang",
            path = "/usr/bin/%s" % CLANGPP_EXE,
        ),
        tool_path(
            name = "ld",
            path = "/usr/bin/%s" % LLVM_LINKER_EXE,
        ),
        tool_path(
            name = "ar",
            path = "/usr/bin/%s" % LLVM_AR_EXE,
        ),
        tool_path(
            name = "cpp",
            path = "/bin/bin/%s" % CLANGPP_EXE,
        ),
        tool_path(
            name = "gcov",
            path = "/bin/false",
        ),
        tool_path(
            name = "nm",
            path = "/bin/false",
        ),
        tool_path(
            name = "objdump",
            path = "/bin/false",
        ),
        tool_path(
            name = "strip",
            path = "/bin/false",
        ),
    ]

    features = [
        # NEW
        feature(
            name = "default_linker_flags",
            enabled = True,
            flag_sets = [
                flag_set(
                    actions = all_link_actions,
                    flag_groups = ([
                        flag_group(
                            flags = [
                                "-lstdc++",
                            ],
                        ),
                    ]),
                ),
            ],
        ),
    ]

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        features = features,  # NEW
        cxx_builtin_include_directories = [
            # "/usr/lib/llvm-9/lib/clang/9.0.1/include",
            # "/usr/include",
            # "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1/",
            # "/opt/homebrew/opt/llvm/include/c++/v1",
            # "/opt/homebrew/opt/llvm/include",
            # "/opt/homebrew/Cellar/llvm/16.0.6/lib/clang/16/include",
        ],
        toolchain_identifier = "local",
        host_system_name = "local",
        target_system_name = "local",
        target_cpu = "x86_64",
        target_libc = "unknown",
        compiler = "clang",
        abi_version = "unknown",
        abi_libc_version = "unknown",
        tool_paths = tool_paths,
    )

cc_clang_linux_toolchain_config = rule(
    implementation = _cc_toolchain_config_impl,
    attrs = {},
    provides = [CcToolchainConfigInfo],
)
