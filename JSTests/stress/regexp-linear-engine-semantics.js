//@ runDefault("--useRegExpLinearEngine=1", "--useDollarVM=1")

// The non-backtracking matcher finds the match, and the captures, that backtracking finds.
// Every expectation here is what the YARR JIT, the YARR interpreter and V8 report.

function describe(value)
{
    return JSON.stringify(value, (key, entry) => entry === undefined ? "<undefined>" : entry);
}

function test(regExp, subject, expected, lastIndex = 0)
{
    let statistics = $vm.regExpMatchStatistics(regExp, subject, lastIndex);
    if (statistics.engine !== "linear")
        throw new Error(regExp + " did not run on the non-backtracking matcher: " + statistics.refusal);
    // The compiler says what a position can cost at most, and no match costs more.
    if (statistics.steps > statistics.maximumStepsPerPosition * (subject.length - lastIndex + 1))
        throw new Error(regExp + " on " + describe(subject) + " from " + lastIndex + ": " + statistics.steps + " steps, with a bound of " + statistics.maximumStepsPerPosition + " per position");

    regExp.lastIndex = lastIndex;
    let match = regExp.exec(subject);
    let actual = null;
    if (match) {
        actual = { index: match.index, match: Array.from(match) };
        if (match.groups)
            actual.groups = { ...match.groups };
        if (regExp.global || regExp.sticky)
            actual.lastIndex = regExp.lastIndex;
    }
    if (describe(actual) !== describe(expected))
        throw new Error(regExp + " on " + describe(subject) + " from " + lastIndex + ": expected " + describe(expected) + " but got " + describe(actual));
    if (statistics.index !== (match ? match.index : -1))
        throw new Error(regExp + " on " + describe(subject) + ": regExpMatchStatistics matched at " + statistics.index);
}

// An iteration past the minimum that matches nothing is not taken (RepeatMatcher, step 2.a).
test(/(a*)?/, "b", { index: 0, match: ["", undefined] });
test(/(a*)*/, "b", { index: 0, match: ["", undefined] });
test(/(a*)+/, "b", { index: 0, match: ["", ""] });
test(/(a*){2}/, "b", { index: 0, match: ["", ""] });
test(/(a*){1,3}/, "b", { index: 0, match: ["", ""] });
test(/(a*){0,3}/, "b", { index: 0, match: ["", undefined] });
test(/(a*)*?/, "b", { index: 0, match: ["", undefined] });
test(/(a*)??/, "b", { index: 0, match: ["", undefined] });
test(/(?:a|())*/, "aa", { index: 0, match: ["aa", undefined] });
test(/(?:a|()){2,}/, "aa", { index: 0, match: ["aa", undefined] });
test(/(?:|a)*/, "aa", { index: 0, match: ["aa"] });
test(/(?:|a)+/, "aa", { index: 0, match: ["aa"] });
test(/(a?)*?b/, "aab", { index: 0, match: ["aab", "a"] });
test(/(a{0,2}){2}/, "aaa", { index: 0, match: ["aaa", "a"] });
test(/(a{0,2}?){2}/, "aaa", { index: 0, match: ["", ""] });
test(/(?:a{0,2}){0,2}?b/, "aaab", { index: 0, match: ["aaab"] });
test(/(?:a?b?)*c/, "abbac", { index: 0, match: ["abbac"] });
test(/(?:(?:a*)*b?)*c/, "aabac", { index: 0, match: ["aabac"] });
test(/(?:(a*)|b)*?c/, "aabc", { index: 0, match: ["aabc", undefined] });
test(/((?:)|a)*b/, "aab", { index: 0, match: ["aab", "a"] });

