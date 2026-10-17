#!/usr/bin/env node
// Structure-aware mutation of real-world regexes: parse with regexpp, apply 1-3 AST-located edits
// (wrap a node in a lookbehind/lookahead/group, toggle greediness, add/alter a quantifier, insert a
// backreference to an existing group, duplicate a group name, add a string class under /v, switch
// u->v, add flags ...), keep the ones that still compile in node, and emit case files that
// regex-fuzz.js can replay (`profile file:<path>`): {p, f, subjects[]} per line.
//
// usage: node mutate.mjs --in <corpus.json|dir> --out cases-dir --count 20000 [--seed 1] [--per 3]
import fs from "node:fs";
import path from "node:path";
import { RegExpParser } from "@eslint-community/regexpp";
import RandExp from "randexp";

const arg = (k, d) => { const i = process.argv.indexOf("--" + k); return i > 0 ? process.argv[i + 1] : d; };
const IN = arg("in"), OUT = arg("out", "cases-mut"), COUNT = +arg("count", 20000), SEED = +arg("seed", 1), PER = +arg("per", 3), CHUNK = +arg("chunk", 250);
fs.mkdirSync(OUT, { recursive: true });

// xorshift128 for reproducibility
let s0 = SEED | 0 || 1, s1 = 0x9e3779b9, s2 = 0x243f6a88, s3 = 0xb7e15162;
function rnd() { let t = s0 ^ (s0 << 11); s0 = s1; s1 = s2; s2 = s3; s3 = (s3 ^ (s3 >>> 19) ^ (t ^ (t >>> 8))) >>> 0; return s3 / 4294967296; }
const ri = (n) => (rnd() * n) | 0, pick = (a) => a[ri(a.length)], chance = (p) => rnd() < p;

// ---- load corpus patterns (source, flags)
function loadPatterns(p) {
  const files = fs.statSync(p).isDirectory() ? fs.readdirSync(p).filter(f => f.endsWith(".json")).map(f => path.join(p, f)) : [p];
  const out = [];
  for (const f of files) {
    let j; try { j = JSON.parse(fs.readFileSync(f, "utf8")); } catch { continue; }
    for (const c of j.cases || j) if (c && typeof c.source === "string" && c.source.length && c.source.length < 400) out.push([c.source, c.flags || ""]);
  }
  return out;
}
const corpus = loadPatterns(IN);
console.error("corpus patterns:", corpus.length);

const parser = new RegExpParser({ ecmaVersion: 2025 });
function parse(src, flags) {
  try { return parser.parsePattern(src, 0, src.length, { unicode: flags.includes("u"), unicodeSets: flags.includes("v") }); } catch { return null; }
}
function nodes(ast) {
  const out = [];
  (function walk(n) {
    out.push(n);
    for (const k of ["alternatives", "elements"]) if (Array.isArray(n[k])) n[k].forEach(walk);
    if (n.element) walk(n.element);
  })(ast);
  return out;
}
const splice = (src, node, repl) => src.slice(0, node.start) + repl + src.slice(node.end);
const text = (src, node) => src.slice(node.start, node.end);
const QUANTS = ["*", "+", "?", "{2}", "{0,2}", "{1,3}", "{2,}", "*?", "+?", "??", "{0,2}?", "{1,3}?"];
const isTerm = (n) => ["Character", "CharacterSet", "CharacterClass", "Group", "CapturingGroup", "Backreference", "ExpressionCharacterClass", "ClassStringDisjunction"].includes(n.type);

