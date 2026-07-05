// ICU .res (resource bundle) reader/rewriter for formatVersion 2 and 3.
//
// Parses a bundle into a resource graph, applies structural edits, and
// re-serializes a valid bundle:
//   - deleteTableKeys(): remove named entries from a table; everything that
//     becomes unreachable (containers, leaf bodies, 16-bit strings) is
//     reclaimed.
//   - replaceString(): shrink a string-v2 value in place (used to stub the
//     compiled-away collation rule sources).
//   - dedup(): alias byte-identical sibling resources (containers, aliases,
//     intvectors, binaries) to one copy — a documented property of the format
//     ("It is allowed for all resource types to share values", uresdata.h).
//
// The writer keeps every surviving byte where it was, cuts holes where dead
// bytes were, and relocates every offset (32-bit Resource words, 16-bit
// res16 values, string-v2 offsets, indexes[]) across both unit areas. Local
// key characters are never moved (dead key chars are left in place: they are
// small and relocating key offsets is not worth the risk).
//
// Pool bundles (formatVersion 3) are read for key/string resolution but never
// modified; poolStringIndexLimit semantics follow uresdata.cpp res_init().
//
// Node stdlib only; erasable-TypeScript-only (runs under node --experimental-
// strip-types and bun).

import { createHash } from "node:crypto";

// Resource types (URES_*)
const T_STRING = 0, T_BINARY = 1, T_TABLE = 2, T_ALIAS = 3, T_TABLE32 = 4, T_TABLE16 = 5;
const T_STRING_V2 = 6, T_INT = 7, T_ARRAY = 8, T_ARRAY16 = 9, T_INTVECTOR = 14;
const CONTAINERS = new Set([T_TABLE, T_TABLE32, T_TABLE16, T_ARRAY, T_ARRAY16]);
const RES_BOGUS = 0xffffffff;

interface Node {
  res: number;
  type: number;
  off: number;
  area: 16 | 32;
  /** unit range [start,end) within its area (u16 units for 16, u32 for 32) */
  start: number;
  end: number;
  /** for containers: physical slot byte offsets (body-relative for 32, p16-relative for 16) */
  slots: number[];
  slotVals: number[];
  /** table key offsets as stored (16-bit: key units; 32/Table32: per format) */
  keyOffs: number[];
}

/** A live UTF-16 string in the LOCAL 16-bit area. */
interface Str16 {
  /** unit start (marker included), local 16-bit-area units */
  start: number;
  end: number;
  text: string;
}

export interface Pool {
  keyChars: Buffer;
  p16: Buffer;
}

export function loadPool(buf: Buffer): Pool {
  const hs = buf.readUInt16LE(0);
  const bd = buf.subarray(hs);
  const il = bd.readUInt32LE(4) & 0xff;
  if (il <= 6) throw new Error("pool bundle has no 16-bit-units area (indexLength <= 6)");
  const keysTop = bd.readInt32LE(4 + 4 * 1);
  const t16Top = bd.readInt32LE(4 + 4 * 6);
  return { keyChars: bd.subarray((1 + il) * 4, keysTop * 4), p16: bd.subarray(keysTop * 4, t16Top * 4) };
}

/** Read one string-v2 at unit index `li` of area `u`: [text, unitExtent]. */
function readSv2(u: Buffer, li: number): [string, number] {
  let o = li * 2;
  const first = u.readUInt16LE(o);
  let nlen = 0;
  let len = -1;
  if (first >= 0xdc00 && first <= 0xdfff) {
    if (first < 0xdfef) { len = first & 0x3ff; nlen = 1; }
    else if (first < 0xdfff) { len = ((first - 0xdfef) << 16) | u.readUInt16LE(o + 2); nlen = 2; }
    else { len = (u.readUInt16LE(o + 2) << 16) | u.readUInt16LE(o + 4); nlen = 3; }
  }
  o += nlen * 2;
  if (len < 0) { let e = o; while (u.readUInt16LE(e) !== 0) e += 2; len = (e - o) / 2; }
  // Every stored string is NUL-terminated (ures_getString hands out const
  // UChar* that callers may u_strlen), marker or not.
  return [u.toString("utf16le", o, o + len * 2), nlen + len + 1];
}

/** Encoded units for a string value written implicitly (chars + NUL) or with a marker. */
function encodeSv2(text: string): Buffer {
  const n = text.length;
  const needMarker = n > 0 && (text.charCodeAt(0) >= 0xdc00 && text.charCodeAt(0) <= 0xdfff) || n >= 0x3ef;
  const units: number[] = [];
  if (needMarker) {
    if (n < 0x3ef) units.push(0xdc00 | n);
    else if (n <= 0xfffff) { units.push(0xdfef + (n >> 16), n & 0xffff); }
    else { units.push(0xdfff, n >> 16, n & 0xffff); }
  }
  for (let i = 0; i < n; i++) units.push(text.charCodeAt(i));
  units.push(0);
  const b = Buffer.alloc(units.length * 2);
  units.forEach((v, i) => b.writeUInt16LE(v, i * 2));
  return b;
}