// Every iteration starts with the groups inside it cleared.
test(/(?:(a)|b)*/, "ab", { index: 0, match: ["ab", undefined] });
test(/(?:(a)|b){2}/, "ab", { index: 0, match: ["ab", undefined] });
test(/(?:(a)|(b))+/, "ab", { index: 0, match: ["ab", undefined, "b"] });
test(/(z)((a+)?(b+)?(c))*/, "zaacbbbcac", { index: 0, match: ["zaacbbbcac", "z", "ac", "a", undefined, "c"] });
test(/((a)|(b))+/, "ab", { index: 0, match: ["ab", "b", undefined, "b"] });
test(/((a)|(b)){2}/, "ab", { index: 0, match: ["ab", "b", undefined, "b"] });
test(/((a)|(b))*?$/, "ab", { index: 0, match: ["ab", "b", undefined, "b"] });
test(/(?:(a)|(b)|c)+?$/, "abc", { index: 0, match: ["abc", undefined, undefined] });
test(/x(?:...|(...))*y/, "xabcy", { index: 0, match: ["xabcy", undefined] });
test(/(?:(a)(b)?)+/, "aba", { index: 0, match: ["aba", "a", undefined] });
test(/(?:(a)|(b)|(c)){3}/, "abc", { index: 0, match: ["abc", undefined, undefined, "c"] });

// The alternative backtracking tries first wins, not the longest one.
test(/(a|ab)(c|bcd)(d*)/, "abcd", { index: 0, match: ["abcd", "a", "bcd", ""] });
test(/a|ab/, "ab", { index: 0, match: ["a"] });
test(/(?:a|ab)*c/, "aababc", { index: 0, match: ["aababc"] });
test(/(a|ab)*?b/, "aab", { index: 0, match: ["aab", "a"] });
test(/(a+)+b/, "aab", { index: 0, match: ["aab", "aa"] });
test(/()|a/, "a", { index: 0, match: ["", ""] });
test(/a|()/, "a", { index: 0, match: ["a", undefined] });
test(/(a)|(b)/, "b", { index: 0, match: ["b", undefined, "b"] });
test(/(a|b)*?(?:c|d)/, "abd", { index: 0, match: ["abd", "b"] });
test(/((a)|b)*?c/, "abc", { index: 0, match: ["abc", "b", undefined] });
test(/(?:a*?)(a*)/, "aaa", { index: 0, match: ["aaa", "aaa"] });
test(/(a*?)(a*?)$/, "aaa", { index: 0, match: ["aaa", "", "aaa"] });
test(/(a+?)(a*)/, "aaa", { index: 0, match: ["aaa", "a", "aa"] });

// Counted and lazy quantifiers.
test(/a{2,}?/, "aaaa", { index: 0, match: ["aa"] });
test(/a{2,3}?/, "aaaa", { index: 0, match: ["aa"] });
test(/(a){2,3}?/, "aaaa", { index: 0, match: ["aa", "a"] });
test(/(?:a{2}){2,3}?/, "aaaaaaaa", { index: 0, match: ["aaaa"] });
test(/(?:(?:a)+)+?b/, "aab", { index: 0, match: ["aab"] });
test(/a{0}b/, "ab", { index: 1, match: ["b"] });
test(/(?:ab){2,3}/, "abababab", { index: 0, match: ["ababab"] });
test(/[ab]{3,5}?c/, "ababac", { index: 0, match: ["ababac"] });
test(/(?:a|b){2}(?:a|b){0,2}?$/, "abab", { index: 0, match: ["abab"] });
test(/x{3}/, "xxxx", { index: 0, match: ["xxx"] });
test(/(a{2}){2}/, "aaaaa", { index: 0, match: ["aaaa", "aa"] });

