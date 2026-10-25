#!/usr/bin/env node
// The lanes of bun-webkit: what is built, where, from which Dockerfile and with exactly which settings, and which lanes
// are tested. One lane is one <label>.tar.gz on the release. This file is the only place that says any of it, and the
// only way a lane is built, in CI (.github/workflows/ci.yml) and by hand:
//
//   node .github/scripts/lanes.mjs                              list the lanes and their toolchain images
//   node .github/scripts/lanes.mjs --json                       the same, in full
//   node .github/scripts/lanes.mjs build <label> --output <dir> build a lane into <dir>, like CI does
//                                  [--base-image <ref>]         ... starting from a prebuilt toolchain image
//                                  [--fail-if-slow]             ... and give up early on a slow machine, see SLOW
//   node .github/scripts/lanes.mjs image <name> --push          build a toolchain image and push it to its ref
//   node .github/scripts/lanes.mjs plan                         the outputs of the workflow's `plan` job. Asks the
//                                                               registry which images are already there.
//
// `build` and `image` take --dry-run, which prints the docker command instead of running it.
//
// A lane is built by `docker buildx build` of its platform's Dockerfile. The Dockerfile's `base` stage is the
// toolchain (compilers, SDKs, sysroots) and takes no lane setting; the stages on top build ICU and WebKit from the
// build arguments below. Every lane is cross-compiled from the same kind of machine, linux x86_64. To add, drop, change
// or test a lane, change `platforms` and nothing else.

import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "../..");
// Where the toolchain images are kept.
const REGISTRY = `ghcr.io/${(process.env.GITHUB_REPOSITORY_OWNER ?? "oven-sh").toLowerCase()}/bun-webkit-build-env`;

// Every lane builds on this, in a linux/amd64 container: whatever it is for is a --target and a sysroot to clang.
const BUILDER = "linux-x64-gh";
// A toolchain image is built on a standard runner: it is mostly downloading and unpacking, and those are never
// waited for.
const IMAGE_BUILDER = "ubuntu-latest";
// The macOS and Windows lanes are not tested. The `test` job can run them (`tested: { arm64: { on: "macos-15", variants:
// ["lto"], quick: true } }`, "windows-2025", "windows-11-arm": GitHub's standard runners, 3 or 4 cores, hence quick), and
// did until the run of bcdda91cfc30: macos-arm64-lto took 16 minutes, windows-amd64-lto 145, because --ruby-runner, the
// only runner that works there, runs the tests one at a time (TestRunnerRuby in jsc-stress-test-writer-ruby.rb).

// ThinLTO: the bitcode carries ThinLTO summaries, so the consumer's link gets parallel backends and cross-language
// importing instead of one giant serial full-LTO module. -fno-split-lto-unit keeps every module a pure summary module
// (consistent with rustc's bitcode, which never splits).
const LTO = "-flto=thin -fno-split-lto-unit -fwhole-program-vtables -fforce-emit-vtables";
// On Windows there is no -fwhole-program-vtables -fforce-emit-vtables: whole-program devirtualization drops vtable
// symbols that COFF associative COMDAT sections still name as their parent, and the LTO codegen aborts
// ("Associative COMDAT symbol '??_7...' does not exist").
const LTO_WINDOWS = "/clang:-flto=thin /clang:-fno-split-lto-unit";

const SANITIZERS = "address,undefined";

// All of it is decided when a function is compiled to IR (a function attribute, a store, a call to a checked libc
// function), so it means the same with and without LTO. _FORTIFY_SOURCE is 2 because glibc 2.31 has nothing above it.
const HARDENING =
  "-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fstack-clash-protection -ftrivial-auto-var-init=zero -ftrivial-auto-var-init-max-size=4096" +
  " -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing -fsanitize=array-bounds -fsanitize-trap=array-bounds" +
  " -D_GLIBCXX_ASSERTIONS=1 -fPIE -DBUN_HARDENED=1 -DENABLE_AOT_POSITION_INDEPENDENT_IMAGE=1 -DENABLE_AOT_GUARDED_PAGES=1" +
  " -DBUN_REFUSES_EXECUTE_ONLY_START=1";

