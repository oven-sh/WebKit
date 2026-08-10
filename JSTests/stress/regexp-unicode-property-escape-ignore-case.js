// Property escapes take part in case-insensitive matching under /u and /v (they used to be
// appended to the class verbatim, so /\p{Lu}/iu did not match "a").
function shouldBe(actual, expected, m) { if (actual !== expected) throw new Error((m || "") + " expected " + JSON.stringify(expected) + " got " + JSON.stringify(actual)); }
const probes = ["A", "a", "1", "\u01C5", "\u2160", "\u01C5"];
const table = (re) => probes.map((c) => re.test(c) ? 1 : 0).join("");
// Expected strings are V8's / the spec's CharacterSetMatcher results (u: closure semantics,
// \P{X} = closure of the complement; v: fold-then-membership, \P{X} = complement of the closure).
const expectations = {
    "\\p{Lu}":          { u: "100000", iu: "110101", v: "100000", iv: "110101" },
    "\\P{Lu}":          { u: "011111", iu: "111111", v: "011111", iv: "001010" },
    "[\\p{Lu}]":        { u: "100000", iu: "110101", v: "100000", iv: "110101" },
    "[^\\p{Lu}]":       { u: "011111", iu: "001010", v: "011111", iv: "001010" },
    "[\\P{Lu}]":        { u: "011111", iu: "111111", v: "011111", iv: "001010" },
    "\\p{Ll}":          { u: "010000", iu: "110101", v: "010000", iv: "110101" },
    "\\P{Ll}":          { u: "101111", iu: "111111", v: "101111", iv: "001010" },
    "[\\p{Lu}\\d]":     { u: "101000", iu: "111101", v: "101000", iv: "111101" },
    "[^\\P{Lu}]":       { u: "100000", iu: "000000", v: "100000", iv: "110101" },
    "\\p{L}":           { u: "110101", iu: "110101", v: "110101", iv: "110101" },
    "[\\p{Lu}--[A-Z]]": { v: "000000", iv: "000101" },
    "[\\P{Number}&&\\P{Alphabetic}]": { v: "000000", iv: "000000" },
};
for (const [source, byFlags] of Object.entries(expectations))
    for (const [flags, expected] of Object.entries(byFlags))
        shouldBe(table(new RegExp("^" + source + "$", flags)), expected, "/" + source + "/" + flags);

shouldBe(/\p{ASCII}/iu.test("\u212A"), true);   // KELVIN SIGN ~ k
shouldBe(/[\p{ASCII}]/iu.test("\u017F"), true); // LONG S ~ s
shouldBe(/\P{ASCII}/iu.test("k"), true);        // k ~ KELVIN SIGN, which is outside ASCII
shouldBe(/\P{ASCII}/iv.test("k"), false);
shouldBe(/(?i:\p{Lu})x/u.test("ax"), true);     // modifier group
shouldBe(/(?-i:\p{Lu})x/iu.test("ax"), false);
shouldBe("caf\xC9".replace(/\p{Ll}+/giu, "_"), "_");
shouldBe(/(?<=\p{Lu}{2})!/iu.exec("ab!").index, 2); // inside a JIT-compiled lookbehind
