//@ requireOptions("--useJSThreads=1")
// Date.prototype.toLocaleString(locale) builds an Intl.DateTimeFormat, and the
// VM keeps the last four of those in a small cache keyed by locale. A lookup
// moves the entry it finds to the front. Four threads format at once, each
// with a locale of its own, so nearly every call is a lookup that reorders
// the shared entries. Now and then a thread also uses a fifth locale, which
// replaces an entry. Each thread must get its own locale's text.
load("../harness.js", "caller relative");

const localesByThread = ["en-US", "de-DE", "ja-JP", "ko-KR"];
const evictingLocale = "ru-RU";
const THREADS = localesByThread.length;
const ROUNDS = 400;

const dates = [
    new Date(Date.UTC(2021, 2, 4, 5, 6, 7)),
    new Date(Date.UTC(1999, 11, 31, 23, 59, 58)),
];

function formatAll(locale) {
    return dates.map(d => d.toLocaleString(locale));
}

// Computed before any thread starts.
const expected = localesByThread.map(formatAll);
const expectedEvicting = formatAll(evictingLocale);
for (let t = 0; t < THREADS; ++t)
    shouldBe(expected[t][0] !== expected[(t + 1) % THREADS][0] && expected[t][0] !== expectedEvicting[0], true, "locales must format differently for this test to see a mix-up");

function check(t, round, got, want) {
    for (let i = 0; i < got.length; ++i) {
        if (got[i] !== want[i])
            return "thread " + t + " round " + round + ": " + got[i] + " (expected " + want[i] + ")";
    }
    return null;
}

function work(t) {
    for (let round = 0; round < ROUNDS; ++round) {
        const bad = check(t, round, formatAll(localesByThread[t]), expected[t]);
        if (bad)
            return bad;
        if (round % 50 === 49) {
            const evictBad = check(t, round, formatAll(evictingLocale), expectedEvicting);
            if (evictBad)
                return evictBad;
        }
    }
    return null;
}

for (const r of joinAll(spawnN(THREADS, work)))
    shouldBe(r, null);
shouldBe(work(0), null);
