//@ runDefault("--useRegExpJIT=true")
//@ runNoJIT("--useRegExpJIT=false")

// Lookbehind assertions compiled by the Yarr JIT (mirrored bodies matched
// right-to-left) must produce results identical to the interpreter.

function shouldBe(actual, expected, message) {
  if (JSON.stringify(actual) !== JSON.stringify(expected))
    throw new Error(
      (message ? message + ": " : "") + "expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual),
    );
}

function matchOf(re, s) {
  let m = re.exec(s);
  return m ? [m.index, [...m]] : null;
}

// Fixed-width bodies.
shouldBe(matchOf(/(?<=x)a/, "xa"), [1, ["a"]]);
shouldBe(matchOf(/(?<=x)a/, "ya"), null);
shouldBe(matchOf(/(?<!x)a/, "ya"), [1, ["a"]]);
shouldBe(matchOf(/(?<!x)a/, "xa"), null);
shouldBe(matchOf(/(?<=abc)d/, "abcd"), [3, ["d"]]);
shouldBe(matchOf(/(?<=abc)d/, "abd"), null);
shouldBe(matchOf(/(?<! cu)bot/, "robot"), [2, ["bot"]]);
shouldBe(matchOf(/(?<! cu)bot/, "a cubot"), null);
shouldBe(matchOf(/(?<=\d{3})x/, "123x"), [3, ["x"]]);
shouldBe(matchOf(/(?<=\d{3})x/, "12x"), null);

// Alternation and nested groups inside the body.
shouldBe(matchOf(/(?<=ab|xyz)q/, "abq"), [2, ["q"]]);
shouldBe(matchOf(/(?<=ab|xyz)q/, "xyzq"), [3, ["q"]]);
shouldBe(matchOf(/(?<=ab|xyz)q/, "azq"), null);
shouldBe(matchOf(/(?<=(?:channel\/|google\/))x/, "channel/x"), [8, ["x"]]);
shouldBe(matchOf(/(?<=(?:channel\/|google\/))x/, "google/x"), [7, ["x"]]);
shouldBe(matchOf(/(?<=(?:channel\/|google\/))x/, "chanel/x"), null);

// Variable-width bodies: greedy / lazy quantifiers and optional groups.
shouldBe(matchOf(/(?<=a+)b/, "aaab"), [3, ["b"]]);
shouldBe(matchOf(/(?<=a+)b/, "b"), null);
shouldBe(matchOf(/(?<=ba*)c/, "bc"), [1, ["c"]]);
shouldBe(matchOf(/(?<=ba*)c/, "baaac"), [4, ["c"]]);
shouldBe(matchOf(/(?<=ba*?)c/, "baaac"), [4, ["c"]]);
shouldBe(matchOf(/(?<! ya(?:yandex)?)search/, "yasearch"), [2, ["search"]]);
shouldBe(matchOf(/(?<! ya(?:yandex)?)search/, " yasearch"), null);
shouldBe(matchOf(/(?<! ya(?:yandex)?)search/, " yayandexsearch"), null);
shouldBe(matchOf(/(?<! ya(?:yandex)?)search/, "search"), [0, ["search"]]);
shouldBe(matchOf(/(?<=[a-z]{2,4})\d/, "ab7"), [2, ["7"]]);
shouldBe(matchOf(/(?<=[a-z]{2,4})\d/, "a7"), null);

// Captures inside the body record real (forward) start/end.
shouldBe(matchOf(/(?<=(a)(b))c/, "abc"), [2, ["c", "a", "b"]]);
shouldBe(matchOf(/(?<=(a+))b/, "xaaab"), [4, ["b", "aaa"]]);
shouldBe(matchOf(/(?<=(a+?))b/, "xaaab"), [4, ["b", "a"]]);
shouldBe(matchOf(/(?<!(a))b/, "cb"), [1, ["b", undefined]]);

// Nested lookbehind; lookbehind at start of input; whole-string context.
shouldBe(matchOf(/(?<=(?<=a)b)c/, "abc"), [2, ["c"]]);
shouldBe(matchOf(/(?<=(?<=a)b)c/, "xbc"), null);
shouldBe(matchOf(/(?<=^)a/, "a"), [0, ["a"]]);
shouldBe(matchOf(/(?<!^)a/, "a"), null);
shouldBe(matchOf(/(?<!^)a/, "ba"), [1, ["a"]]);

// Anchors inside the body, including multiline.
shouldBe(matchOf(/(?<=^a)b/m, "xa\nab"), [4, ["b"]]);
shouldBe(matchOf(/(?<=a$)/m, "a\nb"), [1, [""]]);
shouldBe(matchOf(/(?<=\bfoo)bar/, "foobar"), [3, ["bar"]]);
shouldBe(matchOf(/(?<=\bfoo)bar/, "xfoobar"), null);

// The lookbehind sees input before lastIndex / the match start.
{
  let re = /(?<=ab)c/g;
  let s = "abcabc";
  let m = [];
  let r;
  while ((r = re.exec(s)) !== null) m.push(r.index);
  shouldBe(m, [2, 5]);

  let sticky = /(?<=ab)c/y;
  sticky.lastIndex = 2;
  shouldBe(matchOf(sticky, "abc"), [2, ["c"]]);
}

