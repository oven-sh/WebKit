function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected} but got ${actual}`);
}

function join(a, b) { return a + b; }
noInline(join);

const source = "alpha beta gamma delta epsilon zeta eta theta";
const words = source.split(" ");
const wide = "Āété".slice(1);

for (let round = 0; round < 3; ++round) {
    shouldBe(words.indexOf(join("gam", "ma")), 2, "a rope is looked for");
    shouldBe(words.indexOf(source.slice(11, 16)), 2, "a substring is looked for");
    shouldBe([join("al", "pha"), join("be", "ta")].indexOf("beta"), 1, "the elements are ropes");
    shouldBe([join("al", "pha"), join("be", "ta")].indexOf(join("b", "eta")), 1, "ropes on both sides");
    shouldBe(["abcd", "abce", "abcf"].indexOf(join("abc", "f")), 2, "the same length, other characters");
    shouldBe(["abcd", "abce", "abcf"].indexOf("abcg"), -1, "the same length, found nowhere");
    shouldBe(["abc", "abcd"].indexOf("ab"), -1, "a prefix");
    shouldBe(["x", "été"].indexOf(wide), 1, "16-bit characters are looked for among 8-bit ones");
    shouldBe(["x", wide].indexOf("été"), 1, "8-bit characters are looked for among 16-bit ones");
    shouldBe([1, null, undefined, {}, Symbol("1"), 1n, true, "1"].indexOf("1"), 7, "values of other kinds");
    shouldBe([new String("a"), ["a"], "a"].indexOf("a"), 2, "objects that convert to the string");
    shouldBe([, "a", , "b"].indexOf("b"), 3, "holes");
    shouldBe([, , ,].concat(["a"]).indexOf("a"), 3, "holes in front");
    shouldBe(["", "a"].indexOf(""), 0, "the empty string");
    shouldBe(["a", join("", "")].indexOf(""), 1, "the empty string made at run time");
    shouldBe(["a", "b", "a"].indexOf("a", 1), 2, "from an index");
    shouldBe(["a", "b", "a"].indexOf("a", -1), 2, "from an index counted from the end");
    shouldBe(["a", "b", "a"].indexOf("a", 3), -1, "from the length");
    shouldBe(["a", "b"].indexOf("c"), -1, "found nowhere");
}

const names = [];
for (let i = 0; i < 10000; ++i)
    names.push(join("name", String(i)));
shouldBe(names.indexOf(join("name", "9999")), 9999, "the last of many");
shouldBe(names.indexOf(join("name", "5000")), 5000, "one in the middle of many");
shouldBe(names.indexOf("name10000"), -1, "none of many");
shouldBe(names.indexOf(names[1234]), 1234, "the same string");
