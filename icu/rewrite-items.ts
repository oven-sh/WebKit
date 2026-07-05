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

import { ResBundle, type Pool } from "./resbundle.ts";

/** Root-tree CLDR locale bundles ("root.res", "de.res", "zh_Hant_TW.res", ...) */
function isRootTreeLocale(bare: string): boolean {
  return !bare.includes("/") && /^(root|[a-z]{2,3}(_[A-Za-z0-9]+)*)\.res$/.test(bare);
}

const isLineOrTitle = (k: string): boolean => k === "line" || k.startsWith("line_") || k === "title";

export interface Rewrite {
  out: Buffer;
  notes: string[];
}

/**
 * Rewrite one extracted item. Returns null when the item is not a rewritable
 * resource bundle (pool bundles, .nrm/.icu/.dict/.brk, ...) or nothing changed.
 */
export function rewriteItem(bare: string, buf: Buffer, pool: Pool | null): Rewrite | null {
  if (buf.toString("latin1", 12, 16) !== "ResB") return null;
  if (bare.endsWith("/pool.res") || bare === "pool.res" || bare === "res_index.res" || bare.endsWith("/res_index.res")) return null;
  const notes: string[] = [];
  const b = new ResBundle(buf, pool);

  if (bare === "supplementalData.res") {
    if (b.deleteTableKeys([], ["subdivisionContainment"])) notes.push("-subdivisionContainment");
    const idv = b.find(["idValidity"]);
    if (idv !== null) {
      const dead = b.keysOf(idv).filter((k) => k !== "region");
      if (b.deleteTableKeys(["idValidity"], dead)) notes.push(`-idValidity/{${dead.join(",")}}`);
    }
  }

  if (isRootTreeLocale(bare)) {
    const drop = ["characterLabel", "personNames"];
    if (bare !== "root.res") drop.push("parse");
    const hit = drop.filter((k) => b.find([k]) !== null);
    if (hit.length && b.deleteTableKeys([], hit)) notes.push("-" + hit.join(",-"));
  }

  if (bare.startsWith("brkitr/") && bare.endsWith(".res")) {
    const bnd = b.find(["boundaries"]);
    if (bnd !== null) {
      const dead = b.keysOf(bnd).filter(isLineOrTitle);
      if (dead.length && b.deleteTableKeys(["boundaries"], dead)) notes.push(`-boundaries/{${dead.length}}`);
      // If every boundary entry is gone, drop the (now empty) table itself.
      if (b.keysOf(bnd).every(isLineOrTitle)) b.deleteTableKeys([], ["boundaries"]);
    }
    // %%DEPENDENCY is genrb's array of referenced item names; the line*/title
    // rule files are no longer referenced. It only ever lists .brk files here,
    // so drop the whole key when all its entries are line*/title.
    const depRes = b.find(["%%DEPENDENCY"]);
    if (depRes !== null) {
      const vals = b.arrayStrings(depRes);
      if (vals !== null && vals.every((v) => isLineOrTitle(v.replace(/\.brk$/, "")))) {
        if (b.deleteTableKeys([], ["%%DEPENDENCY"])) notes.push("-%%DEPENDENCY");
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
      if (stubbed) notes.push(`~Sequence x${stubbed}`);
    }
  }

  b.dedup();
  const out = b.serialize();
  if (out.length === buf.length && out.equals(buf)) return null;
  if (out.length > buf.length) throw new Error(`${bare}: rewrite grew ${buf.length} -> ${out.length}`);
  notes.push(`${buf.length}->${out.length}`);
  return { out, notes };
}

/** Item names to drop from the package entirely once the brkitr references
 *  to them are rewritten away (icupkg's dependency check would otherwise
 *  refuse; see icu/remove-items.txt for the reachability proof). */
export function dropAfterRewrite(names: readonly string[]): Set<string> {
  return new Set(names.filter((n) => /^brkitr\/(line[^/]*|title)\.brk$/.test(n)));
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
    if (Array.isArray(d["%%DEPENDENCY"]) && d["%%DEPENDENCY"].every((x: any) => isLineOrTitle(String(x?.s ?? "").replace(/\.brk$/, "")))) delete d["%%DEPENDENCY"];
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
