//@ requireOptions("--useJSThreads=1")
// Every thread converts dates through its own DateCache (local-time offset,
// year/month/day, parse-string and time zone caches). The caches are pure
// value caches fed by the one host time zone, so a spawned thread must
// produce exactly the main thread's answers, both on a cold cache and on a
// warm one, and the main thread's answers must be unchanged afterwards.
load("../resources/assert.js", "caller relative");

const THREADS = 3;

const stamps = [
    0, -1, 86400000 * 365, 951782400000, 1078012800000, 4107456000000,
    -2208988800000, 253402300799999, 1700000000000,
];
// Year boundaries (local-time offset cache churn around DST edges).
for (let y = 0; y < 12; ++y)
    stamps.push(Date.UTC(2010 + y, 0, 1) - 1, Date.UTC(2010 + y, 0, 1));
// Spring and autumn transitions for the zones that have them.
for (let y = 2015; y < 2025; ++y)
    stamps.push(Date.UTC(y, 2, 29, 7), Date.UTC(y, 9, 25, 7));
// Dense same-month run (year/month/day cache fast path).
for (let d = 0; d < 24; ++d)
    stamps.push(Date.UTC(2023, 5, 1 + d, d % 24, d, d, d * 7 % 1000));

// Local-time date strings (no "Z"): parsed through the local-time offset path.
const localStrings = [
    "2020-06-15T12:00:00", "2021-01-01T00:00:00", "2019-03-10T02:30:00",
    "2019-11-03T01:30:00", "1985-07-04T09:15:45.123", "2038-01-19T03:14:07",
];

function convert() {
    const out = [];
    for (const ts of stamps) {
        const d = new Date(ts);
        out.push(d.toString(), d.toISOString(), d.toUTCString(),
            d.getFullYear(), d.getMonth(), d.getDate(), d.getDay(), d.getHours(), d.getMinutes(),
            d.getTimezoneOffset(), d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCHours());
        // Setters go through the local-time -> UTC direction of the offset cache.
        const s = new Date(ts);
        s.setHours(12, 34, 56, 789);
        s.setFullYear(2022, 1, 28);
        out.push(s.getTime(), Date.UTC(s.getFullYear(), s.getMonth(), s.getDate()));
        out.push(new Date(s.getFullYear(), s.getMonth(), s.getDate(), 23, 59, 59).getTime());
    }
    for (const str of localStrings)
        out.push(Date.parse(str), Date.parse(str + "Z"), new Date(str).toISOString());
    return out.join("\u0001");
}

const oracle = convert();
shouldBe(convert(), oracle, "main thread warm cache");

const threads = [];
for (let t = 0; t < THREADS; ++t) {
    threads.push(new Thread(() => {
        const cold = convert();
        const warm = convert();
        return cold === warm ? cold : "cold/warm mismatch";
    }));
}
for (let t = 0; t < THREADS; ++t)
    shouldBe(threads[t].join(), oracle, "thread " + t);

shouldBe(convert(), oracle, "main thread after join");
