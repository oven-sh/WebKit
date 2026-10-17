// Differential RegExp fuzzer. Runs under jsc (print/$vm) or node (console.log).
// Usage: jsc --useDollarVM=1 regex-fuzz.js -- <seed> <count> [profile]
//        node regex-fuzz.js <seed> <count> [profile]
// Emits one line per case: <idx>\t<JSON {p,f,s16,ops...}>. Compare line-by-line across engines/tiers.
"use strict";

const OUT = typeof print !== "undefined" ? print : (s) => console.log(s);
const ARGV = typeof arguments !== "undefined" && Array.isArray(arguments) ? arguments : (typeof process !== "undefined" ? process.argv.slice(2) : []);
const HAVE_VM = typeof $vm !== "undefined" && typeof $vm.make16BitStringIfPossible === "function";
const to16 = HAVE_VM ? (s) => $vm.make16BitStringIfPossible(s) : (s) => s;
const IS_NODE = typeof process !== "undefined" && !!(process.versions && process.versions.node);

let SEED = (ARGV[0] | 0) || 1;
const COUNT = (ARGV[1] | 0) || 1000;
const PROFILE = ARGV[2] || "mixed";

// xorshift128+ -ish PRNG, deterministic across engines.
let s0 = SEED * 2654435761 >>> 0, s1 = (SEED ^ 0x9e3779b9) >>> 0, s2 = 0x12345678, s3 = 0xcafebabe;
function rnd() {
    let t = s1 << 9, r = s0 * 5; r = (r << 7 | r >>> 25) * 9;
    s2 ^= s0; s3 ^= s1; s1 ^= s2; s0 ^= s3; s2 ^= t; s3 = s3 << 11 | s3 >>> 21;
    return (r >>> 0) / 4294967296;
}
for (let i = 0; i < 20; ++i) rnd();
const ri = (n) => (rnd() * n) | 0;               // [0,n)
const rr = (a, b) => a + ri(b - a + 1);          // [a,b]
const chance = (p) => rnd() < p;
const pick = (arr) => arr[ri(arr.length)];
function pickW(entries) { // [[weight, value],...]
    let total = 0; for (const e of entries) total += e[0];
    let x = rnd() * total;
    for (const e of entries) { if ((x -= e[0]) < 0) return e[1]; }
    return entries[entries.length - 1][1];
}

// ---------------------------------------------------------------------------
// Alphabets
const ASCII_LETTERS = "abcde";
const ASCII_MORE = "abcdexyzkq";
const DIGITS = "0129";
const PUNCT = " -_!\"/:";
const UPPER = "ABKS";
const BMP_NONASCII = ["é", "ā", "K" /* Kelvin */, "ſ" /* long s */, "İ", "ẞ", "Σ", "ς", "ﬀ", "“"];
const ASTRAL = ["\u{1F600}", "\u{10428}", "\u{10400}", "\u{1D49C}", "\u{1F602}"];
const LONE = ["\uD83D", "\uDE00", "\uD801", "\uDC28"];
const EMOJI_SEQS = ["\u{1F1FA}\u{1F1F8}", "\u{1F1FA}", "\u{1F1E6}\u{1F1FA}", "1\uFE0F\u20E3", "1\u20E3", "#\uFE0F\u20E3", "\u{1F44B}", "\u{1F44B}\u{1F3FB}", "\u{1F44B}\u{1F3FB}\u{1F3FB}", "\u{1F468}\u200D\u{1F469}\u200D\u{1F467}\u200D\u{1F466}", "\u{1F468}\u200D\u{1F469}", "\u{1F3F4}\u{E0067}\u{E0062}\u{E0065}\u{E006E}\u{E0067}\u{E007F}", "\u{1F3F4}", "\u263A\uFE0F", "\u263A", "\u{1F642}", "\u{1F426}\u200D\u{1F525}", "αβγ", "日本", "\u{10400}"];

