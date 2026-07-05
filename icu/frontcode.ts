// Front-coded pool-bundle string areas ("FCS1", pool .res formatVersion 4).
//
// A formatVersion-3 pool bundle's 16-bit area is one blob of string-v2
// values shared by every bundle of its tree. 55% of those bytes are prefixes
// shared with the lexicographic neighbor — redundancy zstd never sees,
// because the pools ship raw (keep-raw.txt): they are read on the hot path
// of every locale lookup in the tree.
//
// This module rebuilds a pool's 16-bit area as:
//
//   units [0, V)       "verbatim" strings, stock string-v2 encoding: the
//                      pinned hot set (icu/pin-strings.txt — everything the
//                      startup / first-Intl trace touches), the empty string
//                      at unit 0, and any string too long for the decoder's
//                      scratch buffer. Reads take exactly today's code path.
//   units [V, ...)     the front-coded region, byte-packed:
//                        u32 magic 'FCS1', u32 stringCount, u32 dirCount,
//                        u32 arenaUnits, u32 blobBytes, u32 maxStringBytes,
//                        u32 slotBase, u32 arenaBase   (this pool's slice of
//                          the archive-provided bun_icu_fc_slots/_arena)
//                        u32 blockOff[dirCount]   (byte offset into blob)
//                        u32 arenaOff[dirCount]   (unit offset into the arena)
//                        u8  blob[blobBytes]
//                      Strings sorted by text, blocks of 16. Block head =
//                      varint(byteLen) + WTF-8; entry = varint(lcpBytes) +
//                      varint(tailBytes) + WTF-8 tail (lcpBytes counts WTF-8
//                      bytes shared with the previous entry's WTF-8 form).
//
// A string reference (a string-v2 offset in a member bundle, or a res16
// value < poolStringIndex16Limit) is either a verbatim unit offset (< V) or
// V + id. The reader (icu/uresdata-frontcode.patch) materializes a block's
// 16 strings on first access into a statically-sized side arena and caches
// the block pointer; the pinned set never materializes anything.
//
// Node stdlib only; erasable TypeScript (node --experimental-strip-types).

const FC_MAGIC = 0x46435331; // "FCS1"
export const FC_BLOCK = 16;
/** Strings whose WTF-8 form exceeds this stay verbatim (bounds the decoder's scratch buffer). */
export const FC_MAX_STRING_BYTES = 480;

/** Per referenced text: its lowest old unit offset and the tightest addressing
 * limit any referencing member imposes (poolStringIndexLimit, and
 * poolStringIndex16Limit when referenced through a 16-bit container). */
export interface FCEntry {
  minOld: number;
  lim: number;
}

export interface FCPool {
  /** the new 16-bit area (bytes, even length) */
  area: Buffer;
  /** text -> new unit offset (verbatim) or verbatimLimit+id (cold) */
  newOff: Map<string, number>;
  verbatimLimit: number;
  stringCount: number;
  /** u16 units the reader's arena needs for this pool (16 per block + marker+chars+NUL per string) */
  arenaUnits: number;
  blockCount: number;
}

const vwrite = (out: number[], n: number) => {
  do {
    let b = n & 0x7f;
    n >>>= 7;
    if (n) b |= 0x80;
    out.push(b);
  } while (n);
};

export const wtf8Bytes = (s: string): number[] => {
  const out: number[] = [];
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
    else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
  }
  return out;
};
/**
 * WTF-8 -> UTF-16 code units. Exactly as strict as the C decoder in
 * icu/uresdata-frontcode.patch (which rejects and fails the lookup), so the
 * build-time round-trip proof also proves the C reader accepts every string.
 */
export const wtf8DecodeBytes = (b: Uint8Array | number[], len: number): string => {
  const units: number[] = [];
  let i = 0;
  while (i < len) {
    const c = b[i] as number;
    if (c < 0x80) { units.push(c); i += 1; }
    else if (c < 0xe0) {
      if (i + 2 > len || c < 0xc0) throw new Error("wtf8: bad 2-byte sequence");
      units.push(((c & 0x1f) << 6) | ((b[i + 1] as number) & 0x3f)); i += 2;
    } else {
      if (i + 3 > len || c >= 0xf0) throw new Error("wtf8: bad 3-byte sequence");
      units.push(((c & 0x0f) << 12) | (((b[i + 1] as number) & 0x3f) << 6) | ((b[i + 2] as number) & 0x3f)); i += 3;
    }
  }
  return String.fromCharCode(...units);
};

