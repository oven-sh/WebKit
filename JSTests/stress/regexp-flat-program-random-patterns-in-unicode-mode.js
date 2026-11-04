//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// Random patterns in unicode mode whose steps can consume a surrogate pair, beside literals, classes, anchors and lookarounds. A subject is made out of its pattern, so that about half of the runs match,
// with surrogate pairs and lone surrogates in it and around it. The validation runs the flat program beside the bytecode interpreter on every match and stops where their answers or their turns differ.
let seed = 1;
const rounds = 2000;
function random() { seed = (seed + 0x6d2b79f5) | 0; let t = Math.imul(seed ^ (seed >>> 15), 1 | seed); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }
const below = n => Math.floor(random() * n);
const pick = list => list[below(list.length)];

const pairs = ["\u{1f600}", "\u{1f600}", "\u{1f60d}", "\u{10400}", "\u{10428}", "\u{10000}", "\u{10ffff}", "\u{1c345}", "\u{1d7d8}"];
const lone = ["\ud83d", "\ude00", "\ud801", "\udc00", "\udbff", "\udfff"];
const wide = ["α", "Α", "ω", "β", "ſ", "K", "Ω", "ẞ", "μ", "Ÿ", " ", "﻿", "￿", "Ā", "☃", "ж"];
const narrow = ["a", "a", "b", "c", "x", "k", "K", "s", "S", " ", "\n", "_", "1", "-", "é", "É", "ß", "µ", "ÿ", "l", "e", "f"];
function anything() { const kind = random(); return kind < 0.35 ? pick(pairs) : kind < 0.5 ? pick(lone) : kind < 0.65 ? pick(wide) : pick(narrow); }
function noise(most) { let text = ""; for (let n = below(most + 1); n > 0; n--) text += anything(); return text; }

// [source, what it may take]
const literals = [["a", ["a", "A"]], ["b", ["b"]], ["x", ["x", "X"]], ["k", ["k", "K", "K"]], ["s", ["s", "S", "ſ"]], ["ab", ["ab", "aB"]], ["lef", ["lef", "LEF"]], ["abcdefgh", ["abcdefgh"]], ["abcdefghi", ["abcdefghi", "ABCDEFGHI"]],
  ["α", ["α", "Α"]], ["αβ", ["αβ", "ΑΒ"]], ["aα", ["aα"]], ["é", ["é", "É"]], ["ß", ["ß", "ẞ"]], ["µ", ["µ", "μ", "Μ"]], ["ÿ", ["ÿ", "Ÿ"]], ["\\u2603", ["☃"]], ["\\u{1f600}", ["\u{1f600}"]], ["\\u{10400}", ["\u{10400}", "\u{10428}"]],
  ["\\ud83d", ["\ud83d"]], ["\\ude00", ["\ude00"]], ["\\ud83d\\ude00", ["\u{1f600}"]], ["a\\u{1f600}b", ["a\u{1f600}b"]], [" ", [" "]], ["-", ["-"]], ["\\n", ["\n"]]];