// Which ICU every platform but macOS (which uses the system's) builds and bundles: icu/source.json, and nowhere else.
const icu = JSON.parse(readFileSync(join(root, "icu/source.json"), "utf8"));
const ICU = { ICU_VERSION: icu.version, ICU_SHA256: icu.sha256 };

// The code generation floor. There is one per architecture: WebKit used to ship a haswell x64 build next to a nehalem
// "baseline" one, which meant every x64 consumer had to pick, and a consumer that picked wrong either raised its CPU
// requirement silently or gave up cross-language LTO because the matching variant did not exist. A consumer that wants
// a higher floor for its own code can still set one, it just does not get it inside JSC.
const NEHALEM = "-march=nehalem";
const ARMV8 = "-march=armv8-a+crc";

// The variant is the label's suffix: bun-webkit-linux-amd64 is "release", bun-webkit-linux-amd64-debug-asan-aot is
// "debug-asan-aot". The optimized variants that ship in bun use mimalloc, which bun links itself (USE_EXTERNAL_MIMALLOC).
//
// `aot: false` is ENABLE_AOT=0: neither the ahead-of-time compiler nor the run time of its programs is in the libraries.
// That is what a bun built with --aot=off links. Each such variant has a twin with both, "-aot", which is what `bun`
// links. Every other variant has both and no twin without. A label says which it is: a lane with both has "-aot" in its
// label, and a lane without has the label it had before there was a compiler.
const variants = {
  "release": { buildType: "Release", mimalloc: true, aot: false },
  "aot": { buildType: "Release", mimalloc: true },
  "lto": { buildType: "Release", mimalloc: true, lto: true, aot: false },
  "lto-aot": { buildType: "Release", mimalloc: true, lto: true },
  "debug-aot": { buildType: "Debug" },
  "asan-aot": { buildType: "Release", sanitizers: SANITIZERS },
  "debug-asan-aot": { buildType: "Debug", sanitizers: SANITIZERS },
  // For fuzzing. Each is "aot" with assertions, coverage or both added and nothing else changed, so that what is
  // fuzzed is as close as it can be to what ships. `assertions` is the assertions of the asan variants without the
  // sanitizers. `coverage` is ENABLE_FUZZILLI: JavaScriptCore (not WTF or bmalloc) calls
  // __sanitizer_cov_trace_pc_guard on every edge. The libraries leave that for bun to define, and jsc takes --reprl.
  //
  // Not "lto": coverage does not link with ThinLTO ("undefined symbol: __sancov_gen_.1"). Of an inline function that
  // several modules have, ThinLTO keeps one copy and makes the others available_externally, their guards included,
  // which are private to the module. Inlining such a copy leaves a reference to guards that nothing defines.
  "assertions-aot": { buildType: "Release", mimalloc: true, assertions: true },
  "coverage-aot": { buildType: "Release", mimalloc: true, coverage: true },
  "assertions-coverage-aot": { buildType: "Release", mimalloc: true, assertions: true, coverage: true },
  // "aot", "lto" and "lto-aot" with ICU and WebKit compiled with HARDENING, and nothing else changed. Only what ships
  // has a twin without the compiler.
  "hardened-aot": { buildType: "Release", mimalloc: true, hardened: true },
  "lto-hardened": { buildType: "Release", mimalloc: true, lto: true, hardened: true, aot: false },
  "lto-hardened-aot": { buildType: "Release", mimalloc: true, lto: true, hardened: true },
  // "lto-hardened-aot" with ENABLE_WEBASSEMBLY=0: there is no global of that name, and JavaScriptCore's code for it is not
  // in the libraries.
  "lto-hardened-aot-no-webassembly": { buildType: "Release", mimalloc: true, lto: true, hardened: true, webAssembly: false },
};
const NO_ASAN = ["release", "aot", "lto", "lto-aot", "debug-aot"];
const ALL = [...NO_ASAN, "asan-aot", "debug-asan-aot"];
const FUZZING = ["assertions-aot", "coverage-aot", "assertions-coverage-aot"];
const HARDENED = ["hardened-aot", "lto-hardened", "lto-hardened-aot", "lto-hardened-aot-no-webassembly"];
// "lto-aot" is "lto" with the compiler, and "asan-aot" is what bun calls "asan".
const withoutAOT = variant => (variant === "aot" ? "release" : variant.replace(/-aot$/, ""));

