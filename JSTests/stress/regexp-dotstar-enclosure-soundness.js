// The dot-star enclosure optimization rewrites /.*X.*/ to record its match bounds
// directly. With /s the leading .* must start where matching began (lastIndex for a
// global RegExp), not at position 0.
function shouldBe(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}
function probe(re, s, lastIndex) { re.lastIndex = lastIndex; const m = re.exec(s); return m ? [m.index, m[0].length, re.lastIndex] : null; }

shouldBe(probe(/.*ob.*/sg, "x\nfoob", 2), [2, 4, 6]);
shouldBe(probe(/.*ob.*/sg, "x\nfoob", 0), [0, 6, 6]);
shouldBe(probe(/.*(?<=o)b.*/sg, "x\nfoob", 2), [2, 4, 6]);
shouldBe(probe(/.*ob.*/g, "xxfoob", 2), [2, 4, 6]);
shouldBe(probe(/.*ob.*/sg, "x\nfoob", 5), null);
shouldBe(probe(/.*o.*/sg, "o\no\no", 3), [3, 2, 5]);
shouldBe("ab\ncd".replace(/.*c.*/sg, (m, i) => "[" + i + ":" + m + "]"), "[0:ab\ncd]");
{
    const re = /.*z.*/sg, s = "z\nz";
    const seen = [];
    let m;
    while ((m = re.exec(s)) !== null) { seen.push([m.index, m[0]]); if (m[0] === "") re.lastIndex++; }
    shouldBe(seen, [[0, "z\nz"]]);
}
for (let i = 0; i < 8; ++i)
    shouldBe(probe(/.*(?:needle).*/sg, $vm.make16BitStringIfPossible("hay\nneedle\nhay"), i), i <= 4 ? [i, 14 - i, 14] : null);

// A leading ^ still constrains where the enclosure may begin once lastIndex > 0.
shouldBe(probe(/^.*a.*/gs, "xxaxx", 1), null);
shouldBe(probe(/^.*a.*/gs, "xxaxx", 0), [0, 5, 5]);
shouldBe(probe(/^.*a.*/gms, "bxxaxx", 3), null);
shouldBe(probe(/^.*a.*/gms, "b\nxaxx", 2), [2, 4, 6]);
shouldBe(probe(/^.*a.*/gm, "bxxaxx", 3), null);
shouldBe(probe(/^.*a.*/gm, "b\nxaxx", 3), null);
shouldBe(probe(/^.*a.*/gm, "b\nxaxx", 2), [2, 4, 6]);
shouldBe(probe(/^.*a.*$/gm, "zz\nya\nq", 1), [3, 2, 5]);
{
    const re = /^.*error.*$/gs;
    shouldBe(re.test("an error here"), true);
    shouldBe(re.test("clean prefix.. then error later"), false); // lastIndex carried over from the first call
    const rm = /^.*error.*$/gm;
    shouldBe(rm.test("an error here"), true);
    shouldBe(rm.test("clean prefix.. then error later\nok"), false);
}
shouldBe(probe(/^.*a.*/gm, $vm.make16BitStringIfPossible("bā\nxaxx"), 3), [3, 4, 7]);
shouldBe(probe(/^.*a.*/gm, $vm.make16BitStringIfPossible("bāxxaxx"), 3), null);

// The enclosure must not change results when the wrapped expression can itself take a
// line terminator (greedy .* prefers the last split point on the line) or captures
// inside an assertion (greedy .* makes the last occurrence's captures win).
shouldBe(/^.*[e\s](?:).*/gim.exec("ecqz\n")[0], "ecqz\n");
shouldBe(/^.*\W.*/.exec("ab\ncd")[0], "ab\ncd");
shouldBe(/^.*[c\Wx9]d*?.*/gm.exec("_bdd!9\ndxkd")[0], "_bdd!9\ndxkd");
shouldBe(/.*[^x]+.*/.exec("ab\ncd")[0], "ab\ncd");
shouldBe(JSON.stringify(/.*(?<=(.).|q).*$/.exec("aAa")), JSON.stringify(["aAa", "A"]));
shouldBe(JSON.stringify(/.*(?=(.))b.*/.exec("abab")), JSON.stringify(["abab", "b"]));
shouldBe(JSON.stringify(/.*(?<=(e|d))a.*$/s.exec("ea da\n")), JSON.stringify(["ea da\n", "d"]));
shouldBe(/.*foo.*/.exec("x\nafoob\ny")[0], "afoob");
shouldBe(/^.*foo.*$/m.exec("x\nafoob\ny")[0], "afoob");
shouldBe(/.*\bfoo\b.*/s.exec("x\na foo b\ny")[0], "x\na foo b\ny");

// Without /m, a ^ is not satisfied just past a newline.
shouldBe(/^.*b.*/.exec("-\nbbbb"), null);
shouldBe(JSON.stringify("aa\n-".match(/^.*..*/gu)), JSON.stringify(["aa"]));
shouldBe(/^.*b.*/m.exec("-\nbbbb")[0], "bbbb");

// U+2028/U+2029 are line terminators too (they live in the newline class's ranges).
shouldBe(new RegExp(".*[^\\r\\n].*").exec("b a")[0], "b a");
shouldBe(new RegExp(".*[a\\u2028].*").exec("a b")[0], "a b");
shouldBe(/.*\d.*/.exec("x a1b y")[0], "a1b");
// A ^ without /m that failed once keeps failing for later occurrences in the same match.
shouldBe(/^.*a.*$/.exec("x\n" + "a".repeat(1000)), null);
shouldBe(/^.*a.*$/s.exec("x\n" + "a".repeat(10))[0], "x\n" + "a".repeat(10));
{ const q = /^.*a.*/g; q.lastIndex = 3; shouldBe(q.exec("bxx" + "a".repeat(50)), null); q.lastIndex = 0; shouldBe(q.exec("bxxa")[0], "bxxa"); }