// Assertions.
test(/\b/, "a", { index: 0, match: [""] });
test(/\B/, "a", null);
test(/\B/, "", { index: 0, match: [""] });
test(/^|a/, "ba", { index: 0, match: [""] });
test(/(?:^a|b)c/, "xbc", { index: 1, match: ["bc"] });
test(/a$|b/m, "b\na", { index: 0, match: ["b"] });
test(/^a/m, "b\na", { index: 2, match: ["a"] });
test(/^a/, "b\na", null);
test(/a$/m, "a\nb", { index: 0, match: ["a"] });
test(/^$/m, "a\n\nb", { index: 2, match: [""] });
test(/\bfoo\b/, "a foo b", { index: 2, match: ["foo"] });
test(/\Boo\b/, "a foo b", { index: 3, match: ["oo"] });
test(/^(?:a|b)*$/, "abab", { index: 0, match: ["abab"] });
test(/^(?:a|b)*$/, "abcb", null);
test(/$^/, "", { index: 0, match: [""] });

// Lookaheads and lookbehinds of bounded length, with the groups inside them.
test(/(?=(a{1,3}))/, "baaabac", { index: 1, match: ["", "aaa"] });
test(/(?=(a{1,3}?))/, "baaabac", { index: 1, match: ["", "a"] });
test(/(?!(a))b/, "b", { index: 0, match: ["b", undefined] });
test(/(?=(a))(?=(?:(b)|a))a/, "a", { index: 0, match: ["a", "a", undefined] });
test(/a(?=b)/, "acab", { index: 2, match: ["a"] });
test(/a(?!b)/, "abac", { index: 2, match: ["a"] });
test(/(?<=(\d)(\d))$/, "1053", { index: 4, match: ["", "5", "3"] });
test(/(?<=\$)\d+(\.\d*)?/, "cost $10.50", { index: 6, match: ["10.50", ".50"] });
test(/(?<!\$)\b\d+/, "cost $10 20", { index: 9, match: ["20"] });
test(/(?<=a(?=b))b/, "ab", { index: 1, match: ["b"] });
test(/(?<!a(?!b))b/, "ab", { index: 1, match: ["b"] });
test(/(?=a(?<=a))a/, "a", { index: 0, match: ["a"] });
test(/(?<=(a)(?=(b)))b/, "ab", { index: 1, match: ["b", "a", "b"] });
test(/(?:(?=(a))a)*/, "aa", { index: 0, match: ["aa", "a"] });
test(/(?:(?=(a))a|b)*/, "ab", { index: 0, match: ["ab", undefined] });
test(/(?<=(a|b){1,2})c/, "abc", { index: 2, match: ["c", "a"] });
test(/(?<=(?:(a)|b){2})c/, "abc", { index: 2, match: ["c", "a"] });
test(/(?<=^a)b/, "ab", { index: 1, match: ["b"] });
test(/(?<=\n)b/, "a\nb", { index: 2, match: ["b"] });
test(/(?<=(a{1,2}))b/, "aab", { index: 2, match: ["b", "aa"] });
test(/(?<=(a{1,2}?))b/, "aab", { index: 2, match: ["b", "a"] });
test(/(?<=([ab]{2})([ab]{1,2}))c/, "ababc", { index: 4, match: ["c", "ab", "ab"] });
test(/(?<=a|bc)d/, "bcd", { index: 2, match: ["d"] });

// A reference to a group that has not matched yet matches the empty string.
test(/\1(a)/, "a", { index: 0, match: ["a", "a"] });
test(/(a\1)/, "aa", { index: 0, match: ["a", "a"] });
test(/(\2a)(b)/, "ab", { index: 0, match: ["ab", "a", "b"] });
test(/(?:\1|(a))+/, "aa", { index: 0, match: ["aa", "a"] });

