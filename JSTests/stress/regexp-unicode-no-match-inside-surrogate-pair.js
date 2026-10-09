//@ runDefault
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--verifyRegExpJITReads=1")

// With the u or v flag the input is a sequence of code points. The offset between the two halves of
// a surrogate pair is not a position of the input, so nothing matches there: not a pattern that can
// match without consuming a character, and not an expression behind the /.*X.*/ rewrite, where the
// reported bounds are those of the line and do not show where the expression matched.

function shouldBe(actual, expected) {
    actual = JSON.stringify(actual);
    expected = JSON.stringify(expected);
    if (actual !== expected)
        throw new Error("expected " + expected + " but got " + actual);
}

// [index, length] of exec(), or null. test() and search() run the match-only code, which has to agree.
function probe(re, string, lastIndex = 0) {
    re.lastIndex = lastIndex;
    const match = re.exec(string);
    re.lastIndex = lastIndex;
    if (re.test(string) !== (match !== null))
        throw new Error(re + ".test() disagrees with exec() on " + JSON.stringify(string));
    re.lastIndex = 0;
    if (!lastIndex && string.search(re) !== (match ? match.index : -1))
        throw new Error(re + ": search() disagrees with exec() on " + JSON.stringify(string));
    return match ? [match.index, match[0].length] : null;
}

const pair = "\u{1F600}"; // \uD83D\uDE00

// 1. /.*X.*/ is rewritten to X followed by a DotStarEnclosure term, which widens a match of X to its
// line and so reports the line start, not the offset where X was attempted.

// \B holds between \uD83D and \uDE00. Every position of "a\u{1F600}a" is a word boundary.
shouldBe(probe(/.*\B.*/u, "a" + pair + "a"), null);
shouldBe(probe(/.*(?<=\uD83D).*/u, "a" + pair + "a"), null);
shouldBe(probe(/^.*\B.*$/u, "a" + pair + "a"), null);
shouldBe(probe(/\B/u, "a" + pair + "a"), null);

// Every flag that keeps the rewrite.
shouldBe(probe(/.*\B.*/v, "a" + pair + "a"), null);
shouldBe(probe(/.*\B.*/su, "a" + pair + "a"), null);
shouldBe(probe(/^.*\B.*$/su, "a" + pair + "a"), null);
shouldBe(probe(/^.*\B.*$/mu, "a" + pair + "a"), null);
shouldBe(probe(/^.*\B.*$/msu, "a" + pair + "a"), null);
shouldBe(probe(/.*\B.*/iu, "a" + pair + "a"), null);
shouldBe(probe(/.*\B.*/gu, "a" + pair + "a"), null);
shouldBe(probe(/.*\B.*/du, "a" + pair + "a"), null);

// Other expressions that hold only between the halves: a lookbehind that reads the lead surrogate
// alone, and a negative lookahead, because no character can be read forward from there.
shouldBe(probe(/.*(?<=[\uD800-\uDBFF]).*/u, pair), null);
shouldBe(probe(/.*(?<=\p{Cs}).*/u, pair), null);
shouldBe(probe(/.*(?<![a\u{1F600}]|^).*/u, "a" + pair + "a"), null);
shouldBe(probe(/.*(?<=\uD83D)(?!a).*/u, "a" + pair + "a"), null);
shouldBe(probe(/.*(?![^]).*/u, "a" + pair + "\nb"), [4, 1]);
shouldBe(probe(/.*(?![^]).*$/u, "a" + pair + "\nb"), [4, 1]);
shouldBe(probe(/.*(?:(?![^])|q).*/u, "a" + pair + "\nb"), [4, 1]);

// An expression with a quantified group, and one that matches without a character in 2^32 ways.
// The JIT gives up a start between the halves as a whole: it does not try those ways one by one.
shouldBe(probe(/.*(?<=\uD83D)(?:a|b)*.*/u, "a" + pair + "a"), null);
shouldBe(probe(/.*(?<=\uD83D)(?:a|b)*?.*/u, "a" + pair + "a"), null);
shouldBe(probe(/.*(?<=\uD83D)(?:a|b)*.*/u, "a" + pair + "\n\uD83Dab"), [4, 3]);
shouldBe(probe(/.*(?<=\uD83D)[^q\n]*.*/u, "a" + pair + "\uD83Dc"), [0, 5]);
{
    const manyWays = new RegExp(".*(?<=\\uD83D)" + "(?:a?|b?)".repeat(32) + ".*", "u");
    shouldBe(probe(manyWays, "a" + (pair + "a").repeat(20)), null);
    shouldBe(probe(manyWays, "a" + pair + "\n\uD83Da"), [4, 2]);
}

