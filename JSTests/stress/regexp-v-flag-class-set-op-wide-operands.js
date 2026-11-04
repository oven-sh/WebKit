//@ runDefault
//@ runDefault("--useRegExpJIT=0")

// The operations of a `v` class set (the union with a nested class, `&&`, `--`, the complement of a nested class)
// merge the sorted matches and ranges of their operands. The time follows the number of matches and ranges, not the
// number of code points they cover: [^b] covers U+0100 to U+10FFFF.

function shouldBe(actual, expected, description) {
    if (actual !== expected)
        throw new Error(`${description}: expected ${expected} but got ${actual}`);
}

// 300 operands that each cover 1,113,856 code points above U+00FF. A walk over those code points takes seconds.
{
    const start = Date.now();
    shouldBe(new RegExp("^[" + "[^b]".repeat(300) + "]$", "v").test("a"), true, "300 operands [^b]");
    shouldBe(new RegExp("^[\\D" + "--[^a]".repeat(300) + "]$", "v").test("a"), true, "\\D minus 300 operands [^a]");
    const elapsed = Date.now() - start;
    if (elapsed > 1000)
        throw new Error(`600 class set operations on the complement of a nested class took ${elapsed}ms`);
}

const maxCodePoint = 0x10ffff;

function escapeCodePoint(codePoint) {
    return "\\u{" + codePoint.toString(16) + "}";
}

// A set is a list of [first, last] runs, in the order the class lists them. Runs can overlap.
function contents(set) {
    let source = "";
    for (const [first, last] of set)
        source += first === last ? escapeCodePoint(first) : escapeCodePoint(first) + "-" + escapeCodePoint(last);
    return source;
}

function has(set, codePoint) {
    return set.some(([first, last]) => codePoint >= first && codePoint <= last);
}

// The code points at and around each end of each run, each end of both halves of the tables (U+00FF, U+0100) and
// of the BMP.
function probesFor(...sets) {
    const probes = new Set([0, 0xff, 0x100, 0xffff, 0x10000, maxCodePoint]);
    for (const set of sets) {
        for (const run of set) {
            for (const end of run) {
                for (let codePoint = Math.max(end - 1, 0); codePoint <= Math.min(end + 1, maxCodePoint); ++codePoint)
                    probes.add(codePoint);
            }
        }
    }
    return [...probes];
}

// `source` is a class with an operation. `model` says which code points it holds. `reference` is a pattern that
// matches the same code points and has no class set operation: with `i` the model does not know which code points
// are case variants of each other, so the class is compared with the reference instead.
function check(source, reference, probes, model, ignoreCase) {
    const regexp = new RegExp("^" + source + "$", "v");
    const ignoreCaseRegexp = ignoreCase ? new RegExp("^" + source + "$", "vi") : null;
    const ignoreCaseReference = ignoreCase ? new RegExp("^(?:" + reference + ")$", "vi") : null;
    for (const codePoint of probes) {
        const string = String.fromCodePoint(codePoint);
        shouldBe(regexp.test(string), model(codePoint), `/^${source}$/v on U+${codePoint.toString(16)}`);
        if (ignoreCase)
            shouldBe(ignoreCaseRegexp.test(string), ignoreCaseReference.test(string), `/^${source}$/vi on U+${codePoint.toString(16)}`);
    }
}