export class ResBundle {
  buf: Buffer;
  hs: number;
  body: Buffer;
  fv: number;
  il: number;
  keysTop: number;
  resTop: number;
  t16Top: number;
  attrs: number;
  psil: number;
  psil16: number;
  lkl: number;
  p16: Buffer;
  pool: Pool | null;
  rootRes: number;
  nodes = new Map<number, Node>();
  byWord = new Map<number, string>();
  /** local string-v2 start unit -> Str16 (deduped by start) */
  strs = new Map<number, Str16>();
  /** edits */
  private replaced = new Map<number, Buffer>(); // local string start unit -> new encoded units
  private deletedFrom = new Map<number, Set<string>>(); // table res -> keys to drop
  private deletedElems = new Map<number, Set<number>>(); // array res -> element indexes to drop
  private dedupEnabled = false;

  constructor(buf: Buffer, pool: Pool | null) {
    this.buf = buf;
    this.hs = buf.readUInt16LE(0);
    if (buf[2] !== 0xda || buf[3] !== 0x27) throw new Error("not a .res");
    this.body = buf.subarray(this.hs);
    this.fv = buf[16];
    if (this.fv !== 2 && this.fv !== 3) throw new Error(`unsupported .res formatVersion ${this.fv}`);
    this.rootRes = this.body.readUInt32LE(0);
    this.il = this.body.readUInt32LE(4) & 0xff;
    const idx = (i: number) => this.body.readInt32LE(4 + 4 * i);
    this.keysTop = idx(1);
    this.resTop = idx(2);
    this.t16Top = this.il > 6 ? idx(6) : this.keysTop;
    this.attrs = this.il > 5 ? idx(5) : 0;
    if (this.attrs & 2) throw new Error("is a pool bundle");
    this.psil = this.fv >= 3 ? this.body.readUInt32LE(4) >>> 8 : 0;
    if (this.il > 5) this.psil |= (this.attrs & 0xf000) << 12;
    this.psil16 = this.il > 5 ? this.attrs >>> 16 : 0;
    this.lkl = this.keysTop > 1 + this.il ? this.keysTop << 2 : 0;
    this.p16 = this.body.subarray(this.keysTop * 4, this.t16Top * 4);
    this.pool = pool;
    if (this.attrs & 4 && !pool) throw new Error("bundle uses a pool bundle; pass it");
    this.walk(this.rootRes);
  }

  private P16(u: number): number { return this.p16.readUInt16LE(u * 2); }

  /** Key of a Table/Table16 entry (RES_GET_KEY16: local below localKeyLimit,
   *  else pool at ko - localKeyLimit). */
  readKey(ko: number): string {
    if (this.lkl && ko < this.lkl) { let e = ko; while (this.body[e]) e++; return this.body.toString("latin1", ko, e); }
    const po = ko - this.lkl;
    let e = po;
    while (this.pool!.keyChars[e]) e++;
    return this.pool!.keyChars.toString("latin1", po, e);
  }
  /** Key of a Table32 entry (RES_GET_KEY32: sign bit selects the pool). */
  private readKey32(ko: number): string {
    if (ko >= 0) { let e = ko; while (this.body[e]) e++; return this.body.toString("latin1", ko, e); }
    const po = ko & 0x7fffffff;
    let e = po;
    while (this.pool!.keyChars[e]) e++;
    return this.pool!.keyChars.toString("latin1", po, e);
  }
  /** Key text for a stored key offset, honoring the table's key encoding. */
  private keyText(n: Node, ko: number): string {
    return n.type === T_TABLE32 ? this.readKey32(ko) : this.readKey(ko);
  }
  /** Key text of entry i of a table node. */
  keyName(n: Node, i: number): string { return this.keyText(n, n.keyOffs[i]); }

  /** Track a LOCAL string-v2 by its (pool-adjusted) offset; returns its text. */
  private noteStr(off: number): string {
    if (off === 0) return "";
    if (off < this.psil) return readSv2(this.pool!.p16, off)[0];
    const lu = off - this.psil;
    const got = this.strs.get(lu);
    if (got) return got.text;
    const [text, extent] = readSv2(this.p16, lu);
    this.strs.set(lu, { start: lu, end: lu + extent, text });
    return text;
  }
  private noteStr16(v: number): string {
    return v < this.psil16 ? readSv2(this.pool!.p16, v)[0] : this.noteStr(v - this.psil16 + this.psil);
  }
  /** Post-edit text of a res16 string value (16-bit container member). */
  private str16After(v: number): string {
    if (v >= this.psil16) {
      const rep = this.replaced.get(v - this.psil16);
      if (rep) return readSv2(rep, 0)[0];
    }
    return this.noteStr16(v);
  }
  /** String values of an ARRAY/ARRAY16 of strings, else null. */
  arrayStrings(res: number): string[] | null {
    const n = this.nodes.get(res);
    if (!n || (n.type !== T_ARRAY && n.type !== T_ARRAY16)) return null;
    return n.slotVals.map((v) => (n.type === T_ARRAY16 ? this.noteStr16(v) : this.stringValue(v) ?? ""));
  }