// Case-insensitive, Latin-1, and true 16-bit (non-Latin-1) strings.
shouldBe(matchOf(/(?<=ab)c/i, "ABC"), [2, ["C"]]);
shouldBe(matchOf(/(?<=é)x/, "éx"), [1, ["x"]]);
shouldBe(matchOf(/(?<=Ā)x/, "Āx"), [1, ["x"]]);
shouldBe(matchOf(/(?<=ab)c/, "Āabc"), [3, ["c"]]);

// Fixed-count pattern characters and \B inside the body.
shouldBe(matchOf(/(?<=a{3})b/, "aaab"), [3, ["b"]]);
shouldBe(matchOf(/(?<=a{3})b/, "aab"), null);
shouldBe(matchOf(/(?<=xa{2})b/, "xaab"), [3, ["b"]]);
shouldBe(matchOf(/(?<=\Bb)c/, "abc"), [2, ["c"]]);
shouldBe(matchOf(/(?<=\Bb)c/, " bc"), null);

// Anchors at the real edges of input inside the body (non-multiline).
shouldBe(matchOf(/(?<=x$)/, "x"), [1, [""]]);
shouldBe(matchOf(/(?<=^x)y/, "xy"), [1, ["y"]]);
shouldBe(matchOf(/(?<=^x)y/, "axy"), null);
shouldBe(matchOf(/(?<=^)a/m, "\na"), [1, ["a"]]);

// Bodies the JIT does not compile fall back to the interpreter and must still
// be correct: backreference, quantified group, nested lookahead, and unicode
// surrogate-pair bodies.
shouldBe(matchOf(/(?<=(a)\1)b/, "aab"), [2, ["b", "a"]]);
shouldBe(matchOf(/(?<=(?:ab)+)c/, "ababc"), [4, ["c"]]);
shouldBe(matchOf(/(?<=(?:ab)+)c/, "xc"), null);
shouldBe(matchOf(/(?<=(?=a)a)b/, "ab"), [1, ["b"]]);
shouldBe(matchOf(/(?<=(?=a)a)b/, "cb"), null);
shouldBe(matchOf(/(?<=\u{1F600})x/u, "\u{1F600}x"), [2, ["x"]]);
shouldBe(matchOf(/(?<=\u{1F600})x/u, "yx"), null);

// Backtracking after a satisfied lookbehind must clear its captures.
shouldBe(matchOf(/(?<=(a))bz|(?<=(a))b/, "ab"), [1, ["b", undefined, "a"]]);

// A large alternation containing lookbehinds must produce the same set of
// matches as the same alternation with the assertions verified independently.
{
  let re = /(?<! cu)bot|(?<!(?:lib))http|(?<! ya(?:yandex)?)search|crawler|spider/;
  let lines = [
    "Googlebot/2.1",
    "a cubot toy",
    "libhttp client",
    "http agent",
    "yandexsearch",
    " yasearch",
    "crawler",
    "spiderman",
  ];
  shouldBe(
    lines.map(l => re.test(l)),
    [true, false, false, true, true, false, true, true],
  );
}

// Lookbehind patterns must not force BOL anchoring onto the enclosing
// alternative through a group that starts with ^ but is optional, quantified,
// negated, or not the alternative's first term (these previously matched only
// because lookbehind kept the whole pattern on the interpreter).
shouldBe(JSON.stringify(/a(?<!x)(^)?$/.exec("xa")), '["a",null]');
shouldBe(JSON.stringify(/(?<=y)a(^)?$/.exec("ya")), '["a",null]');
shouldBe(JSON.stringify(/c(?<=c)(?!(?=^))/.exec("xc")), '["c"]');
shouldBe(JSON.stringify(/(?<!q)b(^)*c/.exec("abc")), '["bc",null]');
shouldBe(JSON.stringify(/\w(?<=\d)(^)?/.exec("a1")), '["1",null]');
shouldBe(JSON.stringify(/z(?<!q)(?=(^)?)/.exec("yz")), '["z",null]');
shouldBe(JSON.stringify("qxaqxa".split(/a(?<!x)(^)?$/)), '["qxaqx",null,""]');
// The same optional-BOL-group family without lookbehind.
shouldBe(JSON.stringify(/(?:^)?a/.exec("ba")), '["a"]');
shouldBe(JSON.stringify(/(^)*a/.exec("ba")), '["a",null]');
shouldBe(JSON.stringify(/\B(?:^)?/.exec("xx")), '[""]');
shouldBe(/\B(?:^)?/.exec("xx").index, 1);
shouldBe(JSON.stringify(/(^)* \b/v.exec("a b")), '[" ",null]');
// Genuine anchoring is unchanged.
shouldBe(/^a/.exec("ba"), null);
shouldBe(/(?:^a)/.exec("ba"), null);
shouldBe(/(^a)+/.exec("ba"), null);
shouldBe(/(?:^a|^b)/.exec("cb"), null);
