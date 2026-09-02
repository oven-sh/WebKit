//@ requireOptions("--useJSThreads=1")
// String.prototype.localeCompare(that, locale) keeps the last collator on the
// global object, with the locale string it was made for. Several threads
// compare at once. Each one switches between two locales that order these
// pairs differently, so every call replaces the cached collator, and a
// collator for the wrong locale gives a wrong sign.
load("../harness.js", "caller relative");

// The first locale of each thread orders every pair below as "a < b". The
// second one puts at least one pair the other way.
const localesByThread = [
    ["en", "sv"],
    ["de", "cs"],
    ["es", "da"],
    ["tr", "fi"],
];
const THREADS = localesByThread.length;
const ROUNDS = 80;

const pairs = [["ä", "z"], ["ch", "d"], ["å", "z"], ["ö", "z"], ["ll", "m"]];

function compareAll(locales) {
    const out = [];
    for (const locale of locales) {
        for (const [a, b] of pairs)
            out.push(a.localeCompare(b, locale));
    }
    return out;
}

// Computed before any thread starts.
const expected = localesByThread.map(compareAll);
for (const e of expected)
    shouldBe(e.slice(0, pairs.length).join() !== e.slice(pairs.length).join(), true, "the two locales of a thread must differ");

function work(t) {
    for (let round = 0; round < ROUNDS; ++round) {
        const got = compareAll(localesByThread[t]);
        for (let i = 0; i < got.length; ++i) {
            if (got[i] !== expected[t][i])
                return "thread " + t + " round " + round + " item " + i + ": " + got[i];
        }
    }
    return null;
}

for (const r of joinAll(spawnN(THREADS, work)))
    shouldBe(r, null);
shouldBe(work(0), null);
