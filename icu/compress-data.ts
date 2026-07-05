#!/usr/bin/env node
// Per-item zstd compression of an ICU common-data package.
//
// Reads items from an ICU CmnD package using ICU's own `icupkg` (no manual
// parsing of the input), compresses each as an individual zstd frame with a
// shared trained dictionary, and writes a new package. Items matching --skip
// globs stay uncompressed (those that would be too expensive to decompress on
// first use — see keep-raw.txt).
//
// The output package uses the compact TOC ("CmnD" formatVersion 2, read by
// icu/ucmndata-toc.patch): item bodies and their DataHeaders are unchanged,
// but the 8-byte-per-entry {nameOffset,dataOffset} table + full-name pool is
// replaced by a directory table, a global 16-way front-coded pool of unique
// basenames, a u16 name id per entry, and a sentinel-terminated offset
// column. A stock ICU rejects the package instead of misreading it, so
// `icupkg -l` cannot be used on the OUTPUT; verifyPackageV2() below replays
// the reader's exact lookup algorithm over every item (and misses) instead.
//
// Output is a libicudata.a containing:
//   icudt<NN>_dat           the repacked package
//   bun_icu_zstd_dict       the trained dictionary
//   bun_icu_zstd_dict_size  u32 dict length
//
// The runtime hook lives in Bun (bun_icu_maybe_decompress); ICU's udata.cpp
// calls it via a weak extern (see udata-decompress-hook.patch).
//
// Node stdlib only; shells out to `icupkg` and `zstd`. Written for Node's
// native type stripping (erasable annotations only).

import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";
import { spawnSync, type SpawnSyncReturns } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { parseArgs } from "node:util";
import { ResBundle, dumpResolved, loadPool, readSv2, type Pool } from "./resbundle.ts";
import { assertExpectedRewrites, dropAfterRewrite, expectedDump, rewriteItem } from "./rewrite-items.ts";
import { fcResolve, foldRefs, rebuildPoolItem, type FCEntry } from "./frontcode.ts";

const args = parseArgs({
  allowPositionals: true,
  options: {
    skip: { type: "string", default: "" },
    icupkg: { type: "string", default: "icupkg" },
    level: { type: "string", default: "19" },
    "dict-size": { type: "string", default: String(128 * 1024) },
    cc: { type: "string", default: process.env.CC || "cc" },
    // Archiver. llvm-ar takes the same "rcs out members" argv shape as binutils ar
    // and handles COFF members, so the Windows cross build passes --ar llvm-ar.
    ar: { type: "string", default: "ar" },
    // The traced hot set of pool strings that stay verbatim (never
    // materialized): icu/pin-strings.txt. "none" only for regenerating the
    // trace itself.
    pins: { type: "string", default: "" },
    // Directory to also write the final .dat + trained dictionary into, for
    // out-of-band verification (the Windows image runs icu/test-package.cpp
    // against them with a HOST-built patched ICU, since it cannot execute its
    // cross-compiled artifacts).
    "emit-dat": { type: "string", default: "" },
    // Object format for the embedded-data assembly. "elf" (default) matches the
    // Linux/musl artifacts; "coff" is used by the Windows cross build (no ELF-only
    // `.type` directives, data goes in `.rdata`, and --cc should be a clang
    // invocation with `--target=<arch>-pc-windows-msvc`).
    "obj-format": { type: "string", default: "elf" },
  },
});
const [inDat, outA] = args.positionals;
if (!inDat || !outA)
  die("usage: node compress-data.ts <in.dat> <out.a> [--skip file] [--icupkg path] [--level N] [--dict-size N] [--cc CC] [--ar AR] [--obj-format elf|coff]");
const ZSTD_LEVEL: number = Number(args.values.level);
const DICT_SIZE: number = Number(args.values["dict-size"]);
const MIN_COMPRESS_BYTES = 64;
const MIN_SAVINGS_BYTES = 4;
const ICUPKG: string = args.values.icupkg;
// --cc may be a multi-word command ("clang --target=x86_64-pc-windows-msvc"); it is split on whitespace when invoked.
const CC: string = args.values.cc;
const AR: string = args.values.ar;
const OBJ_FORMAT: string = args.values["obj-format"];
if (OBJ_FORMAT !== "elf" && OBJ_FORMAT !== "coff") die(`--obj-format must be "elf" or "coff", got "${OBJ_FORMAT}"`);
const SKIP_FILE: string = args.values.skip;
const EMIT_DAT_DIR: string = args.values["emit-dat"];
const PIN_FILE: string = args.values.pins || new URL("./pin-strings.txt", import.meta.url).pathname;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

interface Item {
  /** Bare name as `icupkg -l` reports it, e.g. "curr/de.res". */
  bare: string;
  /** Body to write: either the original bytes (kept raw) or a zstd frame. */
  body: Buffer;
}

/** Verbatim package header — copied byte-for-byte to the output. */
interface Header {
  bytes: Buffer;
  /** TOC name prefix, e.g. "icudt75l" — every TOC entry is "<prefix>/<bare>". */
  tocPrefix: string;
  /** Linker symbol stem, e.g. "icudt75" — what genccode/ICU emit. */
  pkg: string;
}

