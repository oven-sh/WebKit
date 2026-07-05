// Structural rewrites applied to individual .res items by compress-data.ts,
// before per-item compression. Every transform here is a pure data-layout /
// dead-data edit with no reachable behavior change; each one names its proof.
//
//  1. Within-bundle resource dedup (every .res item): byte-identical sibling
//     resources (containers, aliases, intvectors, binaries, strings) are
//     stored once and referenced from every parent slot. Sharing values is a
//     documented property of the format (uresdata.h) and genrb only ever does
//     it for strings.
//  2. supplementalData.res: drop `subdivisionContainment` and the dead
//     `idValidity` classes (everything but `region`). No ICU reader that JSC
//     or Bun can reach opens either.
//  3. root-tree locale bundles: drop `characterLabel` and `personNames`
//     (no linked ICU4C reader), and `parse` outside root.res (root's is the
//     only one a reader consults).
//  4. brkitr: drop the UBRK_LINE / UBRK_TITLE rule entries (`boundaries/line*`,
//     `boundaries/title`) and their %%DEPENDENCY references. Intl.Segmenter
//     exposes only character|word|sentence, and WTF's line-mode break
//     iterator has no JSC/Bun caller. This is what lets compress-data.ts drop
//     the 10 line*/title .brk items themselves.
//  5. Collation rule source text: delete coll/root.res `UCARules` (only
//     reachable through ucol_getRules(UCOL_FULL_RULES); no caller) and stub
//     every tailoring's `Sequence` down to one code unit. The stub is
//     deliberately non-empty: JSC gates its ASCII collation fast path on
//     `ucol_getRules()` LENGTH, so an empty rule string would change
//     Intl.Collator ordering for tailored locales.
//
// Node stdlib only; erasable-TypeScript-only.

import { ResBundle, dumpResolved, type Pool } from "./resbundle.ts";

/** Root-tree CLDR locale bundles ("root.res", "de.res", "zh_Hant_TW.res", ...) */
function isRootTreeLocale(bare: string): boolean {
  return !bare.includes("/") && /^(root|[a-z]{2,3}(_[A-Za-z0-9]+)*)\.res$/.test(bare);
}

const isLineOrTitle = (k: string): boolean => k === "line" || k.startsWith("line_") || k === "title";

export interface Rewrite {
  out: Buffer;
  notes: string[];
  /** resolved view of the ORIGINAL bundle (for the caller's proof step) */
  beforeDump: unknown;
  /** brkitr rule-file names ("line_cj.brk", ...) whose references this
   *  rewrite removed; the packer may only drop items named here. */
  removedBrkRefs: string[];
}

/**
 * Rewrite one extracted item. Returns null when the item is not a rewritable
 * resource bundle (pool bundles, .nrm/.icu/.dict/.brk, ...) or nothing changed.
 */
export function rewriteItem(bare: string, buf: Buffer, pool: Pool | null): Rewrite | null {
  if (buf.toString("latin1", 12, 16) !== "ResB") return null;
  if (bare.endsWith("/pool.res") || bare === "pool.res" || bare === "res_index.res" || bare.endsWith("/res_index.res")) return null;
  const notes: string[] = [];
  const removedBrkRefs: string[] = [];
  const b = new ResBundle(buf, pool);
  const beforeDump = dumpResolved(b);

  if (bare === "supplementalData.res") {
    if (b.deleteTableKeys([], ["subdivisionContainment"])) notes.push("-subdivisionContainment");
    const idv = b.find(["idValidity"]);
    if (idv !== null) {
      const dead = b.keysOf(idv).filter((k) => k !== "region");
      if (b.deleteTableKeys(["idValidity"], dead)) notes.push("-idValidity");
    }
  }

  if (isRootTreeLocale(bare)) {
    const drop = ["characterLabel", "personNames"];
    if (bare !== "root.res") drop.push("parse");
    const hit = drop.filter((k) => b.find([k]) !== null);
    if (hit.length && b.deleteTableKeys([], hit)) for (const k of hit) notes.push("-" + k);
  }

  if (bare.startsWith("brkitr/") && bare.endsWith(".res")) {
    const bnd = b.find(["boundaries"]);
    if (bnd !== null) {
      const keys = b.keysOf(bnd);
      const dead = keys.filter(isLineOrTitle);
      // Record the rule-file names these entries referenced; only files named
      // here may be dropped from the package (see dropAfterRewrite).
      for (const k of dead) {
        const v = b.find(["boundaries", k]);
        const file = v === null ? null : b.stringValue(v);
        if (file) removedBrkRefs.push(file);
      }
      if (dead.length && b.deleteTableKeys(["boundaries"], dead)) notes.push("-boundaries/line*");
      // If every boundary entry is gone, drop the (now empty) table itself.
      if (dead.length === keys.length) b.deleteTableKeys([], ["boundaries"]);
    }
    // %%DEPENDENCY is genrb's array of referenced item names (only ever .brk
    // files here). Remove the entries for the dropped line*/title rule files
    // so no dangling reference survives; if that empties the array, drop the
    // key itself.
    const depRes = b.find(["%%DEPENDENCY"]);
    if (depRes !== null) {
      const vals = b.arrayStrings(depRes);
      if (vals !== null) {
        const dead = vals.map((v, i) => [v, i] as const).filter(([v]) => isLineOrTitle(v.replace(/\.brk$/, "")));
        if (dead.length) {
          removedBrkRefs.push(...dead.map(([v]) => v));
          if (dead.length === vals.length) {
            if (b.deleteTableKeys([], ["%%DEPENDENCY"])) notes.push("-%%DEPENDENCY");
          } else {
            b.deleteArrayElements(depRes, dead.map(([, i]) => i));
            notes.push("~%%DEPENDENCY");
          }
        }
      }
    }
  }

  if (bare === "coll/root.res") {
    if (b.deleteTableKeys([], ["UCARules"])) notes.push("-UCARules");
  }
  if (bare.startsWith("coll/") && bare.endsWith(".res")) {
    const colls = b.find(["collations"]);
    if (colls !== null) {
      let stubbed = 0;
      for (const type of b.keysOf(colls)) {
        const seq = b.find(["collations", type, "Sequence"]);
        if (seq === null || seq >>> 28 !== 6) continue;
        const text = b.stringValue(seq);
        // Keep ucol_getRules() non-empty for tailored locales (JSC's ASCII
        // fast-path predicate); one code unit is enough.
        if (text && text.length > 1) { b.replaceString(seq, " "); stubbed++; }
      }
      if (stubbed) notes.push("~Sequence");
    }
  }

  b.dedup();
  const out = b.serialize();
  if (out.length === buf.length && out.equals(buf) && !notes.length) return null;
  if (out.length > buf.length) throw new Error(`${bare}: rewrite grew ${buf.length} -> ${out.length}`);
  return { out, notes, beforeDump, removedBrkRefs };
}