  private H(s: string): string { return createHash("sha256").update(s, "utf8").digest("base64"); }
  /** Collision-free composition of container children: every part is reduced
   *  to a fixed-width digest so raw string values (which could contain the
   *  join separators) never appear in a parent's hashed representation. */
  private HC(tag: string, parts: readonly string[]): string {
    const h = createHash("sha256").update(tag, "utf8");
    for (const p of parts) h.update(createHash("sha256").update(p, "utf8").digest());
    return "H" + h.digest("base64");
  }

  private walk(res: number): string {
    const memo = this.byWord.get(res);
    if (memo !== undefined) return memo;
    const type = res >>> 28, off = res & 0x0fffffff;
    const body = this.body, BU = (o: number) => body.readUInt32LE(o), BU16 = (o: number) => body.readUInt16LE(o);
    let h: string;
    let node: Node | null = null;
    const leaf = (area: 16 | 32, start: number, end: number): void => {
      node = { res, type, off, area, start, end, slots: [], slotVals: [], keyOffs: [] };
    };
    switch (type) {
      case T_STRING_V2: h = "S" + this.noteStr(off); break;
      case T_INT: h = "I" + ((off << 4) >> 4); break;
      case T_STRING: case T_ALIAS: {
        if (!off) { h = type === T_ALIAS ? "L" : "s"; break; }
        const bo = off * 4, len = body.readInt32LE(bo);
        h = (type === T_ALIAS ? "L" : "s") + body.toString("utf16le", bo + 4, bo + 4 + len * 2);
        // extent: length word + (len+1) UChars, in whole u32 units
        leaf(32, off, off + 1 + ((2 * (len + 1) + 3) >> 2));
        break;
      }
      case T_BINARY: {
        if (!off) { h = "B"; break; }
        const bo = off * 4, len = body.readInt32LE(bo);
        h = "B" + createHash("sha256").update(body.subarray(bo + 4, bo + 4 + len)).digest("base64");
        leaf(32, off, off + 1 + ((len + 3) >> 2));
        break;
      }
      case T_INTVECTOR: {
        if (!off) { h = "V"; break; }
        const bo = off * 4, len = body.readInt32LE(bo);
        h = "V" + body.toString("base64", bo + 4, bo + 4 + len * 4);
        leaf(32, off, off + 1 + len);
        break;
      }
      case T_TABLE: {
        if (!off) { h = "T{}"; break; }
        const bo = off * 4, n = BU16(bo), pad = (n & 1) === 0 ? 2 : 0;
        const slots: number[] = [], vals: number[] = [], keyOffs: number[] = [], parts: string[] = [];
        for (let i = 0; i < n; i++) {
          const so = bo + 2 + 2 * n + pad + 4 * i, v = BU(so);
          slots.push(so); vals.push(v); keyOffs.push(BU16(bo + 2 + 2 * i));
          parts.push(this.readKey(keyOffs[i]) + "\x02" + this.walk(v));
        }
        h = this.HC("T", parts);
        node = { res, type, off, area: 32, start: off, end: off + (2 + 2 * n + pad + 4 * n) / 4, slots, slotVals: vals, keyOffs };
        break;
      }
      case T_TABLE32: {
        if (!off) { h = "T{}"; break; }
        const bo = off * 4, n = body.readInt32LE(bo);
        const slots: number[] = [], vals: number[] = [], keyOffs: number[] = [], parts: string[] = [];
        for (let i = 0; i < n; i++) {
          const so = bo + 4 + 4 * n + 4 * i, v = BU(so);
          slots.push(so); vals.push(v); keyOffs.push(body.readInt32LE(bo + 4 + 4 * i));
          parts.push(this.readKey32(keyOffs[i]) + "\x02" + this.walk(v));
        }
        h = this.HC("T", parts);
        node = { res, type, off, area: 32, start: off, end: off + 1 + 2 * n, slots, slotVals: vals, keyOffs };
        break;
      }
      case T_TABLE16: {
        if (!off) { h = "T{}"; break; }
        const n = this.P16(off);
        const slots: number[] = [], vals: number[] = [], keyOffs: number[] = [], parts: string[] = [];
        for (let i = 0; i < n; i++) {
          const so = (off + 1 + n + i) * 2, v = this.p16.readUInt16LE(so);
          slots.push(so); vals.push(v); keyOffs.push(this.P16(off + 1 + i));
          parts.push(this.readKey(keyOffs[i]) + "\x02S" + this.noteStr16(v));
        }
        h = this.HC("T", parts);
        node = { res, type, off, area: 16, start: off, end: off + 1 + 2 * n, slots, slotVals: vals, keyOffs };
        break;
      }
      case T_ARRAY: {
        if (!off) { h = "A[]"; break; }
        const bo = off * 4, n = body.readInt32LE(bo);
        const slots: number[] = [], vals: number[] = [], parts: string[] = [];
        for (let i = 0; i < n; i++) { const so = bo + 4 + 4 * i, v = BU(so); slots.push(so); vals.push(v); parts.push(this.walk(v)); }
        h = this.HC("A", parts);
        node = { res, type, off, area: 32, start: off, end: off + 1 + n, slots, slotVals: vals, keyOffs: [] };
        break;
      }
      case T_ARRAY16: {
        if (!off) { h = "A[]"; break; }
        const n = this.P16(off);
        const slots: number[] = [], vals: number[] = [], parts: string[] = [];
        for (let i = 0; i < n; i++) { const so = (off + 1 + i) * 2, v = this.p16.readUInt16LE(so); slots.push(so); vals.push(v); parts.push("S" + this.noteStr16(v)); }
        h = this.HC("A", parts);
        node = { res, type, off, area: 16, start: off, end: off + 1 + n, slots, slotVals: vals, keyOffs: [] };
        break;
      }
      default:
        throw new Error("unknown resource type " + type);
    }
    if (node) this.nodes.set(res, node);
    this.byWord.set(res, h!);
    return h!;
  }