function checkOperations(a, b, ignoreCase) {
    const probes = probesFor(a, b);
    const inA = codePoint => has(a, codePoint);
    const inB = codePoint => has(b, codePoint);
    const A = "[" + contents(a) + "]";
    const B = "[" + contents(b) + "]";
    const notA = "[^" + contents(a) + "]";
    const notB = "[^" + contents(b) + "]";

    // The union with a nested class, of either polarity. In the last one the class has content of its own first.
    check(`[${A}${B}]`, `${A}|${B}`, probes, codePoint => inA(codePoint) || inB(codePoint), ignoreCase);
    check(`[${notA}]`, notA, probes, codePoint => !inA(codePoint), ignoreCase);
    check(`[${notA}${B}]`, `${notA}|${B}`, probes, codePoint => !inA(codePoint) || inB(codePoint), ignoreCase);
    check(`[${contents(a)}${notB}]`, `${A}|${notB}`, probes, codePoint => inA(codePoint) || !inB(codePoint), ignoreCase);

    check(`[${A}&&${B}]`, `(?=${B})${A}`, probes, codePoint => inA(codePoint) && inB(codePoint), ignoreCase);
    check(`[${B}&&${A}]`, `(?=${B})${A}`, probes, codePoint => inA(codePoint) && inB(codePoint), ignoreCase);
    check(`[${notA}&&${B}]`, `(?=${B})${notA}`, probes, codePoint => !inA(codePoint) && inB(codePoint), ignoreCase);
    check(`[${notA}&&${notB}]`, `(?=${notB})${notA}`, probes, codePoint => !inA(codePoint) && !inB(codePoint), ignoreCase);

    check(`[${A}--${B}]`, `(?!${B})${A}`, probes, codePoint => inA(codePoint) && !inB(codePoint), ignoreCase);
    check(`[${B}--${A}]`, `(?!${A})${B}`, probes, codePoint => inB(codePoint) && !inA(codePoint), ignoreCase);
    check(`[${notA}--${B}]`, `(?!${B})${notA}`, probes, codePoint => !inA(codePoint) && !inB(codePoint), ignoreCase);
    check(`[${A}--${notB}]`, `(?!${notB})${A}`, probes, codePoint => inA(codePoint) && inB(codePoint), ignoreCase);
    check(`[${notA}--${notB}]`, `(?!${notB})${notA}`, probes, codePoint => !inA(codePoint) && inB(codePoint), ignoreCase);
}

// Every way two runs can lie against each other: in the BMP, across U+00FF, into the surrogates, across U+FFFF and
// at the end of the code point range.
for (const base of [0x4e00, 0xf8, 0xd7fa, 0xfffa, 0x10fff0]) {
    const run = (first, last) => [Math.min(base + first, maxCodePoint), Math.min(base + last, maxCodePoint)];
    const shapes = [
        [[run(0, 8)], [run(10, 12)]], // apart
        [[run(0, 8)], [run(9, 12)]], // touching
        [[run(0, 8)], [run(8, 12)]], // sharing one code point
        [[run(0, 8)], [run(4, 12)]], // overlapping
        [[run(0, 8)], [run(0, 8)]], // equal
        [[run(0, 8)], [run(0, 4)]], // sharing the first code point
        [[run(0, 8)], [run(4, 8)]], // sharing the last code point
        [[run(0, 8)], [run(3, 5)]], // inside
        [[run(0, 8)], [run(2, 2), run(4, 4), run(6, 7)]], // several inside
        [[run(0, 2), run(4, 6), run(8, 10)], [run(1, 9)]], // one across several
        [[run(0, 2), run(4, 6), run(8, 10)], [run(3, 3), run(7, 7)]], // filling the gaps
        [[run(0, 2), run(4, 6), run(8, 10)], [run(2, 4), run(6, 8)]], // bridging the gaps
        [[run(0, 0), run(2, 2), run(4, 4)], [run(1, 1), run(3, 3)]], // single code points that touch
        [[run(0, 0), run(2, 2), run(4, 4)], [run(0, 0), run(4, 4)]],
        [[run(5, 5)], [run(5, 5)]],
        [[run(5, 5)], [run(6, 6)]],
        [[run(0, 8)], []],
        // One operand lists a code point that is inside a range, next to one, or listed before.
        [[run(0, 8), run(4, 4)], [run(3, 5)]],
        [[run(0, 8), run(4, 4)], [run(4, 12)]],
        [[run(0, 8), run(0, 0), run(8, 8), run(9, 9)], [run(8, 10)]],
        [[run(2, 8), run(1, 1), run(9, 9)], [run(0, 1), run(9, 12)]],
        [[run(0, 4), run(8, 12), run(2, 2), run(10, 10)], [run(3, 9), run(9, 9), run(6, 6)]],
        [[run(4, 4), run(4, 4), run(5, 5), run(7, 7)], [run(0, 12), run(6, 6)]],
    ];
    // All of them in the BMP. At the other places: overlapping, one across several, and a code point next to a range.
    for (const [index, [a, b]] of shapes.entries()) {
        if (base === 0x4e00 || [3, 9, 20].includes(index))
            checkOperations(a, b, base === 0xf8);
    }
}

