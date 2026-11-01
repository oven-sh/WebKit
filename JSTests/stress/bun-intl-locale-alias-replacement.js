// A locale identifier is canonicalized with CLDR's alias tables (UTS #35 Annex C): a deprecated language,
// script, region, variant or subdivision is replaced, wherever a language tag enters Intl.
// The canonical forms are those of icu::Locale::createCanonical() in ICU 79.1 (icu/source.json). A newer CLDR can
// change a replacement.

function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrow(run, errorType)
{
    let thrown = null;
    try {
        run();
    } catch (error) {
        thrown = error;
    }
    if (!(thrown instanceof errorType))
        throw new Error(`expected ${errorType.name} but got ${thrown}`);
}

const canonicalForms = {
    // A language.
    "fre": "fr",
    "iw": "he",
    "mo": "ro",
    "tl": "fil",
    "cmn": "zh",
    "jw": "jv",
    // A language whose replacement has a script or a region. The tag's own script or region stays.
    "sh": "sr-Latn",
    "sh-Cyrl": "sr-Cyrl",
    "sh-BA": "sr-Latn-BA",
    "cnr": "sr-ME",
    "cnr-BA": "sr-BA",
    "prs": "fa-AF",
    "swc": "sw-CD",
    "fre-CA": "fr-CA",
    "cmn-Hant-TW": "zh-Hant-TW",
    // A language with a region, and a language with a variant.
    "sgn-DE": "gsg",
    "sgn-US": "ase",
    "hy-arevela": "hy",
    "hy-arevmda": "hyw",
    "aa-saaho": "ssy",
    "zh-guoyu": "zh",
    "art-lojban": "jbo",
    "no-bokmal": "nb",
    "no-nynorsk": "nn",
    "und-arevela": "und",
    "und-aaland": "und-AX",
    // A region.
    "en-UK": "en-GB",
    "de-DD": "de-DE",
    "en-BU": "en-MM",
    "en-ZR": "en-CD",
    "en-840": "en-US",
    "de-276": "de-DE",
    // A region that several regions replace: the one the language is most likely in, or the first.
    "ru-SU": "ru-RU",
    "hy-SU": "hy-AM",
    "und-SU": "und-RU",
    "und-Armn-SU": "und-Armn-AM",
    "sr-YU": "sr-RS",
    "en-YU": "en-RS",
    "nl-AN": "nl-CW",
    // A script, and a variant.
    "und-Qaai": "und-Zinh",
    "el-polytoni": "el-polyton",
    "und-heploc": "und-alalc97",
    "ja-Latn-hepburn-heploc": "ja-Latn-alalc97",
    // A subdivision in "rg" or "sd".
    "en-u-sd-cn11": "en-u-sd-cnbj",
    "en-u-rg-no23": "en-u-rg-no50",
    // The language of a "t" extension, and the order of its fields.
    "und-t-iw-m0-names": "und-t-he-m0-prprname",
    "und-t-sh-latn": "und-t-sr-latn",
    "en-t-hi-h0-hybrid-d0-fwidth": "en-t-hi-d0-fwidth-h0-hybrid",
    // Several at once, and a locale ID that is longer than the buffer it starts in.
    "sh-YU-polytoni-u-sd-cn11": "sr-Latn-RS-polyton-u-sd-cnbj",
    "fre-Latn-FR-u-ca-gregory-co-phonebk-nu-latn": "fr-Latn-FR-u-ca-gregory-co-phonebk-nu-latn",
    "fre-UK-u-ca-gregory-co-phonebk-cu-eur-em-default-hc-h23-ks-level1-lb-strict-lw-normal-ms-metric-nu-latn-rg-no23-sd-cn11-ss-none-tz-atvie-va-posix":
        "fr-GB-u-ca-gregory-co-phonebk-cu-eur-em-default-hc-h23-ks-level1-lb-strict-lw-normal-ms-metric-nu-latn-rg-no50-sd-cnbj-ss-none-tz-atvie-va-posix",
    // Nothing to replace.
    "en": "en",
    "en-US": "en-US",
    "de-DE": "de-DE",
    "no": "no",
    "nb": "nb",
    "zh-Hant-TW": "zh-Hant-TW",
    "sr-Latn-RS": "sr-Latn-RS",
    "es-419": "es-419",
    "ca-ES-valencia": "ca-ES-valencia",
    "en-US-u-ca-gregory": "en-US-u-ca-gregory",
    "de-u-co-phonebk-nu-latn": "de-u-co-phonebk-nu-latn",
    "und": "und",
    "und-Latn": "und-Latn",
    "und-US": "und-US",
    "und-u-ca-gregory": "und-u-ca-gregory",
    "und-x-private": "und-x-private",
};

