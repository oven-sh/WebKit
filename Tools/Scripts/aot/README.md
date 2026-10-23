# AOT compilation tools

| | |
|---|---|
| Check that the stubs return the way the processor predicts (a second, ARM64) | `check-paired-returns.py --image <image or executable> --map <map>` reads the machine code of the stubs and fails on one that computes a return address or returns with `BR`. |
| Catch missing includes hidden by the precompiled header (about a minute per pass) | `check-includes.py <build directory> <base commit>`, with and without `--aot`, `--headers` and `--gate-off`. |
| Catch name collisions between sources that CMake compiles in one unified bundle (two to five minutes) | `check-unified-sources.py <build directory>`; `--write` updates the exception lists in `aot/AOT.cmake`. |

The map is written where the image is made: `testaot compile .. --aotMapFilePath=<map>`, or `BUN_JSC_aotMapFilePath=<map> bun compile ..`.