// Operands that reach both ends of the code point range.
checkOperations([[0, maxCodePoint]], [[0x4e00, 0x4e00]], false);
checkOperations([[0x100, maxCodePoint]], [[0x100, 0x100], [maxCodePoint, maxCodePoint]], false);
checkOperations([[0, 0xff], [0x200, 0xffff]], [[0x80, 0x2ff], [0x10000, maxCodePoint]], true);

// Operands that are not written as runs: class escapes, property escapes and class strings, beside a wide class.
// A class with an operation matches the same code points as a pattern that says the same without one, on a sample
// of the code points.
{
    const codePoints = [];
    const add = codePoint => {
        if (codePoint < 0xd800 || codePoint > 0xdfff)
            codePoints.push(codePoint);
    };
    for (let codePoint = 0; codePoint < 0x250; ++codePoint)
        add(codePoint);
    for (let codePoint = 0x250; codePoint <= maxCodePoint; codePoint += 997)
        add(codePoint);
    for (const edge of [0x4e00, 0x9fff, 0xd7ff, 0xe000, 0xffff, 0x10000, maxCodePoint]) {
        for (let codePoint = edge - 2; codePoint <= Math.min(edge + 2, maxCodePoint); ++codePoint)
            add(codePoint);
    }
    const sample = String.fromCodePoint(...codePoints);
    const same = (withOperation, withoutOperation) => {
        for (const flags of ["gv", "giv"]) {
            const rest = source => sample.replace(new RegExp(source, flags), "");
            shouldBe(rest(withOperation) === rest(withoutOperation), true, `/${withOperation}/${flags} matches what /${withoutOperation}/${flags} matches`);
        }
    };

    same("[\\D--\u4e00]", "(?!\u4e00)\\D");
    same("[\\D--[^\\p{Script=Han}]]", "\\p{Script=Han}");
    same("[\\S&&\\D&&\\W]", "(?!\\s)(?!\\d)\\W");
    same("[\u4e00\u4e01\\D]", "\\D");
    same("[a-z\\S]", "\\S");
    same("[\\W[\\d]]", "\\W|\\d");
    same("[\\D\\q{7}]", "\\D|7");
    same("[\\q{7|\u4e00}--\\D]", "7");
    same("[\\p{L}--\\p{Lu}]", "(?!\\p{Lu})\\p{L}");
    same("[\\p{L}&&\\p{Lu}]", "\\p{Lu}");
    same("[\\p{L}--\\P{Lu}]", "\\p{Lu}");
    same("[\\p{L}--[a-z]]", "(?![a-z])\\p{L}");
    same("[a-zA-Z0-9_\\p{L}]", "\\p{L}|[0-9_]");
    same("[\\p{L}[\\p{N}]]", "\\p{L}|\\p{N}");
    same("[\u4e00\u4e01\\P{L}]", "\\P{L}|[\u4e00\u4e01]");
    same("[[^\\p{L}]]", "\\P{L}");
    same("[[^\\p{L}]&&[^\\p{N}]]", "(?!\\p{L})\\P{N}");
    same("[\\p{Any}--\\p{Assigned}]", "\\P{Assigned}");
    same("[[^\u4e00]--[^\u4e00-\u9fff]]", "[\u4e01-\u9fff]");

    // \P{...} with `i` and `u` is a difference of two classes, built once for each property.
    for (const codePoint of [0x20, 0x31, 0x3000, 0x4e00, 0xac00, 0xffff, 0x10000, 0x1f600, 0x20000, maxCodePoint]) {
        const string = String.fromCodePoint(codePoint);
        shouldBe(/^\P{L}$/iu.test(string), !/^\p{L}$/u.test(string), `/^\\P{L}$/iu on U+${codePoint.toString(16)}`);
    }
}

