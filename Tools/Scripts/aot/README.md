# AOT compilation tools

| | |
|---|---|
| Check that no stub changes a register it is declared to preserve (seconds, ARM64) | `check-stub-clobbers.py --image <image or executable> --map <map>` follows every path through the machine code of the stubs. What is declared is what the register allocator is told. `--print [--stub <name>]` says what each stub changes. |
| Check that no stub changes a register it is declared to preserve (a second, x86-64) | `check-stub-clobbers-x86.py --image <image> --map <map>` is the same for an x86-64 image, with the same rules and the same `--print`; it decodes with `llvm-objdump`. `--declare '<stub>: %rax %r10 ..'` tries a declaration before it is built. |
| Check that the stubs return the way the processor predicts (a second, ARM64) | `check-paired-returns.py --image <image or executable> --map <map>` reads the machine code of the stubs and fails on one that computes a return address or returns with `BR`. |
| Catch missing includes hidden by the precompiled header (about a minute per pass) | `check-includes.py <build directory> <base commit>`, with and without `--headers` and `--gate-off`. |

The map is written where the image is made: `--aotMapFilePath=<map>`.