// ---------------------------------------------------------------------------
// Read side — delegated to ICU's own `icupkg`
// ---------------------------------------------------------------------------

/** List item names (bare, without the "icudtNNl/" TOC prefix), sorted as stored. */
function listItems(dat: string, icupkg: string): string[] {
  const r: SpawnSyncReturns<string> = spawnSync(icupkg, ["-l", dat], { encoding: "utf8" });
  if (r.status !== 0) die(`icupkg -l failed: ${r.stderr}`);
  return r.stdout.split("\n").map((s) => s.trim()).filter(Boolean);
}

/** Extract every item to <dir>/<bare-name> using ICU's own unpacker. */
function extractItems(dat: string, dir: string, icupkg: string): void {
  mkdirSync(dir, { recursive: true });
  run([icupkg, "-x", "*", "-d", dir, dat]);
}

/**
 * Copy the input package's DataHeader verbatim. This is the only place we
 * touch the input file directly: ICU's header format (ucmndata.h DataHeader)
 * is `[u16 headerSize][u8 0xda][u8 0x27][UDataInfo …][copyright pad]` —
 * we read `headerSize` and copy that many bytes unchanged.
 */
function readHeader(dat: string): Header {
  const raw: Buffer = readFileSync(dat);
  const headerSize: number = raw.readUInt16LE(0);
  if (raw[2] !== 0xda || raw[3] !== 0x27) die(`${dat}: not an ICU data file (no 0xda27 magic)`);
  if (raw.toString("latin1", 12, 16) !== "CmnD") die(`${dat}: not a CmnD package`);
  // UDataInfo.formatVersion[0] (byte 16). The input must be a stock v1
  // package; the output sets it to 2 (see writePackageV2).
  if (raw[V2_FORMAT_VERSION_BYTE] !== 1) die(`${dat}: input formatVersion ${raw[V2_FORMAT_VERSION_BYTE]}, expected 1`);
  // First TOC name gives the prefix: header | u32 count | {u32,u32}[count] | "<prefix>/..."\0
  const count: number = raw.readUInt32LE(headerSize);
  const firstName: number = headerSize + raw.readUInt32LE(headerSize + 4);
  const slash: number = raw.indexOf(0x2f, firstName);
  const tocPrefix: string = raw.toString("latin1", firstName, slash);
  if (!/^icudt\d+[lb]$/.test(tocPrefix)) die(`unexpected TOC prefix '${tocPrefix}' (count=${count})`);
  return {
    bytes: Buffer.from(raw.subarray(0, headerSize)),
    tocPrefix,
    pkg: tocPrefix.replace(/[lb]$/, ""),
  };
}

// ---------------------------------------------------------------------------
// keep-raw.txt glob matching
// ---------------------------------------------------------------------------