/**
 * The UBRK_LINE / UBRK_TITLE rule files to drop from the package once the
 * brkitr bundles' references to them are rewritten away (icupkg's dependency
 * check is why they cannot go in remove-items.txt; see the reachability proof
 * there). `removedRefs` is the union of rule-file names whose references the
 * rewrites actually removed: every line/title .brk in the package must be in
 * it, so a future ICU that renames a boundary key (making the rewrite a
 * silent no-op) fails the build instead of shipping a dangling reference.
 */
export function dropAfterRewrite(names: readonly string[], removedRefs: ReadonlySet<string>): Set<string> {
  const drop = new Set(names.filter((n) => /^brkitr\/(line[^/]*|title)\.brk$/.test(n)));
  for (const n of drop) {
    const base = n.slice("brkitr/".length);
    if (!removedRefs.has(base)) throw new Error(`dropAfterRewrite: no rewrite removed the reference to ${n}; refusing to drop it`);
  }
  return drop;
}

/**
 * Edits that MUST have fired, given the items exist in the package. This is
 * the same staleness guard the Dockerfiles apply to remove-items.txt: an ICU
 * upgrade that renames a key would otherwise turn a rewrite into a silent
 * no-op that verifies vacuously.
 */
const REQUIRED_EDITS: readonly [item: string, note: string][] = [
  ["supplementalData.res", "-subdivisionContainment"],
  ["supplementalData.res", "-idValidity"],
  ["root.res", "-characterLabel"],
  ["root.res", "-personNames"],
  ["brkitr/root.res", "-boundaries/line*"],
  ["coll/root.res", "-UCARules"],
  ["coll/zh.res", "~Sequence"],
];

export function assertExpectedRewrites(notesByItem: ReadonlyMap<string, readonly string[]>, present: ReadonlySet<string>): void {
  const missing: string[] = [];
  for (const [item, note] of REQUIRED_EDITS) {
    if (!present.has(item)) { missing.push(`${item} (item not in package)`); continue; }
    if (!(notesByItem.get(item) ?? []).some((n) => n === note || n.startsWith(note))) missing.push(`${item} ${note}`);
  }
  if (missing.length) throw new Error(`expected rewrites did not fire (stale key names after an ICU upgrade?): ${missing.join("; ")}`);
}

// ---------------------------------------------------------------------------
// Build-time proof. `expectedDump(bare, before)` applies the INTENDED edits of
// rewriteItem() to a resolved-view object (resbundle.ts dumpResolved()). The
// packer asserts, for every rewritten item, that the rewritten bundle's
// resolved view equals this — i.e. the rewrite did exactly what it claims and
// nothing else. Deliberately a second, independent statement of the edits.
// ---------------------------------------------------------------------------
export function expectedDump(bare: string, dump: unknown): unknown {
  const d: any = JSON.parse(JSON.stringify(dump));
  if (bare === "supplementalData.res") {
    delete d.subdivisionContainment;
    if (d.idValidity) d.idValidity = { region: d.idValidity.region };
  }
  if (isRootTreeLocale(bare)) {
    delete d.characterLabel;
    delete d.personNames;
    if (bare !== "root.res") delete d.parse;
  }
  if (bare.startsWith("brkitr/") && bare.endsWith(".res") && d.boundaries) {
    for (const k of Object.keys(d.boundaries)) if (isLineOrTitle(k)) delete d.boundaries[k];
    if (Object.keys(d.boundaries).length === 0) delete d.boundaries;
    if (Array.isArray(d["%%DEPENDENCY"])) {
      const kept = d["%%DEPENDENCY"].filter((x: any) => !isLineOrTitle(String(x?.s ?? "").replace(/\.brk$/, "")));
      if (kept.length === 0) delete d["%%DEPENDENCY"];
      else d["%%DEPENDENCY"] = kept;
    }
  }
  if (bare === "coll/root.res") delete d.UCARules;
  if (bare.startsWith("coll/") && d.collations) {
    for (const t of Object.keys(d.collations)) {
      const c = d.collations[t];
      if (c && c.Sequence && typeof c.Sequence.s === "string" && c.Sequence.s.length > 1) c.Sequence = { s: " " };
    }
  }
  return d;
}