const any = () => anything();
const classes = [[".", any], [".", any], [".", any], ["[^a]", any], ["[^x]", any], ["[^k]", any], ["\\P{L}", () => pick(["\u{1f600}", "1", " ", "\ud83d", "☃"])], ["\\p{L}", () => pick(["a", "α", "\u{10400}", "\u{10428}", "é"])], ["\\S", any], ["\\W", any], ["\\D", any],
  ["[\\s\\S]", any], ["[^]", any], ["\\p{Lu}", () => pick(["K", "Α", "\u{10400}", "a", "\u{10428}"])], ["\\p{Ll}", () => pick(["k", "α", "\u{10428}", "\u{10400}"])], ["[\\u{10400}-\\u{1044f}]", () => pick(["\u{10400}", "\u{10428}"])],
  ["[\\u{1f600}-\\u{1f64f}]", () => pick(["\u{1f600}", "\u{1f60d}"])], ["[^\\u{1f600}]", any], ["[\\ud800-\\udbff]", () => pick(["\ud83d", "\ud801", "\udbff"])], ["[\\udc00-\\udfff]", () => pick(["\ude00", "\udc00"])], ["[α-ω]", () => pick(["α", "ω", "Α", "β"])],
  ["[ωα\\u0400-\\u04ffΑ\\u2603]", () => pick(["ω", "α", "ж", "Α", "☃"])], ["[a\\u{1f600}]", () => pick(["a", "\u{1f600}"])], ["[^\\p{L}]", any], ["\\p{Emoji_Presentation}", () => pick(["\u{1f600}", "\u{1f60d}"])], ["\\p{Script=Deseret}", () => pick(["\u{10400}", "\u{10428}"])],
  ["\\p{Any}", any], ["\\p{ASCII}", () => pick(narrow)], ["\\w", () => pick(["a", "k", "_", "1", "ſ", "K"])], ["\\d", () => "1"], ["\\s", () => pick([" ", "\n", " ", "﻿"])], ["[ab]", () => pick(["a", "b"])], ["[a-z]", () => pick(["a", "k", "s", "K", "K"])],
  ["[k]", () => pick(["k", "K", "K"])], ["[^\\W]", () => pick(["a", "_"])], ["[\\p{L}--[a-z]]", () => pick(["α", "\u{10400}"])], ["[\\p{L}&&\\p{ASCII}]", () => "a"], ["[[a-z]\\u{1f600}]", () => pick(["a", "\u{1f600}"])], ["[\\q{ab|\\u{1f600}}x]", () => pick(["ab", "\u{1f600}", "x"])],
  ["[^\\ud83d]", any], ["[^\\u{10000}-\\u{10ffff}]", () => pick(narrow)], ["[\\u{10000}-\\u{10ffff}]", () => pick(pairs)], ["[\\0-\\u{10ffff}]", any]];
const anchors = ["^", "$", "\\b", "\\B"];
const quantifiers = [["", 1, 1], ["", 1, 1], ["", 1, 1], ["", 1, 1], ["*", 0, 3], ["+", 1, 3], ["?", 0, 1], ["*?", 0, 2], ["+?", 1, 2], ["??", 0, 1], ["{2}", 2, 2], ["{1,2}", 1, 2], ["{2,}", 2, 3], ["{0,3}?", 0, 2], ["{3}", 3, 3]];

