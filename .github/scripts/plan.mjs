#!/usr/bin/env node
// The lanes of .github/workflows/build-reusable.yml: what is built, on which runner, with which script and settings,
// and which lanes are tested. One lane is one <label>.tar.gz on the release.
//
//   node .github/scripts/plan.mjs             print the lanes, one per line
//   node .github/scripts/plan.mjs --json      print them in full
//   node .github/scripts/plan.mjs --outputs   print the outputs of the workflow's `plan` job ($GITHUB_OUTPUT format)
//
// The build matrix, the test matrix and the list of assets the `release` job insists on are all derived from
// `platforms` below. To add, drop or test a lane, change it there and nowhere else.

const X64 = "linux-x64-gh";
const ARM64 = "linux-arm64-gh";

// ThinLTO: the bitcode carries ThinLTO summaries, so the consumer's link gets parallel backends and cross-language
// importing instead of one giant serial full-LTO module. -fno-split-lto-unit keeps every module a pure summary module
// (consistent with rustc's bitcode, which never splits).
const LTO = "-flto=thin -fno-split-lto-unit -fwhole-program-vtables -fforce-emit-vtables";
// On Windows there is no -fwhole-program-vtables -fforce-emit-vtables: whole-program devirtualization drops vtable
// symbols that COFF associative COMDAT sections still name as their parent, and the LTO codegen aborts
// ("Associative COMDAT symbol '??_7...' does not exist").
const LTO_WINDOWS = "/clang:-flto=thin /clang:-fno-split-lto-unit";

const SANITIZERS = "address,undefined";

// The variant is the label's suffix: bun-webkit-linux-amd64 is "release", bun-webkit-linux-amd64-debug-asan is
// "debug-asan". The optimized variants that ship in bun use mimalloc, which bun links itself (USE_EXTERNAL_MIMALLOC).
const variants = {
  "release": { buildType: "Release", mimalloc: true },
  "lto": { buildType: "Release", mimalloc: true, lto: true },
  "debug": { buildType: "Debug" },
  "asan": { buildType: "Release", sanitizers: SANITIZERS },
  "debug-asan": { buildType: "Debug", sanitizers: SANITIZERS },
};
const ALL = Object.keys(variants);
const NO_ASAN = ["release", "lto", "debug"];

