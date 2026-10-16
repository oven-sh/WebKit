// A lookbehind reads the input before the position the match starts at, so a ^ inside it can hold
// at the start of the input while the match starts later. The loop copy optimizeBOL() makes must
// therefore keep everything nested in a lookbehind, including a lookahead, whose ^ is a Forward
// term again. See copyTerm() in YarrPattern.cpp.
//
// regexp-lookahead-with-bol-inside-lookbehind-is-not-dropped.js has the same shapes with a second
// alternative that has no ^. Every pattern here has an alternative that starts with ^ as well: the
// parser does not count a ^ inside a lookbehind (assertionBOL()), so only that other ^ makes
// recomputeStartsWithBOL() and optimizeBOL() run at all. The bug is in Yarr's pattern analysis, so
// it reproduces in every tier; the loop only makes sure the JIT tiers agree.

function shouldBe(actual, expected, context) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected: " + expected + " for " + context);
}

// `expected` is null, or the match index followed by the matched text and the captures.
function check(re, string, expected) {
    const context = re + " against " + JSON.stringify(string);
    shouldBe(re.test(string), expected !== null, context);
    re.lastIndex = 0;
    const result = re.exec(string);
    re.lastIndex = 0;
    if (expected === null) {
        shouldBe(result, null, context);
        return;
    }
    if (result === null)
        throw new Error("no match, expected index " + expected[0] + " for " + context);
    shouldBe(result.index, expected[0], context);
    shouldBe(result.length, expected.length - 1, context);
    for (let i = 0; i < result.length; ++i)
        shouldBe(result[i], expected[i + 1], context + " capture " + i);
}

