//@ requireOptions("--useJSThreads=1")
// Several Intl objects compute parts of their state on first use: a Locale's
// subtags and keywords, the numbering system and calendar that resolvedOptions()
// reports, the formatter behind formatRange(), the pieces formatToParts() and
// the segment objects are built from. With the GIL off several threads can use
// one shared Intl object first at the same time. Each must get the same,
// complete answer, and nothing may be freed under another thread.
load("../harness.js", "caller relative");

if (typeof Intl !== "object")
    throw new Error("no Intl");

const THREADS = 4;
const ROUNDS = 300;

function describeLocale(locale) {
    return [locale.baseName, locale.language, locale.script, locale.region,
        locale.calendar, locale.caseFirst, locale.collation, locale.hourCycle,
        locale.numberingSystem, String(locale.numeric), locale.toString(),
        locale.maximize().toString(), locale.minimize().toString(),
        typeof locale.variants === "string" ? locale.variants : ""].join("|");
}

function describeResolved(format) {
    const o = format.resolvedOptions();
    return Object.keys(o).map((k) => k + "=" + o[k]).join("|");
}

const tags = ["en-US-u-ca-gregory-nu-latn", "de-DE-u-co-phonebk-hc-h23", "ja-JP-u-ca-japanese", "sr-Latn-RS", "zh-Hant-TW-u-nu-hanidec", "fr-CA-u-fw-mon"];

// Reference answers, computed on the main thread from objects nobody shares.
const localeExpected = tags.map((tag) => describeLocale(new Intl.Locale(tag)));
const numberExpected = tags.map((tag) => describeResolved(new Intl.NumberFormat(tag)));
const dateExpected = tags.map((tag) => describeResolved(new Intl.DateTimeFormat(tag, { timeZone: "UTC" })));
const d1 = new Date(Date.UTC(2020, 0, 2, 3, 4, 5));
const d2 = new Date(Date.UTC(2021, 5, 7, 8, 9, 10));
const rangeExpected = tags.map((tag) => new Intl.DateTimeFormat(tag, { timeZone: "UTC" }).formatRange(d1, d2));
const relativeExpected = tags.map((tag) => JSON.stringify(new Intl.RelativeTimeFormat(tag).formatToParts(-3, "day")));
const text = "The quick brown fox. It jumped! Over 3.5 lazy dogs?";
const segmentExpected = JSON.stringify(Array.from(new Intl.Segmenter("en", { granularity: "word" }).segment(text), (s) => s.segment));
const hasDuration = typeof Intl.DurationFormat === "function";
const durationExpected = hasDuration ? tags.map((tag) => new Intl.DurationFormat(tag, { style: "long" }).format({ hours: 1, minutes: 2, seconds: 3 })) : null;

for (let round = 0; round < 6; ++round) {
    // Fresh shared objects every round, so that every round races the first use.
    const shared = [];
    for (let r = 0; r < ROUNDS / 6; ++r) {
        for (let i = 0; i < tags.length; ++i) {
            shared.push({
                i,
                locale: new Intl.Locale(tags[i]),
                number: new Intl.NumberFormat(tags[i]),
                date: new Intl.DateTimeFormat(tags[i], { timeZone: "UTC" }),
                relative: new Intl.RelativeTimeFormat(tags[i]),
                segments: new Intl.Segmenter("en", { granularity: "word" }).segment(text),
                duration: hasDuration ? new Intl.DurationFormat(tags[i], { style: "long" }) : null,
            });
        }
    }
    const gate = { go: 0 };
    const threads = spawnN(THREADS, (t) => {
        while (Atomics.load(gate, "go") === 0) { }
        let checked = 0;
        for (const s of shared) {
            const i = s.i;
            if (describeLocale(s.locale) !== localeExpected[i])
                throw new Error("thread " + t + ": locale " + tags[i] + ": " + describeLocale(s.locale));
            if (describeResolved(s.number) !== numberExpected[i])
                throw new Error("thread " + t + ": NumberFormat " + tags[i] + ": " + describeResolved(s.number));
            if (describeResolved(s.date) !== dateExpected[i])
                throw new Error("thread " + t + ": DateTimeFormat " + tags[i] + ": " + describeResolved(s.date));
            if (s.date.formatRange(d1, d2) !== rangeExpected[i])
                throw new Error("thread " + t + ": formatRange " + tags[i]);
            if (JSON.stringify(s.relative.formatToParts(-3, "day")) !== relativeExpected[i])
                throw new Error("thread " + t + ": RelativeTimeFormat " + tags[i]);
            // containing() on the shared Segments object, from several threads.
            const words = [];
            for (let at = 0; at < text.length; ) {
                const seg = s.segments.containing(at);
                words.push(seg.segment);
                at = seg.index + seg.segment.length;
            }
            if (JSON.stringify(words) !== segmentExpected)
                throw new Error("thread " + t + ": segments " + JSON.stringify(words));
            if (hasDuration && s.duration.format({ hours: 1, minutes: 2, seconds: 3 }) !== durationExpected[i])
                throw new Error("thread " + t + ": DurationFormat " + tags[i]);
            ++checked;
        }
        return checked;
    });
    Atomics.store(gate, "go", 1);
    for (const checked of joinAll(threads))
        shouldBe(checked, shared.length, "objects checked by a thread, round " + round);
}