// One mutation; returns [src, flags] or null.
function mutateOnce(src, flags, ast) {
  const all = nodes(ast);
  const terms = all.filter(isTerm);
  const groups = all.filter(n => n.type === "CapturingGroup");
  const quants = all.filter(n => n.type === "Quantifier");
  const alts = all.filter(n => n.type === "Alternative" && n.elements.length);
  const classes = all.filter(n => n.type === "CharacterClass" && !n.negate);
  const ops = [
    // wrap a term or a whole alternative in a lookaround / group / quantified group
    [4, () => { const n = pick(chance(0.6) ? terms : alts); if (!n) return null; const open = pick(["(?<=", "(?<!", "(?=", "(?!", "(?:", "(", "(?<m" + ri(9) + ">"]); return [splice(src, n, open + text(src, n) + ")" + (open.startsWith("(?<=") || open.startsWith("(?<!") || open === "(?=" || open === "(?!" ? "" : (chance(0.5) ? pick(QUANTS) : ""))), flags]; }],
    // prepend a lookbehind of a copy of some other term
    [3, () => { const n = pick(terms), m = pick(terms); if (!n || !m) return null; return [splice(src, n, pick(["(?<=", "(?<!"]) + text(src, m) + ")" + text(src, n)), flags]; }],
    // toggle / add / change quantifier
    [4, () => { if (quants.length && chance(0.6)) { const q = pick(quants); const body = text(src, q.element); return [splice(src, q, body + pick(QUANTS)), flags]; } const n = pick(terms); if (!n) return null; return [splice(src, n, (n.type === "Character" && text(src, n).length > 1 ? "(?:" + text(src, n) + ")" : text(src, n)) + pick(QUANTS)), flags]; }],
    // backreference to an existing (or newly created) group, placed after or before it
    [3, () => { if (groups.length) { const g = groups[ri(groups.length)]; const idx = groups.indexOf(g) + 1; const ref = g.name && chance(0.5) ? "\\k<" + g.name + ">" : "\\" + idx; const n = pick(terms); if (!n) return null; return [chance(0.7) ? splice(src, n, text(src, n) + ref + pick(["", "*?", "+", "{0,2}"])) : splice(src, n, ref + pick(["", "?", "*?"]) + text(src, n)), flags]; } const n = pick(terms); if (!n) return null; return [splice(src, n, "(" + text(src, n) + ")" + pick([".*?", "", "[^]*?"]) + "\\" + (groups.length + 1)), flags]; }],
    // alternation: add an alternative (copy of another alt, empty, or a literal from the pattern)
    [3, () => { const a = pick(alts); if (!a) return null; const other = pick(alts); return [splice(src, a, text(src, a) + "|" + pick([text(src, other), "", text(src, other).slice(0, 1 + ri(3)), "(?:)"])), flags]; }],
    // flags
    [3, () => { let f = flags; const op = pick(["+u", "+v", "u2v", "+i", "+g", "+y", "+m", "+s", "+d", "-i"]); if (op === "u2v") f = f.includes("u") ? f.replace("u", "v") : f + "v"; else if (op[0] === "+") { if (!f.includes(op[1])) f += op[1]; } else f = f.replace(op[1], ""); if (f.includes("u") && f.includes("v")) f = f.replace("u", ""); return f === flags ? null : [src, f]; }],
    // /v: turn a class into one with strings or a set operation
    [2, () => { const c = pick(classes); if (!c) return null; const inner = text(src, c).slice(1, -1); const lits = (src.match(/[a-zA-Z0-9]{1,3}/g) || ["ab", "a"]).slice(0, 6); const q = "\\q{" + [...new Set([pick(lits), pick(lits) + pick(lits), pick(lits), ""].filter((x, i) => i < 3 || chance(0.3)))].join("|") + "}"; const repl = pick(["[" + inner + q + "]", "[" + q + inner + "]", "[[" + inner + "]--" + q + "]", "[[" + inner + "]&&[" + inner + q + "]]", "[\\p{RGI_Emoji_Flag_Sequence}" + inner + "]"]); let f = flags.replace("u", ""); if (!f.includes("v")) f += "v"; return [splice(src, c, repl), f]; }],
    // astral / special characters spliced into literals
    [2, () => { const n = pick(terms.filter(t => t.type === "Character")); if (!n) return null; return [splice(src, n, text(src, n) + pick(["\u{1F600}", "\\u{1F600}", "\uD83D", "é", "ſ", "K", "\\u212A", "ß", "\u{10400}"])), flags.includes("u") || flags.includes("v") || chance(0.5) ? flags : flags + "u"]; }],
    // anchors / boundaries / modifiers
    [2, () => { const n = pick(terms); if (!n) return null; return [splice(src, n, pick(["^", "$", "\\b", "\\B", "(?i:", "(?-i:", "(?s:"]).replace(/\($/, "(") + text(src, n) + (["(?i:", "(?-i:", "(?s:"].some(x => false) ? "" : "")), flags]; }],
    [2, () => { const n = pick(terms); if (!n) return null; const m = pick(["(?i:", "(?s:", "(?m:", "(?-i:", "(?i-s:"]); return [splice(src, n, m + text(src, n) + ")"), m.includes("-i") && !flags.includes("i") ? flags + "i" : flags]; }],
  ];
  const total = ops.reduce((a, [w]) => a + w, 0);
  let r = rnd() * total;
  for (const [w, f] of ops) { if ((r -= w) < 0) { try { return f(); } catch { return null; } } }
  return null;
}

function compiles(src, flags) { try { new RegExp(src, flags); return true; } catch { return false; } }
function normFlags(f) { return [...new Set(f)].filter(c => "dgimsuvy".includes(c)).sort().join(""); }

function subjectsFor(src, flags, orig) {
  const subs = new Set();
  try { const r = new RandExp(new RegExp(orig[0], orig[1].replace(/[vd]/g, ""))); r.max = 6; for (let i = 0; i < 3; i++) subs.add(r.gen()); } catch {}
  try { const r = new RandExp(new RegExp(src, flags.replace(/[vd]/g, ""))); r.max = 6; for (let i = 0; i < 3; i++) subs.add(r.gen()); } catch {}
  const pool = (src.match(/[^\\()[\]{}|^$*+?.]{1,4}/g) || ["a"]);
  for (let i = 0; i < 3; i++) { let s = ""; const n = 1 + ri(6); for (let k = 0; k < n; k++) s += chance(0.7) ? pick(pool) : pick(["\n", " ", "😀", "\uD83D", "é", "K", "_", "1", "Z", "aa"]); subs.add(s); }
  for (const s of [...subs]) { if (s.length > 2 && chance(0.5)) subs.add(s.slice(1)); if (chance(0.4)) subs.add(s + pick(["x", "😀", "\n"])); }
  subs.add("");
  return [...subs].filter(s => s.length <= 300).slice(0, 10);
}

let made = 0, tried = 0, fileIdx = 0, buf = [];
const flush = () => { if (!buf.length) return; fs.writeFileSync(path.join(OUT, `mut-${String(fileIdx++).padStart(5, "0")}.jsonl`), buf.map(c => JSON.stringify(c)).join("\n") + "\n"); buf = []; };
while (made < COUNT && tried < COUNT * 20) {
  tried++;
  const orig = pick(corpus);
  let [src, flags] = orig; flags = normFlags(flags);
  let ast = parse(src, flags); if (!ast) continue;
  const steps = 1 + ri(PER);
  let ok = false;
  for (let k = 0; k < steps; k++) {
    const m = mutateOnce(src, flags, ast); if (!m) break;
    const [s2, f2raw] = m; const f2 = normFlags(f2raw);
    if (s2.length > 600 || !compiles(s2, f2)) break;
    const a2 = parse(s2, f2); if (!a2) break;
    src = s2; flags = f2; ast = a2; ok = true;
  }
  if (!ok || (src === orig[0] && flags === normFlags(orig[1]))) continue;
  buf.push({ p: src, f: flags, s: subjectsFor(src, flags, orig), o: orig[0] });
  made++;
  if (buf.length >= CHUNK) flush();
}
flush();
console.error(`wrote ${made} mutated cases (${tried} attempts) into ${OUT} (${fileIdx} files)`);
