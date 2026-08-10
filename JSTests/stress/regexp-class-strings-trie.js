// Classes containing strings (\q{...}, properties of strings) are expanded as a trie of
// alternatives; the longest member matching at a position must still win, then shorter ones on
// backtrack, and the empty string member matches last.
function shouldBe(actual, expected) { if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error("expected " + JSON.stringify(expected) + " got " + JSON.stringify(actual)); }
shouldBe("aab ab b".match(/[\q{aab|ab|b|a}]/gv), ["aab", "ab", "b"]);
shouldBe("aab".match(/^[\q{aab|ab|a}]b/v), null);
shouldBe("ab".match(/^[\q{ab|a}]b$/v), ["ab"]);
shouldBe("abc".match(/[\q{abc|ab}c]/gv), ["abc"]);
shouldBe("abd".match(/[\q{abc|ab}c]/gv), ["ab"]);
shouldBe(/[\q{}]/v.test(""), true);
shouldBe("xa".match(/[\q{a|}]/gv), ["", "a", ""]);
shouldBe(/^[\q{abc|abd|ab|a}]$/v.exec("abd")[0], "abd");
shouldBe(/^[\q{abc|abd|ab|a}]$/v.exec("abe"), null);
shouldBe(/^[\q{abc|abd|ab|a}]e$/v.exec("abe")[0], "abe");
shouldBe(/[\q{abc|abd|ab|a}]e/v.exec("xae")[0], "ae");
shouldBe(/^[\q{AB|a}]$/iv.test("ab"), true);
shouldBe(/^[\q{ab}\q{cd}x]$/v.test("cd"), true);
shouldBe(/^[\p{RGI_Emoji}--\q{👋🏻}]$/v.test("👋🏻"), false);
shouldBe(/^[\p{RGI_Emoji}--\q{👋🏻}]$/v.test("👋🏼"), true);
shouldBe(/^[\p{RGI_Emoji}&&\p{RGI_Emoji_Flag_Sequence}]$/v.test("🇺🇸"), true);
shouldBe(/^[\p{RGI_Emoji}&&\p{RGI_Emoji_Flag_Sequence}]$/v.test("🙂"), false);
const rgi = /^\p{RGI_Emoji}$/v;
for (const s of ["🙂", "👋", "👋🏻", "👨‍👩‍👧‍👦", "🇺🇸", "#️⃣", "🏴󠁧󠁢󠁥󠁮󠁧󠁿", "☺️", "🧑🏻‍🤝‍🧑🏿", "🐦‍🔥"])
    shouldBe(rgi.test(s), true);
for (const s of ["a", "👋🏻x", "🇺", "👨‍👩", "#", "☺", "", "🙂🙂"])
    shouldBe(rgi.test(s), false);
shouldBe("x👋🏻y👨‍👩‍👧z🙂 🇺🇸🇺".match(/\p{RGI_Emoji}/gv), ["👋🏻", "👨‍👩‍👧", "🙂", "🇺🇸"]);
shouldBe("q👨‍👩‍👧‍👦x".search(/\p{RGI_Emoji_ZWJ_Sequence}/v), 1);
shouldBe(/(?<=\p{RGI_Emoji})!/v.exec("👋🏻!").index, 4);
// A wide (16-bit subject) group dispatched on code points, mixed with Latin-1 alternatives.
const kw = /(?:αλφα|βήτα|γάμμα|δέλτα|alpha|beta|[κλ]άππα|😀x|😁y|\u{10400}z)=(\d)/u;
shouldBe(kw.exec("… δέλτα=4")[1], "4");
shouldBe(kw.exec("… λάππα=5")[1], "5");
shouldBe(kw.exec("… 😁y=6")[1], "6");
shouldBe(kw.exec("… \u{10400}z=7")[1], "7");
shouldBe(kw.exec("… 😁x=6"), null);
shouldBe(kw.exec("beta=1")[1], "1");
// Review cases: /i and lookbehinds keep longest-first across case variants / suffixes; long
// single strings do not recurse per character; branch heads that are single members end inside
// their branch.
shouldBe(/[\q{Ab|abcd}]/vi.exec("abcd")[0], "abcd");
shouldBe(/[\q{abc|aB}]/vi.exec("abc")[0], "abc");
shouldBe(/[\q{kx|Kxy|q1|q2|q3|q4|q5}]/vi.exec("kxy")[0], "kxy");
shouldBe(/(?<=([\q{ba|cba|q1|q2|q3|q4|q5}]))x/v.exec("cbax")[1], "cba");
shouldBe(/(?<=([\q{ab|zab|q1|q2|q3|q4|q5}]))x/v.exec("zabx")[1], "zab");
shouldBe(/(?<=([\q{ab|ca|q1|q2|q3|q4|q5}\s\S]))$/v.exec("xca")[1], "ca");
shouldBe(/^[\q{ab|ca|q1|q2|q3|q4|q5}\s\S]$/v.exec("ca")[0], "ca");
shouldBe(/^[\q{ab|ca|q1|q2|q3|q4|q5}\s\S]/v.exec("cx")[0], "c");
shouldBe(/[\q{ab|ac|q1|q2|q3|q4|q5}a]/v.exec("xa")[0], "a");
shouldBe(/[\q{ab|ac|q1|q2|q3|q4|q5}a]/v.exec("xac")[0], "ac");
{
    let threw = false, matched = null;
    try { matched = new RegExp("[\\q{" + "ab".repeat(25000) + "|q1|q2|q3|q4|q5}]", "v").exec("x" + "ab".repeat(25000)); } catch (e) { threw = e instanceof SyntaxError; }
    shouldBe(threw || (matched && matched[0].length === 50000), true);
}
// Hunt cases: any-character singles take the bare branch head themselves (no second way to match
// one character), long shared prefixes compile, duplicate members are one member.
{
    const t = Date.now();
    shouldBe(/^[\q{ab|cd|ef|gh|ij|kl}\s\S]*z$/v.test("a".repeat(26)), false);
    shouldBe(Date.now() - t < 1000, true);
    shouldBe("xaby".match(/[\q{ab|cd|ef|gh|ij|kl}\s\S]/gv), ["x", "ab", "y"]);
    const P = "x".repeat(30000);
    shouldBe(new RegExp("[\\q{" + P + "a|" + P + "b|cd|ef|gh|ij}]$", "v").exec("q" + P + "b")[0].length, 30001);
    shouldBe(/^[\q{ab|ab}--\q{ab}]/v.exec("ab"), null);
    shouldBe("ab a".match(/[\q{ab|ab|a|a|}]/gv), ["ab", "", "a", ""]);
}