/** string-v2 unit sequence for a fresh string: marker (if needed) + chars + NUL */
export function sv2Units(text: string): number[] {
  const n = text.length;
  const out: number[] = [];
  const needMarker = n > 0x3ee || (n > 0 && text.charCodeAt(0) >= 0xdc00 && text.charCodeAt(0) <= 0xdfff);
  if (needMarker) {
    if (n < 0x3ef) out.push(0xdc00 | n);
    else if (n <= 0xfffff) out.push(0xdfef + (n >> 16), n & 0xffff);
    else out.push(0xdfff, n >> 16, n & 0xffff);
  }
  for (let i = 0; i < n; i++) out.push(text.charCodeAt(i));
  out.push(0);
  return out;
}

/** The arena unit cost of one materialized cold string: explicit marker + chars + NUL. */
function arenaUnitsFor(text: string): number {
  const n = text.length;
  const marker = n < 0x3ef ? 1 : n <= 0xfffff ? 2 : 3;
  return marker + n + 1;
}

/**
 * Build a formatVersion-4 pool 16-bit area from the referenced string set.
 * `refs`: every referenced old unit offset -> its text.
 * `pinned`: texts that must stay verbatim (the traced hot set).
 */
export function buildFCPool(entries: Map<string, FCEntry>, pinned: Set<string>, slotBase = 0, arenaBase = 0): FCPool {
  // Partition. A text is front-codable only if its tightest referencing
  // member can still address its id (checked below after V is known), it is
  // not pinned, and it fits the decoder's scratch buffer.
  let verbatim: string[] = [];
  let cold: string[] = [];
  // "∅∅∅" is ICU's no-inheritance marker: isNoInheritanceMarker() reads it
  // as raw units, so it must keep genrb's exact (implicit-length) encoding.
  const NO_INHERITANCE = "\u2205\u2205\u2205";
  for (const [text] of entries) {
    if (text === "") continue;
    if (text !== NO_INHERITANCE && !pinned.has(text) && wtf8Bytes(text).length <= FC_MAX_STRING_BYTES) cold.push(text);
    else verbatim.push(text);
  }
  // Constrained strings must stay verbatim if their limit could not cover a
  // front-coded id. Iterate: moving one to verbatim grows V.
  const vsize = (ts: string[]) => ts.reduce((a, t) => a + sv2Units(t).length, 0) + 1;
  for (;;) {
    const bound = vsize(verbatim) + 1 + cold.length; // worst-case highest reference
    const move = cold.filter((t) => entries.get(t)!.lim <= bound);
    if (!move.length) break;
    const mv = new Set(move);
    verbatim = verbatim.concat(move);
    cold = cold.filter((t) => !mv.has(t));
  }
  cold.sort();

  // --- verbatim region: unit 0 is a NUL (offset 0 == the empty string).
  // Tightest addressing limit first (original offset as the tiebreak) so a
  // string a member can only reach through a small poolStringIndex16Limit
  // lands early. This is best-effort placement; the per-string asserts at
  // the end are the guarantee, and they fail the build loudly.
  const vunits: number[] = [0];
  const newOff = new Map<string, number>();
  newOff.set("", 0);
  for (const t of verbatim.sort((a, b) => (entries.get(a)!.lim - entries.get(b)!.lim) || (entries.get(a)!.minOld - entries.get(b)!.minOld))) {
    newOff.set(t, vunits.length);
    vunits.push(...sv2Units(t));
  }
  if (vunits.length & 1) vunits.push(0); // 4-byte-align the FC header
  const V = vunits.length;

  // --- front-coded region
  const dirCount = Math.ceil(cold.length / FC_BLOCK);
  const blockOff: number[] = [];
  const arenaOff: number[] = [];
  const blob: number[] = [];
  let arenaUnits = 0;
  let maxStringBytes = 0;
  for (let i = 0; i < cold.length; i++) {
    const s = cold[i];
    const w = wtf8Bytes(s);
    if (w.length > maxStringBytes) maxStringBytes = w.length;
    if (i % FC_BLOCK === 0) {
      blockOff.push(blob.length);
      arenaOff.push(arenaUnits);
      arenaUnits += FC_BLOCK; // u16 entry offsets at each block's arena base
      vwrite(blob, w.length);
      blob.push(...w);
    } else {
      const prev = cold[i - 1];
      let l = 0;
      const m = Math.min(prev.length, s.length);
      while (l < m && prev.charCodeAt(l) === s.charCodeAt(l)) l++;
      const lcpB = wtf8Bytes(s.slice(0, l)).length;
      vwrite(blob, lcpB);
      vwrite(blob, w.length - lcpB);
      blob.push(...w.slice(lcpB));
    }
    newOff.set(s, V + i);
    arenaUnits += arenaUnitsFor(s);
  }

  // --- assemble
  const hdrBytes = 32 + 8 * dirCount;
  const areaBytes = V * 2 + (cold.length ? hdrBytes + blob.length + ((blob.length & 1) ? 1 : 0) : 0);
  const area = Buffer.alloc(areaBytes);
  vunits.forEach((u, i) => area.writeUInt16LE(u, i * 2));
  if (cold.length) {
    let o = V * 2;
    area.writeUInt32LE(FC_MAGIC, o); o += 4;
    area.writeUInt32LE(cold.length, o); o += 4;
    area.writeUInt32LE(dirCount, o); o += 4;
    area.writeUInt32LE(arenaUnits, o); o += 4;
    area.writeUInt32LE(blob.length, o); o += 4;
    area.writeUInt32LE(maxStringBytes, o); o += 4;
    area.writeUInt32LE(slotBase, o); o += 4;
    area.writeUInt32LE(arenaBase, o); o += 4;
    for (const b of blockOff) { area.writeUInt32LE(b, o); o += 4; }
    for (const a of arenaOff) { area.writeUInt32LE(a, o); o += 4; }
    Buffer.from(blob).copy(area, o);
  }

  for (const [text, e] of entries) {
    const n = newOff.get(text);
    if (n === undefined) throw new Error(`no offset assigned for a referenced string`);
    if (text !== "" && n >= e.lim) throw new Error(`pool string needs offset < ${e.lim} but got ${n}`);
  }
  return { area, newOff, verbatimLimit: V, stringCount: cold.length, arenaUnits, blockCount: dirCount };
}