function step() {
    // The lookbehind walks back from 1 over "a" to 0, and ^ holds at 0.
    check(/(?<=(?=^)a)b|^2/, "ab", [1, "b"]);
    check(/(?<=(?=^|x)a)b|^2/, "ab", [1, "b"]);
    check(/(?<=(?=^)\w)=|^#/, "a=1", [1, "="]);
    check(/^2|(?<=(?=^)a)b/, "ab", [1, "b"]);
    check(/^2|(?<=(?=^)a)b|^3/, "ab", [1, "b"]);
    check(/(?<=(?=^)aa)b|^2/, "aab", [2, "b"]);
    check(/(?<=(?=^)a+)b|^2/, "aaab", [3, "b"]);
    check(/(?<=(?=^)a)b|^2/i, "AB", [1, "B"]);
    check(/(?<=(?=^)a)b|^2/u, "ab", [1, "b"]);
    check(/(?<=(?=^)a)b|^2/s, "ab", [1, "b"]);

    // The ^ alternative and the rest of the pattern keep their own answers.
    check(/(?<=(?=^)a)b|^2/, "2", [0, "2"]);
    check(/(?<=(?=^)a)b|^2/, "ab2", [1, "b"]);
    check(/(?<=(?=^)a)b|^2/, "xab", null);
    check(/(?<=(?=^)a)b|^2/, "b", null);
    check(/(?<=(?=^)a)b|^2/, "a2", null);
    check(/(?<=(?=^)a)b|^2/, "", null);

    // The lookahead can sit anywhere under the lookbehind.
    check(/(?<=(?:(?=^))a)b|^2/, "ab", [1, "b"]);
    check(/(?<=((?=^)a))b|^2/, "ab", [1, "b", "a"]);
    check(/(?<=(?=(?=^))a)b|^2/, "ab", [1, "b"]);
    check(/(?<=(?=(?:^))a)b|^2/, "ab", [1, "b"]);
    check(/(?<=(?=(^))a)b|^2/, "ab", [1, "b", ""]);
    check(/(?<=(?=^a)a)b|^2/, "ab", [1, "b"]);
    check(/(?<=(?=^ab)a)b|^2/, "ab", [1, "b"]);
    check(/(?<=(?=^ab)a)b|^2/, "ac", null);
    check(/(?<=c|(?=^)a)b|^2/, "ab", [1, "b"]);
    check(/(?<=c|(?=^)a)b|^2/, "xcb", [2, "b"]);
    check(/(?<=(?<=(?=^)a)b)c|^2/, "abc", [2, "c"]);
    check(/(?<=(?=^)a)(?<=(?=^)a)b|^2/, "ab", [1, "b"]);

    // ... and the lookbehind anywhere in the alternative that is not anchored.
    check(/(?:(?<=(?=^)a)b)|^2/, "ab", [1, "b"]);
    check(/((?<=(?=^)a)b)|^2/, "ab", [1, "b", "b"]);
    check(/(?=(?<=(?=^)a)b)b|^2/, "ab", [1, "b"]);
    check(/(?=(?<=(?=^)a))b|^2/, "ab", [1, "b"]);
    check(/b(?<=(?=^)ab)|^2/, "ab", [1, "b"]);
    check(/b(?<=(?=^)ab)c|^2/, "abc", [1, "bc"]);
    check(/(?<=(?=^)a)|^2/, "ab", [1, ""]);
    check(/(?:(?<=(?=^)a)b|c)d|^2/, "abd", [1, "bd"]);
    check(/(?:(?<=(?=^)a)b|c)d|^2/, "xcd", [1, "cd"]);
    check(/(?:(?<=(?=^)a)b|c)d|^2/, "xabd", null);

    // A lookahead that is not under a lookbehind reads forward from a position that is not before
    // the match start, so its ^ content is still dropped from the loop copy.
    check(/x(?=^a)ab|^2/, "xab", null);
    check(/x(?=^)|^2/, "ax", null);
    check(/(?=^)a|^2/, "ba", null);
    check(/(?=^)a|^2/, "ab", [0, "a"]);
    check(/(?<=a)(?=^)b|^2/, "ab", null);

    // A negative lookbehind was never filtered. Its content decides in the same way.
    check(/(?<!(?=^)a)b|^2/, "ab", null);
    check(/(?<!(?=^)a)b|^2/, "xab", [2, "b"]);
    check(/(?<!(?=^)a)b|^2/, "b", [0, "b"]);

    // A ^ that is itself a term of the lookbehind was never filtered either.
    check(/(?<=^a)b|^2/, "ab", [1, "b"]);
    check(/(?<=^a)b|^2/, "xab", null);

    // A search that starts after 0 and matches later than where it starts runs the loop copy too.
    shouldBe(JSON.stringify("aab".match(/^a|(?<=(?=^)aa)b/g)), '["a","b"]', "match global");
    shouldBe("aab".replace(/^a|(?<=(?=^)aa)b/g, "Z"), "ZaZ", "replace global");
    shouldBe("ab".replace(/(?<=(?=^)a)b|^2/, "Z"), "aZ", "replace at 1");
    shouldBe("2ab".replace(/(?<=(?=^)a)b|^2/, "Z"), "Zab", "replace at 0");
    shouldBe("ab".search(/(?<=(?=^)a)b|^2/), 1, "search");
    shouldBe(JSON.stringify("ab ab".match(/(?<=(?=^)a)b|^2/g)), '["b"]', "match global, one match");
    shouldBe(JSON.stringify("ab".split(/(?<=(?=^)a)|^2/)), '["a","b"]', "split");
    {
        const re = /^a?^|(?<=(?=^)aa)b/g;
        re.lastIndex = 1;
        const result = re.exec("aab");
        shouldBe(result && result.index, 2, "exec from lastIndex 1");
        shouldBe(result && result[0], "b", "exec from lastIndex 1");
        shouldBe(re.lastIndex, 3, "lastIndex after exec from 1");
    }
    {
        const re = /(?<=(?=^)a)b|^2/y;
        re.lastIndex = 1;
        shouldBe(re.test("ab"), true, "sticky at 1");
        re.lastIndex = 2;
        shouldBe(re.test("xab"), false, "sticky at 2");
    }
}

for (var i = 0; i < testLoopCount; ++i)
    step();
