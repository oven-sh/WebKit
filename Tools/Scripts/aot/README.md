# AOT compilation tools

| | |
|---|---|
| Check that the stubs return the way the processor predicts (a second, ARM64) | `check-paired-returns.py --image <image or executable> --map <map>` reads the machine code of the stubs and fails on one that computes a return address or returns with `BR`. |
| Catch missing includes hidden by the precompiled header (about a minute per pass) | `check-includes.py <build directory> <base commit>`, with and without `--aot`, `--headers` and `--gate-off`. |
| Catch a `Structure*` that the C++ compiler made from its `StructureID` behind a call that may collect, so that no register or stack word held it during the call (90 seconds, any finished binary with symbols, x86-64 or ARM64) | `find-structures-decoded-behind-calls.py <binary> --expected structures-decoded-behind-calls-expected.txt`, with `--symbols <file>` for a stripped binary and `--show` for the instructions. The cure of a new row is `WTF::opaque()` around the decode, or a deferral of collections. |
| Catch names that two sources declare for themselves, which do not compile once a unified bundle holds both (five seconds, no build) | `check-unified-sources.py`, with `--generated <build>/JavaScriptCore/DerivedSources/aot` for the generated headers too. |

The map is written where the image is made: `testaot compile .. --aotMapFilePath=<map>`, or `BUN_JSC_aotMapFilePath=<map> bun compile ..`.