// Each platform:
//   label(arch)     the label of the release variant; the other variants append -<variant>
//   script          the release script, which builds into $temp/bun-webkit
//   packageOS       the "os" of the tarball's package.json
//   runner(arch)    where it builds. Everything but linux and linux-musl is cross-compiled from a linux x64 host.
//   lanes           arch -> variants built for it
//   env(arch, v)    what the release script takes, on top of WEBKIT_RELEASE_TYPE, LTO_FLAG and USE_*_MIMALLOC
//   tested          variants whose jsc shell the `test` job runs the JavaScriptCore tests with. Those have to be able to
//                   run on the runner that built them, and want assertions: a plain Release build compiles out $vm and
//                   the JIT disassembler (BUN_ENABLE_JSDOLLARVM / BUN_ENABLE_JIT_DISASSEMBLER default to ASSERT_ENABLED).
const platforms = [
  {
    name: "linux",
    label: arch => `bun-webkit-linux-${arch}`,
    script: "release.sh",
    packageOS: "linux",
    runner: arch => (arch === "arm64" ? ARM64 : X64),
    lanes: { amd64: ALL, arm64: ALL },
    tested: ["asan"],
    env: (arch, v) => ({
      RELEASE_FLAGS: "-O3 -DNDEBUG=1",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
      // Explicit --target: Ubuntu's clang defaults to x86_64-pc-linux-gnu, while bun's own objects and its Rust code
      // are <arch>-unknown-linux-gnu; with LTO, lld warns about the vendor mismatch once per JavaScriptCore object.
      MARCH_FLAG:
        arch === "arm64"
          ? "--target=aarch64-unknown-linux-gnu -march=armv8-a+crc -mtune=ampere1"
          : "--target=x86_64-unknown-linux-gnu -march=nehalem",
      CPU: "native",
      cpu: "native",
    }),
  },
  {
    name: "linux-musl",
    label: arch => `bun-webkit-linux-${arch}-musl`,
    script: "musl-release.sh",
    packageOS: "linux",
    runner: arch => (arch === "arm64" ? ARM64 : X64),
    lanes: { amd64: NO_ASAN, arm64: NO_ASAN },
    buildType: v => (v.buildType === "Release" ? "MinSizeRel" : v.buildType),
    env: arch => ({
      MARCH_FLAG: arch === "arm64" ? "-march=armv8-a+crc -mtune=ampere1" : "-march=nehalem",
      CPU: "native",
      cpu: "native",
    }),
  },
  {
    // clang --target + a pinned macOS SDK, linked with ld64.lld (Dockerfile.macos). mac-release.bash remains for
    // building on a real Mac.
    name: "macos",
    label: arch => `bun-webkit-macos-${arch}`,
    script: "macos-cross-release.sh",
    packageOS: "darwin",
    runner: () => X64,
    // ASAN is arm64 only: the darwin sanitizer runtime (mirrored at the compiler-rt-darwin-* release tag, a Linux LLVM
    // install doesn't ship it) is extracted from the official LLVM macOS release, which is published for arm64 only.
    lanes: { arm64: ALL, amd64: NO_ASAN },
    env: (arch, v, variant) => ({
      MACOS_ARCH: arch === "arm64" ? "arm64" : "x86_64",
      CPP_FLAGS: v.mimalloc ? "" : "-D_LIBCXX_ENABLE_ASSERTIONS=1",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
      ENABLE_MALLOC_HEAP_BREAKDOWN: variant === "debug-asan" ? "OFF" : "",
    }),
  },
  {
    // clang-cl --target + an xwin-downloaded MSVC CRT/Windows SDK + lld-link (Dockerfile.windows).
    // windows-release.ps1 remains for building on a real Windows machine.
    name: "windows",
    label: arch => `bun-webkit-windows-${arch}`,
    script: "windows-cross-release.sh",
    packageOS: "windows",
    runner: () => X64,
    lanes: {
      // ASAN is x64 only: LLVM ships no Windows ARM64 ASAN runtime. The sanitizer runtime (import lib, /MT runtime
      // thunk, DLL) comes from the compiler-rt-windows-* release tag.
      amd64: ["release", "lto", "debug", "asan"],
      // No arm64 lto: LLVM 21's CodeView emitter has no register mapping for ARM64 NEON quad-register tuples
      // ("LLVM ERROR: unknown codeview register Q22_Q23_Q24_Q25") and the LTO codegen allocates values into them.
      // No arm64 debug here either, see windows-native below.
      arm64: ["release"],
    },
    lto: LTO_WINDOWS,
    env: (arch, v) => ({
      WIN_ARCH: arch === "arm64" ? "arm64" : "x64",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
    }),
  },
  {
    // The one lane that builds on Windows, with windows-release.ps1 instead of a release script and Docker: /MTd needs
    // the ARM64 *debug* CRT (libcmtd.lib), which xwin's splat doesn't produce (no CRT.arm64.Debug package in the VS
    // manifest mapping), so it can't be cross-compiled yet.
    name: "windows-native",
    label: arch => `bun-webkit-windows-${arch}`,
    script: "windows-release.ps1",
    packageOS: "windows",
    runner: () => "windows-11-arm",
    lanes: { arm64: ["debug"] },
    env: () => ({}),
  },
  {
    // clang --target + a base.txz sysroot.
    name: "freebsd",
    label: arch => `bun-webkit-freebsd-${arch}`,
    script: "freebsd-release.sh",
    packageOS: "freebsd",
    runner: () => X64,
    lanes: { amd64: NO_ASAN, arm64: NO_ASAN },
    env: arch => ({ FREEBSD_ARCH: arch === "arm64" ? "aarch64" : "x86_64" }),
  },
  {
    // The NDK only ships linux-x86_64 prebuilts.
    name: "android",
    label: arch => `bun-webkit-linux-${arch}-android`,
    script: "android-release.sh",
    packageOS: "android",
    runner: () => X64,
    lanes: { arm64: NO_ASAN, amd64: NO_ASAN },
    env: arch => ({ ANDROID_ARCH: arch === "arm64" ? "aarch64" : "x86_64" }),
  },
];

const lanes = platforms.flatMap(platform =>
  Object.entries(platform.lanes).flatMap(([arch, names]) =>
    names.map(variant => {
      const v = variants[variant];
      const buildType = platform.buildType?.(v) ?? v.buildType;
      return {
        label: platform.label(arch) + (variant === "release" ? "" : `-${variant}`),
        platform: platform.name,
        runner: platform.runner(arch),
        script: platform.script,
        build_type: buildType,
        package_os: platform.packageOS,
        package_cpu: arch === "arm64" ? "arm64" : "x64",
        test: platform.tested?.includes(variant) ?? false,
        env: {
          WEBKIT_RELEASE_TYPE: buildType,
          LTO_FLAG: v.lto ? (platform.lto ?? LTO) : "",
          USE_MIMALLOC: v.mimalloc ? "ON" : "OFF",
          USE_EXTERNAL_MIMALLOC: v.mimalloc ? "ON" : "OFF",
          ...platform.env(arch, v, variant),
        },
      };
    }),
  ),
);

const labels = lanes.map(lane => lane.label);
const duplicate = labels.find((label, i) => labels.indexOf(label) !== i);
if (duplicate) throw new Error(`two lanes are called ${duplicate}`);

const mode = process.argv[2];
if (mode === "--outputs") {
  const matrix = include => JSON.stringify({ include });
  console.log(`build=${matrix(lanes.filter(lane => !lane.test))}`);
  console.log(`build_tested=${matrix(lanes.filter(lane => lane.test))}`);
  console.log(`test=${matrix(lanes.filter(lane => lane.test).map(({ label, runner }) => ({ label, runner })))}`);
  console.log(`labels=${JSON.stringify(labels)}`);
} else if (mode === "--json") {
  console.log(JSON.stringify(lanes, null, 2));
} else {
  for (const lane of lanes) {
    console.log(`${lane.label.padEnd(42)} ${lane.runner.padEnd(15)} ${lane.script.padEnd(25)} ${lane.build_type}${lane.test ? "  (tested)" : ""}`);
  }
  console.log(`${lanes.length} lanes`);
}