// The search goes on past the pair: a later position where the expression holds is found, and the
// match is widened to the line of that position.
shouldBe(probe(/.*\B.*/u, "a" + pair + "aa"), [0, 5]);
shouldBe(probe(/.*\B.*/u, "a" + pair + "a\naa"), [5, 2]);
shouldBe(probe(/^.*\B.*$/mu, "a" + pair + "a\naa\n" + pair), [5, 2]);
shouldBe(probe(/.*\B.*/su, "a" + pair + "a\naa"), [0, 7]);
shouldBe(probe(/^.*\B.*$/u, "a" + pair + "a\naa"), null);
shouldBe(probe(/.*(?<=\uD83D).*/u, "a" + pair + "\n\uD83Da"), [4, 2]);
shouldBe(probe(/.*\B.*/u, "a" + (pair + "a").repeat(200)), null);
shouldBe(probe(/.*\B.*/u, "a" + (pair + "a").repeat(200) + "a"), [0, 602]);
shouldBe(probe(/.*\B.*/u, ("a" + pair + "a\n").repeat(200) + "aa"), [1000, 2]);

// A global RegExp resumed before, inside, or after the pair.
shouldBe(probe(/.*\B.*/gu, "a" + pair + "a", 1), null);
shouldBe(probe(/.*\B.*/gu, "a" + pair + "a", 2), null);
shouldBe(probe(/.*\B.*/gu, "a" + pair + "a", 3), null);
shouldBe(probe(/.*\B.*/gu, "a" + pair + "a\naa", 2), [5, 2]);
shouldBe(probe(/.*\B.*/gsu, "a" + pair + "a\naa", 2), [1, 6]);
{
    const re = /.*\B.*/gu;
    re.lastIndex = 2;
    shouldBe(re.exec("a" + pair + "a\naa").index, 5);
    shouldBe(re.lastIndex, 7);
    re.lastIndex = 2;
    shouldBe(re.exec("a" + pair + "a"), null);
    shouldBe(re.lastIndex, 0);
}

// What the String methods see.
shouldBe(("a" + pair + "a").match(/.*\B.*/u), null);
shouldBe(("a" + pair + "a").match(/.*\B.*/gu), null);
shouldBe([...("a" + pair + "a").matchAll(/.*\B.*/gu)].length, 0);
shouldBe(("a" + pair + "a").replace(/.*\B.*/u, "x"), "a" + pair + "a");
shouldBe(("a" + pair + "a").replace(/.*\B.*/gu, "x"), "a" + pair + "a");
shouldBe(("a" + pair + "a").split(/.*\B.*/u), ["a" + pair + "a"]);
shouldBe(("a" + pair + "a\naa").replace(/.*\B.*/gu, "x"), "a" + pair + "a\nx");

// A position next to a whole pair, or next to a surrogate that has no partner, is a position.
shouldBe(probe(/.*\B.*/u, pair), [0, 2]);
shouldBe(probe(/.*\B.*/u, pair + pair), [0, 4]);
shouldBe(probe(/.*\b.*/u, pair + "a"), [0, 3]);
shouldBe(probe(/.*(?<=\u{1F600}).*/u, "a" + pair + "a"), [0, 4]);
shouldBe(probe(/.*(?<=\uD83D).*/u, "a\uD83Da"), [0, 3]);
shouldBe(probe(/.*(?<=\uDE00).*/u, "a\uDE00a"), [0, 3]);
shouldBe(probe(/.*(?<=\uD83D).*/u, "\uDE00\uD83D"), [0, 2]);
shouldBe(probe(/.*\B.*/u, "\uDE00\uDE00"), [0, 2]);
shouldBe(probe(/.*\B.*/u, "a\uDE00\uD83Da"), [0, 4]);
shouldBe(probe(/.*(?<=\uD83D)\B.*/u, "a\uD83D\uD83D" + pair), [0, 5]);

