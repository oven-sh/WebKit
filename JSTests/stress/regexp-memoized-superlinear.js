//@ runDefault("--useMemoizedRegExp=1")
//@ runDefault("--useMemoizedRegExp=1", "--useRegExpJIT=0")
//@ runDefault("--useMemoizedRegExp=1", "--forceMemoizedRegExp=1", "--useRegExpJIT=0")

// Each pattern is "a shape that backtracks exponentially (or with a high power of the input
// length) and fails | something that matches". Without match memoization the first alternative
// either runs into Yarr::matchLimit, which makes the whole match fail, or does not finish. With it,
// the first alternative fails in time proportional to the input and the second one matches.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

function shouldMatchWithLastGroup(re, input) {
    for (let i = 0; i < 2; ++i) {
        const match = re.exec(input);
        if (!match)
            throw new Error(re + " did not match an input of length " + input.length);
        shouldBe(match.index, 0, String(re));
        shouldBe(match[0], input, String(re));
        for (let group = 1; group < match.length - 1; ++group)
            shouldBe(match[group], undefined, re + " group " + group);
        shouldBe(match[match.length - 1], input, re + " last group");
        shouldBe(re.test(input), true, String(re));
    }
}

const a = n => "a".repeat(n);

// Nested quantifiers.
shouldMatchWithLastGroup(/^(?:(a*)*b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(a+)+b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(a|a)*b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(a|aa)*b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(?:a*?)*?b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:((a*)*)*b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(?:a?){2,}b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:((a+){2,3}){2,3}b|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(.*a){12}b|([^]*))$/, a(300) + "c");
shouldMatchWithLastGroup(/^(?:(?:a|b|ab)*c|([^]*))$/, "ab".repeat(100) + "d");
shouldMatchWithLastGroup(/^(?:(?:(?:a|b)*a){2}c|([^]*))$/, "ab".repeat(200) + "d");
shouldMatchWithLastGroup(/^(?:(?:xy|x){2,3}?(?:y|yx)*w|([^]*))$/, "xy".repeat(200) + "z");

// Groups with many categories of iteration.
shouldMatchWithLastGroup(/^(?:(a|a){1,60}b|([^]*))$/, a(40) + "c");
shouldMatchWithLastGroup(/^(?:(a|a){40}b|([^]*))$/, a(40) + "c");
shouldMatchWithLastGroup(/^(?:(a|a){33,}b|([^]*))$/, a(60) + "c");

// A match that the JIT code finishes on its own without the option, and hands over with it: iterations
// of the inner group are entered far more often than a term with a cache point is backtracked into.
{
    const re = /(?:(?:ab){32})*c/;
    const input = "ab".repeat(3000) + "x" + "ab".repeat(32) + "c";
    for (let i = 0; i < 2; ++i) {
        const match = re.exec(input);
        shouldBe(match !== null, true, String(re));
        shouldBe(match.index, 6001, String(re));
        shouldBe(match[0].length, 65, String(re));
    }
}

// Optional characters in front of something that fails.
shouldMatchWithLastGroup(new RegExp("^(?:" + "\\w?\\d?".repeat(20) + "!|([^]*))$"), "1".repeat(40));

// Consecutive loops over the same characters.
shouldMatchWithLastGroup(/^(?:a*a*a*a*b|([^]*))$/, a(3000) + "c");
// Two loops are enough, even when the second one cannot be backtracked into (nothing it matches can start
// what follows it): every character the first gives back runs the second again.
shouldMatchWithLastGroup(/^(?:a*a*b|([^]*))$/, a(10000) + "c");
shouldMatchWithLastGroup(/^(?:\w*?\w*@|([^]*))$/, a(10000) + "c");
shouldMatchWithLastGroup(/^(?:a*?a*?a*?a*?b|([^]*))$/, a(3000) + "c");
shouldMatchWithLastGroup(/^(?:.*a.*a.*a.*b|([^]*))$/, a(3000) + "c");
shouldMatchWithLastGroup(/^(?:a{0,500}a{0,500}a{0,500}b|([^]*))$/, a(1000) + "c");
shouldMatchWithLastGroup(/^(?:[^\/]*\/?[^\/]*\/?[^\/]*\/?[^\/]*x|([^]*))$/, a(3000) + "c");

