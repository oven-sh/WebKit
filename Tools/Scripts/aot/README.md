# AOT compilation tools

| | |
|---|---|
| Check that the stubs return the way the processor predicts (a second, ARM64) | `check-paired-returns.py --image <image or executable> --map <map>` reads the machine code of the stubs and fails on one that computes a return address or returns with `BR`. |
| Catch missing includes hidden by the precompiled header (about a minute per pass) | `check-includes.py <build directory> <base commit>`, with and without `--aot`, `--headers` and `--gate-off`. |
| Catch a `Structure*` that the C++ compiler made from its `StructureID` behind a call that may collect, so that no register or stack word held it during the call (90 seconds, any finished binary with symbols, x86-64 or ARM64) | `find-structures-decoded-behind-calls.py <binary> --expected structures-decoded-behind-calls-expected.txt`, with `--symbols <file>` for a stripped binary and `--show` for the instructions. The cure of a new row is `WTF::opaque()` around the decode, or a deferral of collections. |
| Catch a function that lets the address of its own frame record out and then leaves by a jump to its last callee, so that the frame is gone while walks of the stack begin at it (a minute, any finished ARM64 binary with symbols) | `find-frame-records-handed-to-jumps.py <binary> --expected frame-records-handed-to-jumps-expected.txt` lists them. |
| Check the syntax of the sources you changed without building them, also with the defines of another lane (three seconds a source, all at once) | `check-syntax.py <build directory> --changed-since <commit>`, or sources by name, or `--all`; `--define NAME=VALUE` adds a define. |

The map is written where the image is made: `testaot compile .. --aotMapFilePath=<map>`, or `BUN_JSC_aotMapFilePath=<map> bun compile ..`.