  // ----------------------------------------------------------------- queries
  /** Resolve a path of table keys from the root; returns the Resource word or null. */
  find(path: string[]): number | null {
    let res = this.rootRes;
    for (const key of path) {
      const n = this.nodes.get(res);
      if (!n || (n.type !== T_TABLE && n.type !== T_TABLE32 && n.type !== T_TABLE16)) return null;
      let next: number | null = null;
      for (let i = 0; i < n.keyOffs.length; i++) {
        if (this.keyName(n, i) === key) {
          next = n.type === T_TABLE16 ? ((T_STRING_V2 << 28) | (n.slotVals[i] < this.psil16 ? n.slotVals[i] : n.slotVals[i] - this.psil16 + this.psil)) >>> 0 : n.slotVals[i];
          break;
        }
      }
      if (next === null) return null;
      res = next;
    }
    return res;
  }
  keysOf(tableRes: number): string[] {
    const n = this.nodes.get(tableRes);
    if (!n) return [];
    return n.keyOffs.map((_, i) => this.keyName(n, i));
  }
  stringValue(res: number): string | null {
    const t = res >>> 28;
    if (t === T_STRING_V2) return this.noteStr(res & 0x0fffffff);
    if (t === T_STRING || t === T_ALIAS) {
      const off = res & 0x0fffffff;
      if (!off) return "";
      const len = this.body.readInt32LE(off * 4);
      return this.body.toString("utf16le", off * 4 + 4, off * 4 + 4 + len * 2);
    }
    return null;
  }

  // --------------------------------------------------------------- mutations
  /** Delete the named keys from the table at `path`. Returns how many matched. */
  deleteTableKeys(path: string[], keys: string[]): number {
    const res = this.find(path);
    if (res === null) throw new Error(`deleteTableKeys: no table at /${path.join("/")}`);
    const n = this.nodes.get(res);
    if (!n || (n.type !== T_TABLE && n.type !== T_TABLE32 && n.type !== T_TABLE16)) throw new Error(`not a table: /${path.join("/")}`);
    const present = new Set(this.keysOf(res));
    const hit = keys.filter((k) => present.has(k));
    if (hit.length) {
      const set = this.deletedFrom.get(res) ?? new Set<string>();
      for (const k of hit) set.add(k);
      this.deletedFrom.set(res, set);
    }
    return hit.length;
  }
  /** Delete elements (by index) from the ARRAY/ARRAY16 at `res`. */
  deleteArrayElements(res: number, indexes: readonly number[]): void {
    const n = this.nodes.get(res);
    if (!n || (n.type !== T_ARRAY && n.type !== T_ARRAY16)) throw new Error("deleteArrayElements: not an array");
    const set = this.deletedElems.get(res) ?? new Set<number>();
    for (const i of indexes) {
      if (i < 0 || i >= n.slotVals.length) throw new Error(`deleteArrayElements: index ${i} out of range`);
      set.add(i);
    }
    this.deletedElems.set(res, set);
  }
  /** Replace a string-v2 value (must be LOCAL and not shorter than the replacement). */
  replaceString(res: number, text: string): void {
    if (res >>> 28 !== T_STRING_V2) throw new Error("replaceString: not a string-v2");
    const off = res & 0x0fffffff;
    if (off < this.psil) throw new Error("replaceString: pool string");
    const lu = off - this.psil;
    const s = this.strs.get(lu);
    if (!s) throw new Error("replaceString: unknown string");
    const enc = encodeSv2(text);
    if (enc.length / 2 > s.end - s.start) throw new Error("replaceString: replacement longer than original");
    this.replaced.set(lu, enc);
  }
  dedup(): void { this.dedupEnabled = true; }