function loadSkipGlobs(file: string): RegExp[] {
  if (!file) return [];
  return readFileSync(file, "utf8")
    .split("\n")
    .map((l) => l.replace(/#.*$/, "").trim())
    .filter(Boolean)
    .map(globToRegExp);
}

/**
 * icu/pin-strings.txt: `<tree>\t<JSON string>` per line — the pool strings
 * the startup / first-Intl trace reads, kept verbatim (never materialized).
 */
function loadPinnedTexts(file: string): Map<string, Set<string>> {
  const out = new Map<string, Set<string>>();
  if (file === "none") return out;
  let text: string;
  try {
    text = readFileSync(file, "utf8");
  } catch {
    die(`pin list ${file} is missing (pass --pins none only to regenerate the trace)`);
  }
  for (const line of text!.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const tab = line.indexOf("\t");
    if (tab < 0) die(`pin list: malformed line ${JSON.stringify(line)}`);
    const tree = line.slice(0, tab);
    if (!out.has(tree)) out.set(tree, new Set());
    out.get(tree)!.add(JSON.parse(line.slice(tab + 1)));
  }
  return out;
}

/** keep-raw.txt globs use only `*` (single path segment). */
function globToRegExp(glob: string): RegExp {
  const re = glob.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*");
  return new RegExp(`^${re}$`);
}

// ---------------------------------------------------------------------------
// Compression — zstd CLI
// ---------------------------------------------------------------------------

function trainDict(samplesDir: string, out: string, size: number): void {
  // --train-cover (exhaustive segment search) yields a better dict than the
  // default fastcover for this corpus — slower to train, build-time only.
  run(["zstd", "-q", "--train", "--train-cover", "-r", samplesDir, "-o", out, `--maxdict=${size}`]);
}

/** Compress one file with the shared dict. Reads from disk so the frame
 *  header carries the content size (zstd omits it for stdin). --no-check
 *  drops the per-frame XXH64 (data lives in .rodata); --no-dictID drops the
 *  per-frame dict identifier (we have exactly one). */
function compressFile(path: string, dict: string, level: number, tmpOut: string): Buffer {
  run(["zstd", "-q", "-f", "--no-check", "--no-dictID", `-${level}`, "-D", dict, path, "-o", tmpOut]);
  return readFileSync(tmpOut);
}

// ---------------------------------------------------------------------------
// Write side — the only hand-rolled binary code.
//
// ICU's CmnD package layout after the DataHeader (ucmndata.h UDataOffsetTOC):
//
//   u32  count
//   { u32 nameOffset; u32 dataOffset; }[count]   // offsets relative to TOC start
//   char names[]                                 // NUL-terminated, in TOC order
//   item bodies[]                                // each 16-byte aligned
//
// This v1 writer exists only for assertRoundTrip(): reproducing the INPUT
// byte-for-byte proves the offset/padding math this file relies on. The
// OUTPUT package uses the compact v2 TOC (writePackageV2 / verifyPackageV2).
// ---------------------------------------------------------------------------

function writePackage(header: Header, items: readonly Item[]): Buffer {
  const tocStart: number = header.bytes.length;
  const tocBytes: number = 4 + items.length * 8;

  // Name pool: NUL-terminated "<prefix>/<bare>" in TOC order, padded so items start 16-aligned.
  let nameOff: number = tocBytes;
  const nameOffsets: number[] = [];
  const namePool: Buffer[] = [];
  for (const it of items) {
    nameOffsets.push(nameOff);
    const n = Buffer.from(`${header.tocPrefix}/${it.bare}\0`, "latin1");
    namePool.push(n);
    nameOff += n.length;
  }
  const namesBuf: Buffer = padTo16(Buffer.concat(namePool), tocStart + tocBytes);
  let dataOff: number = tocBytes + namesBuf.length;

  // Item bodies, each 16-aligned relative to file start.
  const dataOffsets: number[] = [];
  const bodies: Buffer[] = [];
  for (const it of items) {
    const pad = (16 - ((tocStart + dataOff) % 16)) % 16;
    if (pad) { bodies.push(Buffer.alloc(pad, 0xaa)); dataOff += pad; }
    dataOffsets.push(dataOff);
    bodies.push(it.body);
    dataOff += it.body.length;
  }

  // Assemble: header | count | (nameOff, dataOff)[] | names | bodies.
  const toc: Buffer = Buffer.alloc(tocBytes);
  toc.writeUInt32LE(items.length, 0);
  for (let i = 0; i < items.length; i++) {
    toc.writeUInt32LE(nameOffsets[i], 4 + i * 8);
    toc.writeUInt32LE(dataOffsets[i], 8 + i * 8);
  }
  return Buffer.concat([header.bytes, toc, namesBuf, ...bodies]);
}

function padTo16(buf: Buffer, absoluteStart: number): Buffer {
  const pad = (16 - ((absoluteStart + buf.length) % 16)) % 16;
  return pad ? Buffer.concat([buf, Buffer.alloc(pad, 0xaa)]) : buf;
}

/** Prove writePackage is exact for this input: rebuild with raw bodies and
 *  require byte-identity with the original package. */
function assertRoundTrip(inDat: string, header: Header, names: readonly string[], itemsDir: string): void {
  const original: Buffer = readFileSync(inDat);
  const raw: Item[] = names.map((bare): Item => ({ bare, body: readFileSync(join(itemsDir, bare)) }));
  const rebuilt: Buffer = writePackage(header, raw);
  if (Buffer.compare(original, rebuilt) !== 0) {
    const at = firstDiff(original, rebuilt);
    die(
      `round-trip FAILED: writePackage(raw items) != input ` +
      `(sizes ${original.length}/${rebuilt.length}, first diff at byte ${at}). ` +
      `UDataOffsetTOC layout assumption is wrong for this ICU package.`
    );
  }
  console.error(`[icu-compress] round-trip OK: writePackage reproduces input exactly (${original.length} bytes)`);
}

function firstDiff(a: Buffer, b: Buffer): number {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return n;
}

// ---------------------------------------------------------------------------
// Compact TOC ("CmnD" formatVersion 2) — the reader is icu/ucmndata-toc.patch.
//
// Layout after the (verbatim, formatVersion-bumped) DataHeader; every offset
// is relative to the TOC start, exactly like formatVersion 1:
//
//   u32[12] header: count, treeCount, nameCount, bucketCount, maxNameLength,
//                   dataOffsetsOff, nameIdsOff, treesOff, bucketDirOff, 0,0,0
//   u32 dataOffsets[count+1]   ([count] is a sentinel; length = next - this)
//   u16 nameIds[count]         (ascending within each tree)
//   { u32 dirNameOffset; u32 firstEntry; u32 entryCount; } trees[treeCount]
//   u32 bucketDir[bucketCount] (start offset of each front-coded block)
//   name pool: per block, head\0 then up to 15 x { u8 lcp; suffix\0 };
//              then the tree directory strings ("icudt75l/", "icudt75l/coll/")
//   item bodies, each 16-aligned, in (tree, basename) order
// ---------------------------------------------------------------------------

const V2_HEADER_WORDS = 12;
const V2_BLOCK = 16;
/** Must match CMN2_NAME_BUFFER_SIZE in icu/ucmndata-toc.patch. */
const V2_NAME_BUFFER = 64;
/** DataHeader byte offset of UDataInfo.formatVersion[0]: u16 headerSize,
 *  u8 magic1, u8 magic2, then UDataInfo{u16,u16,u8 x4,u8 dataFormat[4]}. */
const V2_FORMAT_VERSION_BYTE = 16;

interface V2Entry { dir: string; base: string; item: Item; }

/** "coll/zh.res" -> dir "icudt75l/coll/", base "zh.res". */
function splitV2(header: Header, items: readonly Item[]): V2Entry[] {
  return items.map((item): V2Entry => {
    const i = item.bare.lastIndexOf("/");
    return { dir: `${header.tocPrefix}/${i < 0 ? "" : item.bare.slice(0, i + 1)}`, base: i < 0 ? item.bare : item.bare.slice(i + 1), item };
  });
}

function lcpLen(a: string, b: string): number {
  const n = Math.min(a.length, b.length);
  let i = 0;
  while (i < n && a[i] === b[i]) i++;
  return i;
}

function align4(n: number): number { return (n + 3) & ~3; }

function writePackageV2(header: Header, items: readonly Item[]): Buffer {
  const split: V2Entry[] = splitV2(header, items);
  for (const s of split)
    for (let i = 0; i < s.base.length; i++)
      if (s.base.charCodeAt(i) <= 0x20 || s.base.charCodeAt(i) > 0x7e) die(`non-ASCII item name: ${s.dir}${s.base}`);

  // Global pool of unique basenames. JS string sort on ASCII == the reader's
  // byte-wise strcmp order.
  const names: string[] = [...new Set(split.map((s) => s.base))].sort();
  const nameId = new Map<string, number>(names.map((n, i) => [n, i]));
  const maxNameLength: number = Math.max(...names.map((n) => n.length));
  if (maxNameLength >= V2_NAME_BUFFER) die(`verify2: basename longer than the reader's ${V2_NAME_BUFFER}-byte buffer`);
  if (names.length > 0xffff) die(`too many distinct basenames for a u16 id: ${names.length}`);

  // Entries sorted by (tree, basename): contiguous per tree, ids ascending.
  const entries: V2Entry[] = split
    .slice()
    .sort((a, b) => (a.dir < b.dir ? -1 : a.dir > b.dir ? 1 : nameId.get(a.base)! - nameId.get(b.base)!));
  const dirs: string[] = [...new Set(entries.map((e) => e.dir))];

  // Name pool: 16-way front-coded blocks, then the tree directory strings.
  const bucketCount: number = Math.ceil(names.length / V2_BLOCK);
  const pool: number[] = [];
  const bucketRel: number[] = [];
  for (let b = 0; b < bucketCount; b++) {
    bucketRel.push(pool.length);
    const end = Math.min(names.length, (b + 1) * V2_BLOCK);
    for (let i = b * V2_BLOCK; i < end; i++) {
      const isHead = i === b * V2_BLOCK;
      const lcp = isHead ? 0 : lcpLen(names[i - 1], names[i]);
      if (!isHead) pool.push(lcp);
      const tail = names[i].slice(isHead ? 0 : lcp);
      for (let k = 0; k < tail.length; k++) pool.push(tail.charCodeAt(k));
      pool.push(0);
    }
  }
  const dirRel = new Map<string, number>();
  for (const d of dirs) {
    dirRel.set(d, pool.length);
    for (let k = 0; k < d.length; k++) pool.push(d.charCodeAt(k));
    pool.push(0);
  }

  // Region offsets, relative to the TOC start.
  const count: number = entries.length;
  const dataOffsetsOff: number = 4 * V2_HEADER_WORDS;
  const nameIdsOff: number = dataOffsetsOff + 4 * (count + 1);
  const treesOff: number = align4(nameIdsOff + 2 * count);
  const bucketDirOff: number = treesOff + 12 * dirs.length;
  const poolOff: number = bucketDirOff + 4 * bucketCount;
  const tocStart: number = header.bytes.length;

  // Item bodies, each 16-aligned relative to the file start.
  let dataOff: number = poolOff + pool.length;
  const dataOffsets: number[] = [];
  const bodies: Buffer[] = [];
  for (const e of entries) {
    const pad = (16 - ((tocStart + dataOff) % 16)) % 16;
    if (pad) { bodies.push(Buffer.alloc(pad, 0xaa)); dataOff += pad; }
    dataOffsets.push(dataOff);
    bodies.push(e.item.body);
    dataOff += e.item.body.length;
  }
  dataOffsets.push(dataOff);

  const toc: Buffer = Buffer.alloc(poolOff + pool.length);
  let o = 0;
  for (const v of [count, dirs.length, names.length, bucketCount, maxNameLength, dataOffsetsOff, nameIdsOff, treesOff, bucketDirOff, V2_BLOCK, 0, 0]) { toc.writeUInt32LE(v, o); o += 4; }
  for (let i = 0; i <= count; i++) toc.writeUInt32LE(dataOffsets[i], dataOffsetsOff + 4 * i);
  for (let i = 0; i < count; i++) toc.writeUInt16LE(nameId.get(entries[i].base)!, nameIdsOff + 2 * i);
  const perDir = new Map<string, number>();
  for (const e of entries) perDir.set(e.dir, (perDir.get(e.dir) ?? 0) + 1);
  let first = 0;
  dirs.forEach((d, t) => {
    const n = perDir.get(d)!;
    toc.writeUInt32LE(poolOff + dirRel.get(d)!, treesOff + 12 * t);
    toc.writeUInt32LE(first, treesOff + 12 * t + 4);
    toc.writeUInt32LE(n, treesOff + 12 * t + 8);
    first += n;
  });
  bucketRel.forEach((r, b) => toc.writeUInt32LE(poolOff + r, bucketDirOff + 4 * b));
  Buffer.from(pool).copy(toc, poolOff);

  // Same DataHeader bytes as the input, with formatVersion[0] bumped to 2 so
  // a stock ICU fails closed (U_INVALID_FORMAT_ERROR) instead of misreading.
  const outHeader: Buffer = Buffer.from(header.bytes);
  outHeader[V2_FORMAT_VERSION_BYTE] = 2;
  return Buffer.concat([outHeader, toc, ...bodies]);
}

/** Replay the reader's exact lookup algorithm (icu/ucmndata-toc.patch) for
 *  every item name, plus deliberate misses, and require the exact body bytes
 *  back. This replaces the `icupkg -l` check, which cannot read the new TOC. */
function verifyPackageV2(dat: Buffer, header: Header, items: readonly Item[]): void {
  const tocStart: number = header.bytes.length;
  if (dat[V2_FORMAT_VERSION_BYTE] !== 2) die(`verify2: output formatVersion[0] is ${dat[V2_FORMAT_VERSION_BYTE]}, expected 2`);
  const u32 = (rel: number): number => dat.readUInt32LE(tocStart + rel);
  const cstr = (rel: number): string => dat.toString("latin1", tocStart + rel, dat.indexOf(0, tocStart + rel));
  const [count, treeCount, nameCount, bucketCount, maxNameLength, dataOffsetsOff, nameIdsOff, treesOff, bucketDirOff, nameBlockSize] =
    [0, 4, 8, 12, 16, 20, 24, 28, 32, 36].map(u32);
  if (maxNameLength >= V2_NAME_BUFFER) die("verify2: maxNameLength does not fit the reader's buffer");
  if (count !== items.length) die(`verify2: item count ${count} != ${items.length}`);
  if (nameBlockSize !== V2_BLOCK) die(`verify2: nameBlockSize ${nameBlockSize} != ${V2_BLOCK}`);

  const lookup = (full: string): { off: number; len: number } | null => {
    const slash = full.lastIndexOf("/");
    const dirLen = slash < 0 ? 0 : slash + 1;
    let tree = -1;
    for (let t = 0; t < treeCount; t++) if (cstr(u32(treesOff + 12 * t)) === full.slice(0, dirLen)) { tree = t; break; }
    if (tree < 0) return null;
    const base = full.slice(dirLen);
    let lo = 0, hi = bucketCount;
    while (lo < hi) { const m = (lo + hi) >> 1; if (cstr(u32(bucketDirOff + 4 * m)) <= base) lo = m + 1; else hi = m; }
    const b = lo - 1;
    if (b < 0) return null;
    let p = tocStart + u32(bucketDirOff + 4 * b);
    let cur = dat.toString("latin1", p, dat.indexOf(0, p));
    p += cur.length + 1;
    let nameIdFound = -1;
    const inBlock = Math.min(V2_BLOCK, nameCount - b * V2_BLOCK);
    for (let k = 0; k < inBlock; k++) {
      if (k > 0) {
        const lcp = dat[p++];
        const suffix = dat.toString("latin1", p, dat.indexOf(0, p));
        p += suffix.length + 1;
        cur = cur.slice(0, lcp) + suffix;
      }
      if (cur === base) { nameIdFound = b * V2_BLOCK + k; break; }
      if (cur > base) break;
    }
    if (nameIdFound < 0) return null;
    let start = u32(treesOff + 12 * tree + 4);
    let limit = start + u32(treesOff + 12 * tree + 8);
    while (start < limit) {
      const i = (start + limit) >> 1;
      const id = dat.readUInt16LE(tocStart + nameIdsOff + 2 * i);
      if (id < nameIdFound) start = i + 1;
      else if (id > nameIdFound) limit = i;
      else return { off: u32(dataOffsetsOff + 4 * i), len: u32(dataOffsetsOff + 4 * (i + 1)) - u32(dataOffsetsOff + 4 * i) };
    }
    return null;
  };

  let checked = 0;
  for (const it of items) {
    const full = `${header.tocPrefix}/${it.bare}`;
    const r = lookup(full);
    if (!r) die(`verify2: '${full}' not found`);
    // The reader's length spans up to the next item's (16-aligned) start.
    if (r.len < it.body.length) die(`verify2: '${full}' length ${r.len} < ${it.body.length}`);
    if (!it.body.equals(dat.subarray(tocStart + r.off, tocStart + r.off + it.body.length))) die(`verify2: '${full}' body mismatch`);
    checked++;
  }
  for (const miss of [`${header.tocPrefix}/nope.res`, `${header.tocPrefix}/zzz/root.res`, "no-slash", `${header.tocPrefix}/`, `${header.tocPrefix}/coll/`])
    if (lookup(miss) !== null) die(`verify2: false positive for '${miss}'`);
  if (checked !== count) die(`verify2: checked ${checked} != count ${count}`);
  console.error(`[icu-compress] verify2 OK: ${checked}/${count} lookups return the exact item bodies; first item at TOC+${u32(dataOffsetsOff)}`);
}

// ---------------------------------------------------------------------------
// Archive — embed package + dict as read-only-data symbols
// ---------------------------------------------------------------------------

function emitArchive(datPath: string, dictPath: string, pkg: string, outA: string, cc: string, work: string, fc: { slots: number; arenaUnits: number } | null): void {
  // ELF: .rodata + `.type ..., @object` (proper object type/size in the symbol
  // table). COFF: the conventional read-only section is .rdata, and `.type` is
  // an ELF-only directive — COFF symbols carry no object type.
  const coff: boolean = OBJ_FORMAT === "coff";
  const section: string = coff ? ".section .rdata" : ".section .rodata";
  const type = (sym: string): string[] => (coff ? [] : [`.type ${sym}, @object`]);
  const asm = join(work, "icudt.S");
  writeFileSync(asm, [
    section, ".balign 16",
    `.global ${pkg}_dat`, ...type(`${pkg}_dat`), `${pkg}_dat:`, `.incbin "${datPath}"`,
    "",
    ".balign 16", ".global bun_icu_zstd_dict", ...type("bun_icu_zstd_dict"),
    "bun_icu_zstd_dict:", `.incbin "${dictPath}"`, ".Ldict_end:",
    "",
    ".balign 4", ".global bun_icu_zstd_dict_size", ...type("bun_icu_zstd_dict_size"),
    "bun_icu_zstd_dict_size:", ".long .Ldict_end - bun_icu_zstd_dict", "",
    // Side tables for the front-coded pool strings (uresdata-frontcode.patch):
    // zero-initialized common symbols, sized exactly by the packer, costing
    // no file bytes. `.comm` places them in .bss for both ELF and COFF, but
    // the alignment operand is bytes on ELF and log2(bytes) on COFF.
    ...(fc
      ? [
          `.comm bun_icu_fc_slots, ${fc.slots * 8}, ${coff ? 4 : 16}`,
          `.comm bun_icu_fc_arena, ${fc.arenaUnits * 2}, ${coff ? 4 : 16}`,
          // their exact sizes, so the reader can reject a package whose
          // header claims a slice this archive never allocated
          ".balign 4", ".global bun_icu_fc_slot_count", ...type("bun_icu_fc_slot_count"),
          `bun_icu_fc_slot_count: .long ${fc.slots}`,
          ".global bun_icu_fc_arena_unit_count", ...type("bun_icu_fc_arena_unit_count"),
          `bun_icu_fc_arena_unit_count: .long ${fc.arenaUnits}`,
          "",
        ]
      : []),
  ].join("\n"));
  const obj = join(work, `${pkg}l_dat.o`);
  run([...cc.split(/\s+/), "-c", asm, "-o", obj]);
  rmSync(outA, { force: true });
  run([AR, "rcs", outA, obj]);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

function main(): void {
  const work = mkdtempSync(join(tmpdir(), "icu-compress-"));
  process.on("exit", () => rmSync(work, { recursive: true, force: true }));

  const header: Header = readHeader(inDat);
  const allNames: string[] = listItems(inDat, ICUPKG);
  // The Dockerfiles' coarse category filter (converters/translit/rbnf/
  // stringprep/confusables/unames) must already have run on this input; its
  // grep fails the build when it matches nothing, and this guards the other
  // direction so the two cannot silently drift apart.
  const leftover = allNames.filter((n) => /\.(cnv|spp|cfu)$|^cnvalias\.icu$|^translit\/|^rbnf\/|^unames\.icu$/.test(n));
  if (leftover.length) die(`input still contains ${leftover.length} converter/translit/rbnf items (was icupkg -r skipped?): ${leftover.slice(0, 4).join(", ")}...`);
  const itemsDir: string = join(work, "items");
  extractItems(inDat, itemsDir, ICUPKG);

  // Round-trip invariant: writePackage on the raw items must reproduce the
  // input byte-for-byte. If this fails, our offset/padding math doesn't match
  // ICU's UDataOffsetTOC for this package and the build must not proceed.
  assertRoundTrip(inDat, header, allNames, itemsDir);

  // Structural rewrites (rewrite-items.ts): dead-data drops, collation rule
  // stubs, within-bundle dedup. Each rewritten bundle is proven equal to the
  // original modulo the intended edits at build time by rewriteItem itself.
  const poolCache = new Map<string, Pool | null>();
  const poolFor = (bare: string): Pool | null => {
    const tree = bare.includes("/") ? bare.slice(0, bare.lastIndexOf("/")) : "";
    if (!poolCache.has(tree)) {
      const p = join(itemsDir, tree, "pool.res");
      try {
        poolCache.set(tree, loadPool(readFileSync(p)));
      } catch (e) {
        // Only "this tree has no pool bundle" is benign; a corrupt pool must fail.
        if ((e as NodeJS.ErrnoException).code !== "ENOENT") die(`loading ${p}: ${(e as Error).message}`);
        poolCache.set(tree, null);
      }
    }
    return poolCache.get(tree)!;
  };
  let rewritten = 0;
  const rewriteNotes = new Map<string, readonly string[]>();
  const removedBrkRefs = new Set<string>();
  for (const bare of allNames) {
    const p = join(itemsDir, bare);
    const orig = readFileSync(p);
    let r: ReturnType<typeof rewriteItem>;
    try {
      r = rewriteItem(bare, orig, poolFor(bare));
      if (!r) continue;
      // Prove the rewrite: the output's resolved resource tree must equal the
      // input's with exactly the intended edits (see expectedDump).
      const want = JSON.stringify(expectedDump(bare, r.beforeDump));
      const got = JSON.stringify(dumpResolved(new ResBundle(r.out, poolFor(bare))));
      if (want !== got) throw new Error(`rewrite verification failed (${r.notes.join(",")})`);
    } catch (e) {
      die(`rewriting ${bare}: ${(e as Error).message}`);
    }
    writeFileSync(p, r!.out);
    rewritten++;
    rewriteNotes.set(bare, r!.notes);
    for (const f of r!.removedBrkRefs) removedBrkRefs.add(f);
  }
  // Guard against an ICU upgrade renaming a key out from under a rewrite (the
  // rewrite would no-op and verify vacuously), then drop the brkitr rule files
  // whose references were provably removed. icupkg cannot drop those (it
  // enforces referential integrity on the unrewritten input).
  try {
    assertExpectedRewrites(rewriteNotes, new Set(allNames));
  } catch (e) {
    die((e as Error).message);
  }
  let dropped!: Set<string>;
  try {
    dropped = dropAfterRewrite(allNames, removedBrkRefs);
  } catch (e) {
    die((e as Error).message);
  }
  const names: string[] = allNames.filter((n) => !dropped.has(n));
  console.error(`[icu-compress] rewrote ${rewritten} bundles, dropped ${dropped.size} unreferenced brkitr rule files`);

  // ---------------------------------------------------------------------------
  // Front-code the pool bundles (formatVersion 4; icu/frontcode.ts, read by
  // icu/uresdata-frontcode.patch): each pooled tree's shared value strings
  // become a verbatim (pinned + tightly-addressed) region plus a front-coded
  // region, and every member's pool references are renumbered. Proven here,
  // for every member: the fully-resolved view is unchanged, and every
  // front-coded string round-trips through the reference decoder.
  const pinned = loadPinnedTexts(PIN_FILE);
  const treeOf = (bare: string): string => (bare.includes("/") ? bare.slice(0, bare.lastIndexOf("/") + 1) : "");
  let fcSlots = 0, fcArenaUnits = 0, fcSaved = 0;
  for (const poolName of names.filter((n) => n === "pool.res" || n.endsWith("/pool.res"))) {
    const tree = treeOf(poolName);
    const poolPath = join(itemsDir, poolName);
    const poolBuf = readFileSync(poolPath);
    const oldPool = loadPool(poolBuf);
    const members = names.filter((n) => n !== poolName && n.endsWith(".res") && treeOf(n) === tree);
    const refs = new Map<number, string>();
    const entries = new Map<string, FCEntry>();
    for (const m of members) {
      const b = new ResBundle(readFileSync(join(itemsDir, m)), oldPool);
      if (!(b.psil > 0)) continue;
      const mr = new Map<number, string>(), m16 = new Set<number>();
      b.collectPoolRefs(mr, m16);
      foldRefs(entries, mr, m16, b.psil, b.psil16);
      for (const [k, v] of mr) refs.set(k, v);
    }
    if (!entries.size) continue;
    const pins = pinned.get(tree) ?? new Set<string>();
    // Pinned strings that no longer exist (an ICU upgrade, or a different
    // ICU major on this platform) only cost coverage; test-package's
    // startup-phase materialization gate is what enforces freshness.
    const missing = [...pins].filter((t) => !entries.has(t));
    if (missing.length) console.error(`[icu-compress] warning: ${missing.length}/${pins.size} pinned strings not present in tree "${tree}" of this ICU version`);
    const fp = rebuildPoolItem(poolBuf, refs, entries, pins, fcSlots, fcArenaUnits);
    // A tree whose every referenced string is pinned or constrained has
    // nothing to front-code; it must stay formatVersion 3 (an fv4 pool with
    // no front-coded region is invalid by design — the reader fails closed).
    if (fp.stringCount === 0) {
      console.error(`[icu-compress] ${poolName}: nothing to front-code, left as-is`);
      continue;
    }
    fcSlots += fp.blockCount;
    fcArenaUnits += fp.arenaUnits;
    const newPool = loadPool(fp.out);
    for (const [o, n] of fp.remap) {
      const got = n < fp.verbatimLimit ? readSv2(newPool.p16, n)[0] : fcResolve(newPool.p16, fp.verbatimLimit, n);
      if (got !== refs.get(o)) die(`${poolName}: front-coded string round-trip failed`);
    }
    for (const m of members) {
      const src = readFileSync(join(itemsDir, m));
      const b2 = new ResBundle(src, oldPool);
      if (!(b2.psil > 0)) continue;
      b2.setPoolRemap((off: number) => fp.remap.get(off)!);
      const out: Buffer = b2.serialize();
      const before = JSON.stringify(dumpResolved(new ResBundle(src, oldPool)));
      const after = JSON.stringify(dumpResolved(new ResBundle(out, newPool)));
      if (before !== after) die(`pool renumbering changed the resolved view of ${m}`);
      writeFileSync(join(itemsDir, m), out);
    }
    writeFileSync(poolPath, fp.out);
    fcSaved += fp.savedBytes;
    console.error(`[icu-compress] ${poolName}: ${fp.stringCount} strings front-coded, ${fp.pinnedCount} pinned verbatim, -${fp.savedBytes} B`);
  }
  if (fcSlots) console.error(`[icu-compress] pool front-coding: -${fcSaved} B; side tables ${fcSlots * 8 + fcArenaUnits * 2} B of .bss`);

  const skip: RegExp[] = loadSkipGlobs(SKIP_FILE);
  const keepRaw = (bare: string): boolean => skip.some((r) => r.test(bare));
  // Every keep-raw glob must still match something. A stale glob (after an
  // ICU rename) would silently stop protecting a startup-hot item — the exact
  // regression the zone/ entries exist to prevent.
  const deadGlobs = skip.filter((r) => !names.some((n) => r.test(n)));
  if (deadGlobs.length) die(`keep-raw globs match no item: ${deadGlobs.join(" ")}`);

  // Train the dictionary only on items we will actually compress — including
  // kept-raw items wastes dict capacity and slows decode of the rest.
  const trainDir: string = join(work, "to-compress");
  mkdirSync(trainDir);
  for (const bare of names) {
    if (keepRaw(bare)) continue;
    const dst = join(trainDir, bare.replace(/\//g, "_"));
    writeFileSync(dst, readFileSync(join(itemsDir, bare)));
  }
  const dictPath: string = join(work, "dict.zstdict");
  trainDict(trainDir, dictPath, DICT_SIZE);

  const tmpOut: string = join(work, "z.out");
  let kept = 0, comp = 0, rawB = 0, outB = 0;
  const items: Item[] = names.map((bare): Item => {
    const path = join(itemsDir, bare);
    const raw = readFileSync(path);
    rawB += raw.length;
    let body: Buffer = raw;
    if (raw.length >= MIN_COMPRESS_BYTES && !keepRaw(bare)) {
      const z = compressFile(path, dictPath, ZSTD_LEVEL, tmpOut);
      // Only worth a runtime decode if compression actually beat the frame
      // overhead (4-byte magic + a few header bytes). Otherwise keep raw.
      if (z.length + MIN_SAVINGS_BYTES < raw.length) { body = z; comp++; } else kept++;
    } else kept++;
    outB += body.length;
    return { bare, body };
  });

  const pkg: Buffer = writePackageV2(header, items);
  verifyPackageV2(pkg, header, items);
  const outDat: string = join(work, `${header.tocPrefix}.dat`);
  writeFileSync(outDat, pkg);

  emitArchive(outDat, dictPath, header.pkg, outA, CC, work, fcSlots ? { slots: fcSlots, arenaUnits: fcArenaUnits } : null);
  if (EMIT_DAT_DIR) {
    mkdirSync(EMIT_DAT_DIR, { recursive: true });
    writeFileSync(join(EMIT_DAT_DIR, `${header.tocPrefix}.dat`), pkg);
    writeFileSync(join(EMIT_DAT_DIR, "zstd.dict"), readFileSync(dictPath));
  }

  console.error(
    `[icu-compress] ${names.length} items: ${comp} compressed, ${kept} raw  ` +
    `${rawB}→${outB} (${((100 * outB) / rawB).toFixed(0)}%)  ` +
    `pkg ${readFileSync(inDat).length}→${readFileSync(outDat).length} + dict ${readFileSync(dictPath).length}`
  );
}

// ---------------------------------------------------------------------------

function run(cmd: readonly string[]): void {
  const r = spawnSync(cmd[0], cmd.slice(1), { stdio: ["ignore", "ignore", "inherit"] });
  if (r.status !== 0) die(`${cmd.join(" ")} exited ${r.status}`);
}

function die(msg: string): never {
  console.error(`[icu-compress] ${msg}`);
  process.exit(1);
}

main();