// Each platform:
//   label(arch)     the label of the release variant; the other variants append -<variant>
//   dockerfile      builds the lane: `base` is the toolchain, the stages on top build ICU and WebKit
//   packageOS       the "os" of the tarball's package.json
//   lanes           arch -> variants built for it
//   args(arch, v)   the Dockerfile's build arguments, on top of WEBKIT_RELEASE_TYPE, LTO_FLAG and USE_*_MIMALLOC
//   image(arch)     the name of the toolchain image, one per distinct `base` stage: per architecture where `base`
//                   is built for one (MACOS_ARCH, FREEBSD_ARCH). See `images` below.
//   imageInputs     files and directories the `base` stage copies in, besides the Dockerfile
//   tested          per architecture, the variants whose jsc shell the `test` job runs the JavaScriptCore tests with,
//                   and the runner it does that on: a machine of the lane's own platform and architecture. "lto-aot" is
//                   what bun ships, "lto" is that without the compiler, and nothing else runs "lto-hardened-aot". The
//                   asan lanes are not tested: the tests take 65 to 100 minutes there on Linux and macOS and do not
//                   finish in 275 on Windows. `quick` is --quick, for a runner with few cores.
//   testPrograms    what a tested lane builds besides what it ships, for the `test` job (the Dockerfile's TEST_PROGRAMS)
//   bunTests        per architecture, the variants bun's CI links and then runs bun's tests with (buildPlatforms and
//                   testPlatforms in bun's .buildkite/ci.ts, ltoDefault in its scripts/build/config.ts): of a variant
//                   that has an "-aot" twin it links the twin. See `programs`.
const platforms = [
  {
    label: arch => `bun-webkit-linux-${arch}`,
    // The container is ubuntu 20.04 x86_64, for its glibc (2.31); arm64 is cross-compiled against an ubuntu 20.04
    // arm64 sysroot in the same container.
    dockerfile: "Dockerfile",
    packageOS: "linux",
    lanes: { amd64: [...ALL, ...FUZZING, ...HARDENED], arm64: ALL },
    tested: {
      amd64: { on: "linux-x64-gh", variants: ["lto", "lto-aot", "lto-hardened-aot"] },
      arm64: { on: "linux-arm64-gh", variants: ["lto", "lto-aot"] },
    },
    testPrograms: ["testb3", "testair", "testmasm", "TestWTF"],
    bunTests: arch => (arch === "arm64" ? ["lto"] : ["lto", "asan"]),
    image: () => "linux-glibc",
    args: (arch, v) => ({
      ...ICU,
      LINUX_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      RELEASE_FLAGS: "-O3 -DNDEBUG=1",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
      ENABLE_ASSERTS: v.assertions ? "ON" : "AUTO",
      ENABLE_FUZZILLI: v.coverage ? "ON" : "OFF",
      HARDENING_FLAGS: v.hardened ? HARDENING : "",
      ENABLE_WEBASSEMBLY: v.webAssembly === false ? "OFF" : "ON",
      // Explicit --target: Ubuntu's clang defaults to x86_64-pc-linux-gnu, while bun's own objects and its Rust code
      // are <arch>-unknown-linux-gnu; with LTO, lld warns about the vendor mismatch once per JavaScriptCore object.
      MARCH_FLAG:
        arch === "arm64"
          ? `--target=aarch64-unknown-linux-gnu ${ARMV8} -mtune=ampere1`
          : `--target=x86_64-unknown-linux-gnu ${NEHALEM}`,
    }),
  },
  {
    label: arch => `bun-webkit-linux-${arch}-musl`,
    // The container is alpine x86_64; arm64 is cross-compiled against an alpine aarch64 sysroot in the same container.
    dockerfile: "Dockerfile.musl",
    packageOS: "linux",
    lanes: { amd64: NO_ASAN, arm64: NO_ASAN },
    buildType: v => (v.buildType === "Release" ? "MinSizeRel" : v.buildType),
    bunTests: () => ["lto"],
    image: () => "linux-musl",
    args: arch => ({
      ...ICU,
      LINUX_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      MARCH_FLAG: arch === "arm64" ? `${ARMV8} -mtune=ampere1` : NEHALEM,
    }),
  },
  {
    // clang --target + a pinned macOS SDK, linked with ld64.lld.
    label: arch => `bun-webkit-macos-${arch}`,
    dockerfile: "Dockerfile.macos",
    packageOS: "darwin",
    // ASAN is arm64 only: the darwin sanitizer runtime (mirrored at the compiler-rt-darwin-* release tag, a Linux LLVM
    // install doesn't ship it) is extracted from the official LLVM macOS release, which is published for arm64 only.
    lanes: { arm64: ALL, amd64: NO_ASAN },
    bunTests: () => ["lto"],
    image: arch => `macos-${arch}`,
    imageInputs: ["macos-cross"],
    args: (arch, v, variant) => ({
      MACOS_ARCH: arch === "arm64" ? "arm64" : "x86_64",
      MACOS_DEPLOYMENT_TARGET: "13.0",
      MARCH_FLAG: arch === "arm64" ? "-mcpu=apple-m1" : NEHALEM,
      CPP_FLAGS: v.mimalloc ? "" : "-D_LIBCXX_ENABLE_ASSERTIONS=1",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
      // Debug builds break the heap down by type, except under ASAN.
      ENABLE_MALLOC_HEAP_BREAKDOWN: variant === "debug-aot" ? "ON" : "OFF",
    }),
  },
  {
    // clang-cl --target + an xwin-downloaded MSVC CRT and Windows SDK + lld-link.
    label: arch => `bun-webkit-windows-${arch}`,
    dockerfile: "Dockerfile.windows",
    packageOS: "windows",
    // There is no asan lane for arm64, and no lto one: bun ships its plain release build.
    lanes: {
      // ASAN is x64 only: LLVM ships no Windows ARM64 ASAN runtime. The sanitizer runtime (import lib, /MT runtime
      // thunk, DLL) comes from the compiler-rt-windows-* release tag.
      amd64: ["release", "aot", "lto", "lto-aot", "debug-aot", "asan-aot"],
      // No arm64 lto: LLVM's CodeView emitter (still true of 23.1) has no register mapping for ARM64 NEON quad-register tuples
      // ("LLVM ERROR: unknown codeview register Q22_Q23_Q24_Q25") and the LTO codegen allocates values into them.
      arm64: ["release", "aot", "debug-aot"],
    },
    lto: LTO_WINDOWS,
    bunTests: arch => [arch === "arm64" ? "release" : "lto"],
    image: () => "windows",
    args: (arch, v) => ({
      ...ICU,
      WIN_ARCH: arch === "arm64" ? "arm64" : "x64",
      WIN_TRIPLE_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      // WebKit is built by clang-cl, ICU by clang's GNU driver.
      MARCH_FLAG: `/clang:${arch === "arm64" ? ARMV8 : NEHALEM}`,
      ICU_MARCH_FLAG: arch === "arm64" ? ARMV8 : NEHALEM,
      ENABLE_SANITIZERS: v.sanitizers ?? "",
    }),
  },
  {
    // clang --target + a base.txz sysroot.
    label: arch => `bun-webkit-freebsd-${arch}`,
    dockerfile: "Dockerfile.freebsd",
    packageOS: "freebsd",
    lanes: { amd64: NO_ASAN, arm64: NO_ASAN },
    image: arch => `freebsd-${arch}`,
    args: arch => ({
      ...ICU,
      FREEBSD_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      FREEBSD_VERSION: "14.4",
      MARCH_FLAG: arch === "arm64" ? `${ARMV8} -mtune=ampere1` : NEHALEM,
    }),
  },
  {
    // The NDK, which only ships linux-x86_64 prebuilts.
    label: arch => `bun-webkit-linux-${arch}-android`,
    dockerfile: "Dockerfile.android",
    packageOS: "android",
    lanes: { arm64: NO_ASAN, amd64: NO_ASAN },
    image: () => "android",
    args: arch => ({
      ...ICU,
      ANDROID_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      ANDROID_API: "28",
      MARCH_FLAG: arch === "arm64" ? `${ARMV8} -mtune=cortex-a78` : NEHALEM,
    }),
  },
];