// The first and the last code unit of each surrogate range make a pair. The code units next to the
// ranges (U+D7FF, U+E000) do not, and a 16-bit subject with no surrogate has only positions.
shouldBe(probe(/.*(?<=\uD800).*/u, "a\uD800\uDC00"), null);
shouldBe(probe(/.*(?<=\uDBFF).*/u, "a\uDBFF\uDFFF"), null);
shouldBe(probe(/.*(?<=\uD800).*/u, "a\uD800\uDFFF\n\uD800"), [4, 1]);
shouldBe(probe(/.*(?<=\uD7FF).*/u, "a\uD7FF\uDC00"), [0, 3]);
shouldBe(probe(/.*(?<=\uDBFF).*/u, "a\uDBFF\uE000"), [0, 3]);
shouldBe(probe(/.*(?<=\uDBFF).*/u, "a\uDBFF\uDBFF\uDC00"), [0, 4]);
shouldBe(probe(/.*(?<=\uE000).*/u, "\uD83D\uE000\uDE00"), [0, 3]);
shouldBe(probe(/.*(?<=\uD7FF).*/u, "\uFFFF\uD7FF\uE000"), [0, 3]);
shouldBe(probe(/.*(?<=\u4E2D).*/u, "a\u4E2D\u6587a\n\u6587\u6587"), [0, 4]);
shouldBe(probe(/.*(?<=\u6587)\B.*/u, "a\u4E2D\u6587a\n\u6587\u6587"), [5, 2]);

// Neither engine reads a code unit outside the subject. These subjects share the characters of a
// longer string. One ends with a lead surrogate, and a trail surrogate comes next in the longer
// string. The other begins with a trail surrogate, and a lead surrogate comes before it there.
// They are 32 code units long: a short substring can get a copy of its characters, and shares none.
function leadAtEndOfSharedCharacters() {
    return ("a".repeat(31) + pair + "q").substring(0, 32);
}
function trailAtStartOfSharedCharacters() {
    return ("q" + pair + "a".repeat(31)).substring(2);
}
shouldBe(probe(/(?<=[^]{33})/u, leadAtEndOfSharedCharacters()), null);
shouldBe(probe(/.*(?<=\uD83D).*/u, leadAtEndOfSharedCharacters()), [0, 32]);
shouldBe(probe(/.*(?=\uDE00).*/u, trailAtStartOfSharedCharacters()), [0, 32]);

// Without the u or v flag the input is code units, and the offset between the halves is a position.
shouldBe(probe(/.*\B.*/, "a" + pair + "a"), [0, 4]);
shouldBe(probe(/.*(?<=\uD83D).*/, "a" + pair + "a"), [0, 4]);
shouldBe(probe(/^.*\B.*$/, "a" + pair + "a"), [0, 4]);
shouldBe(probe(/.*\B.*/s, "a" + pair + "\na"), [0, 5]);
shouldBe(probe(/\B/, "a" + pair + "a"), [2, 0]);
shouldBe(probe(/(?<=\uD83D)/, pair), [1, 0]);

// A sticky pattern is not rewritten and does not search.
shouldBe(probe(/.*\B.*/yu, "a" + pair + "a"), null);
shouldBe(probe(/.*\B.*/yu, "a" + pair + "aa"), [0, 5]);
shouldBe(probe(/\B/yu, "a" + pair + "a", 2), null);
shouldBe(probe(/a/yu, pair + "a"), null);
shouldBe(probe(/a/y, pair + "a"), null);
shouldBe(probe(/a/yu, "ba"), null);
shouldBe(probe(/a/y, "ba"), null);

// An 8-bit subject has no surrogates.
shouldBe(probe(/.*\B.*/u, "ab"), [0, 2]);
shouldBe(probe(/.*\B.*/u, "a b"), null);

// 2. A body that can match without consuming a character, in each shape of the JIT's search loop.
// What tells the shapes apart is how the last alternative leads back to the first one.

// One alternative.
shouldBe(probe(/\B/u, pair), [0, 0]);
shouldBe(probe(/\B/gu, pair, 1), [0, 0]);
shouldBe(probe(/\B/gu, pair, 2), [2, 0]);
shouldBe(probe(/(?<=\uD83D)/u, pair), null);
shouldBe(probe(/(?<=\uD83D)/u, "a" + pair + pair + "\uD83D"), [6, 0]);
shouldBe(probe(/(?![^])/u, "a" + pair + "\nb"), [5, 0]);
shouldBe(probe(/a?\B/u, "a" + pair), [3, 0]);
shouldBe(probe(/(?:)\B(?:a|)/u, "a" + pair + "a-"), [5, 0]);

// Fixed size, the last alternative one unit longer than the first.
shouldBe(probe(/\B|q/u, "a" + pair + "a"), null);
shouldBe(probe(/\B|q/u, "a" + pair), [3, 0]);
shouldBe(probe(/(?<=\uD83D)|q/u, pair + pair + "q"), [4, 1]);

