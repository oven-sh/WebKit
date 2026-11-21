//@ skip if $hostOS == "playstation"
//@ runDefault
//@ runNoisyTestWithEnv "host-locale-without-region", "LANG=de", "LC_ALL=de"
//@ runNoisyTestWithEnv "host-locale-with-script", "LANG=sr_Latn_RS", "LC_ALL=sr_Latn_RS"

// ICU's table of likely subtags gives a locale the script and the region that it does not name. To load it takes
// about 22 million instructions and 435 page faults (a jsc without LTO). A locale that names its region needs nothing
// of it to format a date or to give its week. The bundled ICU (icu/calendar-likely-subtags.patch) asks the table only
// when it uses the answer, so these operations do not load it:
//
// - a date format, a relative time format, and the week and hour cycles of a locale with a region and no script;
// - a date format in each calendar: ICU computes the default century of a calendar class with a calendar of its own;
// - Intl.DateTimeFormat's change of the Gregorian cutover, for which ICU made a calendar in its default locale;
// - the name of an offset time zone, for which ICU made another one.
//
// The second and the third run have a default locale without a region (LANG=de) and one with a script
// (LANG=sr_Latn_RS). There, each of the last two loaded the table. The runner gives all three runs the same name.
//
// Every operation names its time zone. On a JavaScriptCore that reads the host's zone through a calendar of the root
// locale (DateCache), the first read of that zone loads the table, and that is not what this test is about.
//
// icuDataItemCount() is how many items of its data ICU has loaded. The shell has it where its ICU data is packed as
// Bun's is (jsc.cpp, BUN_ICU_ZSTD), and that ICU is the bundled one: the values below are the ones of its CLDR. In
// any other shell the operations run and nothing is compared.

const bundledICU = typeof icuDataItemCount === "function";