// Every lane links jsc, testFFI and testaot, which checks that they link. Each is a full copy of JavaScriptCore, so a lane
// only ships the ones something downloads it for:
//   jsc        everywhere
//   testFFI    in the lanes bun's CI tests, which packs it next to bun (packageAndUpload in bun's scripts/build/ci.ts),
//              and in the lanes the `test` job here runs
//   testaot    in the lanes the `test` job here runs. It has the table of built-ins of the stand-in host (aot/tests/host).
// and, in lib/, the objects of jsc and of testaot without a table (shell/CMakeLists.txt), which bun links with its own:
//   jsc-objects, testaot-objects   in those same lanes, and in linux-amd64-debug-asan-aot for bun's "AoT cargo test" job.
//                                  bun's build looks for them in the tarball (scripts/build/deps/webkit.ts).
function programs(platform, arch, variant, test) {
  const objects = ["jsc-objects", "testaot-objects"];
  if (test && variants[variant].aot === false) return ["jsc", "testFFI"];
  if (test) return ["jsc", "testFFI", "testaot", ...objects];
  if (platform.bunTests?.(arch).includes(withoutAOT(variant))) return variants[variant].aot === false ? ["jsc", "testFFI"] : ["jsc", "testFFI", ...objects];
  return platform.label(arch) === "bun-webkit-linux-amd64" && variant === "debug-asan-aot" ? ["jsc", ...objects] : ["jsc"];
}

