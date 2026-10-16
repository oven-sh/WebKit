// A lookahead does not backtrack into its content after it has matched, and the loop copy that
// optimizeBOL() makes is also tried at the index where the once-through alternatives failed. A copy
// of a lookahead that has lost its ^ alternative takes a later alternative at the start of the
// input, where the pattern itself takes the ^ alternative, and a backreference then reads other
// captures. So the loop copy keeps such a lookahead whole, or drops it whole when all of its
// alternatives need ^. See copyTerm() in YarrPattern.cpp. The bug is in Yarr's pattern analysis, so
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
    // At 0 the lookahead takes ^(a), so \1 is "a" and the rest fails. It is not tried again with
    // the other alternative, and at 1 it has nothing to match.
    check(/(?=^(a)|a)\1a/, "ab", null);
    check(/(?=^a|(a))\1b/, "ab", null);
    check(/(?=^(a)|a)\1a|^2/, "ab", null);
    check(/^2|(?=^(a)|a)\1a/, "ab", null);
    check(/(?=^(?<n>a)|a)\k<n>a/, "ab", null);
    check(/(?=^(a)|a)(?:\1a)/, "ab", null);
    check(/(?=^(a)|a)a(?<=\1a)/, "ab", null);
    check(/(?=^(a)|a)(?!\1b)a/, "ab", null);
    check(/(?=(^)a|(a))\2b/, "ab", null);

    // The alternatives can be deeper in the lookahead, and the lookahead deeper in the pattern.
    check(/(?=(?:^(a)|a))\1a/, "ab", null);
    check(/(?=((?:^(a)|a)))\2a/, "ab", null);
    check(/(?=(?=^(a)|a))\1a/, "ab", null);
    check(/(?:(?=^(a)|a))\1a/, "ab", null);
    check(/((?=^(a)|a)\2a)/, "ab", null);
    check(/(?=(?=^(a)|a)\1a)a/, "ab", null);

    // An optional group with ^ is content the loop copy drops, too.
    check(/(?=(?:^(a))?(.))\1a/, "ab", null);
    check(/(?=(?:^(a))?(.))\1a/, "aa", [0, "aa", "a", "a"]);
    check(/(?=(?:^(a))?(.))\1a/, "ba", [1, "a", undefined, "a"]);

    // A lookbehind alternative is not filtered, so it keeps the lookahead in the loop copy.
    check(/(?=^(a)|(?<=(?=^)))\1a/, "ab", null);
    check(/(?=^(a)|(?<=(?=^)x))\1a/, "xa", [1, "a", undefined]);
    check(/(?=^(a)|(?<=(?=^)x))\1a/, "yxa", null);

    // The same patterns still match where the pattern does.
    check(/(?=^(a)|a)\1a/, "aa", [0, "aa", "a"]);
    check(/(?=^(a)|a)\1a/, "ba", [1, "a", undefined]);
    check(/(?=^(a)|a)\1a/, "bab", [1, "a", undefined]);
    check(/(?=^a|(a))\1b/, "xab", [1, "ab", "a"]);
    check(/(?=^(a)|b)\1b/, "xb", [1, "b", undefined]);
    check(/(?=^(a)|a)\1a|^2/, "2", [0, "2", undefined]);
    {
        const re = /(?=^(a)|a)\1a/g;
        re.lastIndex = 1;
        const result = re.exec("aa");
        shouldBe(result && result.index, 1, "exec from lastIndex 1");
        shouldBe(result && result[1], undefined, "exec from lastIndex 1");
    }

    // A lookahead whose alternatives all need ^ matches at 0 only.
    check(/(?=^(a))\1b|^2/, "ab", [0, "ab", "a"]);
    check(/(?=^(a))\1b|^2/, "xab", null);
    check(/x(?=^(a))\1|^2/, "xa", null);
    check(/(?=^(a)|^(b))\1b|^2/, "ab", [0, "ab", "a", undefined]);
    check(/(?=^(a)|^(b))\1b|^2/, "xab", null);

    // Without a backreference nothing reads the captures, and the result is the same.
    check(/(?=^(a)|a)a/, "ab", [0, "a", "a"]);
    check(/(?=^(a)|a)a/, "ba", [1, "a", undefined]);
    check(/(?=^(a)|(b))./, "ab", [0, "a", "a", undefined]);
    check(/(?=^(a)|(b))./, "xb", [1, "b", undefined, "b"]);
    shouldBe("ab".replace(/(?=^(a)|(b))./g, "[$1$2]"), "[a][b]", "replace global");
}

for (var i = 0; i < testLoopCount; ++i)
    step();