  // --------------------------------------------------------------- serialize
  /**
   * Emit a valid bundle with all edits applied. Surviving bytes stay in
   * place; dead ranges become holes; every offset is relocated.
   */
  serialize(): Buffer {
    const body = this.body;
    // 1. post-mutation child lists (tables with deleted keys)
    const effSlotVals = new Map<number, number[]>();
    const effKeyOffs = new Map<number, number[]>();
    for (const n of this.nodes.values()) {
      const del = this.deletedFrom.get(n.res);
      if (del) {
        const keep = n.keyOffs.map((ko, i) => [ko, n.slotVals[i], i] as const).filter(([, , i]) => !del.has(this.keyName(n, i)));
        effKeyOffs.set(n.res, keep.map((x) => x[0]));
        effSlotVals.set(n.res, keep.map((x) => x[1]));
        continue;
      }
      const delIdx = this.deletedElems.get(n.res);
      if (delIdx) effSlotVals.set(n.res, n.slotVals.filter((_, i) => !delIdx.has(i)));
    }
    const childVals = (n: Node): number[] => effSlotVals.get(n.res) ?? n.slotVals;
    const childKeys = (n: Node): number[] => effKeyOffs.get(n.res) ?? n.keyOffs;

    // 2. post-mutation content hash (for dedup) — recompute over the effective graph
    const hash2 = new Map<number, string>();
    const rehash = (res: number): string => {
      const got = hash2.get(res);
      if (got !== undefined) return got;
      hash2.set(res, "?cycle");
      const t = res >>> 28, off = res & 0x0fffffff;
      const n = this.nodes.get(res);
      let h: string;
      if (t === T_STRING_V2) {
        const lu = off - this.psil;
        h = "S" + (off !== 0 && off >= this.psil && this.replaced.has(lu) ? readSv2(this.replaced.get(lu)!, 0)[0] : off === 0 ? "" : this.noteStr(off));
      } else if (!n) h = this.byWord.get(res)!;
      else if (CONTAINERS.has(t)) {
        const keys = childKeys(n), vals = childVals(n);
        const isTab = t === T_TABLE || t === T_TABLE32 || t === T_TABLE16;
        const parts: string[] = [];
        for (let i = 0; i < vals.length; i++) {
          const cs = n.type === T_TABLE16 || n.type === T_ARRAY16 ? "S" + this.str16After(vals[i]) : rehash(vals[i]);
          parts.push(isTab ? this.keyText(n, keys[i]) + "\x02" + cs : cs);
        }
        h = this.HC(isTab ? "T" : "A", parts);
      } else h = this.byWord.get(res)!;
      hash2.set(res, h);
      return h;
    };

    // 3. liveness from the (remapped) root; dedup canon by post-mutation hash
    const canon = new Map<string, number>();
    const alias = new Map<number, number>();
    const live = new Set<number>();
    const liveStr = new Set<number>(); // local string start units
    const order: number[] = [];
    const visit = (res: number): number => {
      const t = res >>> 28, off = res & 0x0fffffff;
      if (t === T_STRING_V2 && off !== 0 && off >= this.psil) liveStr.add(off - this.psil);
      const n = this.nodes.get(res);
      if (!n) return res;
      const known = alias.get(res);
      if (known !== undefined) return known;
      const h = rehash(res);
      if (this.dedupEnabled) {
        const first = canon.get(h);
        if (first !== undefined && first !== res) { alias.set(res, first); visit(first); return first; }
        canon.set(h, res);
      }
      alias.set(res, res);
      if (live.has(res)) return res;
      live.add(res);
      order.push(res);
      if (n.type === T_TABLE16 || n.type === T_ARRAY16) {
        for (const v of childVals(n)) if (v >= this.psil16) liveStr.add(v - this.psil16);
      } else {
        for (const v of childVals(n)) visit(v);
      }
      return res;
    };
    const newRoot = visit(this.rootRes);
    const remap = (w: number): number => {
      if (!this.nodes.has(w)) return w;
      const a = alias.get(w);
      return a === undefined ? w : a;
    };

    // 4. dead ranges become holes. ONLY provably-dead bytes are removed:
    //    duplicate/unreachable resource bodies, the tails of shrunken tables
    //    and shortened strings, and dead strings — minus any byte a live
    //    (possibly suffix-nested) string or resource still covers. An
    //    identity rewrite therefore emits byte-identical output (inter-
    //    resource alignment padding is untouched).
    type Range = { start: number; end: number };
    const len16 = (this.t16Top - this.keysTop) * 2;
    const union = (rs: Range[]): Range[] => {
      rs.sort((a, b) => a.start - b.start || a.end - b.end);
      const out: Range[] = [];
      for (const r of rs) {
        if (r.end <= r.start) continue;
        const last = out[out.length - 1];
        if (last && r.start <= last.end) last.end = Math.max(last.end, r.end);
        else out.push({ ...r });
      }
      return out;
    };
    const subtract = (a: Range[], b: Range[]): Range[] => {
      const out: Range[] = [];
      let j = 0;
      for (const r of union(a)) {
        let s = r.start;
        while (j < b.length && b[j].end <= s) j++;
        let k = j;
        while (k < b.length && b[k].start < r.end) {
          if (b[k].start > s) out.push({ start: s, end: b[k].start });
          s = Math.max(s, b[k].end);
          k++;
        }
        if (s < r.end) out.push({ start: s, end: r.end });
      }
      return out;
    };
    const live16: Range[] = [], live32: Range[] = [], dead16: Range[] = [], dead32: Range[] = [];
    for (const n of this.nodes.values()) {
      const isLive = live.has(n.res);
      let end = n.end;
      if (isLive && effSlotVals.has(n.res)) {
        const m = effSlotVals.get(n.res)!.length;
        if (n.type === T_TABLE) end = n.off + (2 + 2 * m + ((m & 1) === 0 ? 2 : 0) + 4 * m) / 4;
        else if (n.type === T_TABLE32) end = n.off + 1 + 2 * m;
        else if (n.type === T_TABLE16) end = n.off + 1 + 2 * m;
        else if (n.type === T_ARRAY || n.type === T_ARRAY16) end = n.off + 1 + m;
        (n.area === 16 ? dead16 : dead32).push({ start: end, end: n.end }); // shrunk tail
      }
      (isLive ? (n.area === 16 ? live16 : live32) : (n.area === 16 ? dead16 : dead32)).push({ start: n.start, end });
    }
    for (const [su, s] of this.strs) {
      if (liveStr.has(su)) {
        const rep = this.replaced.get(su);
        live16.push({ start: s.start, end: rep ? s.start + rep.length / 2 : s.end });
        if (rep) dead16.push({ start: s.start + rep.length / 2, end: s.end });
      } else {
        dead16.push({ start: s.start, end: s.end });
      }
    }
    // genrb suffix-shares strings (string B stored as the tail of string A),
    // so replacement writes must not land inside a string that stays live and
    // unreplaced: writing R's stub at R.start would corrupt an enclosing A's
    // tail, and a string starting inside R's written range would lose its
    // head. (Two overlapping strings that are BOTH replaced are fine: each
    // stub lands at its own start and the shared tail is dead.)
    for (const [lu, enc] of this.replaced) {
      const wEnd = lu + enc.length / 2;
      for (const [su2, s2] of this.strs) {
        if (su2 === lu) continue;
        if (su2 > lu && su2 < wEnd) throw new Error(`replaceString: string @${su2} starts inside the replaced range @${lu}`);
        if (su2 < lu && s2.end > lu && liveStr.has(su2) && !this.replaced.has(su2)) throw new Error(`replaceString: target @${lu} is inside live unreplaced string @${su2}`);
      }
    }
    const holes16 = subtract(dead16, union(live16));
    const holes32raw = subtract(dead32, union(live32));

    const mkReloc = (holes: Range[]) => {
      for (let i = 1; i < holes.length; i++) if (holes[i].start < holes[i - 1].end) throw new Error("overlapping holes");
      const cum: number[] = [];
      let s = 0;
      for (const h of holes) { cum.push(s); s += h.end - h.start; }
      return {
        total: s,
        f: (u: number): number => {
          let lo = 0, hi = holes.length;
          while (lo < hi) { const m = (lo + hi) >> 1; if (holes[m].start <= u) lo = m + 1; else hi = m; }
          if (lo > 0 && u < holes[lo - 1].end) throw new Error("relocating a dead unit " + u);
          return u - (lo > 0 ? cum[lo - 1] + (holes[lo - 1].end - holes[lo - 1].start) : 0);
        },
      };
    };
    const r16 = mkReloc(holes16);
    // genrb aligns every URES_BINARY payload (off*4 + 4) to 16 bytes within
    // the bundle body; readers reinterpret those bytes as wider types (the
    // collation images as int64). Every live binary must therefore keep its
    // payload alignment, which means the total leftward shift of the 32-bit
    // area at that point must be a whole number of 16-byte quanta:
    //  (a) the 16-bit area shrinks by a multiple of 4 u32 units (pad with up
    //      to 7 dead u16s), and
    //  (b) every 32-bit hole that precedes a live binary is shrunk to keep
    //      the cumulative removed-word count = 0 (mod 4); the give-back words
    //      simply remain in place as dead padding.
    const pad16 = r16.total % 8;
    const old16units = len16;
    const new16units = old16units - r16.total + pad16;
    const d16u32 = (old16units - new16units) / 2;
    const liveBinOffs = [...this.nodes.values()]
      .filter((n) => live.has(n.res) && n.type === T_BINARY && n.off !== 0)
      .map((n) => n.off)
      .sort((a, b) => a - b);
    {
      let cum = 0;
      let bi = 0;
      for (const h of holes32raw) {
        while (bi < liveBinOffs.length && liveBinOffs[bi] < h.start) bi++;
        const size = h.end - h.start;
        if (bi < liveBinOffs.length) {
          const keep = ((cum + size) >> 2 << 2) - cum; // largest 0..size with (cum+keep) % 4 == 0
          h.start = h.end - Math.max(0, keep);
        }
        cum += h.end - h.start;
      }
    }
    const holes32 = holes32raw.filter((h) => h.end > h.start);
    const r32 = mkReloc(holes32);
    const newT16Top = this.t16Top - d16u32;
    const newResTop = this.resTop - d16u32 - r32.total;
    // The invariant the two adjustments above exist to preserve.
    for (const off of liveBinOffs) if ((off - (r32.f(off) - d16u32)) % 4 !== 0) throw new Error(`binary at ${off} would lose its 16-byte payload alignment`);

    const relocStrOff = (off: number): number => (off === 0 || off < this.psil ? off : r16.f(off - this.psil) + this.psil);
    const relocWord = (w0: number): number => {
      const w = remap(w0);
      const t = w >>> 28, off = w & 0x0fffffff;
      if (off === 0 || t === T_INT) return w >>> 0;
      if (t === T_TABLE16 || t === T_ARRAY16) return (((t << 28) | r16.f(off)) >>> 0);
      if (t === T_STRING_V2) return (((t << 28) | relocStrOff(off)) >>> 0);
      return (((t << 28) | (r32.f(off) - d16u32)) >>> 0);
    };
    const relocRes16 = (v: number): number => (v < this.psil16 ? v : r16.f(v - this.psil16) + this.psil16);

    // 5. build the output areas.
    // 5a. header + indexes + local keys (verbatim; patch root + tops)
    const head = Buffer.from(body.subarray(0, this.keysTop * 4));
    head.writeUInt32LE(relocWord(newRoot), 0);
    head.writeInt32LE(newResTop, 4 + 4 * 2);
    if (this.il > 3) head.writeInt32LE(newResTop, 4 + 4 * 3);
    if (this.il > 6) head.writeInt32LE(newT16Top, 4 + 4 * 6);
    // 5b. 16-bit area
    const src16 = Buffer.from(this.p16);
    for (const [su, enc] of this.replaced) enc.copy(src16, su * 2);
    for (const res of order) {
      const n = this.nodes.get(res)!;
      if (n.area !== 16) continue;
      const vals = childVals(n), keys = childKeys(n);
      const m = vals.length;
      if (effSlotVals.has(res)) {
        // rewrite the whole (smaller) table/array in place
        let u = n.off;
        src16.writeUInt16LE(m, u * 2); u++;
        if (n.type === T_TABLE16) { for (let i = 0; i < m; i++) src16.writeUInt16LE(keys[i], (u + i) * 2); u += m; }
        for (let i = 0; i < m; i++) src16.writeUInt16LE(relocRes16(vals[i]), (u + i) * 2);
      } else {
        for (let i = 0; i < n.slots.length; i++) src16.writeUInt16LE(relocRes16(n.slotVals[i]), n.slots[i]);
      }
    }
    const c16: Buffer[] = [];
    let cur16 = 0;
    for (const h of holes16) { c16.push(src16.subarray(cur16 * 2, h.start * 2)); cur16 = h.end; }
    c16.push(src16.subarray(cur16 * 2));
    if (pad16) c16.push(Buffer.alloc(pad16 * 2, 0xaa));
    const out16 = Buffer.concat(c16);
    if (out16.length !== new16units * 2) throw new Error(`16-bit area size ${out16.length} != ${new16units * 2}`);
    // 5c. 32-bit area
    const base32 = this.t16Top;
    const src32 = Buffer.from(body.subarray(this.t16Top * 4, this.resTop * 4));
    const W32 = (u: number, v: number) => src32.writeUInt32LE(v >>> 0, (u - base32) * 4);
    for (const res of order) {
      const n = this.nodes.get(res)!;
      if (n.area !== 32 || !CONTAINERS.has(n.type)) continue;
      const vals = childVals(n), keys = childKeys(n);
      const m = vals.length;
      if (effSlotVals.has(res)) {
        if (n.type === T_TABLE) {
          const bo = n.off * 4, pad = (m & 1) === 0 ? 2 : 0;
          src32.writeUInt16LE(m, bo - base32 * 4);
          for (let i = 0; i < m; i++) src32.writeUInt16LE(keys[i], bo - base32 * 4 + 2 + 2 * i);
          if (pad) src32.writeUInt16LE(0, bo - base32 * 4 + 2 + 2 * m);
          for (let i = 0; i < m; i++) src32.writeUInt32LE(relocWord(vals[i]), bo - base32 * 4 + 2 + 2 * m + pad + 4 * i);
        } else if (n.type === T_TABLE32) {
          W32(n.off, m);
          for (let i = 0; i < m; i++) W32(n.off + 1 + i, keys[i]);
          for (let i = 0; i < m; i++) W32(n.off + 1 + m + i, relocWord(vals[i]));
        } else if (n.type === T_ARRAY) {
          W32(n.off, m);
          for (let i = 0; i < m; i++) W32(n.off + 1 + i, relocWord(vals[i]));
        }
      } else {
        for (let i = 0; i < n.slots.length; i++) src32.writeUInt32LE(relocWord(n.slotVals[i]), n.slots[i] - base32 * 4);
      }
    }
    const c32: Buffer[] = [];
    let cur32 = base32;
    for (const h of holes32) { c32.push(src32.subarray((cur32 - base32) * 4, (h.start - base32) * 4)); cur32 = h.end; }
    c32.push(src32.subarray((cur32 - base32) * 4));
    const out32 = Buffer.concat(c32);
    if (out32.length !== (newResTop - newT16Top) * 4) throw new Error(`32-bit area size ${out32.length} != ${(newResTop - newT16Top) * 4}`);

    let newBody = Buffer.concat([head, out16, out32]);
    const total = this.hs + newBody.length;
    const tail = (16 - (total & 15)) & 15;
    return Buffer.concat([this.buf.subarray(0, this.hs), newBody, Buffer.alloc(tail, 0xaa)]);
  }
}

