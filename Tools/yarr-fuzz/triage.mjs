#!/usr/bin/env node
// Classify jsc-vs-node mismatches into known/expected categories; print residuals.
import { readFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
const here = dirname(fileURLToPath(import.meta.url));
const dir = process.argv[2] || join(here, "out");
const pairFilter = process.argv[3] || "node";
const files = readdirSync(dir).filter((f) => f.startsWith("mismatch-"));
const unesc = (s) => s.replace(/\\u([0-9a-f]{4})/g, (m, h) => String.fromCharCode(parseInt(h, 16)));
const cats = {};
const residual = [];
for (const f of files) {
    for (const line of readFileSync(join(dir, f), "utf8").split("\n")) {
        if (!line) continue;
        let o; try { o = JSON.parse(line); } catch { continue; }
        if (!o.pair.includes(pairFilter)) continue;
        const p = unesc(o.a.p), fl = o.a.f, s = unesc(o.a.s);
        const uni = /[uv]/.test(fl);
        let cat = null;
        if (o.a.err || o.b.err) cat = "syntax-error-diff";
        else if ((o.ta > 300 || o.tb > 300) && /node/.test(o.pair)) cat = "likely-known: backtracking limit (slow case)";
        else if ((o.a.r && o.a.r.x) || (o.b.r && o.b.r.x)) cat = "runtime-error-diff";
        else if (uni && /i/.test(fl) && /\\[pP]\{/.test(p)) cat = "known: /iu property-escape case folding";
        else if (uni && /[\ud800-\udfff]/.test(s)) cat = "likely-known: surrogate positions under /u (V8 mid-pair starts)";
        else if (!uni && /i/.test(fl) && /[ſKẞİﬀſK]/i.test(p + s)) cat = "known: non-unicode /i folding of special chars";
        else if (/y/.test(fl) && /^\.\*|\|\.\*|\(\.\*/.test(p)) cat = "known: dotstar enclosure + sticky";
        else if (uni && /i/.test(fl) && /\\[wWbB]/.test(p) && /[ſK]/i.test(p + s)) cat = "known-ish: \\w /iu with kelvin/long-s";
        if (cat) { cats[cat] = (cats[cat] || 0) + 1; continue; }
        residual.push(o);
    }
}
console.log("categories:", cats);
console.log("residual:", residual.length);
for (const o of residual.slice(0, Number(process.env.N || 60))) {
    console.log("----", o.pair, "seed", o.seed, "k", o.k, "profile", o.profile);
    console.log(" /" + unesc(o.a.p) + "/" + o.a.f, "  s=" + JSON.stringify(unesc(o.a.s)), "w=" + o.a.w);
    console.log("  A", JSON.stringify(o.a.r || o.a.err));
    console.log("  B", JSON.stringify(o.b.r || o.b.err));
}