// One loop is enough when the match is attempted from one position after another: each attempt runs
// the loop over the characters the last one ran it over.
{
    const spaces = " ".repeat(30000);
    for (let i = 0; i < 2; ++i) {
        shouldBe(/\s+$/.exec(spaces + "x"), null, "/\\s+$/");
        shouldBe(/a*b/.test(a(30000)), false, "/a*b/");
        shouldBe(/[^@]+@x/.exec(a(30000)), null, "/[^@]+@x/");
        shouldBe(/x(?:\s+)$/.exec("x" + spaces + "y"), null, "/x(?:\\s+)$/");
        shouldBe(/\s+$/.exec("x" + spaces).index, 1, "/\\s+$/ matching");
    }
}

// Lookarounds.
shouldMatchWithLastGroup(/^(?:(?=(a*)*b)a|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:(?!(a|a)*c$)a|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:[^]*(?<=b(a|a)*)c|([^]*))$/, a(200) + "c");
shouldMatchWithLastGroup(/^(?:[^]*(?<!^(a|aa)*)c|([^]*))$/, a(200) + "c");

// What URLPattern generates for /files/:name{.:ext}+, /:owner-:repo-:ref and /*/a/*/b/*/c (oven-sh/bun#42603).
shouldMatchWithLastGroup(/^(?:\/files(?:\/([^\/]+?))(?:\.((?:[^\/]+?)(?:\.(?:[^\/]+?))*))|([^]*))$/v, "/files/a" + ".b".repeat(100) + "/");
shouldMatchWithLastGroup(/^(?:(?:\/([^\/]+?))-([^\/]+?)-([^\/]+?)|([^]*))$/v, "/" + "a-".repeat(2000) + "/");
shouldMatchWithLastGroup(/^(?:(?:\/(.*))\/a(?:\/(.*))\/b(?:\/(.*))\/c|([^]*))$/v, "/" + "/a/b".repeat(1000) + "/");

// A match that Yarr::matchLimit used to turn into null: what URLPattern generates for the search "{-:x}+-_:r".
{
    const re = /^(?:-([^]+?))+-_([^]+?)$/v;
    const match = re.exec("-a-_" + "-b".repeat(40));
    shouldBe(match !== null, true, String(re));
    shouldBe(match[1], "a", String(re));
    shouldBe(match[2], "-b".repeat(40), String(re));
}

// 16-bit subjects, non-BMP characters, ignoreCase.
shouldMatchWithLastGroup(/^(?:(\u{1F600}*)*b|([^]*))$/u, "\u{1F600}".repeat(150) + "c");
shouldMatchWithLastGroup(/^(?:(.|..)*b|([^]*))$/us, "\u{1F600}".repeat(150) + "c");
shouldMatchWithLastGroup(/^(?:(a|A)*b|([^]*))$/i, "aA".repeat(100) + "\u0100");
shouldMatchWithLastGroup(/^(?:(?:\/([^\/]+?))-([^\/]+?)-([^\/]+?)|([^]*))$/v, "/" + "\u{1F600}-".repeat(1200) + "/");

// Global and sticky: the search starts at lastIndex.
{
    const re = /(?:(a|a)*b|c)/g;
    const input = a(200) + "c";
    re.lastIndex = 3;
    const match = re.exec(input);
    shouldBe(match.index, 200, String(re));
    shouldBe(re.lastIndex, 201, String(re));
}
{
    const re = /(?:(a|a)*b|a*c)/y;
    const input = "xx" + a(200) + "c";
    re.lastIndex = 2;
    const match = re.exec(input);
    shouldBe(match.index, 2, String(re));
    shouldBe(match[0], a(200) + "c", String(re));
    shouldBe(re.lastIndex, input.length, String(re));
}
