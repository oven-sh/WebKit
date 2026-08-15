//@ runDefault("--useDollarVM=1")
//@ skip if $hostOS == "playstation"

// https://bugs.webkit.org/show_bug.cgi?id=318362
// Distinct from the DateInstanceData staleness covered by
// intl-datetimeformat-stale-data-*-across-tz-change.js: there the stale state
// is a held Date instance's cached GregorianDateTime, while here even a freshly
// created Date is affected. The per-global default formatters
// (JSGlobalObject::m_defaultDateTimeFormat / m_defaultDateFormat /
// m_defaultTimeFormat) backing the no-argument Date.prototype.toLocale*String
// paths capture the DateCache's default time zone when they are materialized,
// so they have to be re-armed once the DateCache has been refreshed for a time
// zone change (DateCache::cachedTimeZoneID() moved), or they keep formatting in
// the old time zone while every other construction shape moves.
//
// This covers the time zone flavor of the default-formatter staleness only.
// The language-change flavor
// (intl-datetimeformat-default-formatter-stale-across-language-change.js) and
// the evicted-DateInstanceData case filed under the same bug
// (intl-datetimeformat-stale-data-evicted-across-tz-change.js) are still
// skipped.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${JSON.stringify(want)}, got ${JSON.stringify(got)}`);
}

if (!$vm.setHostTimeZone("America/Los_Angeles"))
    throw new Error("Failed to set host time zone to America/Los_Angeles");

setTimeout(() => {
    // 2024-06-15 00:00 UTC.
    //   In LA  (PDT, UTC-7): 2024-06-14 17:00 PDT.
    //   In JST (UTC+9):      2024-06-15 09:00 JST.
    const ms = Date.UTC(2024, 5, 15, 0, 0);
    const warm = new Date(ms);

    // Initialize all three per-global default formatters under LA time. Each
    // no-argument call must agree with its explicit-empty-options shape, which
    // builds a fresh formatter with the same required/defaults.
    const laAll = warm.toLocaleString();
    const laDate = warm.toLocaleDateString();
    const laTime = warm.toLocaleTimeString();
    expect("LA toLocaleString agrees with slow path", laAll, warm.toLocaleString(undefined, {}));
    expect("LA toLocaleDateString agrees with slow path", laDate, warm.toLocaleDateString(undefined, {}));
    expect("LA toLocaleTimeString agrees with slow path", laTime, warm.toLocaleTimeString(undefined, {}));

    if (!$vm.setHostTimeZone("Asia/Tokyo"))
        throw new Error("Failed to set host time zone to Asia/Tokyo");

    // $vm.setHostTimeZone only bumps WTF::lastTimeZoneID(); this VM's DateCache
    // is refreshed by the entry-scope service on the next entry, so right now
    // every shape still sees LA. Using the default formatters inside that
    // window is the interesting part: they have to agree with the
    // not-yet-refreshed DateCache now, and whatever this builds must still be
    // replaced once the DateCache has been refreshed below. A re-arm keyed on
    // WTF::lastTimeZoneID() itself would rebuild them right here from the
    // stale DateCache, stamp them as current, and leave them stale afterwards.
    const preRefreshZone = new Intl.DateTimeFormat().resolvedOptions().timeZone;
    expect("pre-refresh toLocaleString agrees with slow path",
        warm.toLocaleString(), warm.toLocaleString(undefined, {}));
    expect("pre-refresh toLocaleDateString agrees with slow path",
        warm.toLocaleDateString(), warm.toLocaleDateString(undefined, {}));
    expect("pre-refresh toLocaleTimeString agrees with slow path",
        warm.toLocaleTimeString(), warm.toLocaleTimeString(undefined, {}));
    if (preRefreshZone === "America/Los_Angeles")
        expect("pre-refresh toLocaleString is still LA", warm.toLocaleString(), laAll);
    else
        expect("pre-refresh tz", preRefreshZone, "Asia/Tokyo");

    setTimeout(() => {
        expect("post-change tz",
            new Intl.DateTimeFormat().resolvedOptions().timeZone, "Asia/Tokyo");

        // A fresh Date cannot carry stale DateInstanceData; any disagreement
        // below belongs to the default formatters.
        const fresh = new Date(ms);
        expect("fresh JST hours", fresh.getHours(), 9);

        expect("JST toLocaleString agrees with slow path",
            fresh.toLocaleString(), fresh.toLocaleString(undefined, {}));
        expect("JST toLocaleDateString agrees with slow path",
            fresh.toLocaleDateString(), fresh.toLocaleDateString(undefined, {}));
        expect("JST toLocaleTimeString agrees with slow path",
            fresh.toLocaleTimeString(), fresh.toLocaleTimeString(undefined, {}));

        // LA and JST renderings differ in both calendar day and hour, so the
        // no-argument outputs must actually move.
        if (fresh.toLocaleString() === laAll)
            throw new Error(`toLocaleString did not move with the TZ change: "${laAll}"`);
        if (fresh.toLocaleDateString() === laDate)
            throw new Error(`toLocaleDateString did not move with the TZ change: "${laDate}"`);
        if (fresh.toLocaleTimeString() === laTime)
            throw new Error(`toLocaleTimeString did not move with the TZ change: "${laTime}"`);
    }, 0);
}, 0);