function expect(label, got, want)
{
    if (bundledICU && got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

function weekInfo(tag)
{
    return JSON.stringify(new Intl.Locale(tag).getWeekInfo());
}

const utc = { timeZone: "UTC" };
const longDate = { dateStyle: "long", timeZone: "UTC" };

// Date formats, by their defaults, by styles, by components, in parts and as a range.
expect("en-US", new Intl.DateTimeFormat("en-US", utc).format(0), "1/1/1970");
expect("en-US with styles", new Intl.DateTimeFormat("en-US", { dateStyle: "full", timeStyle: "long", timeZone: "UTC" }).format(0), "Thursday, January 1, 1970 at 12:00:00 AM UTC");
expect("de-DE with components", new Intl.DateTimeFormat("de-DE", { weekday: "long", year: "numeric", month: "long", day: "numeric", hour: "numeric", minute: "2-digit", timeZone: "UTC" }).format(0), "Donnerstag, 1. Januar 1970 um 0:00");
expect("en-GB in parts", new Intl.DateTimeFormat("en-GB", { dateStyle: "medium", timeZone: "UTC" }).formatToParts(0).map(part => part.value).join(""), "1 Jan 1970");
expect("ja-JP range", new Intl.DateTimeFormat("ja-JP", longDate).formatRange(0, 40 * 86400000), "1970/01/01～1970/02/10");
expect("en-001", new Intl.DateTimeFormat("en-001", { dateStyle: "short", timeZone: "UTC" }).format(0), "01/01/1970");
expect("es-419", new Intl.DateTimeFormat("es-419", longDate).format(0), "1 de enero de 1970");
expect("toLocaleString", new Date(0).toLocaleString("en-US", utc), "1/1/1970, 12:00:00 AM");
expect("toLocaleDateString", new Date(0).toLocaleDateString("fr-FR", utc), "01/01/1970");
expect("toLocaleTimeString", new Date(0).toLocaleTimeString("ko-KR", utc), "오전 12:00:00");

// The Gregorian calendar from the start of ECMAScript time: no Julian dates before 1582.
expect("year 2", new Intl.DateTimeFormat("en-US", longDate).format(-62198755200000), "January 1, 2");
expect("year 1500", new Intl.DateTimeFormat("en-US", longDate).format(Date.UTC(1500, 1, 29)), "March 1, 1500");

// Each calendar, and some in a locale that uses them.
for (const calendar of Intl.supportedValuesOf("calendar")) {
    const format = new Intl.DateTimeFormat(`en-US-u-ca-${calendar}`, longDate);
    if (format.resolvedOptions().calendar !== calendar)
        throw new Error(`en-US-u-ca-${calendar} has the calendar ${format.resolvedOptions().calendar}`);
    format.format(0);
}
expect("buddhist", new Intl.DateTimeFormat("th-TH", longDate).format(0), "1 มกราคม 2513");
expect("persian", new Intl.DateTimeFormat("fa-IR", longDate).format(0), "۱۱ دی ۱۳۴۸");
expect("hebrew", new Intl.DateTimeFormat("he-IL-u-ca-hebrew", longDate).format(0), "23 בטבת 5730");
expect("islamic-umalqura", new Intl.DateTimeFormat("ar-SA-u-ca-islamic-umalqura", longDate).format(0), "٢٢ شوال ١٣٨٩ هـ");
expect("japanese", new Intl.DateTimeFormat("ja-JP-u-ca-japanese", longDate).format(0), "昭和45年1月1日");
expect("chinese", new Intl.DateTimeFormat("zh-CN-u-ca-chinese", longDate).format(0), "1969己酉年十一月24");
expect("islamic-civil", new Intl.DateTimeFormat("en-US-u-ca-islamic-civil", longDate).format(0), "Shawwal 22, 1389 AH");
expect("roc", new Intl.DateTimeFormat("de-DE-u-ca-roc", longDate).format(0), "1. Januar 59 Minguo");
expect("indian", new Intl.DateTimeFormat("en-IN-u-ca-indian", longDate).format(0), "11 Pausa, 1891 Saka");
expect("coptic", new Intl.DateTimeFormat("en-US-u-ca-coptic", longDate).format(0), "Kiahk 23, 1686 AM");
expect("ethiopic", new Intl.DateTimeFormat("am-ET-u-ca-ethiopic", longDate).format(0), "23 ታኅሣሥ 1962 AM");

// The name of a time zone that is an offset, and of one that has rules.
expect("offset zone, short name", new Intl.DateTimeFormat("en-US", { timeZone: "+05:30", timeZoneName: "short" }).format(0), "1/1/1970, GMT+5:30");
expect("offset zone, long name", new Intl.DateTimeFormat("de-DE", { timeZone: "-08:00", timeZoneName: "long", hour: "numeric" }).format(0), "16 Uhr GMT-08:00");
expect("named zone", new Intl.DateTimeFormat("en-US", { timeZone: "Asia/Tokyo", timeZoneName: "long", hour: "numeric" }).format(0), "9 AM Japan Standard Time");

// What else opens a calendar for a locale.
expect("week of en-US", weekInfo("en-US"), `{"firstDay":7,"weekend":[6,7]}`);
expect("week of de-DE", weekInfo("de-DE"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of ar-EG", weekInfo("ar-EG"), `{"firstDay":6,"weekend":[5,6]}`);
expect("hour cycles of en-US", new Intl.Locale("en-US").getHourCycles().join(), "h12");
expect("hour cycles of ja-JP", new Intl.Locale("ja-JP").getHourCycles().join(), "h23");
expect("calendars of th-TH", new Intl.Locale("th-TH").getCalendars().join(), "buddhist,gregory");
expect("relative time", new Intl.RelativeTimeFormat("en-US").format(-1, "day"), "1 day ago");
expect("relative time in es-MX", new Intl.RelativeTimeFormat("es-MX", { numeric: "auto" }).format(1, "day"), "mañana");
expect("name of a date field", new Intl.DisplayNames("en-US", { type: "dateTimeField" }).of("month"), "month");

if (bundledICU) {
    // To maximize a locale takes the table. If it is loaded here, nothing above loaded it.
    const locale = new Intl.Locale("en");
    const before = icuDataItemCount();
    expect("maximized locale", locale.maximize().toString(), "en-Latn-US");
    if (icuDataItemCount() === before)
        throw new Error("ICU had the table of likely subtags before a locale was maximized: a date format, a week or a zone name of a locale with a region loaded it");
}

// ICU's calendar of the default century is made for en_US, which has to be what the table gives the root locale.
if (new Intl.Locale("und").maximize().toString() !== "en-Latn-US")
    throw new Error(`the root locale maximizes to ${new Intl.Locale("und").maximize()}, and the default century of ICU's calendars is computed in en_US (icu/calendar-likely-subtags.patch)`);

// A locale without a region takes its region from the table, and a locale with a script and a region asks it whether
// the script is the likely one. Their weeks and dates are the same as before.
expect("week of en", weekInfo("en"), `{"firstDay":7,"weekend":[6,7]}`);
expect("week of de", weekInfo("de"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of ar", weekInfo("ar"), `{"firstDay":6,"weekend":[5,6]}`);
expect("week of und", weekInfo("und"), `{"firstDay":7,"weekend":[6,7]}`);
expect("week of und-AE", weekInfo("und-AE"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of zh-Hant-TW", weekInfo("zh-Hant-TW"), `{"firstDay":7,"weekend":[6,7]}`);
expect("week of sr-Latn-RS", weekInfo("sr-Latn-RS"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of sr-Cyrl-RS", weekInfo("sr-Cyrl-RS"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of en-Latn-GB", weekInfo("en-Latn-GB"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of en-Arab-AE", weekInfo("en-Arab-AE"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of fa-AF", weekInfo("fa-AF"), `{"firstDay":6,"weekend":[4,5]}`);
expect("week of en with the region of GB", weekInfo("en-u-rg-gbzzzz"), `{"firstDay":1,"weekend":[6,7]}`);
expect("week of en-US that starts on Monday", weekInfo("en-US-u-fw-mon"), `{"firstDay":1,"weekend":[6,7]}`);
expect("en", new Intl.DateTimeFormat("en", { dateStyle: "full", timeZone: "UTC" }).format(0), "Thursday, January 1, 1970");
expect("zh-Hant-TW", new Intl.DateTimeFormat("zh-Hant-TW", { dateStyle: "full", timeZone: "UTC" }).format(0), "1970年1月1日 星期四");
expect("sr-Latn-RS", new Intl.DateTimeFormat("sr-Latn-RS", { dateStyle: "full", timeZone: "UTC" }).format(0), "četvrtak, 1. januar 1970.");
