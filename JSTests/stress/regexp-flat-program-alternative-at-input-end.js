//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault

// `(?:R)|$` finds what R finds, and the end of the subject where R finds nothing.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

const made = (source, flags) => new RegExp(source + String(Date.now()).slice(99), flags);

const sources = [
    "(?:^|\\/)private(?=$|\\/$)", "(?:^|\\/)Straße(?=$|\\/$)", "^(?:.*\\/)?[^\\/]*\\.pem(?=$|\\/$)", "(?:^|\\/)[^\\/]*\\.log(?=$|\\/$)", "^private\\/.+(?=$|\\/$)",
    "needle", "(nee)(dle)", "x.*needle", "x[^\\/]*needle", "\\/.*?needle", ".*needle", "^.*needle", "n", "[ne]", "ne?", "ﷺ", "ﷺneedle", "needle$", "n+$", "\\bneedle\\b", "(?=n)needle", "(?<=x)needle", "needle|other", "^other|needle",
];
const fillers = ["x", "xy/", "ﷺ", "n", "\u{1f600}"];
const texts = ["", "needle", "/PRIVATE/", "/Straße", "a.pem", "/a.log", "other"];
const lengths = [0, 1, 4, 5, 16, 17, 130, 257];

let checked = 0;
for (const source of sources) {
    for (const flags of ["", "i", "g", "iu", "v"]) {
        const alone = made(source, flags);
        for (const orEnd of [made("(?:" + source + ")|$", flags), made("$|(?:" + source + ")", flags), made("(?:" + source + "|$)", flags)]) {
            for (const filler of fillers) {
                for (const text of texts) {
                    for (const length of lengths) {
                        const run = filler.repeat(Math.ceil(length / filler.length));
                        for (const subject of [run + text, text + run, run + text + run]) {
                            for (const from of flags === "g" ? [0, 1, length, subject.length] : [0]) {
                                if (from > subject.length)
                                    continue;
                                alone.lastIndex = from;
                                orEnd.lastIndex = from;
                                const expected = alone.exec(subject);
                                const actual = orEnd.exec(subject);
                                const what = JSON.stringify([orEnd.source, flags, filler, text, length, from]);
                                shouldBe(actual.index, expected ? expected.index : subject.length, what);
                                shouldBe(actual[0], expected ? expected[0] : "", what);
                                shouldBe(actual[1], expected ? expected[1] : undefined, what);
                                checked++;
                            }
                        }
                    }
                }
            }
        }
    }
}
shouldBe(checked > 250000, true, "checked");

shouldBe("a,b".replace(made(",|$", "g"), "!"), "a!b!", "replace");
shouldBe("a\nb".search(made("x|$", "")), 3, "a line end is no end");
shouldBe("a\nb".search(made("x|$", "m")), 1, "with m it is");
shouldBe("".search(made("x|$", "")), 0, "an empty subject");
shouldBe(made("x|$", "y").test("ab"), false, "sticky");
