#!/usr/bin/env node
// The lanes of bun-webkit: what is built, where, from which Dockerfile and with exactly which settings, and which lanes
// are tested. One lane is one <label>.tar.gz on the release. This file is the only place that says any of it, and the
// only way a lane is built, in CI (.github/workflows/ci.yml) and by hand:
//
//   node .github/scripts/lanes.mjs                              list the lanes and their toolchain images
//   node .github/scripts/lanes.mjs --json                       the same, in full
//   node .github/scripts/lanes.mjs build <label> --output <dir> build a lane into <dir>, like CI does
//                                  [--base-image <ref>]         ... starting from a prebuilt toolchain image
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

import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readdirSync, readFileSync, statSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "../..");
// Where the toolchain images are kept.
const REGISTRY = `ghcr.io/${(process.env.GITHUB_REPOSITORY_OWNER ?? "oven-sh").toLowerCase()}/bun-webkit-build-env`;

// Every lane builds on this, in a linux/amd64 container: whatever it is for is a --target and a sysroot to clang.
const BUILDER = "linux-x64-gh";
// A toolchain image is built on a standard runner: it is mostly downloading and unpacking, and those are never
// waited for.
const IMAGE_BUILDER = "ubuntu-latest";
// Where a tested lane is tested: a machine that can run it.
const TESTERS = { amd64: "linux-x64-gh", arm64: "linux-arm64-gh" };

// ThinLTO: the bitcode carries ThinLTO summaries, so the consumer's link gets parallel backends and cross-language
// importing instead of one giant serial full-LTO module. -fno-split-lto-unit keeps every module a pure summary module
// (consistent with rustc's bitcode, which never splits).
const LTO = "-flto=thin -fno-split-lto-unit -fwhole-program-vtables -fforce-emit-vtables";
// On Windows there is no -fwhole-program-vtables -fforce-emit-vtables: whole-program devirtualization drops vtable
// symbols that COFF associative COMDAT sections still name as their parent, and the LTO codegen aborts
// ("Associative COMDAT symbol '??_7...' does not exist").
const LTO_WINDOWS = "/clang:-flto=thin /clang:-fno-split-lto-unit";

const SANITIZERS = "address,undefined";

