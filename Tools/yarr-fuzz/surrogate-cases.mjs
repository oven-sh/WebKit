#!/usr/bin/env node
// Cases for where a /u or /v match may start: patterns whose alternatives can match without
// consuming a character (\B, lookbehinds, negative lookaheads, optional terms), alone or wrapped as
// /.*X.*/ (the DotStarEnclosure rewrite), over subjects with surrogate pairs and lone surrogates.
// The offset between the two halves of a pair is where the engines can disagree. Emits case files
// that regex-fuzz.js can replay (`profile file:<path>`): {p, f, s:[subjects]} per line. The same
// arguments give the same file.
//
// usage: node surrogate-cases.mjs <count> <plain|dotstar> <out.jsonl>
import fs from "node:fs";

const [count, kind, out] = [Number(process.argv[2]), process.argv[3], process.argv[4]];
if (!Number.isInteger(count) || count <= 0 || !["plain", "dotstar"].includes(kind) || !out) {
    console.error("usage: node surrogate-cases.mjs <count> <plain|dotstar> <out.jsonl>");
    process.exit(1);
}

let seed = 0x9e3779b9;
const rnd = (n) => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed % n; };
const pick = (a) => a[rnd(a.length)];

// Terms that match without consuming a character, terms that may, and terms that must consume one.
const zero = ["\\B", "\\b", "(?<=\\uD83D)", "(?<!\\uD83D)", "(?<=\\uDE00)", "(?<!\\uDE00)", "(?![^])", "(?!.)", "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?<=\\p{Cs})", "(?<=[^a])", "(?<![^])", "(?=\\uDE00)", "(?!\\uDE00)", "(?=\\B)", "(?<=\\B)", "(?:)", "$", "^", "(?<=\\u{1F600})", "(?<!\\u{1F600})", "(?<=\\uD83D\\uDE00)"];
const optional = ["a?", "a*", "q?", "(?:a|)", "(?:|a)", "a??", "\\uDE00?", "\\uD83D?", "\\u{1F600}?", ".?", "[^a]*", "(?:ab)?", "a{0,2}", "\\1", "(?:\\B|a)", "(?:q|\\b)", "\\s*", "\\W*", "\\p{Cs}?"];
const consume = ["a", "q", "qq", "qqq", "ab", ".", "[^a]", "\\uDE00", "\\uD83D", "\\u{1F600}", "\\p{Cs}", "[\\uDC00-\\uDFFF]", "[\\uD800-\\uDBFF]", "\\W", "\\w", "\\S", "[^]", "a+", "\\n", "-"];
const E = "\u{1F600}", L = "\uD83D", T = "\uDE00";
const pieces = ["a", "a", "b", "q", "-", " ", "\n", E, E, E, L, T, "\u{10428}", "\u0101", "_", "\u2028"];

function alternative() {
    const shape = rnd(10);
    if (shape < 4)
        return pick(zero) + (rnd(3) ? "" : pick(zero));
    if (shape < 6)
        return pick(optional) + (rnd(2) ? pick(zero) : "");
    if (shape < 7)
        return pick(zero) + pick(optional);
    if (shape < 9)
        return (rnd(3) ? "" : pick(zero)) + pick(consume) + (rnd(3) ? "" : pick(optional));
    return pick(consume) + pick(consume);
}

function body() {
    const n = [1, 1, 2, 2, 2, 3, 3, 4][rnd(8)];
    const alts = [];
    for (let i = 0; i < n; i++)
        alts.push(alternative());
    // Make sure a good share of the bodies can match empty.
    if (rnd(4) && !alts.some((a) => /^(\\[Bb]|\(\?|\$|\^|[^|]*[?*])/.test(a)))
        alts[rnd(alts.length)] = pick(zero);
    return alts.join("|");
}

function subject() {
    const n = [0, 1, 2, 3, 4, 5, 6, 8, 12][rnd(9)];
    let s = "";
    for (let i = 0; i < n; i++)
        s += pick(pieces);
    return s;
}

const flagSets = ["u", "u", "u", "v", "gu", "gu", "gv", "mu", "su", "iu", "gmu", "gsu", "giu", "du", "yu", "gyu", "msu", "gmsv"];
const lines = [];
const seen = new Set();
// The patterns are distinct and their number is finite: give up when new ones stop coming.
let attemptsLeft = 100 * count + 10000;
while (lines.length < count) {
    if (!attemptsLeft--) {
        console.error("only " + lines.length + " distinct " + kind + " patterns after " + (100 * count + 10000) + " attempts");
        process.exit(1);
    }
    let p = body();
    let f = pick(flagSets);
    if (kind === "dotstar") {
        const lead = rnd(5) ? ".*" : pick([".*?", "[^]*", ".+"]);
        p = (rnd(2) ? "^" : "") + lead + (rnd(3) ? p.split("|")[0] : "(?:" + p + ")") + ".*" + (rnd(2) ? "$" : "");
        f = f.replace("y", "");
    }
    try { new RegExp(p, f); } catch { continue; }
    const key = p + "/" + f;
    if (seen.has(key))
        continue;
    seen.add(key);
    const subjects = [];
    for (let i = 0; i < 4; i++)
        subjects.push(subject());
    subjects.push("a" + E + "a", "a" + E, E + "a" + E, "ab" + E + "\n" + E + E + "q");
    lines.push(JSON.stringify({ p, f, s: subjects }));
}
fs.writeFileSync(out, lines.join("\n") + "\n");
console.log("wrote", lines.length, "patterns to", out);