function escapeLit(ch, inClass, flags) {
    // Return pattern source for literal ch.
    const cp = ch.codePointAt(0);
    if (/[\\^$.*+?()[\]{}|\/]/.test(ch) || (inClass && /[-\]\\^]/.test(ch)) || (flags.v && inClass && /[()[\]{}\/\-\\|!#$%&*+,.:;<=>?@^`~]/.test(ch)))
        return "\\" + ch;
    if (cp > 0xffff) {
        return pickW([[3, ch], [2, "\\u{" + cp.toString(16) + "}"], [(flags.u || flags.v) ? 0 : 1, "\\u" + ch.charCodeAt(0).toString(16) + "\\u" + ch.charCodeAt(1).toString(16)]]);
    }
    if (cp >= 0xd800 && cp <= 0xdfff)
        return pickW([[1, ch], [2, "\\u" + cp.toString(16)]]);
    if (cp > 0x7e || cp < 0x20)
        return pickW([[2, ch], [1, "\\u" + cp.toString(16).padStart(4, "0")]]);
    return ch;
}

// ---------------------------------------------------------------------------
// Pattern generator. ctx: {flags, depth, groups (count so far), names[], inLookbehind, budget}
function genLiteralChar(ctx) {
    const prof = ctx.profile;
    return pickW([
        [10, () => pick(ASCII_LETTERS)],
        [4, () => pick(ASCII_MORE)],
        [2, () => pick(DIGITS)],
        [2, () => pick(PUNCT)],
        [2, () => pick(UPPER)],
        [prof.unicodeWeight, () => pick(BMP_NONASCII)],
        [prof.unicodeWeight * 1.5, () => pick(ASTRAL)],
        [prof.unicodeWeight * 0.5, () => pick(LONE)],
    ])();
}

function genClass(ctx) {
    const f = ctx.flags;
    const neg = chance(0.25);
    let items = [];
    const n = rr(1, 4);
    for (let i = 0; i < n; ++i) {
        items.push(pickW([
            [6, () => escapeLit(genLiteralChar(ctx), true, f)],
            [3, () => pick(["a-e", "a-z", "0-9", "A-Z", "x-z"])],
            [2, () => pick(["\\d", "\\w", "\\s", "\\D", "\\W", "\\S"])],
            [(f.u || f.v) ? 1.5 : 0, () => pick(["\\p{L}", "\\p{Lu}", "\\P{L}", "\\p{N}", "\\p{Extended_Pictographic}", "\\P{Number}"])],
            [(f.u || f.v) ? 1 : 0, () => "\\u{1F600}-\\u{1F64F}"],
            [(f.u || f.v) ? 0.7 : 0, () => "\\u{10400}-\\u{1044F}"],
        ])());
    }
    if (f.v && chance(0.3) && items.length >= 2) {
        // set operation: wrap each item as a nested class when needed
        const op = pick(["&&", "--"]);
        const wrap = (it) => (it.length > 2 && !it.startsWith("\\") ? "[" + it + "]" : (it.includes("-") && !it.startsWith("\\") ? "[" + it + "]" : it));
        return "[" + (neg ? "^" : "") + items.map(wrap).join(op) + "]";
    }
    if (f.v && chance(0.15 + (ctx.profile.qWeight || 0)))
        return genStringClass(ctx, items);
    return "[" + (neg ? "^" : "") + items.join("") + "]";
}

// A /v class holding strings: \q{...} with few or many members (shared prefixes and suffixes,
// empties, astral, case variants), optionally mixed with ranges/escapes, set operations, and
// properties of strings. Never negated (a negated class may not contain strings).
const STRING_PROPS = ["\\p{RGI_Emoji_Flag_Sequence}", "\\p{Emoji_Keycap_Sequence}", "\\p{RGI_Emoji_Tag_Sequence}", "\\p{RGI_Emoji_Modifier_Sequence}", "\\p{Basic_Emoji}", "\\p{RGI_Emoji_ZWJ_Sequence}", "\\p{RGI_Emoji}"];
function genStringClass(ctx, items) {
    const f = ctx.flags;
    const n = pickW([[3, rr(1, 4)], [4, rr(5, 12)], [2, rr(12, 40)]]);
    let words = genWordList(ctx, n);
    if (chance(0.3)) words = words.map((w) => chance(0.3) ? w.toUpperCase() : w);
    if (chance(0.3) && words.length) words.push(words[0] + words[0]); // a member that extends another
    if (chance(0.25) && words.length > 1) words.push(words[1].slice(1)); // ... and a suffix of one
    const q = "\\q{" + words.join("|") + "}";
    const prop = () => pickW([[6, STRING_PROPS[0]], [4, STRING_PROPS[1]], [2, STRING_PROPS[2]], [2, STRING_PROPS[3]], [1.5, STRING_PROPS[4]], [0.7, STRING_PROPS[5]], [0.5, STRING_PROPS[6]]]);
    return pickW([
        [5, () => "[" + q + "]"],
        [4, () => "[" + items.join("") + q + "]"],
        [2, () => "[" + q + items.join("") + "]"],
        [2, () => "[" + prop() + "]"],
        [2, () => prop()],
        [1.5, () => "[" + prop() + q + "]"],
        [1.5, () => "[[" + q + "]--" + "\\q{" + (words[0] || "a") + "}]"],
        [1, () => "[" + prop() + "--" + "\\q{" + pick(["\u{1F1FA}\u{1F1F8}", "1\uFE0F\u20E3", "\u{1F44B}\u{1F3FB}"]) + "}]"],
        [1, () => "[[" + q + "]&&[" + "\\q{" + words.slice(0, 2).join("|") + "}" + items.join("") + "]]"],
        [1, () => "[" + prop() + "&&" + prop() + "]"],
    ])();
}

function genQuantifier(ctx) {
    const q = pickW([
        [5, "?"], [5, "*"], [5, "+"],
        [2, "{2}"], [2, "{0,2}"], [2, "{1,3}"], [1, "{2,}"], [1, "{0}"], [1, "{1}"], [0.5, "{3,5}"], [0.3, "{0,1}"],
    ]);
    return q + (chance(0.25) ? "?" : "");
}

function genAtom(ctx) {
    const f = ctx.flags;
    const deep = ctx.depth >= ctx.profile.maxDepth || ctx.budget <= 0;
    ctx.budget--;
    return pickW([
        [14, () => escapeLit(genLiteralChar(ctx), false, f)],
        [3, () => "."],
        [4, () => genClass(ctx)],
        [3, () => pick(["\\d", "\\w", "\\s", "\\D", "\\W", "\\S", "\\b", "\\B"])],
        [2, () => pick(["^", "$"])],
        [(f.u || f.v) ? 1 : 0, () => pick(["\\p{L}", "\\P{L}", "\\p{Lu}", "\\p{Extended_Pictographic}"])],
        [ctx.groups > 0 ? 3 : 0.7, () => "\\" + rr(1, Math.max(1, ctx.groups + (chance(0.3) ? 1 : 0)))],
        [ctx.names.length > 0 ? 2 : 0, () => "\\k<" + pick(ctx.names) + ">"],
        [deep ? 0 : 5, () => genGroup(ctx, "(")],
        [deep ? 0 : 4, () => genGroup(ctx, "(?:")],
        [deep ? 0 : 1.5, () => genNamedGroup(ctx)],
        [deep ? 0 : 2, () => genGroup(ctx, pick(["(?=", "(?!"]))],
        [deep ? 0 : ctx.profile.lookbehindWeight, () => genGroup(ctx, pick(["(?<=", "(?<=", "(?<!"]))],
        [deep ? 0 : ctx.profile.altWeight, () => genWideAlternation(ctx)],
    ])();
}

function genNamedGroup(ctx) {
    const name = "n" + ctx.names.length + pick(["", "a", "b"]);
    // Duplicate names only allowed in different alternatives; keep unique to avoid excess syntax errors.
    if (ctx.names.includes(name)) return genGroup(ctx, "(");
    ctx.groups++;
    ctx.names.push(name);
    ctx.depth++;
    const body = genDisjunction(ctx, rr(1, 2), rr(0, 3));
    ctx.depth--;
    return "(?<" + name + ">" + body + ")";
}

function genGroup(ctx, open) {
    if (open === "(") ctx.groups++;
    ctx.depth++;
    const wasLB = ctx.inLookbehind;
    if (open.startsWith("(?<")) ctx.inLookbehind = true;
    const body = genDisjunction(ctx, pickW([[6, 1], [3, 2], [1, 3], [0.5, 4]]), rr(0, 4));
    ctx.inLookbehind = wasLB;
    ctx.depth--;
    return open + body + ")";
}

// Words sharing prefixes -> exercises prefix factoring + dispatch.
function genWordList(ctx, n) {
    const f = ctx.flags;
    const ww = ctx.profile.wideAlphaWeight || 0;
    const alpha = pickW([[5, ASCII_LETTERS], [3, ASCII_MORE], [1, ASCII_MORE + "AB"], [ctx.profile.unicodeWeight, ASCII_LETTERS + "\u{1F600}é"],
        [ww, "αβγδε"], [ww, "αβγab"], [ww * 0.7, "日本語中文"], [ww * 0.7, "\u{1F600}\u{1F601}\u{1F602}\u{10400}"], [ww * 0.5, "aé\u{1F600}日β"], [ww * 0.3, "\u{1F1FA}\u{1F1F8}\u{1F1E6}"]]);
    const alphaArr = [...alpha];
    const words = [];
    const mkword = (len) => { let w = ""; for (let i = 0; i < len; ++i) w += pick(alphaArr); return w; };
    let base = mkword(rr(1, 3));
    for (let i = 0; i < n; ++i) {
        const style = pickW([[4, "share"], [3, "new"], [2, "extend"], [1, "empty"], [1, "same"]]);
        let w;
        if (style === "share") w = base.slice(0, rr(1, [...base].length)) + mkword(rr(0, 3));
        else if (style === "extend") w = (words.length ? pick(words) : base) + mkword(rr(1, 2));
        else if (style === "same") w = words.length ? pick(words) : base;
        else if (style === "empty") w = chance(0.3) ? "" : mkword(1);
        else { base = mkword(rr(1, 3)); w = base; }
        words.push(w);
    }
    return words.map((w) => [...w].map((c) => escapeLit(c, false, f)).join(""));
}

function genWideAlternation(ctx) {
    const n = pickW([[3, rr(2, 5)], [4, rr(4, 9)], [3, rr(8, 20)], [1, rr(16, 40)]]);
    let alts = genWordList(ctx, n);
    // Decorate some alternatives with non-literal tails/heads so factoring meets mixed shapes.
    alts = alts.map((a) => {
        if (chance(0.15)) return a + genQuantifiedAtom(ctx);
        if (chance(0.08)) return genQuantifiedAtom(ctx) + a;
        if (chance(0.06) && a.length) return "(" + (ctx.groups++, a) + ")";
        if (chance(0.05)) return a + pick(["\\b", "$", "(?=" + escapeLit(genLiteralChar(ctx), false, ctx.flags) + ")"]);
        if (chance(0.04)) return "^" + a;
        return a;
    });
    const open = pickW([[5, "(?:"], [3, "("], [ctx.depth === 0 ? 4 : 0, ""], [0.5, "(?="], [ctx.profile.lookbehindWeight * 0.3, "(?<="], [0.3, "(?<!"], [0.3, "(?!"]]);
    if (open === "") return alts.join("|"); // caller may embed at top level
    if (open === "(") ctx.groups++;
    const q = chance(0.25) && !open.startsWith("(?=") && !open.startsWith("(?!") && !open.startsWith("(?<") ? genQuantifier(ctx) : "";
    return open + alts.join("|") + ")" + q;
}

function genQuantifiedAtom(ctx) {
    let a = genAtom(ctx);
    if (chance(0.3)) {
        // Lookaround assertions can't be quantified in u/v mode; and ^/$ \b can't be quantified at all.
        const isAssert = /^\(\?<?[=!]/.test(a) || a === "^" || a === "$" || a === "\\b" || a === "\\B";
        if (!isAssert || (!(ctx.flags.u || ctx.flags.v) && a.startsWith("(?") && chance(0.3)))
            a += genQuantifier(ctx);
    }
    return a;
}

function genAlternative(ctx, nTerms) {
    let s = "";
    for (let i = 0; i < nTerms; ++i) s += genQuantifiedAtom(ctx);
    return s;
}

function genDisjunction(ctx, nAlts, nTerms) {
    const alts = [];
    for (let i = 0; i < nAlts; ++i) alts.push(genAlternative(ctx, i === 0 ? nTerms : rr(0, nTerms + 1)));
    return alts.join("|");
}

function genFlags(profile) {
    const f = {};
    if (chance(0.35)) f.i = true;
    if (chance(0.2)) f.m = true;
    if (chance(0.25)) f.s = true;
    const uv = pickW([[profile.nonUnicodeWeight, ""], [profile.uWeight, "u"], [profile.vWeight, "v"]]);
    if (uv) f[uv] = true;
    if (chance(0.15)) f.y = true;
    if (chance(0.3)) f.g = true;
    if (chance(0.1)) f.d = true;
    return f;
}
const flagStr = (f) => ["d", "g", "i", "m", "s", "u", "v", "y"].filter((k) => f[k]).join("");

function genPattern(profile) {
    const flags = genFlags(profile);
    const ctx = { flags, depth: 0, groups: 0, names: [], inLookbehind: false, budget: profile.budget, profile };
    let src;
    const shape = pickW(profile.shapes);
    switch (shape) {
    case "lookbehind": {
        // something (?<=...) something
        const pre = chance(0.5) ? genAlternative(ctx, rr(0, 2)) : "";
        const lb = genGroup(ctx, pick(["(?<=", "(?<=", "(?<!"]));
        const post = genAlternative(ctx, rr(0, 3));
        src = chance(0.5) ? pre + lb + post : pre + post + lb;
        if (chance(0.2)) src += "|" + genAlternative(ctx, rr(1, 3));
        break;
    }
    case "widealt": {
        const pre = chance(0.3) ? genAlternative(ctx, 1) : "";
        const post = chance(0.4) ? genAlternative(ctx, rr(1, 2)) : "";
        const wa = genWideAlternation(ctx);
        src = pre + wa + post;
        break;
    }
    case "topalt": {
        // top-level keyword list, possibly disjoint first chars
        const n = pickW([[3, rr(2, 6)], [3, rr(6, 12)], [2, rr(12, 24)], [1, rr(24, 60)]]);
        let alts = genWordList(ctx, n);
        if (chance(0.3)) alts = alts.map((a) => chance(0.2) ? a + genQuantifiedAtom(ctx) : a);
        if (chance(0.15)) alts = alts.map((a) => chance(0.3) ? "(" + (ctx.groups++, a) + ")" : a);
        src = alts.join("|");
        if (chance(0.2)) src = "\\b(?:" + src + ")\\b";
        else if (chance(0.15)) src = "^(?:" + src + ")$";
        break;
    }
    case "bm": {
        // literal-ish run to attract Boyer-Moore, with optional class positions
        let s = "";
        const n = rr(2, 7);
        for (let i = 0; i < n; ++i) s += pickW([[6, () => escapeLit(genLiteralChar(ctx), false, flags)], [2, () => genClass(ctx)], [1, () => "."], [1, () => pick(["\\d", "\\w", "\\s"])]])();
        src = (chance(0.3) ? genAlternative(ctx, 1) : "") + s + (chance(0.5) ? genAlternative(ctx, rr(1, 2)) : "");
        if (chance(0.3)) src = src + "|" + genAlternative(ctx, rr(1, 3));
        break;
    }
    case "dotstar": {
        // .*X.* enclosure shapes (with/without anchors), which take a dedicated fast path.
        const dot = chance(0.7) ? ".*" : pick([".*?", "[^]*", "[\\s\\S]*", ".+"]);
        src = (chance(0.5) ? "^" : "") + dot + genAlternative(ctx, rr(1, 3)) + (chance(0.8) ? ".*" : dot) + (chance(0.5) ? "$" : "");
        if (chance(0.6)) flags.g = true;
        if (chance(0.4)) flags.s = true;
        if (chance(0.4)) flags.m = true;
        break;
    }
    default:
        src = genDisjunction(ctx, pickW([[6, 1], [3, 2], [1, 3]]), rr(1, 5));
    }
    return { src, flags, ctx };
}

// ---------------------------------------------------------------------------
// Subject generator: draw from the pattern's literal characters plus noise.
function literalPool(src) {
    const pool = [];
    // crude: take non-meta chars from source, decode \u{...} and \uXXXX
    src.replace(/\\u\{([0-9a-fA-F]+)\}|\\u([0-9a-fA-F]{4})|\\(.)|([^\\])/gsu, (m, a, b, c, d) => {
        if (a) pool.push(String.fromCodePoint(parseInt(a, 16) > 0x10ffff ? 0x41 : parseInt(a, 16)));
        else if (b) pool.push(String.fromCharCode(parseInt(b, 16)));
        else if (c) { if (!/[dDwWsSbBpPkqu0-9nrtfv]/.test(c)) pool.push(c); }
        else if (d && !/[()[\]{}|?*+^$.<>=!:,\-&]/.test(d)) pool.push(d);
        return "";
    });
    return pool.length ? pool : ["a", "b"];
}

function genSubject(pat, profile) {
    const pool = literalPool(pat.src);
    const len = pickW([[1, 0], [3, rr(1, 4)], [6, rr(3, 12)], [3, rr(10, 30)], [profile.longSubjectWeight, rr(60, 400)], [profile.longSubjectWeight * 0.2, rr(1000, 3000)]]);
    let s = "";
    for (let i = 0; i < len; ++i) {
        s += pickW([
            [12, () => pick(pool)],
            [3, () => pick(ASCII_MORE)],
            [1, () => pick(PUNCT)],
            [1, () => pick(UPPER)],
            [0.7, () => "\n"],
            [profile.unicodeWeight * 0.7, () => pick(BMP_NONASCII)],
            [profile.unicodeWeight, () => pick(ASTRAL)],
            [profile.unicodeWeight * 0.5, () => pick(LONE)],
            [pat.flags.i ? 2 : 0.2, () => pick(pool).toUpperCase()],
        ])();
    }
    // Sometimes plant words from a wide alternation verbatim.
    if (profile.emojiSubjects && chance(profile.emojiSubjects)) {
        const at = ri(s.length + 1);
        s = s.slice(0, at) + pick(EMOJI_SEQS) + (chance(0.5) ? pick(EMOJI_SEQS) : "") + s.slice(at);
    }
    if (chance(0.4)) {
        const words = pat.src.split(/[|{}\[\]()]/).filter((w) => /^[\p{L}\p{N}é\u{1F600}-\u{1F64F}\u{10400}]+$/u.test(w));
        if (words.length) {
            const at = ri(s.length + 1);
            s = s.slice(0, at) + pick(words) + (chance(0.5) ? pick(words) : "") + s.slice(at);
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// Execution + canonical result encoding.
function encMatch(m, withIndices) {
    if (m === null) return null;
    const out = [m.index];
    for (let i = 0; i < m.length; ++i) out.push(m[i] === undefined ? "~U" : m[i]);
    if (m.groups) { const g = {}; for (const k of Object.keys(m.groups)) g[k] = m.groups[k] === undefined ? "~U" : m.groups[k]; out.push(g); }
    if (withIndices && m.indices) out.push(JSON.stringify(m.indices) + (m.indices.groups ? JSON.stringify(m.indices.groups) : ""));
    return out;
}

function safeStr(s) {
    // Make output ASCII-only and lossless for lone surrogates.
    let o = "";
    for (let i = 0; i < s.length; ++i) {
        const c = s.charCodeAt(i);
        if (c >= 0x20 && c < 0x7f && c !== 0x5c) o += s[i];
        else o += "\\u" + c.toString(16).padStart(4, "0");
    }
    return o;
}

function runCase(idx, pat, subjectRaw, use16, profile) {
    const fs = flagStr(pat.flags);
    const rec = { p: safeStr(pat.src), f: fs, s: safeStr(subjectRaw), w: use16 ? 16 : 8 };
    let re;
    try {
        re = new RegExp(pat.src, fs);
    } catch (e) {
        rec.err = IS_NODE ? "SyntaxError" : String(e.name);
        return rec;
    }
    const subject = use16 ? to16(subjectRaw) : subjectRaw;
    const r = {};
    try {
        re.lastIndex = 0;
        r.e = encMatch(re.exec(subject), !!pat.flags.d);
        r.li = re.lastIndex;
        re.lastIndex = 0;
        r.t = re.test(subject);
        // lastIndex sweep (exercises sticky / surrogate-pair start adjustment / BM search restarts)
        if (profile.sweep && subject.length <= 40 && chance(profile.sweep)) {
            const sw = [];
            for (let k = 0; k <= subject.length + 1; ++k) {
                re.lastIndex = k;
                const m = re.exec(subject);
                sw.push(m ? m.index + ":" + m[0].length : "-");
            }
            r.sw = sw.join(",");
        }
        // Metamorphic checks (intra-engine oracle): semantically-neutral rewrites of the
        // pattern, and the same subject at the other string width, must give identical
        // exec results. Rewrites push the pattern across the factoring / fold / dispatch
        // thresholds and the containsLookbehind code paths without changing its language.
        if (profile.meta && chance(profile.meta)) {
            const baseE = JSON.stringify(r.e) + "|" + r.t;
            const meta = {};
            const pads = [];
            for (let k = 0; k < 9; ++k) pads.push("\\u0000" + pick(["q", "z", "qz", "zq", "qq"]) + k); // never match: subjects never contain U+0000
            const variants = {
                grp: "(?:" + pat.src + ")",
                dead: pat.src + "|(?!)",
                pad: pat.src + "|" + pads.join("|"),
                padgrp: "(?:" + pat.src + "|" + pads.join("|") + ")",
                lb: "(?<![^\\s\\S])(?:" + pat.src + ")",
            };
            for (const name in variants) {
                let vre;
                try { vre = new RegExp(variants[name], fs); } catch (e) { continue; } // e.g. pattern too large
                vre.lastIndex = 0;
                const ve = encMatch(vre.exec(subject), !!pat.flags.d);
                vre.lastIndex = 0;
                const vt = vre.test(subject);
                const enc = JSON.stringify(ve && ve.map((v) => typeof v === "string" ? v : v)) + "|" + vt;
                if (enc !== baseE) meta[name] = safeStr(enc.slice(0, 200));
            }
            // Other width, same content (only when the content fits Latin-1 so both widths exist).
            if (HAVE_VM && !/[^\0-\xff]/.test(subjectRaw)) {
                const other = use16 ? subjectRaw : to16(subjectRaw); // flat 8-bit literal vs forced 16-bit
                re.lastIndex = 0;
                const oe = encMatch(re.exec(other), !!pat.flags.d);
                re.lastIndex = 0;
                const ot = re.test(other);
                const enc = JSON.stringify(oe) + "|" + ot;
                if (enc !== baseE) meta.width = safeStr(enc.slice(0, 200));
            }
            if (Object.keys(meta).length) r.META = meta;
            re.lastIndex = 0;
        }
        const op = ri(8);
        if (op === 6) {
            re.lastIndex = 0;
            const rp = subject.replace(re, "");
            r.r0 = safeStr(rp.length > 300 ? rp.length + ":" + rp.slice(0, 100) : rp);
        }
        if (op === 7 && pat.src.length < 400 && subject.length < 200 && !/[\n\r\u2028\u2029\/]/.test(pat.src)) {
            // Literal regexp + literal subject inside a hot function: reaches DFG's
            // strength-reduction constant folding of exec/test/replace (jsc) when tiering is eager.
            let fn = null;
            try { fn = new Function("return [" + JSON.stringify(subjectRaw) + ".replace(/" + pat.src + "/" + fs + ", " + (chance(0.5) ? '""' : '"<$1>"') + "), /" + pat.src + "/" + fs + ".test(" + JSON.stringify(subjectRaw) + "), (/" + pat.src + "/" + fs.replace(/[gy]/g, "") + ".exec(" + JSON.stringify(subjectRaw) + ")||[]).join('|')]"); } catch (e) { fn = null; }
            if (fn) {
                let last;
                for (let k = 0; k < 60; ++k) last = fn();
                r.fd = safeStr(JSON.stringify(last).slice(0, 300));
            }
        }
        if (op === 0 || pat.flags.g) {
            re.lastIndex = 0;
            const all = subject.match(re);
            r.m = all === null ? null : all.map((x) => x === undefined ? "~U" : safeStr(String(x)));
            if (r.m && r.m.length > 50) r.m = [r.m.length, r.m.slice(0, 10)];
        }
        if (op === 1) {
            re.lastIndex = 0;
            const rp = subject.replace(re, "[$&|$1]");
            r.r = safeStr(rp.length > 300 ? rp.length + ":" + rp.slice(0, 100) : rp);
        }
        if (op === 2) {
            const sp = subject.split(re, 20);
            r.sp = sp.map((x) => x === undefined ? "~U" : safeStr(x));
        }
        if (op === 3 && !pat.flags.g && !pat.flags.y) {
            re.lastIndex = 0;
            r.sr = subject.search(re);
        }
        if (op === 4 && pat.flags.g) {
            re.lastIndex = 0;
            let n = 0, last = null;
            for (const m of subject.matchAll(re)) { n++; last = m.index + ":" + safeStr(m[0]); if (n > 60) break; }
            r.ma = n + "@" + last;
        }
        // Hot loop to tickle DFG RegExpTest/RegExpExec inlining + JIT tier of the regexp itself.
        if (op === 5 || chance(profile.hotLoop)) {
            let cnt = 0;
            const lim = subject.length > 200 ? 30 : 200;
            for (let k = 0; k < lim; ++k) { re.lastIndex = 0; if (re.test(subject)) cnt++; }
            let cnt2 = 0;
            for (let k = 0; k < (lim >> 1); ++k) { re.lastIndex = 0; const m = re.exec(subject); if (m) cnt2 += m.index + m[0].length; }
            r.h = cnt + "/" + cnt2;
        }
    } catch (e) {
        r.x = IS_NODE ? "ERR" : String(e && e.name) + ":" + String(e && e.message).slice(0, 80);
    }
    // stringify match arrays with safeStr on strings
    if (r.e) r.e = r.e.map((v) => typeof v === "string" ? safeStr(v) : (v && typeof v === "object" && !Array.isArray(v) ? Object.fromEntries(Object.entries(v).map(([k, x]) => [k, safeStr(String(x))])) : v));
    rec.r = r;
    return rec;
}

// ---------------------------------------------------------------------------
const PROFILES = {
    mixed:      { shapes: [[3, "generic"], [4, "lookbehind"], [3, "widealt"], [3, "topalt"], [2, "bm"], [1.5, "dotstar"]], lookbehindWeight: 3, altWeight: 2, unicodeWeight: 1.2, uWeight: 3, vWeight: 1, nonUnicodeWeight: 5, maxDepth: 3, budget: 30, longSubjectWeight: 1, sweep: 0.25, hotLoop: 0.1, p16: 0.4, meta: 0.35 },
    lookbehind: { shapes: [[1, "generic"], [8, "lookbehind"], [1, "widealt"]], lookbehindWeight: 6, altWeight: 2, unicodeWeight: 2, uWeight: 5, vWeight: 1.5, nonUnicodeWeight: 4, maxDepth: 4, budget: 35, longSubjectWeight: 0.7, sweep: 0.5, hotLoop: 0.1, p16: 0.5, meta: 0.3 },
    alt:        { shapes: [[1, "generic"], [1, "lookbehind"], [5, "widealt"], [5, "topalt"]], lookbehindWeight: 1.5, altWeight: 5, unicodeWeight: 0.8, uWeight: 2, vWeight: 0.7, nonUnicodeWeight: 6, maxDepth: 3, budget: 40, longSubjectWeight: 1, sweep: 0.2, hotLoop: 0.25, p16: 0.35, meta: 0.5 },
    unicode:    { shapes: [[3, "generic"], [4, "lookbehind"], [2, "widealt"], [2, "topalt"], [3, "bm"]], lookbehindWeight: 3, altWeight: 2, unicodeWeight: 4, uWeight: 6, vWeight: 3, nonUnicodeWeight: 2, maxDepth: 3, budget: 30, longSubjectWeight: 1, sweep: 0.4, hotLoop: 0.1, p16: 0.7, meta: 0.3 },
    bm:         { shapes: [[2, "generic"], [1, "lookbehind"], [2, "widealt"], [3, "topalt"], [8, "bm"]], lookbehindWeight: 1, altWeight: 2, unicodeWeight: 1.5, uWeight: 3, vWeight: 1, nonUnicodeWeight: 5, maxDepth: 2, budget: 20, longSubjectWeight: 5, sweep: 0.15, hotLoop: 0.15, p16: 0.5, meta: 0.3 },
    deep:       { shapes: [[5, "generic"], [4, "lookbehind"], [3, "widealt"], [1, "topalt"], [1, "bm"]], lookbehindWeight: 4, altWeight: 3, unicodeWeight: 1.5, uWeight: 3, vWeight: 1.5, nonUnicodeWeight: 4, maxDepth: 5, budget: 60, longSubjectWeight: 0.5, sweep: 0.2, hotLoop: 0.1, p16: 0.5, meta: 0.4 },
    fold:       { shapes: [[1, "generic"], [2, "lookbehind"], [6, "widealt"], [8, "topalt"]], lookbehindWeight: 2, altWeight: 6, unicodeWeight: 0.6, uWeight: 1.5, vWeight: 0.5, nonUnicodeWeight: 7, maxDepth: 3, budget: 50, longSubjectWeight: 1.5, sweep: 0.2, hotLoop: 0.3, p16: 0.3, meta: 0.6 },
    strings:    { shapes: [[3, "generic"], [2, "lookbehind"], [5, "widealt"], [4, "topalt"], [1, "bm"]], lookbehindWeight: 2, altWeight: 4, unicodeWeight: 3, uWeight: 3, vWeight: 8, nonUnicodeWeight: 1, maxDepth: 3, budget: 35, longSubjectWeight: 0.8, sweep: 0.4, hotLoop: 0.15, p16: 0.75, meta: 0.4, qWeight: 0.5, wideAlphaWeight: 4, emojiSubjects: 0.6 },
    wide:       { shapes: [[1, "generic"], [2, "lookbehind"], [6, "widealt"], [6, "topalt"]], lookbehindWeight: 2, altWeight: 6, unicodeWeight: 2.5, uWeight: 5, vWeight: 3, nonUnicodeWeight: 3, maxDepth: 3, budget: 50, longSubjectWeight: 1, sweep: 0.3, hotLoop: 0.25, p16: 0.7, meta: 0.5, qWeight: 0.15, wideAlphaWeight: 6, emojiSubjects: 0.3 },
    small:      { shapes: [[6, "generic"], [2, "lookbehind"], [1, "widealt"], [1, "topalt"], [1, "bm"], [2, "dotstar"]], lookbehindWeight: 2, altWeight: 1, unicodeWeight: 1, uWeight: 3, vWeight: 1, nonUnicodeWeight: 5, maxDepth: 2, budget: 12, longSubjectWeight: 0.3, sweep: 0.3, hotLoop: 0.2, p16: 0.4, meta: 0.5 },
};
const profile = PROFILES[PROFILE] || PROFILES.mixed;

// RNG discipline: wall-clock decisions (skipping a pathological pattern's remaining
// subjects) must never perturb the random stream, or two engines running the same
// seed would desynchronise. Every case therefore runs on a private fork of the
// stream and the main stream advances by a fixed stride per case regardless.
const forkRng = () => [s0, s1, s2, s3];
const joinRng = (st) => { [s0, s1, s2, s3] = st; for (let i = 0; i < 7; ++i) rnd(); };

// "file:<path>" profile: replay externally supplied cases ({p, f, s:[subjects]} per JSON line,
// e.g. from mutate.mjs) through the same operations and encodings; SEED selects a slice of COUNT
// cases starting at (SEED-1)*COUNT so the driver's seed ranges partition the file.
let FILE_CASES = null;
if (PROFILE.startsWith("file:")) {
    const RF = typeof readFile !== "undefined" ? readFile : (f) => require("fs").readFileSync(f, "utf8");
    const flagObj = (str) => Object.fromEntries([..."dgimsuvy"].map((k) => [k, str.includes(k)]));
    FILE_CASES = RF(PROFILE.slice(5)).split("\n").filter(Boolean).map((l) => { try { const c = JSON.parse(l); return { src: c.p, flags: flagObj(c.f || ""), subjects: c.s || [""] }; } catch (e) { return null; } }).filter(Boolean);
}
const fileProfile = { ...PROFILES.mixed, sweep: 0.4, meta: 0.5, hotLoop: 0.15, p16: 0.5 };
for (let idx = 0; idx < COUNT; ++idx) {
    let pat, subjects = null;
    if (FILE_CASES) {
        const c = FILE_CASES[(SEED - 1) * COUNT + idx];
        if (!c) break;
        pat = { src: c.src, flags: c.flags, ctx: {} };
        subjects = c.subjects;
    } else
        pat = genPattern(profile);
    const nSubj = subjects ? subjects.length : pickW([[5, 1], [3, 2], [1, 3]]);
    let skipRest = false;
    for (let j = 0; j < nSubj; ++j) {
        const subj = subjects ? subjects[j] : genSubject(pat, profile);
        const use16 = chance((subjects ? fileProfile : profile).p16);
        const saved = forkRng();
        if (!skipRest) {
            let rec;
            const t0 = Date.now();
            try { rec = runCase(idx, pat, subj, use16, subjects ? fileProfile : profile); }
            catch (e) { rec = { p: safeStr(pat.src), f: flagStr(pat.flags), fatal: String(e) }; }
            const ms = Date.now() - t0;
            OUT(idx + "." + j + "\t" + JSON.stringify(rec) + "\t" + ms);
            if (ms > 1500) skipRest = true; // pathological backtracking: don't spend more subjects on this pattern
        }
        joinRng(saved);
    }
}
OUT("DONE " + SEED + " " + COUNT + " " + PROFILE);
