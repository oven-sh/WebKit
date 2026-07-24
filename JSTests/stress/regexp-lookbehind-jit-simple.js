// YARR JIT now compiles lookbehind assertions whose body reduces to a bounded set
// of fixed-width flat alternatives (PatternCharacter / CharacterClass, count 1,
// optionally grouped or alternated). These cases used to force the entire pattern
// into the interpreter; check that they still produce the same results as the
// interpreter path once JIT-compiled.

function assertSame(name, actual, expected) {
    function fmt(m) { return m === null ? "null" : JSON.stringify([m[0], m.index]); }
    if (fmt(actual) !== fmt(expected))
        throw new Error("FAILED " + name + ": got " + fmt(actual) + ", expected " + fmt(expected));
}

var cases = [
    ["(?<! cu)bot", "", "mybot"],
    ["(?<! cu)bot", "", "my cubot"],
    ["(?<! cu)bot", "", "bot"],
    ["(?<! cu)bot", "", "cubot"],
    ["(?<! cu)bot", "", " cubot"],
    ["(?<! cu)bot", "", " cubotbot"],
    ["(?<=cu)bot", "", "cubot"],
    ["(?<=cu)bot", "", "mybot"],
    ["(?<=cu)bot", "", "bot"],
    ["(?<=cu)bot", "", "xxcubot"],
    ["(?<!abc|de)X", "", "abcX"],
    ["(?<!abc|de)X", "", "deX"],
    ["(?<!abc|de)X", "", "zzX"],
    ["(?<!abc|de)X", "", "X"],
    ["(?<!abc|de)X", "", "bcX"],
    ["(?<!abc|de)X", "", "xdeX"],
    ["(?<!abc|de)X", "", "xabcX"],
    ["(?<=abc|de)X", "", "abcX"],
    ["(?<=abc|de)X", "", "deX"],
    ["(?<=abc|de)X", "", "zzX"],
    ["(?<=abc|de)X", "", "adeXabcX"],
    ["(?<!(?:lib))http", "", "libhttp"],
    ["(?<!(?:lib))http", "", "xxxhttp"],
    ["(?<!(?:lib))http", "", "http"],
    ["(?<!(?:lib))http", "", "alibhttp"],
    ["(?<! (?:channel/|google/))google", "", " channel/google"],
    ["(?<! (?:channel/|google/))google", "", "xxxxxxx channel/google"],
    ["(?<! (?:channel/|google/))google", "", "google"],
    ["(?<! ya(?:yandex)?)search", "", " yasearch"],
    ["(?<! ya(?:yandex)?)search", "", " yayandexsearch"],
    ["(?<! ya(?:yandex)?)search", "", "zzzsearch"],
    ["(?<! ya(?:yandex)?)search", "", "search"],
    ["(?<! ya(?:yandex)?)search", "", "yayandexsearch"],
    ["(?<! ya(?:yandex)?)search", "", "xxx yasearch"],
    ["(?<! ya(?:yandex)?)search", "", "xxx yayandexsearch"],
    ["(?<![0-9])px", "", "10px"],
    ["(?<![0-9])px", "", "apx"],
    ["(?<![0-9])px", "", "px"],
    ["(?<![0-9])px", "", "x10px"],
    ["a(?<!xa)b", "", "xab"],
    ["a(?<!xa)b", "", "yab"],
    ["a(?<!xa)b", "", "ab"],
    ["a(?<!xa)b", "", "xxab"],
    ["(?<!ABC)d", "i", "abcd"],
    ["(?<!ABC)d", "i", "xyzd"],
    ["(?<!ABC)d", "i", "AbCd"],
    ["(?<=[a-z])X", "", "aX"],
    ["(?<=[a-z])X", "", "9X"],
    ["(?<!\\d)px", "", "5px"],
    ["(?<!\\d)px", "", "xpx"],
    ["(?<!a)(?<!b)c", "", "ac"],
    ["(?<!a)(?<!b)c", "", "bc"],
    ["(?<!a)(?<!b)c", "", "xc"],
    ["(?<=a)(?<=.a)b", "", "xab"],
    ["(?<=a)(?<=.a)b", "", "ab"],
    ["ab(?<!ab)", "", "ab"],
    ["ab(?<!cd)", "", "ab"],
    ["(?<!\\s)word", "", " word"],
    ["(?<!\\s)word", "", "xword"],
    ["foo(?<!x)bar", "", "foobar"],
    ["foo(?<!o)bar", "", "foobar"],
    ["(?<!a|bb|ccc)d", "", "ad"],
    ["(?<!a|bb|ccc)d", "", "bbd"],
    ["(?<!a|bb|ccc)d", "", "cccd"],
    ["(?<!a|bb|ccc)d", "", "xd"],
    ["(?<!a|bb|ccc)d", "", "bd"],
    ["(?<!a|bb|ccc)d", "", "ccd"],
    ["x|(?<!a)y", "", "ay"],
    ["x|(?<!a)y", "", "by"],
    ["x|(?<!a)y", "", "x"],
    ["((?<!a)b)", "", "ab"],
    ["((?<!a)b)", "", "xb"],
    ["(?<!ab)c", "", "\u00ffabc"],
    ["(?<!ab)c", "", "\u00ffxxc"],
    ["(?<!\u00e9)x", "", "\u00e9x"],
    ["(?<!\u00e9)x", "", "ax"],
    // Fallback cases: unicode flag, unbounded quantifier, capture, backreference.
    ["(?<!a)b", "u", "ab"],
    ["(?<!a)b", "u", "xb"],
    ["(?<=a+)b", "", "aaab"],
    ["(?<=a+)b", "", "b"],
    ["(?<=a{2,3})b", "", "aab"],
    ["(?<=a{2,3})b", "", "ab"],
    ["(?<=(x))y", "", "xy"],
    ["(?<=(x))y", "", "zy"],
    ["(?<=(\\w)\\1)x", "", "aax"],
    ["(?<=(\\w)\\1)x", "", "abx"],
];

// Oracle: first execution runs before the JIT threshold is crossed.
var expected = new Array(cases.length);
for (var i = 0; i < cases.length; ++i) {
    var r = new RegExp(cases[i][0], cases[i][1]);
    expected[i] = r.exec(cases[i][2]);
}

// Exercise the JIT path.
for (var iter = 0; iter < 50; ++iter) {
    for (var i = 0; i < cases.length; ++i) {
        var r = new RegExp(cases[i][0], cases[i][1]);
        for (var j = 0; j < 4; ++j) r.test("z");
        var m = r.exec(cases[i][2]);
        assertSame("case " + i + " /" + cases[i][0] + "/" + cases[i][1] + " on " + JSON.stringify(cases[i][2]), m, expected[i]);
    }
}
