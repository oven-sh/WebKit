//@ runDefault
//@ run("no-regexp-jit", "--useRegExpJIT=0")
//
// YARR JIT now compiles lookbehind assertions whose body reduces to a bounded set
// of fixed-width flat alternatives (PatternCharacter / CharacterClass, count 1,
// optionally grouped or alternated). These cases used to force the entire pattern
// into the interpreter; check that they produce the same results on both the JIT
// and the interpreter path.

function test(re, input, expected) {
    var m = re.exec(input);
    var actual = m === null ? null : [m[0], m.index];
    var a = JSON.stringify(actual);
    var e = JSON.stringify(expected);
    if (a !== e)
        throw new Error("FAILED " + re + " on " + JSON.stringify(input) + ": got " + a + ", expected " + e);
}

// Simple negative lookbehind.
test(/(?<! cu)bot/, "mybot", ["bot", 2]);
test(/(?<! cu)bot/, "my cubot", null);
test(/(?<! cu)bot/, "bot", ["bot", 0]);
test(/(?<! cu)bot/, "cubot", ["bot", 2]);
test(/(?<! cu)bot/, " cubot", null);
test(/(?<! cu)bot/, " cubotbot", ["bot", 6]);

// Simple positive lookbehind.
test(/(?<=cu)bot/, "cubot", ["bot", 2]);
test(/(?<=cu)bot/, "mybot", null);
test(/(?<=cu)bot/, "bot", null);
test(/(?<=cu)bot/, "xxcubot", ["bot", 4]);

// Alternation in the body (top-level).
test(/(?<!abc|de)X/, "abcX", null);
test(/(?<!abc|de)X/, "deX", null);
test(/(?<!abc|de)X/, "zzX", ["X", 2]);
test(/(?<!abc|de)X/, "X", ["X", 0]);
test(/(?<!abc|de)X/, "bcX", ["X", 2]);
test(/(?<!abc|de)X/, "xdeX", null);
test(/(?<!abc|de)X/, "xabcX", null);
test(/(?<=abc|de)X/, "abcX", ["X", 3]);
test(/(?<=abc|de)X/, "deX", ["X", 2]);
test(/(?<=abc|de)X/, "zzX", null);
test(/(?<=abc|de)X/, "adeXabcX", ["X", 3]);
test(/(?<!a|bb|ccc)d/, "ad", null);
test(/(?<!a|bb|ccc)d/, "bbd", null);
test(/(?<!a|bb|ccc)d/, "cccd", null);
test(/(?<!a|bb|ccc)d/, "xd", ["d", 1]);
test(/(?<!a|bb|ccc)d/, "bd", ["d", 1]);
test(/(?<!a|bb|ccc)d/, "ccd", ["d", 2]);

// Nested non-capturing group.
test(/(?<!(?:lib))http/, "libhttp", null);
test(/(?<!(?:lib))http/, "xxxhttp", ["http", 3]);
test(/(?<!(?:lib))http/, "http", ["http", 0]);
test(/(?<!(?:lib))http/, "alibhttp", null);

// Nested non-capturing alternation.
test(/(?<! (?:channel\/|google\/))google/, " channel/google", null);
test(/(?<! (?:channel\/|google\/))google/, "xxxxxxx channel/google", null);
test(/(?<! (?:channel\/|google\/))google/, "google", ["google", 0]);

// Optional nested group.
test(/(?<! ya(?:yandex)?)search/, " yasearch", null);
test(/(?<! ya(?:yandex)?)search/, " yayandexsearch", null);
test(/(?<! ya(?:yandex)?)search/, "zzzsearch", ["search", 3]);
test(/(?<! ya(?:yandex)?)search/, "search", ["search", 0]);
test(/(?<! ya(?:yandex)?)search/, "yayandexsearch", ["search", 8]);
test(/(?<! ya(?:yandex)?)search/, "xxx yasearch", null);
test(/(?<! ya(?:yandex)?)search/, "xxx yayandexsearch", null);

// CharacterClass.
test(/(?<![0-9])px/, "10px", null);
test(/(?<![0-9])px/, "apx", ["px", 1]);
test(/(?<![0-9])px/, "px", ["px", 0]);
test(/(?<![0-9])px/, "x10px", null);
test(/(?<=[a-z])X/, "aX", ["X", 1]);
test(/(?<=[a-z])X/, "9X", null);
test(/(?<!\d)px/, "5px", null);
test(/(?<!\d)px/, "xpx", ["px", 1]);
test(/(?<!\s)word/, " word", null);
test(/(?<!\s)word/, "xword", ["word", 1]);
test(/(?<![^a])b/, "ab", ["b", 1]);
test(/(?<![^a])b/, "xb", null);
test(/(?<![^a])b/, "b", ["b", 0]);
test(/(?<!.)b/, "ab", null);
test(/(?<!.)b/, "b", ["b", 0]);
test(/(?<!.)b/, "\nb", ["b", 1]);