// Flags and modifiers.
test(/A/i, "a", { index: 0, match: ["a"] });
test(/[a-c]+/i, "xABCx", { index: 1, match: ["ABC"] });
test(/[^a-c]+/i, "ABCxYzABC", { index: 3, match: ["xYz"] });
test(/\u00E9/i, "\u00c9", { index: 0, match: ["\u00c9"] });
test(/(?i:a)b/, "Ab", { index: 0, match: ["Ab"] });
test(/(?i:a)b/, "AB", null);
test(/a(?-i:b)/i, "AB", null);
test(/a(?-i:b)/i, "Ab", { index: 0, match: ["Ab"] });
test(/./s, "\n", { index: 0, match: ["\n"] });
test(/./, "\n", null);
test(/(?s:.)./, "\n\n", null);
test(/(?s:.)./, "\na", { index: 0, match: ["\na"] });
test(/(?m:^a)/, "b\na", { index: 2, match: ["a"] });
test(/[^]/, "\n", { index: 0, match: ["\n"] });
test(/a.c/s, "a\nc", { index: 0, match: ["a\nc"] });
test(/a.c/, "a\u2028c", null);

// Case folding: U+212A is K and U+017F is s only for /iu and /iv.
test(/k/i, "\u212a", null);
test(/k/iu, "\u212a", { index: 0, match: ["\u212a"] });
test(/\u212a/i, "k", null);
test(/\u212a/iu, "k", { index: 0, match: ["k"] });
test(/[a-z]/i, "\u212a", null);
test(/[a-z]/iu, "\u212a", { index: 0, match: ["\u212a"] });
test(/\w/iu, "\u017f", { index: 0, match: ["\u017f"] });
test(/\w/i, "\u017f", null);
test(/\W/iu, "S", null);
test(/[^\W]/iu, "S", { index: 0, match: ["S"] });
test(/\bs/iu, "\u017fs", { index: 0, match: ["\u017f"] });
test(/\Bs/iu, "\u017fs", { index: 1, match: ["s"] });
test(/\u1e9e/iu, "\u00df", { index: 0, match: ["\u00df"] });
test(/[\u00df]/iu, "\u1e9e", { index: 0, match: ["\u1e9e"] });
test(/\u00df/i, "SS", null);
test(/[^k]/iv, "\u212a", null);

// A /u or /v pattern matches code points. Any other pattern matches code units.
test(/\u{1F600}/u, "\ud83d\ude00", { index: 0, match: ["\ud83d\ude00"] });
test(/./u, "\ud83d\ude00", { index: 0, match: ["\ud83d\ude00"] });
test(/./, "\ud83d\ude00", { index: 0, match: ["\ud83d"] });
test(/[^a]/u, "\ud83d\ude00", { index: 0, match: ["\ud83d\ude00"] });
test(/\ud83d/u, "\ud83d\ude00", null);
test(/\ud83d/, "\ud83d\ude00", { index: 0, match: ["\ud83d"] });
test(/\ude00/u, "\ud83d\ude00", null);
test(/\ude00/, "\ud83d\ude00", { index: 1, match: ["\ude00"] });
test(/\ud83d/u, "\ud83d", { index: 0, match: ["\ud83d"] });
test(/^.$/u, "\ud83d\ude00", { index: 0, match: ["\ud83d\ude00"] });
test(/^..$/, "\ud83d\ude00", { index: 0, match: ["\ud83d\ude00"] });
test(/(?<=.)a/u, "\ud83d\ude00a", { index: 2, match: ["a"] });
test(/(?<=^.)a/u, "\ud83d\ude00a", { index: 2, match: ["a"] });
test(/(?<=^..)a/, "\ud83d\ude00a", { index: 2, match: ["a"] });
test(/[\u{1F600}-\u{1F64F}]+/u, "a\ud83d\ude00\ud83d\ude01b", { index: 1, match: ["\ud83d\ude00\ud83d\ude01"] });
test(/\p{Lu}\p{Ll}+/u, "x\u00c9cole", { index: 1, match: ["\u00c9cole"] });
test(/\P{L}+/u, "ab12cd", { index: 2, match: ["12"] });
test(/[\p{L}--[a-c]]+/v, "abcdef", { index: 3, match: ["def"] });
test(/[[a-z]&&[aeiou]]+/v, "xaeiox", { index: 1, match: ["aeio"] });
test(/[\q{abc|d}x]+/v, "xabcdy", { index: 0, match: ["xabcd"] });
test(/^[\q{abc|ab|a}]$/v, "ab", { index: 0, match: ["ab"] });
test(/\p{RGI_Emoji}/v, "a\ud83d\ude00", { index: 1, match: ["\ud83d\ude00"] });