/**
 * Reference decoder for the front-coded region (mirrors the C reader byte
 * for byte). `off` must be at or above `verbatimLimit`; verbatim offsets are
 * plain string-v2 and are read by the caller (resbundle.ts readSv2).
 */
export function fcResolve(area: Buffer, verbatimLimit: number, off: number): string {
  if (off < verbatimLimit) throw new Error("fcResolve: verbatim offset");
  const H = verbatimLimit * 2;
  if (area.readUInt32LE(H) !== FC_MAGIC) throw new Error("bad FC magic");
  const stringCount = area.readUInt32LE(H + 4);
  const dirCount = area.readUInt32LE(H + 8);
  const id = off - verbatimLimit;
  if (id >= stringCount) throw new Error(`FC id ${id} out of range`);
  const dir = H + 32;
  const blobStart = dir + 8 * dirCount;
  const b = (id / FC_BLOCK) | 0;
  const want = id % FC_BLOCK;
  let p = blobStart + area.readUInt32LE(dir + 4 * b);
  // LEB128, at most 4 bytes (mirrors the C reader's rejection)
  const vread = (): number => { let n = 0, sh = 0; for (;;) { if (sh >= 28) throw new Error("varint too long"); const x = area[p++]; n |= (x & 0x7f) << sh; if (!(x & 0x80)) return n; sh += 7; } };
  // scratch = the current entry's full WTF-8 bytes (exactly what the C decoder keeps)
  let scratch: number[] = [];
  const hl = vread();
  for (let i = 0; i < hl; i++) scratch.push(area[p + i]);
  p += hl;
  for (let k = 1; k <= want; k++) {
    const lcpB = vread(), tailB = vread();
    scratch = scratch.slice(0, lcpB);
    for (let i = 0; i < tailB; i++) scratch.push(area[p + i]);
    p += tailB;
  }
  return wtf8DecodeBytes(scratch, scratch.length);
}

