// Spec-literal reference matcher (ECMA-262 22.2.2, continuation-passing) for a RegExp subset:
// literals, ., classes (ranges, \d\w\s\D\W\S, negation), ^ $ \b \B, groups (capturing, named,
// non-capturing), alternation, quantifiers (greedy/lazy, {m,n}), backreferences (\1, \k<n>),
// lookahead/lookbehind (positive/negative). Flags: g y m s u d (u = code points; no i, no v).
// It is deliberately naive (exponential where the spec is), so callers cap the step count.
//
// exec(pattern, flags, subject, lastIndex=0) -> { index, captures:[[s,e]|undefined...], groups } | null | "UNSUPPORTED" | "STEPS"
import { RegExpParser } from "@eslint-community/regexpp";

const parser = new RegExpParser({ ecmaVersion: 2025 });

export function refExec(source, flags, input, lastIndex = 0, stepLimit = 200000) {
  if (/[iv]/.test(flags)) return "UNSUPPORTED";
  const unicode = flags.includes("u"), dotAll = flags.includes("s"), multiline = flags.includes("m"), sticky = flags.includes("y"), global = flags.includes("g");
  let ast;
  try { ast = parser.parsePattern(source, 0, source.length, { unicode, unicodeSets: false }); } catch { return "UNSUPPORTED"; }

  // Input as a list of "characters": code points under /u, code units otherwise; keep unit offsets.
  const chars = [], offs = []; // chars[i] = code, offs[i] = unit offset of char i; offs[chars.length] = input.length
  for (let i = 0; i < input.length;) {
    const cp = unicode ? input.codePointAt(i) : input.charCodeAt(i);
    offs.push(i); chars.push(cp); i += (unicode && cp > 0xffff) ? 2 : 1;
  }
  offs.push(input.length);
  const n = chars.length;
  const unitToChar = new Map(offs.map((u, i) => [u, i]));

  // Count capturing groups & names in order.
  const groupNodes = [];
  (function walk(nd) { if (nd.type === "CapturingGroup") groupNodes.push(nd); for (const k of ["alternatives", "elements"]) if (Array.isArray(nd[k])) nd[k].forEach(walk); if (nd.element) walk(nd.element); })(ast);
  const nCaps = groupNodes.length;
  const groupIndex = new Map(groupNodes.map((g, i) => [g, i + 1]));
  const nameToIndices = {};
  groupNodes.forEach((g, i) => { if (g.name) (nameToIndices[g.name] ||= []).push(i + 1); });

  let steps = 0;
  class Steps extends Error {}
  const tick = () => { if (++steps > stepLimit) throw new Steps(); };

  const isWord = (c) => (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95;
  const isLT = (c) => c === 10 || c === 13 || c === 0x2028 || c === 0x2029;
  const isSpace = (c) => c === 9 || c === 10 || c === 11 || c === 12 || c === 13 || c === 32 || c === 160 || c === 0x1680 || (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
  const isDigit = (c) => c >= 48 && c <= 57;

  function classMatcher(node) {
    // returns (c) => bool for CharacterClass / CharacterSet / Character / CharacterClassRange elements
    switch (node.type) {
      case "Character": return (c) => c === node.value;
      case "CharacterClassRange": return (c) => c >= node.min.value && c <= node.max.value;
      case "CharacterSet":
        if (node.kind === "any") return (c) => dotAll || !isLT(c);
        if (node.kind === "digit") return (c) => isDigit(c) !== node.negate;
        if (node.kind === "word") return (c) => isWord(c) !== node.negate;
        if (node.kind === "space") return (c) => isSpace(c) !== node.negate;
        throw new Steps("UNSUPPORTED"); // \p{..}
      case "CharacterClass": {
        if (node.unicodeSets) throw new Steps("UNSUPPORTED");
        const parts = node.elements.map(classMatcher);
        return (c) => parts.some((f) => f(c)) !== node.negate;
      }
      default: throw new Steps("UNSUPPORTED");
    }
  }

  // State: captures array caps[1..nCaps] = [s,e] (char indices) or undefined. Matchers: (x:{end, caps}, cont) => result|null
  // direction: +1 forward, -1 backward.
  function compileDisjunction(alts, dir) {
    const ms = alts.map((a) => compileAlternative(a, dir));
    if (ms.length === 1) return ms[0];
    return (x, c) => { for (const m of ms) { tick(); const r = m(x, c); if (r) return r; } return null; };
  }
  function compileAlternative(alt, dir) {
    let terms = alt.elements.map((e) => compileTerm(e, dir));
    if (dir < 0) terms = terms.reverse();
    // fold right: m1 then m2 ...
    return terms.reduceRight((acc, m) => (x, c) => m(x, (y) => acc(y, c)), (x, c) => c(x));
  }
  function compileTerm(node, dir) {
    switch (node.type) {
      case "Assertion": return compileAssertion(node, dir);
      case "Quantifier": return compileQuantifier(node, dir);
      case "Group": {
        if (node.modifiers) throw new Steps("UNSUPPORTED");
        return compileDisjunction(node.alternatives, dir);
      }
      case "CapturingGroup": {
        const m = compileDisjunction(node.alternatives, dir);
        const idx = groupIndex.get(node);
        return (x, c) => m(x, (y) => {
          const caps = y.caps.slice();
          caps[idx] = dir > 0 ? [x.end, y.end] : [y.end, x.end];
          return c({ end: y.end, caps });
        });
      }
      case "Backreference": {
        const targets = typeof node.ref === "number" ? [node.ref] : (nameToIndices[node.ref] || []);
        return (x, c) => {
          tick();
          let cap;
          for (const t of targets) if (x.caps[t]) { cap = x.caps[t]; break; }
          if (!cap) return c(x);
          const [s, e] = cap, len = e - s;
          if (dir > 0) {
            if (x.end + len > n) return null;
            for (let i = 0; i < len; i++) if (chars[s + i] !== chars[x.end + i]) return null;
            return c({ end: x.end + len, caps: x.caps });
          }
          if (x.end - len < 0) return null;
          for (let i = 0; i < len; i++) if (chars[s + i] !== chars[x.end - len + i]) return null;
          return c({ end: x.end - len, caps: x.caps });
        };
      }
      case "Character": case "CharacterSet": case "CharacterClass": {
        const f = classMatcher(node);
        return (x, c) => {
          tick();
          const at = dir > 0 ? x.end : x.end - 1;
          if (at < 0 || at >= n) return null;
          if (!f(chars[at])) return null;
          return c({ end: x.end + dir, caps: x.caps });
        };
      }
      case "ExpressionCharacterClass": case "ClassStringDisjunction": throw new Steps("UNSUPPORTED");
      default: throw new Steps("UNSUPPORTED:" + node.type);
    }
  }
  function compileAssertion(node, dir) {
    switch (node.kind) {
      case "start": return (x, c) => (x.end === 0 || (multiline && isLT(chars[x.end - 1]))) ? c(x) : null;
      case "end": return (x, c) => (x.end === n || (multiline && isLT(chars[x.end]))) ? c(x) : null;
      case "word": return (x, c) => { const a = x.end > 0 && isWord(chars[x.end - 1]), b = x.end < n && isWord(chars[x.end]); return ((a !== b) !== node.negate) ? c(x) : null; };
      case "lookahead": case "lookbehind": {
        const m = compileDisjunction(node.alternatives, node.kind === "lookahead" ? 1 : -1);
        if (!node.negate) return (x, c) => { const r = m(x, (y) => y); if (!r) return null; return c({ end: x.end, caps: r.caps }); };
        return (x, c) => { const r = m(x, (y) => y); if (r) return null; return c(x); };
      }
      default: throw new Steps("UNSUPPORTED");
    }
  }
  function capsIn(node) { const out = []; (function walk(nd) { if (nd.type === "CapturingGroup") out.push(groupIndex.get(nd)); for (const k of ["alternatives", "elements"]) if (Array.isArray(nd[k])) nd[k].forEach(walk); if (nd.element) walk(nd.element); })(node); return out; }
  function compileQuantifier(node, dir) {
    const m = compileTerm(node.element, dir);
    const min = node.min, max = node.max === Infinity ? Infinity : node.max, greedy = node.greedy;
    const inner = capsIn(node.element);
    // RepeatMatcher(m, min, max, greedy, x, c, parenIndex, parenCount)
    const rm = (min, max, x, c) => {
      tick();
      if (max === 0) return c(x);
      const d = (y) => {
        if (min === 0 && y.end === x.end) return null; // empty iteration once min met -> failure
        return rm(min === 0 ? 0 : min - 1, max === Infinity ? Infinity : max - 1, y, c);
      };
      const caps = x.caps.slice(); for (const i of inner) caps[i] = undefined; // clear inner captures per iteration
      const xr = { end: x.end, caps };
      if (min !== 0) return m(xr, d);
      if (!greedy) { const z = c(x); if (z) return z; return m(xr, d); }
      const z = m(xr, d); if (z) return z; return c(x);
    };
    return (x, c) => rm(min, max, x, c);
  }

  let top;
  try { top = compileDisjunction(ast.alternatives, 1); } catch (e) { return "UNSUPPORTED"; }

  // RegExpBuiltinExec
  let li = (global || sticky) ? lastIndex : 0;
  if (li > input.length) return null;
  try {
    for (;;) {
      // under /u, a lastIndex inside a surrogate pair: spec says start matching at that *unit* index; the matcher's
      // notion of "character index" needs the char containing/at that unit. Spec (22.2.7.2 step 11-13) uses the unit
      // index directly and AdvanceStringIndex to move; a start in the middle of a pair matches from the trail unit as
      // its own character in the Pattern semantics? No: with /u the input is a list of code points and lastIndex is
      // interpreted via that list -- an index that is not a code point boundary cannot start a match; the loop just
      // advances. We model: if li is not a char boundary, no match attempt at li.
      const ci = unitToChar.get(li);
      let r = null;
      if (ci !== undefined) r = top({ end: ci, caps: new Array(nCaps + 1).fill(undefined) }, (y) => y);
      if (r) {
        const caps = [[li, offs[r.end]]];
        for (let i = 1; i <= nCaps; i++) caps.push(r.caps[i] ? [offs[r.caps[i][0]], offs[r.caps[i][1]]] : undefined);
        const groups = Object.keys(nameToIndices).length ? Object.fromEntries(Object.entries(nameToIndices).map(([nm, idxs]) => { let v; for (const i of idxs) if (caps[i]) v = caps[i]; return [nm, v]; })) : undefined;
        return { index: li, captures: caps, groups, lastIndex: (global || sticky) ? offs[r.end] : lastIndex };
      }
      if (sticky) return null;
      // AdvanceStringIndex
      if (li >= input.length) return null;
      li += (unicode && li + 1 < input.length && (input.charCodeAt(li) & 0xfc00) === 0xd800 && (input.charCodeAt(li + 1) & 0xfc00) === 0xdc00) ? 2 : 1;
      if (li > input.length) return null;
    }
  } catch (e) {
    if (e instanceof Steps) return String(e.message).startsWith("UNSUPPORTED") ? "UNSUPPORTED" : "STEPS";
    throw e;
  }
}

// Convenience: same shape as the fuzzer's exec encoding: [index, m0, m1..] with "~U" for undefined
export function refExecEncoded(source, flags, input, lastIndex = 0) {
  const r = refExec(source, flags, input, lastIndex);
  if (r === null || typeof r === "string") return r;
  return [r.index, ...r.captures.map((c) => c ? input.slice(c[0], c[1]) : "~U")];
}