// One more asset of every release, next to the lanes' tarballs: what a tarball has under Source/JavaScriptCore/aot but the
// tests, at the same paths. That is the schema, the two generators and the generated files the engine was compiled with:
// 2 MB. bun's jobs that link no engine (its lint and type-check jobs) read it in place of a lane's tarball.
// `from` packs it, out of its own output.
const SOURCES = { label: "bun-webkit-aot-sources", from: "bun-webkit-linux-amd64-lto-aot" };

// A toolchain image is tagged with a hash of what goes into it: the Dockerfile up to the end of its `base` stage (less
// comments, and less the global ARGs that `base` does not take, which are lane settings), the files that stage copies
// in, and the build arguments it takes (the ARGs it declares that a lane sets: FREEBSD_VERSION, MACOS_DEPLOYMENT_TARGET,
// ANDROID_API, ...). Changing any of those makes a new tag, which `plan` finds missing and `image` builds; changing
// anything else (the ICU or WebKit stages, the sources, other settings, a comment) leaves it alone.
function imageRef(platform, arch, buildArgs) {
  const dockerfile = readFileSync(join(root, platform.dockerfile), "utf8");
  const from = /^FROM\s.*\sAS\s+base\s*$/im.exec(dockerfile);
  if (!from) throw new Error(`${platform.dockerfile} has no \`base\` stage`);
  const next = /^FROM\s/m.exec(dockerfile.slice(from.index + from[0].length));
  const end = next ? from.index + from[0].length + next.index : dockerfile.length;
  const code = text => text.split("\n").filter(line => line.trim() !== "" && !/^\s*#/.test(line));
  const base = code(dockerfile.slice(from.index, end));
  const taken = new Set(base.map(line => /^ARG\s+(\w+)/.exec(line)?.[1]).filter(Boolean));
  const above = code(dockerfile.slice(0, from.index)).filter(line => taken.has(/^ARG\s+(\w+)/.exec(line)?.[1]) || !/^ARG\s/.test(line));
  const hash = createHash("sha256").update([...above, ...base].join("\n"));
  for (const name of taken) if (name in buildArgs) hash.update(`\n${name}=${buildArgs[name]}`);
  const add = path => {
    if (statSync(join(root, path)).isDirectory()) return readdirSync(join(root, path)).sort().forEach(entry => add(join(path, entry)));
    if (!path.endsWith(".md")) hash.update(path).update(readFileSync(join(root, path)));
  };
  for (const input of platform.imageInputs ?? []) add(input);
  return `${REGISTRY}:${platform.image(arch)}-${hash.digest("hex").slice(0, 16)}`;
}

const lanes = platforms.flatMap(platform =>
  Object.entries(platform.lanes).flatMap(([arch, names]) =>
    names.map(variant => {
      const v = variants[variant];
      const buildType = platform.buildType?.(v) ?? v.buildType;
      const test = platform.tested?.[arch]?.variants.includes(variant) ?? false;
      const buildArgs = {
        WEBKIT_RELEASE_TYPE: buildType,
        LTO_FLAG: v.lto ? (platform.lto ?? LTO) : "",
        USE_MIMALLOC: v.mimalloc ? "ON" : "OFF",
        USE_EXTERNAL_MIMALLOC: v.mimalloc ? "ON" : "OFF",
        // Not without the compiler: testb3 and testair set the policies that only the compiler sets.
        ...(platform.testPrograms ? { TEST_PROGRAMS: test && v.aot !== false ? platform.testPrograms.join(" ") : "" } : {}),
        ...platform.args(arch, v, variant),
      };
      // wtf/PlatformEnable.h turns it on wherever the FTL is on, unless it is defined. Every Dockerfile hands MARCH_FLAG to
      // each compile of WebKit.
      if (v.aot === false) buildArgs.MARCH_FLAG += " -DENABLE_AOT=0";
      const label = platform.label(arch) + (variant === "release" ? "" : `-${variant}`);
      if ((v.aot !== false) !== /-aot(-|$)/.test(label)) throw new Error(`${label}: a lane has "-aot" in its label where it has the compiler, and only there`);
      return {
        label,
        sources: label === SOURCES.from ? SOURCES.label : "",
        runner: BUILDER,
        dockerfile: platform.dockerfile,
        package_os: platform.packageOS,
        package_cpu: arch === "arm64" ? "arm64" : "x64",
        test,
        test_runner: platform.tested?.[arch]?.on,
        test_quick: platform.tested?.[arch]?.quick ?? false,
        programs: programs(platform, arch, variant, test).join(" "),
        aot: v.aot === false ? 0 : 1,
        // What of HARDENING the headers in the tarball test: they lay classes out by it.
        defines: v.hardened ? [...HARDENING.matchAll(/-D((?:BUN|ENABLE)_\w+=\S+)/g)].map(match => match[1]).join(" ") : "",
        image: imageRef(platform, arch, buildArgs),
        build_args: buildArgs,
      };
    }),
  ),
);

// `build --fail-if-slow`, which CI passes. Some runners are five times slower than the rest, from the first step to the
// last. A lane on one of them fails within minutes instead of holding up the release, and "Re-run failed jobs" builds it
// on another runner.
//   compressSeconds  icu/compress-data.ts does the same work in every lane that runs it. It takes 67 to 115 seconds, and
//                    over 440 on a slow runner.
//   minutes(lane)    the whole docker build: twice the longest that the lane takes (19 minutes; 36 with musl, LTO and
//                    the compiler). It has to stay under the job's timeout-minutes.
const SLOW = {
  compressSeconds: 240,
  minutes: lane => (lane.dockerfile === "Dockerfile.musl" && lane.build_args.LTO_FLAG && lane.aot ? 75 : 40),
};

const labels = lanes.map(lane => lane.label);
const duplicate = labels.find((label, i) => labels.indexOf(label) !== i);
if (duplicate) throw new Error(`two lanes are called ${duplicate}`);
if (!labels.includes(SOURCES.from)) throw new Error(`no lane is called ${SOURCES.from}, which packs ${SOURCES.label}`);

// The toolchain images the lanes start from. One is built by the first lane that uses it: the `base` stage does not
// read the lane's build arguments, only the ones that say which toolchain it is (MACOS_ARCH, FREEBSD_ARCH, ...).
const images = [...new Map(lanes.toReversed().map(lane => [lane.image, lane])).values()].toReversed().map(lane => ({
  name: lane.image.slice(REGISTRY.length + 1, lane.image.lastIndexOf("-")),
  image: lane.image,
  runner: IMAGE_BUILDER,
  lane,
}));

const twice = images.find((image, i) => images.findIndex(other => other.name === image.name) !== i);
if (twice) throw new Error(`two different toolchain images are called ${twice.name}: a \`base\` stage takes a build argument that differs between lanes that share that name`);
// `test` waits for the tested lanes and `release` for both halves, and a job cannot have an empty matrix.
if (!lanes.some(lane => lane.test) || lanes.every(lane => lane.test)) throw new Error("at least one lane has to be tested, and at least one not");

function docker(lane, rest, dryRun, failIfSlow = false) {
  const argv = ["buildx", "build", "-f", lane.dockerfile, "--platform", "linux/amd64", "--progress=plain"];
  for (const [key, value] of Object.entries(lane.build_args)) argv.push("--build-arg", `${key}=${value}`);
  argv.push(...rest, ".");
  if (dryRun) return console.log(JSON.stringify(["docker", ...argv]));
  console.error(`docker ${argv.map(arg => (/^[\w./:=,-]+$/.test(arg) ? arg : `'${arg}'`)).join(" ")}`);
  if (failIfSlow) return dockerUnlessSlow(lane, argv);
  const { status, error } = spawnSync("docker", argv, { cwd: root, stdio: "inherit" });
  if (error) throw error;
  process.exit(status ?? 1);
}

// --progress=plain goes to stderr: every line is passed on, and the two lines of icu/compress-data.ts are timed.
function dockerUnlessSlow(lane, argv) {
  const child = spawn("docker", argv, { cwd: root, stdio: ["inherit", "inherit", "pipe"] });
  let gaveUp = false;
  const giveUp = what => {
    if (gaveUp) return;
    gaveUp = true;
    console.log(`::error::${lane.label}: ${what}. This runner is too slow. Use "Re-run failed jobs" to build the lane on another one.`);
    child.kill();
    setTimeout(() => process.exit(1), 15_000);
  };
  setTimeout(giveUp, SLOW.minutes(lane) * 60_000, `the build has taken ${SLOW.minutes(lane)} minutes`);
  let compress;
  createInterface({ input: child.stderr }).on("line", line => {
    console.error(line);
    if (line.includes("[icu-compress] round-trip OK")) compress = setTimeout(giveUp, SLOW.compressSeconds * 1000, `icu/compress-data.ts has taken ${SLOW.compressSeconds} seconds`);
    else if (/\[icu-compress\] \d+ items/.test(line)) clearTimeout(compress);
  });
  child.on("error", error => { throw error; });
  child.on("exit", () => gaveUp && process.exit(1));
  child.on("close", status => process.exit(status ?? 1));
}

function option(args, name) {
  const i = args.indexOf(name);
  return i === -1 ? undefined : (args[i + 1] ?? fail(`${name} takes a value`));
}

function fail(message) {
  console.error(message);
  process.exit(2);
}

const [command, ...args] = process.argv.slice(2);
if (command === "build") {
  const lane = lanes.find(lane => lane.label === args[0]) ?? fail(`no lane is called ${args[0]}; run this with no arguments for the list`);
  const output = option(args, "--output") ?? fail("build takes --output <dir>");
  const baseImage = option(args, "--base-image");
  docker(
    lane,
    [
      // In place of building the Dockerfile's `base` stage.
      ...(baseImage ? ["--build-context", `base=docker-image://${baseImage}`] : []),
      "--target=artifact",
      "--output",
      `type=local,dest=${resolve(output)}`,
    ],
    args.includes("--dry-run"),
    args.includes("--fail-if-slow"),
  );
} else if (command === "image") {
  const image = images.find(image => image.name === args[0]) ?? fail(`no image is called ${args[0]}: ${images.map(image => image.name).join(", ")}`);
  if (!args.includes("--push")) fail("image takes --push");
  // --provenance=false: a plain image manifest, not an index with an attestation hanging off it.
  docker(image.lane, ["--target=base", "--tag", image.image, "--provenance=false", "--push"], args.includes("--dry-run"));
} else if (command === "plan") {
  const matrix = include => JSON.stringify({ include });
  const build = ({ label, sources, runner, image, package_os, package_cpu, test, programs, aot, defines }) => ({ label, sources, runner, image, package_os, package_cpu, test, programs, aot, defines });
  console.log(`build=${matrix(lanes.filter(lane => !lane.test).map(build))}`);
  console.log(`build_tested=${matrix(lanes.filter(lane => lane.test).map(build))}`);
  console.log(`test=${matrix(lanes.filter(lane => lane.test).map(lane => ({ label: lane.label, runner: lane.test_runner, quick: lane.test_quick })))}`);
  console.log(`labels=${JSON.stringify([...labels, SOURCES.label])}`);
  // Only the toolchain images that are not in the registry yet get an `image` job: normally none.
  const missing = images.filter(({ image }) => {
    const { status, stderr } = spawnSync("docker", ["manifest", "inspect", image], { encoding: "utf8", stdio: ["ignore", "ignore", "pipe"] });
    const there = status === 0;
    // Anything but "there is no such thing" is the registry failing, and must not set off a rebuild of everything.
    if (!there && !/manifest unknown|name unknown|not found|no such manifest/i.test(stderr)) fail(`cannot tell whether ${image} exists:\n${stderr}`);
    console.error(`${image} ${there ? "is there" : "is missing, to be built"}`);
    return !there;
  });
  console.log(`images=${matrix(missing.map(({ name, runner }) => ({ name, runner })))}`);
  console.log(`build_images=${missing.length > 0}`);
  // All of them, by tag: what the `prune` job keeps.
  console.log(`image_tags=${JSON.stringify(images.map(({ image }) => image.slice(REGISTRY.length + 1)))}`);
} else if (command === "--json") {
  console.log(JSON.stringify(lanes, null, 2));
} else if (command === undefined) {
  for (const lane of lanes) {
    console.log(`${lane.label.padEnd(42)} ${lane.runner.padEnd(15)} ${lane.dockerfile.padEnd(19)} ${lane.build_args.WEBKIT_RELEASE_TYPE.padEnd(10)} ${lane.programs}${lane.test ? "  (tested)" : ""}`);
  }
  console.log(`${lanes.length} lanes`);
  for (const { image, runner } of images) console.log(`${image}  ${runner}`);
  console.log(`${images.length} toolchain images`);
} else {
  fail(`unknown command ${command}; see the top of ${fileURLToPath(import.meta.url)}`);
}