// Named groups, and names used by more than one alternative.
test(/(?<year>\d{4})-(?<month>\d{2})/, "on 2026-09", { index: 3, match: ["2026-09", "2026", "09"], groups: { year: "2026", month: "09" } });
test(/(?<n>a)|(?<n>b)/, "b", { index: 0, match: ["b", undefined, "b"], groups: { n: "b" } });
test(/(?:(?<n>a)|(?<n>b))+/, "ab", { index: 0, match: ["ab", undefined, "b"], groups: { n: "b" } });
test(/(?:(?<n>a)|(?<n>b)|c)+/, "abc", { index: 0, match: ["abc", undefined, undefined], groups: { n: undefined } });
test(/(?<a>x)|(?<b>y)/, "y", { index: 0, match: ["y", undefined, "y"], groups: { a: undefined, b: "y" } });

// A pattern that .* wraps, which the backtracking engines match as a DotStarEnclosure.
test(/.*a.*/, "xxaxx\nyy", { index: 0, match: ["xxaxx"] });
test(/^.*a.*$/, "xxaxx", { index: 0, match: ["xxaxx"] });
test(/^.*a.*$/, "xx\nxaxx", null);
test(/.*a.*/s, "xx\naxx\nyy", { index: 0, match: ["xx\naxx\nyy"] });
test(/^.*b.*$/m, "aaa\nabc\nccc", { index: 4, match: ["abc"] });
test(/.*foo\s+bar.*$/, "a foo  bar b", { index: 0, match: ["a foo  bar b"] });
test(/^.*[e\s].*/, "eq\n", { index: 0, match: ["eq\n"] });

// /g and /y start at lastIndex. /y matches there or not at all.
test(/a/g, "aXa", { index: 2, match: ["a"], lastIndex: 3 }, 1);
test(/a/y, "aXa", null, 1);
test(/a/y, "aXa", { index: 2, match: ["a"], lastIndex: 3 }, 2);
test(/a/gy, "aXa", { index: 2, match: ["a"], lastIndex: 3 }, 2);
test(/(?:)/g, "ab", { index: 1, match: [""], lastIndex: 1 }, 1);
test(/(?:)/y, "ab", { index: 2, match: [""], lastIndex: 2 }, 2);
test(/$/g, "ab", { index: 2, match: [""], lastIndex: 2 }, 1);
test(/\b/g, "ab cd", { index: 2, match: [""], lastIndex: 2 }, 1);
test(/(?<=a)/g, "aab", { index: 1, match: [""], lastIndex: 1 }, 0);
test(/(?<=^a)/g, "aab", { index: 1, match: [""], lastIndex: 1 }, 1);
test(/^a/g, "aa", null, 1);
test(/^a/gm, "a\na", { index: 2, match: ["a"], lastIndex: 3 }, 1);
test(/^a/y, "ba", null, 1);
test(/^a/my, "b\na", { index: 2, match: ["a"], lastIndex: 3 }, 2);
test(/.*a.*/g, "bxxaxx", { index: 3, match: ["axx"], lastIndex: 6 }, 3);
test(/^.*a.*/gms, "xx\na", { index: 3, match: ["a"], lastIndex: 4 }, 1);
test(/./gu, "\ud83d\ude00a", { index: 2, match: ["a"], lastIndex: 3 }, 2);
test(/a|b/y, "cab", { index: 1, match: ["a"], lastIndex: 2 }, 1);
test(/(a)|(b)/g, "ab", { index: 1, match: ["b", undefined, "b"], lastIndex: 2 }, 1);
test(/(?=(a))/g, "ba", { index: 1, match: ["", "a"], lastIndex: 1 }, 1);