// --------------------------------------------------------------------------
// Resolved-view dump: a canonical, offset-free JSON of a bundle's entire
// resource tree. Two bundles with equal dumps are indistinguishable to
// ures_* readers. Used to prove every rewrite (identity or intended edit).
export function dumpResolved(b: ResBundle): unknown {
  const go = (res: number): unknown => {
    const t = res >>> 28, off = res & 0x0fffffff;
    if (t === T_INT) return { int: (off << 4) >> 4 };
    if (t === T_STRING_V2 || t === T_STRING) return { s: t === T_STRING_V2 ? (off === 0 ? "" : b.stringValue(res)) : b.stringValue(res) };
    if (t === T_ALIAS) return { alias: b.stringValue(res) };
    if (t === T_BINARY) { const bo = off * 4, len = off ? b.body.readInt32LE(bo) : 0; return { bin: off ? createHash("sha256").update(b.body.subarray(bo + 4, bo + 4 + len)).digest("hex") : "" }; }
    if (t === T_INTVECTOR) { const bo = off * 4, len = off ? b.body.readInt32LE(bo) : 0; const v: number[] = []; for (let i = 0; i < len; i++) v.push(b.body.readInt32LE(bo + 4 + 4 * i)); return { iv: v }; }
    if (off === 0) return t === T_ARRAY || t === T_ARRAY16 ? [] : {}; // empty container
    const n = b.nodes.get(res);
    if (!n) throw new Error("dump: unknown node type " + t);
    if (n.type === T_TABLE || n.type === T_TABLE32 || n.type === T_TABLE16) {
      const o: Record<string, unknown> = {};
      for (let i = 0; i < n.keyOffs.length; i++) {
        const k = b.keyName(n, i);
        o[k] = n.type === T_TABLE16 ? { s: b.stringValue(((T_STRING_V2 << 28) | (n.slotVals[i] < b.psil16 ? n.slotVals[i] : n.slotVals[i] - b.psil16 + b.psil)) >>> 0) } : go(n.slotVals[i]);
      }
      return o;
    }
    return n.slotVals.map((v) => (n.type === T_ARRAY16 ? { s: b.stringValue(((T_STRING_V2 << 28) | (v < b.psil16 ? v : v - b.psil16 + b.psil)) >>> 0) } : go(v)));
  };
  return go(b.rootRes);
}