for (const [tag, canonical] of Object.entries(canonicalForms)) {
    shouldBe(Intl.getCanonicalLocales(tag)[0], canonical);
    shouldBe(new Intl.Locale(tag).toString(), canonical);
    // A canonical form is its own canonical form.
    shouldBe(Intl.getCanonicalLocales(canonical)[0], canonical);
    shouldBe(new Intl.Locale(new Intl.Locale(tag)).toString(), canonical);
}

// Tags that differ only in an alias are one locale.
shouldBe(Intl.getCanonicalLocales(["fr", "fre", "FRE", "fra"]).join(), "fr");
shouldBe(Intl.getCanonicalLocales(["sh", "tl", "cmn", "no"]).join(), "sr-Latn,fil,zh,no");

// Intl.Locale canonicalizes the tag, applies the options, and canonicalizes again. So "SU" is replaced for
// "und-Armn", before the language becomes "ru", and an option is replaced too.
shouldBe(new Intl.Locale("und-Armn-SU", { language: "ru" }).toString(), "ru-Armn-AM");
shouldBe(new Intl.Locale("en", { region: "uk" }).toString(), "en-GB");
shouldBe(new Intl.Locale("fre", { region: "uk" }).toString(), "fr-GB");
shouldBe(new Intl.Locale("und", { language: "sh" }).toString(), "sr-Latn");
shouldBe(new Intl.Locale("sh", { region: "BA" }).toString(), "sr-Latn-BA");
shouldBe(new Intl.Locale("und", { script: "Qaai" }).toString(), "und-Zinh");
shouldBe(new Intl.Locale("ja-Latn", { variants: "hepburn-heploc" }).toString(), "ja-Latn-alalc97");
shouldBe(new Intl.Locale("fre", { calendar: "gregory" }).toString(), "fr-u-ca-gregory");

// What Intl.Locale derives from the identifier follows the canonical form.
{
    const serboCroatian = new Intl.Locale("sh");
    shouldBe(serboCroatian.language, "sr");
    shouldBe(serboCroatian.script, "Latn");
    shouldBe(serboCroatian.baseName, "sr-Latn");
    shouldBe(serboCroatian.maximize().toString(), "sr-Latn-RS");
    const unitedKingdom = new Intl.Locale("en-uk");
    shouldBe(unitedKingdom.region, "GB");
    shouldBe(unitedKingdom.baseName, "en-GB");
    shouldBe(unitedKingdom.maximize().toString(), "en-Latn-GB");
    shouldBe(new Intl.Locale("iw-Hebr-IL").minimize().toString(), "he");
}

// Every constructor, and every function that takes a locale, resolves the canonical form.
shouldBe(new Intl.NumberFormat("fre").resolvedOptions().locale, "fr");
shouldBe(new Intl.DateTimeFormat("iw").resolvedOptions().locale, "he");
shouldBe(new Intl.Collator("sh").resolvedOptions().locale, "sr-Latn");
shouldBe(new Intl.PluralRules("tl").resolvedOptions().locale, "fil");
shouldBe(new Intl.RelativeTimeFormat("mo").resolvedOptions().locale, "ro");
shouldBe(new Intl.ListFormat("fre-CA").resolvedOptions().locale, "fr-CA");
shouldBe(new Intl.DisplayNames("fre", { type: "region" }).resolvedOptions().locale, "fr");
shouldBe(new Intl.Segmenter("fre").resolvedOptions().locale, "fr");
shouldBe(new Intl.DurationFormat("fre").resolvedOptions().locale, "fr");
shouldBe(Intl.NumberFormat.supportedLocalesOf(["fre", "sh", "en-UK"]).join(), "fr,sr-Latn,en-GB");
shouldBe((1234.5).toLocaleString("fre"), (1234.5).toLocaleString("fr"));
shouldBe(new Date(0).toLocaleDateString("fre", { timeZone: "UTC", dateStyle: "full" }), new Date(0).toLocaleDateString("fr", { timeZone: "UTC", dateStyle: "full" }));
shouldBe("a".localeCompare("b", "fre"), "a".localeCompare("b", "fr"));

// A tag that ICU has no room for is refused as before.
{
    const variants = Array.from({ length: 26 }, (unused, index) => "variant" + String.fromCharCode(97 + index));
    const oversized = "fre-Latn-UK-" + variants.join("-");
    shouldThrow(() => Intl.getCanonicalLocales(oversized), RangeError);
    shouldThrow(() => new Intl.Locale(oversized), TypeError);
}
