//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// A pattern in unicode mode with a step that can consume a surrogate pair (a negated class, a class with characters beyond the BMP, the dot, a property escape)
// runs on a flat program, whatever the subject's characters. The option compares it with the bytecode interpreter at every match.

function shouldBe(actual, expected, what) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(what + ": " + JSON.stringify(actual) + " is not " + JSON.stringify(expected));
}

function make(source, flags) { return new RegExp(source, flags); }

shouldBe("ab cd-e".replace(make("[^a-z\\u{1d400}]", "gu"), "_"), "ab_cd_e", "a negated class with a character beyond the BMP");
shouldBe("ab cd-e".replace(make("[^a-z]+", "gu"), ""), "abcde", "a run of a negated class");
shouldBe("a\nb".match(make(".", "gu")), ["a", "b"], "the dot");
shouldBe("a\nb".match(make(".", "gsu")), ["a", "\n", "b"], "the dot that takes line ends");
shouldBe("xéÿy1".match(make("\\p{L}+", "u")), ["xéÿy"], "letters");
shouldBe("xé 1".replace(make("\\P{L}", "gu"), "."), "xé..", "everything but letters");
shouldBe(make("[^µ]", "iu").exec("µµa").index, 2, "micro sign without regard to case");
shouldBe(make("[^k]", "iu").exec("kKx").index, 2, "k without regard to case");
shouldBe(make("[^s]", "iu").exec("sSx").index, 2, "s without regard to case");
shouldBe(make("^[^<]*<([^>]*)>", "u").exec("text <tag> more").slice(), ["text <tag>", "tag"], "groups between negated classes");
shouldBe("a𝐀b".replace(make("[^a-z]", "gu"), "_"), "a_b", "a pair is one character of a 16-bit subject");
shouldBe("a\ud835b".replace(make("[^a-z]", "gu"), "_"), "a_b", "a lone surrogate");
shouldBe("a𝐀b".match(make(".", "gu")).length, 3, "the dot takes a pair");
shouldBe("abc".replace(make("[^[a-z]--[b]]", "gv"), "_"), "a_c", "a negated set difference");

let seed = 12345;
function random(limit) {
    seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
    return (seed >>> 8) % limit;
}
function pick(list) { return list[random(list.length)]; }

const atoms = [
    "a", "b", "é", "ÿ", "µ", "ß", "k", "s", "\\.", ".", "\\d", "\\D", "\\w", "\\W", "\\s", "\\S", "[^a]", "[^a-c]", "[^\\d]", "[^\\w]", "[^\\sé]", "[a\\u{1d400}]", "[^a\\u{1d400}-\\u{1d7ff}]",
    "[\\u{10000}-\\u{10ffff}]", "[^\\u{10000}-\\u{10ffff}]", "\\p{L}", "\\P{L}", "\\p{Lu}", "\\p{Ll}", "[^\\p{L}]", "[\\p{N}\\p{P}]", "[^\\p{Lu}b]", "\\p{ASCII}", "\\P{ASCII}", "[^Ā-￿]", "[À-Ű]", "[^K]", "[^ſ]",
];
const quantifiers = ["", "", "", "*", "+", "?", "*?", "+?", "??", "{2}", "{1,3}", "{0,2}?"];
// No run of runs: the time such a pattern takes grows too fast with the subject.
const quantifiersOfGroups = ["", "", "?", "??", "{1,2}"];
function term(depth) {
    const kind = random(depth > 2 ? 6 : 10);
    if (kind < 6)
        return pick(atoms) + pick(quantifiers);
    if (kind === 6)
        return "(?:" + alternatives(depth + 1) + ")" + pick(quantifiersOfGroups);
    if (kind === 7)
        return "(" + alternatives(depth + 1) + ")" + pick(quantifiersOfGroups);
    if (kind === 8)
        return "(?" + pick(["=", "!"]) + alternatives(depth + 1) + ")";
    return pick(["^", "$", "\\b", "\\B"]);
}
function sequence(depth) {
    let text = "";
    for (let count = 1 + random(3); count--;)
        text += term(depth);
    return text;
}
function alternatives(depth) {
    let text = sequence(depth);
    while (!random(3))
        text += "|" + sequence(depth);
    return text;
}
const characters = "aabbcckKsS \n\t.,-_019<>éÉÿµß ×åÅ";
function subject() {
    let text = "";
    for (let count = random(16); count--;)
        text += characters[random(characters.length)];
    return text;
}

let matches = 0;
for (let round = 0; round < 2000; ++round) {
    const pattern = make(alternatives(0), pick(["u", "gu", "iu", "giu", "mu", "su", "gimsu", "v", "giv", "yu"]));
    for (let use = 0; use < 4; ++use) {
        const text = subject();
        pattern.lastIndex = 0;
        if (pattern.test(text))
            ++matches;
        pattern.lastIndex = 0;
        text.replace(pattern, "<$&>");
        pattern.lastIndex = random(4);
        pattern.exec(text);
        pattern.lastIndex = 0;
        pattern.exec(text + "Ā");
    }
}
if (!matches)
    throw new Error("nothing matched");
