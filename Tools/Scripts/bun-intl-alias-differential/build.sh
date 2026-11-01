#!/bin/bash
# How the differential and the cost driver were built and run (Linux x64, bun's build of this checkout).
#
# Build the engine through bun, with this checkout as WebKit:
#   BUN_WEBKIT_PATH=<this checkout> BUN_ICU_ROOT=<include/ and lib/ of a prebuilt WebKit> \
#     bun scripts/build.ts --profile=debug-local --build-dir=build/debug-local --target=jsc
#
# driver.cpp and bench.cpp are compiled with the flags of any JavaScriptCore runtime object of that build
# (ninja -t commands <object>), ref.cpp with the ICU headers alone (it needs the C++ API).
#
# Differential (debug objects, so that the ASSERT behind the quick exit runs), in build/debug-local:
#   clang++ driver.o ref.o @harness.rsp -Wl,@jsc.lazy.rsp -fsanitize=address -static-libstdc++ -static-libgcc \
#     -fuse-ld=lld -fno-pic -Wl,-no-pie -Wl,--gc-sections -lc -lpthread -ldl -l:libatomic.a -o harness
#   node gen.js > corpus.txt; node genlong.js; node genlong2.js
#   ASAN_OPTIONS=detect_leaks=0 ./harness diff 10 < corpus.txt
# With the alias tables of a macOS release: compile data/misc/metadata.txt of apple-oss-distributions/ICU with genrb
# into <dir>/icudt79l/metadata.res and run with ICU_DATA=<dir>/ (apple-runs.sh).
#
# Cost (release objects), in build/release-local: relink-bench.sh, then measure.sh.