let groups = 0;
// A piece: { source, sample() }
function repeated(piece, needsGroup) {
  const [mark, least, most] = pick(quantifiers);
  if (!mark) return piece;
  return { source: (needsGroup ? "(?:" + piece.source + ")" : piece.source) + mark, sample() { let text = ""; for (let n = least + below(most - least + 1); n > 0; n--) text += piece.sample(); return text; } };
}
function literal() { const [source, samples] = pick(literals); return { source, sample: () => pick(samples), isLong: source.length > 1 && source[0] !== "\\" || source.length > 9 }; }
function characterClass() { const [source, sample] = pick(classes); return { source, sample }; }
function atom() { if (random() < 0.4) { const piece = literal(); return repeated(piece, piece.isLong); } return repeated(characterClass(), false); }
function fixedWidth(most) { const pieces = []; for (let n = 1 + below(most); n > 0; n--) pieces.push(random() < 0.15 ? { source: pick(anchors), sample: () => "" } : random() < 0.4 ? literal() : characterClass()); return joined(pieces); }
function joined(pieces) { return { source: pieces.map(piece => piece.source).join(""), sample: () => pieces.map(piece => piece.sample()).join("") }; }
function oneOf(pieces) { return { source: pieces.map(piece => piece.source).join("|"), sample: () => pick(pieces).sample() }; }
function term(depth) {
  const kind = random();
  if (depth > 2 || kind < 0.62) return atom();
  if (kind < 0.68) return { source: pick(anchors), sample: () => "" };
  if (kind < 0.74) { const inner = random() < 0.7 ? fixedWidth(3) : alternatives(depth + 1); return { source: "(?<=" + inner.source + ")", sample: () => "", behind: inner }; }
  if (kind < 0.78) { const inner = random() < 0.7 ? fixedWidth(2) : alternatives(depth + 1); return { source: "(?<!" + inner.source + ")", sample: () => "" }; }
  if (kind < 0.83) { const inner = random() < 0.6 ? fixedWidth(3) : alternatives(depth + 1); return { source: "(?=" + inner.source + ")", sample: () => "", ahead: inner }; }
  if (kind < 0.86) { const inner = fixedWidth(2); return { source: "(?!" + inner.source + ")", sample: () => "" }; }
  if (kind < 0.93) { const inner = alternatives(depth + 1); return repeated({ source: pick(["(?:", "(?:", "(?:", "(?i:", "(?-i:", "(?s:", "(?m:", "(?i-s:", "(?-m:"]) + inner.source + ")", sample: inner.sample }, false); }
  if (kind < 0.98 || !groups) { groups++; const inner = alternatives(depth + 1); return repeated({ source: "(" + inner.source + ")", sample: inner.sample }, false); }
  return { source: "\\" + (1 + below(groups)), sample: () => "" };
}
// What a lookbehind wants is put before it and what a lookahead wants behind it, half of the time.
function sequence(depth, most) {
  const pieces = []; for (let n = 1 + below(most); n > 0; n--) pieces.push(term(depth));
  return { source: pieces.map(piece => piece.source).join(""), sample() { let text = ""; for (const piece of pieces) { if (piece.behind && random() < 0.5) text += piece.behind.sample(); text += piece.sample(); if (piece.ahead && random() < 0.5) text += piece.ahead.sample(); } return text; } };
}
function alternatives(depth) { const pieces = [sequence(depth, 3)]; while (random() < (depth ? 0.3 : 0.2)) pieces.push(sequence(depth, 3)); return oneOf(pieces); }
function whole() {
  groups = 0;
  const shape = random();
  if (shape < 0.3) return alternatives(0);
  if (shape < 0.42) return joined([literal(), repeated(characterClass(), false), literal()]);
  if (shape < 0.54) return joined([fixedWidth(4), { source: "$", sample: () => "" }]);
  if (shape < 0.62) return joined([{ source: "^", sample: () => "" }, sequence(0, 4)]);
  if (shape < 0.72) { const behind = fixedWidth(3), rest = sequence(0, 3); return { source: "(?<" + pick(["=", "=", "!"]) + behind.source + ")" + rest.source, sample: () => behind.sample() + rest.sample() }; }
  if (shape < 0.8) { const inner = alternatives(0); return { source: pick(["", "^"]) + ".*" + pick(["", "?"]) + (inner.source.includes("|") ? "(?:" + inner.source + ")" : inner.source) + ".*" + pick(["", "$"]), sample: inner.sample }; }
  if (shape < 0.9) return oneOf([sequence(0, 3), sequence(0, 3), sequence(0, 3), sequence(0, 2)]);
  return joined([sequence(0, 2), repeated(characterClass(), false), sequence(0, 2), { source: pick(["", "$", "\\b", "(?!.)"]), sample: () => "" }]);
}
const allFlags = ["u", "u", "u", "v", "v", "iu", "iu", "iv", "su", "mu", "gu", "yu", "giu", "siv", "miu", "dgu", "yiv", "gmsu"];
let made = 0, ran = 0, matched = 0;
for (let round = 0; round < rounds; round++) {
  const piece = whole(), flags = pick(allFlags);
  let pattern; try { pattern = new RegExp(piece.source, flags); } catch (error) { continue; }
  made++;
  for (let i = 0; i < 8; i++) {
    const text = i < 6 ? noise(i < 2 ? 0 : 3) + piece.sample() + noise(i & 1 ? 0 : 3) : noise(9);
    pattern.lastIndex = pattern.global || pattern.sticky ? below(Math.min(text.length, 4) + 1) : 0;
    if (pattern.exec(text)) matched++;
    ran++;
  }
}
if (made < rounds / 2 || matched < ran / 3)
    throw new Error(made + " patterns, " + ran + " runs, " + matched + " matches");
