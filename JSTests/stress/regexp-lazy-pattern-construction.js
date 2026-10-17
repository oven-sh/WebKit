//@ runDefault
//@ runDefault("--useLazyRegExpPatternConstruction=false")

// Patterns longer than RegExp's eager-construction threshold are only syntax-checked and
// capture-counted at creation; the YarrPattern is first built when the RegExp is compiled.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function shouldThrowSyntaxError(source, flags) {
    let threw = false;
    try {
        new RegExp(source, flags);
    } catch (e) {
        threw = e instanceof SyntaxError;
    }
    if (!threw)
        throw new Error("expected SyntaxError at construction for /" + source.slice(0, 40) + "/" + flags);
}

let filler = "x".repeat(80);

// Capture count comes from the syntax-only pass and must agree with the later compile.
{
    let re = new RegExp("(a)(?:b)(c)(" + filler + ")?|(d)");
    let m = re.exec("abc");
    shouldBe(m.length, 5, "captures");
    shouldBe(m[1], "a", "m[1]");
    shouldBe(m[2], "c", "m[2]");
    shouldBe(m[3], undefined, "m[3]");
    shouldBe("zzd".replace(re, "[$4]"), "zz[d]", "replace $4");
}

// Named groups still take the full construction path.
{
    let re = new RegExp("(?<year>\\d{4})-(?<month>\\d{2})" + "(?:" + filler + ")?", "u");
    let m = re.exec("2026-09");
    shouldBe(m.groups.year, "2026", "year");
    shouldBe(m.groups.month, "09", "month");
    let dup = new RegExp("(?<n>a)" + filler + "|(?<n>b)");
    shouldBe(dup.exec("b").groups.n, "b", "duplicate named group");
}

// A long literal pattern: atom fast paths kick in once compiled; results must not change.
{
    let literal = "abcdefghij".repeat(8);
    let re = new RegExp(literal, "g");
    let subject = "--" + literal + "--" + literal;
    shouldBe(subject.replace(re, "R"), "--R--R", "replace literal");
    shouldBe(subject.split(new RegExp(literal)).length, 3, "split literal");
    shouldBe((subject.match(re) || []).length, 2, "match literal");
}

// Parser-detected errors are still thrown at construction.
shouldThrowSyntaxError("(" + filler, "");
shouldThrowSyntaxError(filler + "\\2(a)", "u");
shouldThrowSyntaxError(filler + "[b-a]", "");
shouldThrowSyntaxError(filler + "\\k<nope>(?<yes>a)", "u");
shouldThrowSyntaxError("[^\\q{ab}]" + filler, "v");
shouldThrowSyntaxError(filler + "a{2,1}", "");

// Back references are validated against the counted captures (non-unicode: octal escape fallback).
shouldBe(new RegExp("(a)" + filler.slice(2) + "|\\1b").test("b"), true, "backreference to an unmatched group matches empty");
shouldBe(new RegExp(filler + "\\8").test(filler + "8"), true, "\\8 without captures is a literal");

// lastIndex / sticky / multiline behave the same on the lazy path.
{
    let re = new RegExp("^(" + filler.slice(0, 70) + ")$", "gm");
    let subject = filler.slice(0, 70) + String.fromCharCode(10) + filler.slice(0, 70);
    shouldBe((subject.match(re) || []).length, 2, "multiline");
}

// Lookbehind captures, modifier groups and non-capturing groups are counted like YarrPatternConstructor does.
{
    let re = new RegExp("(?<=(a)b)(?i:c)(?:d)(e)" + "(?:" + filler + ")?", "d");
    let m = re.exec("abcde");
    shouldBe(m.length, 3, "lookbehind/modifier captures");
    shouldBe(m[1], "a", "lookbehind capture");
    shouldBe(m[2], "e", "trailing capture");
    shouldBe(m.indices.length, 3, "hasIndices shape");
    shouldBe(m.indices[1][0], 0, "hasIndices lookbehind start");
    shouldBe(re.exec("abCdE"), null, "modifier group scope");
}

// /v set operations and class strings on the lazy path.
{
    let re = new RegExp("[\\p{L}--[a-z]][\\q{xy|z}]" + "(?:" + filler + ")?", "v");
    shouldBe(re.test("Axy"), true, "v set difference + class string");
    shouldBe(re.test("axy"), false, "v set difference excludes");
    shouldBe(re.test("Bz"), true, "v class string single");
}

// Shared Unicode property classes under /iu (case-closure of a shared class must not disturb the base class).
{
    let upper = new RegExp("\\p{Lu}" + "(?:" + filler + ")?", "u");
    let upperIgnoreCase = new RegExp("\\p{Lu}" + "(?:" + filler + ")?", "iu");
    let notUpper = new RegExp("\\P{Lu}" + "(?:" + filler + ")?", "u");
    shouldBe(upper.test("a"), false, "\\p{Lu} u");
    shouldBe(upperIgnoreCase.test("a"), true, "\\p{Lu} iu");
    shouldBe(upper.test("a"), false, "\\p{Lu} u after iu compile");
    shouldBe(notUpper.test("a"), true, "\\P{Lu} u");
    shouldBe(/\p{Lu}/u.test("a"), false, "short \\p{Lu} shares the class");
}

// Deeply nested long patterns take the eager path, so where they throw does not depend on the option.
{
    let depth = 20000;
    let source = "(?:".repeat(depth) + "a" + ")".repeat(depth);
    let constructed = null;
    let threwAtConstruction = false;
    try {
        constructed = new RegExp(source);
    } catch (e) {
        threwAtConstruction = e instanceof SyntaxError;
    }
    if (!threwAtConstruction) {
        let result = true;
        try {
            result = constructed.test("a");
        } catch (e) {
            shouldBe(e instanceof SyntaxError, true, "deeply nested pattern compile error type");
        }
        shouldBe(result, true, "deeply nested pattern runs");
    }
}