// Strings, and code points that have case variants.
{
    const test = (source, flags, matching, notMatching) => {
        const regexp = new RegExp(source, flags);
        for (const string of matching)
            shouldBe(regexp.test(string), true, `/${source}/${flags}.test(${JSON.stringify(string)})`);
        for (const string of notMatching)
            shouldBe(regexp.test(string), false, `/${source}/${flags}.test(${JSON.stringify(string)})`);
    };
    test("^[a[^b]]$", "v", ["a", "c", "\u4e00", "\u{1f600}"], ["b", "ab"]);
    test("^[\\q{ab}[^\\q{a}]]$", "v", ["ab", "b", "\u4e00"], ["a", "abb"]);
    test("^[\\q{ab|\u4e00}--[^\u4e00]]$", "v", ["ab", "\u4e00"], ["a", "b", "\u4e01"]);
    test("^[\\q{ab|c}&&[^b]]$", "v", ["c"], ["ab", "a", "b"]);
    test("^[\\q{ab|\u4e00}[^\u4e00]]$", "v", ["ab", "\u4e00", "a", "\u{1f600}"], ["abb", ""]);
    test("^[\u4e00[^b]]$", "vi", ["a", "\u4e00", "\u{1f600}"], ["b", "B"]);
    test("^[[^k]&&\\w]$", "vi", ["a", "_", "\u017f"], ["k", "K", "\u212a", "-"]);
    test("^[[^\u03c3]--\\p{Nd}]$", "vi", ["a", "A", "\u03b1"], ["\u03c3", "\u03c2", "\u03a3", "7"]);

    // With `i`, each alternative of \q{} adds its case variants after it: the operand lists code points out of
    // order, and more than once when the alternatives are case variants of each other.
    test("^[\\q{\u0431}]$", "vi", ["\u0431", "\u0411"], ["\u0430"]);
    test("^[[\u4e00]\\q{\u0431}]$", "vi", ["\u0431", "\u0411", "\u4e00"], ["\u0430"]);
    test("^[[\u0100-\u{10ffff}]&&\\q{\u0431}]$", "vi", ["\u0431", "\u0411"], ["\u0430", "\u4e00", "a"]);
    test("^[[\u0100-\u{10ffff}]--\\q{\u0431}]$", "vi", ["\u0430", "\u4e00"], ["\u0431", "\u0411", "a"]);
    test("^[[\u0100-\u{10ffff}]--\\q{\u0410|\u0411}]$", "vi", ["\u0432", "\u0412"], ["\u0430", "\u0410", "\u0431", "\u0411"]);
    test("^[\\q{\u03c3|\u03c2|\u03a3}--[^\u03a3]]$", "vi", ["\u03c3", "\u03c2", "\u03a3"], ["\u03b1", "s"]);
    test("^[[\u0100-\u{10ffff}]--\\q{\u0431|\u0411|\u4e00}]$", "vi", ["\u0430", "\u4e01", "s", "k"], ["\u0431", "\u0411", "\u4e00", "a"]);
    test("^[[\u0100-\u{10ffff}]&&\\q{\u0431|\u0411|\u4e00}]$", "vi", ["\u0431", "\u0411", "\u4e00"], ["\u0430", "\u4e01", "a"]);
}

// JavaScriptCore accepts an operator after two operands, as in [\p{L}\p{Lu}--x], where the specification has a
// SyntaxError. The left side is then built like a plain union: it lists the code points of \p{Lu} one by one,
// although they are inside the ranges of \p{L}. While such a class is accepted, it holds the right code points.
{
    const isLetter = codePoint => /^\p{L}$/u.test(String.fromCodePoint(codePoint));
    const isUppercaseLetter = codePoint => /^\p{Lu}$/u.test(String.fromCodePoint(codePoint));
    const checkIfAccepted = (source, set, model) => {
        let regexp;
        try {
            regexp = new RegExp("^" + source + "$", "v");
        } catch (error) {
            if (!(error instanceof SyntaxError))
                throw error;
            return;
        }
        for (const codePoint of probesFor(set, [[0x41, 0x5a], [0x100, 0x17f]]))
            shouldBe(regexp.test(String.fromCodePoint(codePoint)), model(codePoint), `/^${source}$/v on U+${codePoint.toString(16)}`);
    };
    for (const set of [[[0x102, 0x102], [0x103, 0x2c1]], [[0x100, 0x100], [0x104, 0x104], [0x106, 0x110]], [[0x41, 0x41], [0xff, 0x101]]]) {
        checkIfAccepted(`[\\p{L}\\p{Lu}--[${contents(set)}]]`, set, codePoint => isLetter(codePoint) && !has(set, codePoint));
        checkIfAccepted(`[\\p{L}\\p{Lu}&&[${contents(set)}]]`, set, codePoint => isLetter(codePoint) && has(set, codePoint));
        checkIfAccepted(`[[\\u{100}-\\u{17f}]\\p{Lu}--[${contents(set)}]]`, set, codePoint => (codePoint >= 0x100 && codePoint <= 0x17f || isUppercaseLetter(codePoint)) && !has(set, codePoint));
    }
}