/** A rebuilt formatVersion-4 pool bundle plus its reference remap. */
export interface FCPoolItem {
  out: Buffer;
  remap: Map<number, number>;
  verbatimLimit: number;
  stringCount: number;
  arenaUnits: number;
  blockCount: number;
  /** bytes saved on this item */
  savedBytes: number;
  pinnedCount: number;
}
/** Aggregate per-member pool references into per-text constraint entries. */
export function foldRefs(
  agg: Map<string, FCEntry>,
  refs: Map<number, string>,
  res16: Set<number>,
  psil: number,
  psil16: number,
): void {
  for (const [off, text] of refs) {
    const lim = res16.has(off) ? Math.min(psil, psil16) : psil;
    const e = agg.get(text);
    if (!e) agg.set(text, { minOld: off, lim });
    else {
      if (off < e.minOld) e.minOld = off;
      if (lim < e.lim) e.lim = lim;
    }
  }
}

/**
 * Rebuild a pool bundle (.res, formatVersion 3) with a front-coded string
 * area (formatVersion 4). `refs` = every referenced old unit offset -> text
 * (from every member bundle of the tree); `entries` = per text, its lowest
 * old offset and the tightest addressing limit any referencing member
 * imposes; `pinned` = texts that must stay verbatim.
 */
export function rebuildPoolItem(
  poolBuf: Buffer,
  refs: Map<number, string>,
  entries: Map<string, FCEntry>,
  pinned: Set<string>,
  slotBase = 0,
  arenaBase = 0,
): FCPoolItem {
  const hs = poolBuf.readUInt16LE(0);
  const bd = poolBuf.subarray(hs);
  const il = bd.readUInt32LE(4) & 0xff;
  if (poolBuf[16] !== 3 || il < 8) throw new Error("pool bundle is not formatVersion 3 with 8 indexes");
  const oldIdx: number[] = [];
  for (let i = 0; i < il; i++) oldIdx.push(bd.readInt32LE(4 + 4 * i));
  const keysTop = oldIdx[1], t16Top = oldIdx[6];
  if (oldIdx[2] !== t16Top || oldIdx[3] !== t16Top) throw new Error("pool bundle has a 32-bit resource area");
  const keyBytes = bd.subarray((1 + il) * 4, keysTop * 4);

  const fc = buildFCPool(entries, pinned, slotBase, arenaBase);
  const remap = new Map<number, number>();
  for (const [off, text] of refs) remap.set(off, fc.newOff.get(text)!);
  const NEW_IL = 9;
  const areaPadded = (fc.area.length + 3) & ~3;
  const newKeysTop = 1 + NEW_IL + keyBytes.length / 4;
  const new16Top = newKeysTop + areaPadded / 4;
  const body = Buffer.alloc(new16Top * 4, 0xaa);
  body.writeUInt32LE(bd.readUInt32LE(0), 0); // root resource word
  const w = (i: number, v: number) => body.writeInt32LE(v | 0, 4 + 4 * i);
  w(0, (oldIdx[0] & ~0xff) | NEW_IL);
  w(1, newKeysTop);
  w(2, new16Top);
  w(3, new16Top);
  w(4, oldIdx[4]);
  w(5, oldIdx[5]);
  w(6, new16Top);
  w(7, oldIdx[7]); // pool checksum: members compare this stored word
  w(8, fc.verbatimLimit);
  keyBytes.copy(body, (1 + NEW_IL) * 4);
  fc.area.copy(body, newKeysTop * 4);
  const out = Buffer.alloc(hs + body.length);
  poolBuf.copy(out, 0, 0, hs);
  out[16] = 4; // UDataInfo.formatVersion[0]
  body.copy(out, hs);
  return {
    out,
    remap,
    verbatimLimit: fc.verbatimLimit,
    stringCount: fc.stringCount,
    arenaUnits: fc.arenaUnits,
    blockCount: fc.blockCount,
    savedBytes: poolBuf.length - out.length,
    pinnedCount: [...new Set(refs.values())].filter((t) => t && pinned.has(t)).length,
  };
}