// The code generation floor. There is one per architecture: WebKit used to ship a haswell x64 build next to a nehalem
// "baseline" one, which meant every x64 consumer had to pick, and a consumer that picked wrong either raised its CPU
// requirement silently or gave up cross-language LTO because the matching variant did not exist. A consumer that wants
// a higher floor for its own code can still set one, it just does not get it inside JSC.
const NEHALEM = "-march=nehalem";
const ARMV8 = "-march=armv8-a+crc";

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
//   dockerfile      builds the lane: `base` is the toolchain, the stages on top build ICU and WebKit
//   packageOS       the "os" of the tarball's package.json
//   lanes           arch -> variants built for it
//   args(arch, v)   the Dockerfile's build arguments, on top of WEBKIT_RELEASE_TYPE, LTO_FLAG and USE_*_MIMALLOC
//   image(arch)     the name of the toolchain image, one per distinct `base` stage: per architecture where `base`
//                   is built for one (MACOS_ARCH, FREEBSD_ARCH). See `images` below.
//   imageInputs     files and directories the `base` stage copies in, besides the Dockerfile
//   tested          variants whose jsc shell the `test` job runs the JavaScriptCore tests with, on a runner of the
//                   lane's architecture (TESTERS). Those want assertions: a plain Release build compiles out $vm and
//                   the JIT disassembler (BUN_ENABLE_JSDOLLARVM / BUN_ENABLE_JIT_DISASSEMBLER default to ASSERT_ENABLED).
const platforms = [
  {
    name: "linux",
    label: arch => `bun-webkit-linux-${arch}`,
    // The container is ubuntu 20.04 x86_64, for its glibc (2.31); arm64 is cross-compiled against an ubuntu 20.04
    // arm64 sysroot in the same container.
    dockerfile: "Dockerfile",
    packageOS: "linux",
    lanes: { amd64: ALL, arm64: ALL },
    tested: ["asan"],
    image: () => "linux",
    args: (arch, v) => ({
      LINUX_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      RELEASE_FLAGS: "-O3 -DNDEBUG=1",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
      // Explicit --target: Ubuntu's clang defaults to x86_64-pc-linux-gnu, while bun's own objects and its Rust code
      // are <arch>-unknown-linux-gnu; with LTO, lld warns about the vendor mismatch once per JavaScriptCore object.
      MARCH_FLAG:
        arch === "arm64"
          ? `--target=aarch64-unknown-linux-gnu ${ARMV8} -mtune=ampere1`
          : `--target=x86_64-unknown-linux-gnu ${NEHALEM}`,
    }),
  },
  {
    name: "linux-musl",
    label: arch => `bun-webkit-linux-${arch}-musl`,
    // The container is alpine x86_64; arm64 is cross-compiled against an alpine aarch64 sysroot in the same container.
    dockerfile: "Dockerfile.musl",
    packageOS: "linux",
    lanes: { amd64: NO_ASAN, arm64: NO_ASAN },
    buildType: v => (v.buildType === "Release" ? "MinSizeRel" : v.buildType),
    image: () => "linux-musl",
    args: arch => ({
      LINUX_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      MARCH_FLAG: arch === "arm64" ? `${ARMV8} -mtune=ampere1` : NEHALEM,
    }),
  },
  {
    // clang --target + a pinned macOS SDK, linked with ld64.lld. mac-release.bash remains for building on a real Mac.
    name: "macos",
    label: arch => `bun-webkit-macos-${arch}`,
    dockerfile: "Dockerfile.macos",
    packageOS: "darwin",
    // ASAN is arm64 only: the darwin sanitizer runtime (mirrored at the compiler-rt-darwin-* release tag, a Linux LLVM
    // install doesn't ship it) is extracted from the official LLVM macOS release, which is published for arm64 only.
    lanes: { arm64: ALL, amd64: NO_ASAN },
    image: arch => `macos-${arch}`,
    imageInputs: ["macos-cross"],
    args: (arch, v, variant) => ({
      MACOS_ARCH: arch === "arm64" ? "arm64" : "x86_64",
      MACOS_DEPLOYMENT_TARGET: "13.0",
      MARCH_FLAG: arch === "arm64" ? "-mcpu=apple-m1" : NEHALEM,
      CPP_FLAGS: v.mimalloc ? "" : "-D_LIBCXX_ENABLE_ASSERTIONS=1",
      ENABLE_SANITIZERS: v.sanitizers ?? "",
      // Debug builds break the heap down by type, except under ASAN.
      ENABLE_MALLOC_HEAP_BREAKDOWN: variant === "debug" ? "ON" : "OFF",
    }),
  },
  {
    // clang-cl --target + an xwin-downloaded MSVC CRT and Windows SDK + lld-link. windows-release.ps1 remains for
    // building on a real Windows machine.
    name: "windows",
    label: arch => `bun-webkit-windows-${arch}`,
    dockerfile: "Dockerfile.windows",
    packageOS: "windows",
    lanes: {
      // ASAN is x64 only: LLVM ships no Windows ARM64 ASAN runtime. The sanitizer runtime (import lib, /MT runtime
      // thunk, DLL) comes from the compiler-rt-windows-* release tag.
      amd64: ["release", "lto", "debug", "asan"],
      // No arm64 lto: LLVM 21's CodeView emitter has no register mapping for ARM64 NEON quad-register tuples
      // ("LLVM ERROR: unknown codeview register Q22_Q23_Q24_Q25") and the LTO codegen allocates values into them.
      arm64: ["release", "debug"],
    },
    lto: LTO_WINDOWS,
    image: () => "windows",
    args: (arch, v) => ({
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
    name: "freebsd",
    label: arch => `bun-webkit-freebsd-${arch}`,
    dockerfile: "Dockerfile.freebsd",
    packageOS: "freebsd",
    lanes: { amd64: NO_ASAN, arm64: NO_ASAN },
    image: arch => `freebsd-${arch}`,
    args: arch => ({
      FREEBSD_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      FREEBSD_VERSION: "14.3",
      MARCH_FLAG: arch === "arm64" ? `${ARMV8} -mtune=ampere1` : NEHALEM,
    }),
  },
  {
    // The NDK, which only ships linux-x86_64 prebuilts.
    name: "android",
    label: arch => `bun-webkit-linux-${arch}-android`,
    dockerfile: "Dockerfile.android",
    packageOS: "android",
    lanes: { arm64: NO_ASAN, amd64: NO_ASAN },
    image: () => "android",
    args: arch => ({
      ANDROID_ARCH: arch === "arm64" ? "aarch64" : "x86_64",
      ANDROID_API: "28",
      MARCH_FLAG: arch === "arm64" ? `${ARMV8} -mtune=cortex-a78` : NEHALEM,
    }),
  },
];

// A toolchain image is tagged with a hash of what goes into it: the Dockerfile up to the end of its `base` stage and
// the files that stage copies in. Changing either makes a new tag, which the `image` job finds missing and builds;
// changing anything else (the ICU or WebKit stages, the sources) leaves it alone.
function imageRef(platform, arch) {
  const dockerfile = readFileSync(join(root, platform.dockerfile), "utf8");
  const base = /^FROM\s.*\sAS\s+base\s*$/im.exec(dockerfile);
  if (!base) throw new Error(`${platform.dockerfile} has no \`base\` stage`);
  const next = /^FROM\s/m.exec(dockerfile.slice(base.index + base[0].length));
  const hash = createHash("sha256").update(next ? dockerfile.slice(0, base.index + base[0].length + next.index) : dockerfile);
  const add = path => {
    if (!statSync(join(root, path)).isDirectory()) return hash.update(path).update(readFileSync(join(root, path)));
    for (const entry of readdirSync(join(root, path)).sort()) add(join(path, entry));
  };
  for (const input of platform.imageInputs ?? []) add(input);
  return `${REGISTRY}:${platform.image(arch)}-${hash.digest("hex").slice(0, 16)}`;
}

const lanes = platforms.flatMap(platform =>
  Object.entries(platform.lanes).flatMap(([arch, names]) =>
    names.map(variant => {
      const v = variants[variant];
      const buildType = platform.buildType?.(v) ?? v.buildType;
      return {
        label: platform.label(arch) + (variant === "release" ? "" : `-${variant}`),
        runner: BUILDER,
        dockerfile: platform.dockerfile,
        package_os: platform.packageOS,
        package_cpu: arch === "arm64" ? "arm64" : "x64",
        test: platform.tested?.includes(variant) ?? false,
        test_runner: TESTERS[arch],
        image: imageRef(platform, arch),
        build_args: {
          WEBKIT_RELEASE_TYPE: buildType,
          LTO_FLAG: v.lto ? (platform.lto ?? LTO) : "",
          USE_MIMALLOC: v.mimalloc ? "ON" : "OFF",
          USE_EXTERNAL_MIMALLOC: v.mimalloc ? "ON" : "OFF",
          ...platform.args(arch, v, variant),
        },
      };
    }),
  ),
);

const labels = lanes.map(lane => lane.label);
const duplicate = labels.find((label, i) => labels.indexOf(label) !== i);
if (duplicate) throw new Error(`two lanes are called ${duplicate}`);

// The toolchain images the lanes start from. One is built by the first lane that uses it: the `base` stage does not
// read the lane's build arguments, only the ones that say which toolchain it is (MACOS_ARCH, FREEBSD_ARCH, ...).
const images = [...new Map(lanes.toReversed().map(lane => [lane.image, lane])).values()].toReversed().map(lane => ({
  name: lane.image.slice(REGISTRY.length + 1, lane.image.lastIndexOf("-")),
  image: lane.image,
  runner: IMAGE_BUILDER,
  lane,
}));

function docker(lane, rest, dryRun) {
  const argv = ["buildx", "build", "-f", lane.dockerfile, "--platform", "linux/amd64", "--progress=plain"];
  for (const [key, value] of Object.entries(lane.build_args)) argv.push("--build-arg", `${key}=${value}`);
  argv.push(...rest, ".");
  if (dryRun) return console.log(JSON.stringify(["docker", ...argv]));
  console.error(`docker ${argv.map(arg => (/^[\w./:=,-]+$/.test(arg) ? arg : `'${arg}'`)).join(" ")}`);
  const { status, error } = spawnSync("docker", argv, { cwd: root, stdio: "inherit" });
  if (error) throw error;
  process.exit(status ?? 1);
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
  );
} else if (command === "image") {
  const image = images.find(image => image.name === args[0]) ?? fail(`no image is called ${args[0]}: ${images.map(image => image.name).join(", ")}`);
  if (!args.includes("--push")) fail("image takes --push");
  // --provenance=false: a plain image manifest, not an index with an attestation hanging off it.
  docker(image.lane, ["--target=base", "--tag", image.image, "--provenance=false", "--push"], args.includes("--dry-run"));
} else if (command === "plan") {
  const matrix = include => JSON.stringify({ include });
  const build = ({ label, runner, image, package_os, package_cpu }) => ({ label, runner, image, package_os, package_cpu });
  console.log(`build=${matrix(lanes.filter(lane => !lane.test).map(build))}`);
  console.log(`build_tested=${matrix(lanes.filter(lane => lane.test).map(build))}`);
  console.log(`test=${matrix(lanes.filter(lane => lane.test).map(lane => ({ label: lane.label, runner: lane.test_runner })))}`);
  console.log(`labels=${JSON.stringify(labels)}`);
  // Only the toolchain images that are not in the registry yet get an `image` job: normally none.
  const missing = images.filter(({ image }) => {
    const there = spawnSync("docker", ["manifest", "inspect", image], { stdio: "ignore" }).status === 0;
    console.error(`${image} ${there ? "is there" : "is missing, to be built"}`);
    return !there;
  });
  console.log(`images=${matrix(missing.map(({ name, image, runner }) => ({ name, image, runner })))}`);
  console.log(`build_images=${missing.length > 0}`);
} else if (command === "--json") {
  console.log(JSON.stringify(lanes, null, 2));
} else if (command === undefined) {
  for (const lane of lanes) {
    console.log(`${lane.label.padEnd(42)} ${lane.runner.padEnd(15)} ${lane.dockerfile.padEnd(19)} ${lane.build_args.WEBKIT_RELEASE_TYPE}${lane.test ? "  (tested)" : ""}`);
  }
  console.log(`${lanes.length} lanes`);
  for (const { image, runner } of images) console.log(`${image}  ${runner}`);
  console.log(`${images.length} toolchain images`);
} else {
  fail(`unknown command ${command}; see the top of ${fileURLToPath(import.meta.url)}`);
}
