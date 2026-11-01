#!/bin/bash
# Relinks the cost driver against the release objects of /workspace/bun/build/release-local.
cd /workspace/bun/build/release-local || exit 1
/usr/lib/llvm-23/bin/clang++ /tmp/alias-harness/bench.o @/tmp/alias-harness/release.rsp -Wl,@jsc.lazy.rsp -static-libstdc++ -static-libgcc --ld-path=/usr/lib/llvm-23/bin/ld.lld -fno-pic -Wl,-no-pie -Wl,--gc-sections \
  -Wl,--wrap=mi_malloc -Wl,--wrap=mi_zalloc -Wl,--wrap=mi_realloc -Wl,--wrap=mi_malloc_aligned -Wl,--wrap=mi_zalloc_aligned -Wl,--wrap=mi_free \
  -lc -lpthread -ldl -l:libatomic.a -o /tmp/alias-harness/bench
