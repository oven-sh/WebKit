//@ runDefault("--useDollarVM=1")
// A RegExp's compiled code is shared per VM (RegExpCache keys on source and flags), and some code
// generation decisions -- the Boyer-Moore search range, the vector-vs-scalar scan -- are made from
// a sample of the first subject the shared RegExp is run on. Whichever realm or literal got there
// first, and whatever it matched against, every user of that source must get the same results as
// an unprimed compile and as the interpreter. Also: churning through many distinct sources (cache
// eviction and recompilation) must not grow the process without bound.

function shouldBe(actual, expected, message) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error((message ? message + ": " : "") + "expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

const otherRealm = createGlobalObject();

// [source, flags, priming subject run first in the other realm, subjects to check]
const cases = [
    ["\\d+px", "g", "1px", ["width: 120px; height: 4px; margin: 0 12px 0 3px", "no digits here at all, none", "9".repeat(200) + "px"]],
    ["@\\w+", "g", "@a", ["mail me @home or @work, not @", "@".repeat(50) + "x", "plain text without the char"]],
    ["#[0-9a-fA-F]{6}\\b", "g", "#", ["color:#a0b1c2;background:#FFF;border:#12345g #123456", "#".repeat(30) + "abcdef"]],
    ["[&<>\\u00A0]", "g", "&", ["a&b<c>d e and a long tail of ordinary prose so the sample says something", "nothing to escape in this one"]],
    ["\\b(?:break|case|catch|class|const|continue|default|delete|do|else)\\b", "g", "do", ["do { x } else class const y = delete z; caseless", "none of the words"]],
    ["(?<=[a-z]{2})=(\\d)", "", "ab=1", ["xy=7 and q=8", "ab=", "=9"]],
    ["^\\p{RGI_Emoji}+$", "v", "🙂", ["🙂🙂👋🏻🇺🇸", "🙂x", "👨‍👩‍👧‍👦".repeat(300)]],
    ["[\\q{abc|ab|a}\\d]+", "gv", "a", ["xabcab1a", "999", "b"]],
];

function resultsFor(re, subjects) {
    const out = [];
    for (const s of subjects) {
        re.lastIndex = 0;
        const all = [];
        if (re.global || re.sticky) {
            let m;
            while ((m = re.exec(s))) {
                all.push([m.index, m[0], ...m.slice(1)]);
                if (m[0] === "")
                    re.lastIndex++;
            }
        } else {
            const m = re.exec(s);
            all.push(m && [m.index, m[0], ...m.slice(1)]);
        }
        out.push(all, re.test(s), s.search(new RegExp(re.source, re.flags.replace(/[gy]/g, ""))));
        for (const width16 of [false, true]) {
            const t = width16 ? $vm.make16BitStringIfPossible(s) : s;
            re.lastIndex = 0;
            out.push(t.replace(re, "[$&]"), t.split(new RegExp(re.source, re.flags.replace(/[gy]/g, ""))).length);
        }
    }
    return out;
}

for (const [source, flags, primer, subjects] of cases) {
    // 1. The other realm compiles this source first and primes it with a short subject.
    const theirs = otherRealm.eval(`(function () { const re = new RegExp(${JSON.stringify(source)}, ${JSON.stringify(flags)}); re.test(${JSON.stringify(primer)}); return re; })()`);
    // 2. This realm's literal-equivalent RegExp of the same source (shares the compiled RegExp).
    const ours = new RegExp(source, flags);
    // 3. An unshared compile of the same language: a distinct source, so a distinct cache entry,
    //    first run directly on the long subjects.
    const fresh = new RegExp("(?:)" + source, flags);

    const expected = resultsFor(fresh, subjects);
    shouldBe(resultsFor(ours, subjects), expected, source + " (shared, primed elsewhere)");
    shouldBe(resultsFor(theirs, subjects), expected, source + " (the other realm's object)");
    // And the same objects again after the long subjects (recompiles for 16-bit, match-only, etc.).
    shouldBe(resultsFor(ours, subjects), expected, source + " (second pass)");

    // 4. Priming order the other way around: this realm first with a long 16-bit subject, then
    //    the other realm's fresh object of that source on 8-bit subjects.
    const source2 = "(?:)(?:)" + source;
    const mine = new RegExp(source2, flags);
    mine.test($vm.make16BitStringIfPossible(subjects[0] + "Ā"));
    const theirs2 = otherRealm.eval(`new RegExp(${JSON.stringify(source2)}, ${JSON.stringify(flags)})`);
    shouldBe(resultsFor(theirs2, subjects), expected, source + " (primed 16-bit here, used 8-bit there)");
}

// Cache churn: many distinct sources, each compiled (JIT where eligible), run once and dropped.
// Footprint may rise while the cache and JIT pools warm up, but must level off.
function churn(count, tag) {
    let sink = 0;
    for (let i = 0; i < count; ++i) {
        const re = new RegExp("(?:k" + tag + "x" + i + "|q" + (i % 97) + "|zz(?<=z.)|\\d+" + (i % 7) + ")", i % 3 ? "g" : "u");
        sink += re.test("prefix q" + (i % 97) + " k" + tag + "x" + i + " 123" + (i % 7)) ? 1 : 0;
    }
    return sink;
}
churn(20000, "warm");
fullGC();
const before = MemoryFootprint().current;
for (let round = 0; round < 5; ++round) {
    shouldBe(churn(20000, "r" + round), 20000, "churn results");
    fullGC();
}
const after = MemoryFootprint().current;
// Five more rounds of 20k distinct RegExps must not cost more than a modest, bounded amount.
if (after - before > 96 * 1024 * 1024)
    throw new Error("footprint grew by " + ((after - before) >> 20) + " MB across RegExp cache churn");