// The last alternative longer than the first.
shouldBe(probe(/a?\B|q/u, "X" + pair + "Z"), null);
shouldBe(probe(/\B|qq/u, "a" + pair + "a"), null);
shouldBe(probe(/\B|qq/u, "a" + pair), [3, 0]);
shouldBe(probe(/a?\B|qq/u, "X" + pair + "-"), [3, 0]);

// The last alternative not longer than the first.
shouldBe(probe(/q|\B/u, "a" + pair + "a"), null);
shouldBe(probe(/q|\B/u, "a" + pair), [3, 0]);
shouldBe(probe(/qa?|\B/u, "a" + pair + "-"), [3, 0]);

// The last alternative lacks input at the start before the pair's trail.
shouldBe(probe(/\B|qqq/u, "a" + pair), [3, 0]);
shouldBe(probe(/(?<=\uD83D)|qqq/u, "ab" + pair), null);
shouldBe(probe(/a?\B|qqq/u, "X" + pair), [3, 0]);

// The first alternative lacks input there, and a later one that claims less is tried.
shouldBe(probe(/qq|\B/u, "a" + pair), [3, 0]);
shouldBe(probe(/qq|\B/u, "a" + pair + "a"), null);
shouldBe(probe(/qa?q|\B/u, "a" + pair), [3, 0]);
shouldBe(probe(/qq|\B|qqq/u, "a" + pair), [3, 0]);
shouldBe(probe(/qa?q|\B|qqq/u, "a" + pair), [3, 0]);
shouldBe(probe(/qqqq|(?<=\uD83D)|qq/u, "ab" + pair), null);

// Pairs at both ends of the subject, and a search that runs off the end.
shouldBe(probe(/(?<=\uD83D)|qq/u, pair), null);
shouldBe(probe(/qq|(?<=\uD83D)/u, pair), null);
shouldBe(probe(/(?<=\uD83D)/u, pair + pair + pair), null);
shouldBe(probe(/(?<=\uD83D)/u, pair + "\uD83D"), [3, 0]);
shouldBe(probe(/(?<=\uD83D)/u, "\uD83D" + pair), [1, 0]);
shouldBe(probe(/(?<=\uDE00)/u, pair + "\uDE00"), [3, 0]);
shouldBe(probe(/(?<=\uDE00)/u, "\uDE00" + pair), [1, 0]);

// Every occurrence, through the String methods that drive the search from lastIndex.
shouldBe([...(pair + "a" + pair).matchAll(/\B/gu)].map(match => match.index), [0, 5]);
shouldBe([...("-" + pair + pair + "-").matchAll(/\B|q/gu)].map(match => match.index), [0, 1, 3, 5, 6]);
shouldBe((pair + pair).replace(/(?<=\uD83D)|$/gu, "|"), pair + pair + "|");
shouldBe((pair + "a" + pair).split(/\B/u), [pair + "a" + pair]);
shouldBe(("-" + pair + "-").split(/\B/u), ["-", pair, "-"]);

// 3. A body that must consume a character fails between the halves by itself: its first forward
// read finds half a character there.
shouldBe(probe(/\uDE00/u, pair), null);
shouldBe(probe(/\uDE00/u, "x\uDE00"), [1, 1]);
shouldBe(probe(/(?<=\uD83D)\uDE00/u, pair), null);
shouldBe(probe(/(?<=\uD83D)\uDE00/u, "\uD83Dx\uDE00"), null);
shouldBe(probe(/\B\uDE00/u, pair), null);
shouldBe(probe(/\B[\uDC00-\uDFFF]/u, pair), null);
shouldBe(probe(/\B[^a]/u, pair), [0, 2]);
shouldBe(probe(/\B./su, "a" + pair), null);
shouldBe(probe(/(?<=\uD83D)[^]/u, "a" + pair + "b"), null);
shouldBe(probe(/(?<=\uD83D)\p{Cs}/u, pair + pair), null);
shouldBe(probe(/(?<=(\uDE00).)\1/u, "\uDE00" + pair), null);
shouldBe(probe(/(?<=(\uDE00).)\1a/u, "\uDE00" + pair + "a"), null);

