# r4 artifacts

- `hashdump.cpp` / `hashdump.out` — offline, engine-faithful (build's own WTF
  headers, RapidHash path) dump of hash + bucket(=hash%512) for every key the
  workload resolves through KeyAtomStringCache. Build:
  clang++-21 -O2 -std=c++23 -I WebKitBuild/Debug/WTF/Headers \
    -I WebKitBuild/Debug/bmalloc/Headers -I Source/bmalloc -I WebKitBuild/Debug \
    -include WebKitBuild/Debug/cmakeconfig.h hashdump.cpp
  Cross-validated bit-exact against the in-vivo EVIDENCE §10.2 KEYATOM-IDX lines.
- Round-4 instrumentation was the r3 logic hunks hand-applied (the r3 patch's
  include hunk was left in the tree by the r3 revert, so `patch` refuses the
  file); fully reverted again after the differential, clean jsc relinked.
- Run dirs (volatile): /tmp/r4-soak-1, /tmp/r4-soak-2 (clean-binary soak,
  /tmp/jsc-clean-r4), /tmp/r4-regress, /tmp/r4-fixed (5+5 under 96-way
  stress-ng oversubscription: 0 window entries — suppresses the race),
  /tmp/r4-regress2, /tmp/r4-fixed2 (15+15 without stress-ng: the differential).