// Not at start of the body alternative.
test(/a(?<!xa)b/, "xab", null);
test(/a(?<!xa)b/, "yab", ["ab", 1]);
test(/a(?<!xa)b/, "ab", ["ab", 0]);
test(/a(?<!xa)b/, "xxab", null);
test(/foo(?<!x)bar/, "foobar", ["foobar", 0]);
test(/foo(?<!o)bar/, "foobar", null);
test(/ab(?<!ab)/, "ab", null);
test(/ab(?<!cd)/, "ab", ["ab", 0]);

// Multiple lookbehinds in the same alternative.
test(/(?<!a)(?<!b)c/, "ac", null);
test(/(?<!a)(?<!b)c/, "bc", null);
test(/(?<!a)(?<!b)c/, "xc", ["c", 1]);
test(/(?<=a)(?<=.a)b/, "xab", ["b", 2]);
test(/(?<=a)(?<=.a)b/, "ab", null);

// In body alternative list.
test(/x|(?<!a)y/, "ay", null);
test(/x|(?<!a)y/, "by", ["y", 1]);
test(/x|(?<!a)y/, "x", ["x", 0]);

// Inside a capturing paren (capture is outside the lookbehind).
test(/((?<!a)b)/, "ab", null);
test(/((?<!a)b)/, "xb", ["b", 1]);

// ignoreCase.
test(/(?<!ABC)d/i, "abcd", null);
test(/(?<!ABC)d/i, "xyzd", ["d", 3]);
test(/(?<!ABC)d/i, "AbCd", null);

// 16-bit input strings (code points above U+00FF force Char16 storage).
test(/(?<!ab)c/, "\u0100abc", null);
test(/(?<!ab)c/, "\u0100xxc", ["c", 3]);
test(/(?<!\u0101)x/, "\u0101x", null);
test(/(?<!\u0101)x/, "a\u0100x", ["x", 2]);
test(/(?<![0-9])px/, "\u010010px", null);
test(/(?<![0-9])px/, "\u0100apx", ["px", 2]);
test(/(?<! cu)bot/, "\u0100 cubot", null);
test(/(?<! cu)bot/, "\u0100xxbot", ["bot", 3]);
test(/(?<=\u0101\u0102)x/, "\u0101\u0102x", ["x", 2]);
test(/(?<=\u0101\u0102)x/, "\u0101\u0103x", null);

// Latin-1 (8-bit) input strings with non-ASCII characters.
test(/(?<!ab)c/, "\u00ffabc", null);
test(/(?<!ab)c/, "\u00ffxxc", ["c", 3]);
test(/(?<!\u00e9)x/, "\u00e9x", null);
test(/(?<!\u00e9)x/, "ax", ["x", 1]);

// Lookbehind inside a lookahead.
test(/(?=(?<!a)b)/, "xb", ["", 1]);
test(/(?=(?<!a)b)/, "ab", null);
test(/x(?=(?<!x)y)y/, "xy", null);
test(/x(?=(?<!a)y)y/, "xy", ["xy", 0]);
test(/(?=(?<=a)b)b/, "ab", ["b", 1]);
test(/(?=(?<=a)b)b/, "xb", null);
test(/(?=(?<=a)b)b/, "b", null);
test(/z(?=a(?<!za)b)ab/, "zab", null);
test(/z(?=a(?<!ya)b)ab/, "zab", ["zab", 0]);
test(/(?=(?<!ab|c)d)d/, "abd", null);
test(/(?=(?<!ab|c)d)d/, "cd", null);
test(/(?=(?<!ab|c)d)d/, "xd", ["d", 1]);
test(/(?=(?<!ab|c)d)d/, "d", ["d", 0]);
test(/(?=ab(?<!xab)c)abc/, "xabc", null);
test(/(?=ab(?<!xab)c)abc/, "yabc", ["abc", 1]);
test(/(?=ab(?<!xab)c)abc/, "abc", ["abc", 0]);

// Lookbehind inside a quantified paren.
test(/((?<!a)b)+/, "xbbb", ["bbb", 1]);
test(/((?<!a)b)+/, "abbb", ["bb", 2]);
test(/((?<!a)b)+/, "abxbb", ["bb", 3]);

// Empty body.
test(/(?<!)X/, "aX", null);
test(/(?<=)X/, "aX", ["X", 1]);

// Patterns that cannot be flattened still take the interpreter path.
test(/(?<!a)b/u, "ab", null);
test(/(?<!a)b/u, "xb", ["b", 1]);
test(/(?<=a+)b/, "aaab", ["b", 3]);
test(/(?<=a+)b/, "b", null);
test(/(?<=a{2,3})b/, "aab", ["b", 2]);
test(/(?<=a{2,3})b/, "ab", null);
test(/(?<=(x))y/, "xy", ["y", 1]);
test(/(?<=(x))y/, "zy", null);
test(/(?<=\w{3})x/, "abcx", ["x", 3]);
test(/(?<=\w{3})x/, "abx", null);
test(/(?<=^ab)c/, "abc", ["c", 2]);
test(/(?<=^ab)c/, "xabc", null);