// The same for a quantified term, which reads in a loop of its own.
shouldBe(probe(/(?<=\uD83D)\uDE00+/u, pair + "\uDE00"), null);
shouldBe(probe(/(?<=\uD83D)\uDE00+?/u, pair + "\uDE00"), null);
shouldBe(probe(/(?<=\uD83D)\uDE00{2}/u, pair + "\uDE00"), null);
shouldBe(probe(/(?<=\uD83D)[\uDC00-\uDFFF]+/u, pair + "\uDE00"), null);
shouldBe(probe(/(?<=\uD83D)[^a]{1,2}/u, pair + "\uDE00"), null);
shouldBe(probe(/(?<=\uD83D).+/su, pair + "a"), null);
shouldBe(probe(/(?<=\uD83D)\W*a/u, pair + "a"), null);
shouldBe(probe(/\B\uDE00+/u, pair), null);
shouldBe(probe(/(?<=\uDE00)\uDE00+/u, "\uDE00\uDE00\uDE00"), [1, 2]);

// 4. A pattern whose alternatives all have a fixed size and end in $ starts its search that size
// before the end, which can be between the halves.
shouldBe(probe(/\B\uDE00$/u, "a" + pair), null);
shouldBe(probe(/(?<=\uD83D)\uDE00$/u, pair), null);
shouldBe(probe(/\B$|q$/u, "a" + pair), [3, 0]);
shouldBe(probe(/(?<=\uD83D)$|q$/u, pair), null);
shouldBe(probe(/(?<=\uD83D)$|q$/u, pair + "q"), [2, 1]);

// 5. Sweeps. Behind the /.*X.*/ rewrite an expression gives what the same pattern gives as written
// (a second alternative that never matches keeps the rewrite away), and no pattern reports a match
// that starts or ends between the halves.
{
    const bodies = ["\\B", "\\b", "(?<=\\uD83D)", "(?<!\\uD83D)", "(?<=\\uDE00)", "(?![^])", "(?!.)", "(?<=\\p{Cs})", "(?<!a)(?!a)", "a?\\B", "(?:)", "a*", "(?=\\uDE00)", "(?<![^])", "(?:a|\\B)", "(?<=\\P{L})", "x?", "\\uDE00", "[^a]", "(?<=\\uD83D).", "\\Ba", "(?:a|\\B)*", "(?<=\\uD83D)(?:a|b)*?"];
    const shapes = [["", "", ""], ["^", "$", ""], ["^", "", ""], ["", "$", ""], ["", "", "s"], ["^", "$", "s"], ["^", "", "m"], ["", "$", "m"], ["^", "$", "ms"], ["", "", "g"], ["", "", "gs"], ["^", "", "gm"], ["^", "$", "gms"]];
    const subjects = ["a" + pair + "a", "a" + pair + "\nb", pair, pair + pair, "a" + pair + "a--", "--" + pair + "\n" + pair + "aa", "\uDE00\uDE00", "a\uD83Da", "a\uD83D\uD83D\uDE00", "\uDE00" + pair, pair + "\uD83D", "a\n" + pair, "x " + pair + " y\n" + pair + pair + "z", pair + "\u2028" + pair];
    const splitsPair = (string, offset) => offset > 0 && offset < string.length && (string.charCodeAt(offset) & 0xfc00) === 0xdc00 && (string.charCodeAt(offset - 1) & 0xfc00) === 0xd800;
    const run = (re, string, lastIndex) => {
        re.lastIndex = lastIndex;
        const match = re.exec(string);
        if (match && (splitsPair(string, match.index) || splitsPair(string, match.index + match[0].length)))
            throw new Error(re + " matched between the halves of a pair in " + JSON.stringify(string) + ": index " + match.index + ", length " + match[0].length);
        return match ? [match.index, match[0].length, re.lastIndex] : [null, re.lastIndex];
    };
    for (const unicodeFlag of ["u", "v"]) {
        for (const body of bodies) {
            for (const [prefix, suffix, flags] of shapes) {
                const source = prefix + ".*" + body + ".*" + suffix;
                const rewritten = new RegExp(source, flags + unicodeFlag);
                const asWritten = new RegExp("(?:" + source + ")|(?!)", flags + unicodeFlag);
                const bare = new RegExp(body, flags + unicodeFlag);
                for (const subject of subjects) {
                    const lastIndexLimit = flags.includes("g") ? subject.length : 0;
                    for (let lastIndex = 0; lastIndex <= lastIndexLimit; ++lastIndex) {
                        shouldBe(run(rewritten, subject, lastIndex), run(asWritten, subject, lastIndex));
                        run(bare, subject, lastIndex);
                    }
                }
            }
        }
    }
}