// optimizeDotStarWrappedExpressions() rewrites /^.*X.*$/ to X and a term that says which anchors
// there were, for the backtracking engines. The matcher runs the pattern as it was written.
test(/.*foo.*/, "xx foo yy\nzz", { index: 0, match: ["xx foo yy"] });
test(/.*foo.*/, "aa\nxx foo yy\nzz", { index: 3, match: ["xx foo yy"] });
test(/.*foo.*/, "aa\nbb", null);
test(/^.*foo.*$/, "xx foo yy", { index: 0, match: ["xx foo yy"] });
test(/^.*foo.*$/, "aa\nxx foo yy", null);
test(/^.*foo.*$/m, "aa\nxx foo yy\nzz", { index: 3, match: ["xx foo yy"] });
test(/^.*foo.*/m, "aa\nxx foo yy\nzz foo", { index: 3, match: ["xx foo yy"] });
test(/.*foo.*$/, "xx foo yy\nzz", null);
test(/.*foo.*$/, "aa\nzz foo", { index: 3, match: ["zz foo"] });
test(/.*foo.*$/m, "xx foo yy\nzz", { index: 0, match: ["xx foo yy"] });
test(/.*foo.*/s, "aa\nxx foo yy\nzz", { index: 0, match: ["aa\nxx foo yy\nzz"] });
test(/^.*foo.*$/s, "aa\nxx foo yy\nzz", { index: 0, match: ["aa\nxx foo yy\nzz"] });
test(/^.*foo.*/ms, "aa\nxx foo yy\nzz", { index: 0, match: ["aa\nxx foo yy\nzz"] });
test(/.*?foo.*/, "xfooyfooz", { index: 0, match: ["xfooyfooz"] });
test(/.*foo.*/g, "a foo\nb foo\nc", { index: 6, match: ["b foo"], lastIndex: 11 }, 6);
test(/.*foo.*/g, "a foo b foo c", { index: 3, match: ["oo b foo c"], lastIndex: 13 }, 3);
test(/^.*foo.*/g, "a foo b foo c", null, 3);
test(/^.*foo.*/gm, "a foo\nb foo c", { index: 6, match: ["b foo c"], lastIndex: 13 }, 3);
test(/.*(?:foo|bar).*/, "1\n2 bar 3\n4 foo", { index: 2, match: ["2 bar 3"] });
test(/.*\bfoo\b.*/, "xfoo\n foo \n", { index: 5, match: [" foo "] });
test(/.*a.*/, "", null);
test(/.*a.*/, "\na", { index: 1, match: ["a"] });
test(/.*[e\s].*/, "eq\n", { index: 0, match: ["eq\n"] });
test(/^.*[e\s].*/, "eq\n", { index: 0, match: ["eq\n"] });
test(/.*foo\s+bar.*$/, "x foo\nbar y", { index: 0, match: ["x foo\nbar y"] });
test(/.*\u{1F600}.*/u, "a\ud83d\ude00b\nc", { index: 0, match: ["a\ud83d\ude00b"] });
test(/.*a.*/u, "\ud83d\ude00a\ud83d\nb", { index: 0, match: ["\ud83d\ude00a\ud83d"] });
test(/.*x.*/i, "aXb\nx", { index: 0, match: ["aXb"] });
test(/.*foo.*/y, "a foo b", { index: 0, match: ["a foo b"], lastIndex: 7 });
test(/.*foo.*/y, "a foo b", { index: 2, match: ["foo b"], lastIndex: 7 }, 2);
test(/.*foo(?=.{0,3}bar).*/, "a foo x bar\nfoo", { index: 0, match: ["a foo x bar"] });
test(/.*a{2,3}.*/, "a\nbaaaab\naa", { index: 2, match: ["baaaab"] });
test(/.*(?<!x)foo.*/, "xfoo\nyfoo", { index: 5, match: ["yfoo"] });
