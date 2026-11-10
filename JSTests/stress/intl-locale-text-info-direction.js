function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function direction(tag, options) {
    return new Intl.Locale(tag, options).getTextInfo().direction;
}

function shouldHaveDirection(tags, expected) {
    for (let tag of tags)
        shouldBe(direction(tag), expected, tag);
}

// The script of the tag decides, whatever the language is.
shouldHaveDirection(["und-Arab", "und-Hebr", "und-Thaa", "und-Syrc", "und-Nkoo", "und-Adlm", "und-Mand", "und-Samr", "und-Rohg", "und-Mend", "und-Avst", "und-Phnx"], "rtl");
shouldHaveDirection(["en-Arab", "abcde-Arab", "ku-Arab", "az-Arab", "ms-Arab", "ha-Arab", "tr-Arab", "ky-Arab", "tg-Arab", "man-Nkoo", "ff-Adlm", "uz-Arab", "kk-Arab", "pa-Arab"], "rtl");
shouldHaveDirection(["und-Latn", "ar-Latn", "he-Latn", "sd-Deva", "ks-Deva", "und-Cyrl", "und-Grek", "und-Deva", "und-Thai", "und-Hans", "und-Hant", "und-Jpan", "und-Kore", "und-Hira", "und-Kana", "und-Mong", "mn-Mong"], "ltr");

// A tag without a script takes the script that Add Likely Subtags gives it.
shouldHaveDirection(["ar", "he", "fa", "ur", "ps", "sd", "ug", "yi", "ckb", "ks", "syr", "nqo", "lrc", "mzn"], "rtl");
shouldHaveDirection(["dv", "dv-MV", "arc", "glk", "bal", "bgn", "prd", "sdh", "rhg"], "rtl");
shouldHaveDirection(["en", "es", "pt", "zh", "ja", "ko", "de", "fr", "it", "ru", "nl", "pl", "th", "tr", "sv", "hi", "el", "la", "ku", "az", "pa", "und"], "ltr");

// The region is enough where it changes the likely script.
shouldHaveDirection(["und-MV", "und-IL", "und-IR", "und-PK", "und-AF", "und-EG", "ku-IQ", "az-IR", "az-IQ", "ha-SD", "pa-PK", "uz-AF"], "rtl");
shouldHaveDirection(["und-US", "en-US", "ar-Latn-EG", "ku-TR", "az-AZ", "pa-IN", "sd-IN", "zh-TW", "en-IL", "fa-Latn-IR"], "ltr");

// Variants and keywords change nothing.
shouldHaveDirection(["dv-u-nu-latn", "ar-u-nu-latn", "und-Arab-u-ca-gregory", "az-IR-u-nu-arab-hc-h23", "ar-EG-fonipa", "dv-fonipa-u-nu-latn-x-private", "sl-Arab-rozaj-biske-1994"], "rtl");
shouldHaveDirection(["en-u-ca-hebrew", "en-u-nu-arab", "de-1996-u-co-phonebk", "ar-Latn-u-nu-arab", "sl-rozaj-biske-1994"], "ltr");

// The direction is not known: no likely script, a script without a direction in CLDR's scriptMetadata.txt, or no
// registered script.
shouldHaveDirection(["xx", "tlh", "qfz", "abcdefgh", "xx-ZZ"], undefined);
shouldHaveDirection(["und-Zyyy", "und-Zinh", "und-Zzzz", "und-Brai", "en-Zzzz", "ar-Zyyy"], undefined);
shouldHaveDirection(["ar-Zxxx", "ur-Aran", "und-Zsym", "und-Zmth", "und-Zsye", "und-Hrkt", "und-Latf", "und-Syre"], undefined);
shouldHaveDirection(["und-Aaaa", "und-Qaaq", "und-True", "und-Qaai", "und-Qaac", "und-Miao"], undefined);

// Deprecated language codes are the ones that replace them.
shouldHaveDirection(["iw", "ji"], "rtl");
shouldHaveDirection(["in"], "ltr");

// The options of the constructor, and a Locale made from a Locale.
shouldBe(direction("en", { script: "Arab" }), "rtl", "en with script Arab");
shouldBe(direction("ar", { script: "Latn" }), "ltr", "ar with script Latn");
shouldBe(direction("az", { region: "IR" }), "rtl", "az with region IR");
shouldBe(direction("und", { language: "dv" }), "rtl", "und with language dv");
shouldBe(direction("ar", { language: "en" }), "ltr", "ar with language en");
shouldBe(direction(new Intl.Locale("und-Arab")), "rtl", "a Locale of und-Arab");
shouldBe(direction(new Intl.Locale("dv"), { numberingSystem: "latn" }), "rtl", "a Locale of dv");

// maximize() and minimize() keep the direction.
for (let tag of ["und-Arab", "und-IL", "ku-IQ", "dv", "az-IR", "en", "und", "pa", "pa-PK", "sd-IN", "xx", "tlh", "ur-Aran"]) {
    let locale = new Intl.Locale(tag);
    shouldBe(locale.maximize().getTextInfo().direction, locale.getTextInfo().direction, `maximize() of ${tag}`);
    shouldBe(locale.minimize().getTextInfo().direction, locale.getTextInfo().direction, `minimize() of ${tag}`);
}

// Zzzz in the tag is a script, and its direction is not known. Add Likely Subtags takes Zzzz for no script.
shouldBe(direction("en-Zzzz"), undefined, "en-Zzzz");
shouldBe(new Intl.Locale("en-Zzzz").maximize().toString(), "en-Latn-US", "maximize() of en-Zzzz");
shouldBe(new Intl.Locale("en-Zzzz").maximize().getTextInfo().direction, "ltr", "the maximized en-Zzzz");

// The object: one own data property, also where the direction is not known. Each call makes a new object.
for (let tag of ["en", "ar", "dv", "xx", "en-Zzzz"]) {
    let locale = new Intl.Locale(tag);
    let info = locale.getTextInfo();
    shouldBe(Object.getPrototypeOf(info), Object.prototype, `prototype for ${tag}`);
    shouldBe(JSON.stringify(Reflect.ownKeys(info)), `["direction"]`, `own keys for ${tag}`);
    let descriptor = Object.getOwnPropertyDescriptor(info, "direction");
    shouldBe(descriptor.writable, true, `writable for ${tag}`);
    shouldBe(descriptor.enumerable, true, `enumerable for ${tag}`);
    shouldBe(descriptor.configurable, true, `configurable for ${tag}`);
    shouldBe(descriptor.value, info.direction, `value for ${tag}`);
    shouldBe(locale.getTextInfo() === info, false, `a new object for ${tag}`);
    shouldBe(locale.getTextInfo().direction, info.direction, `the second call for ${tag}`);
}
shouldBe(JSON.stringify(new Intl.Locale("dv").getTextInfo()), `{"direction":"rtl"}`, "dv as JSON");
shouldBe(JSON.stringify(new Intl.Locale("xx").getTextInfo()), `{}`, "xx as JSON");

// The assertions of test262's intl402/Locale/prototype/getTextInfo (tc39/test262#5023), by file.
{
    const languages = ["und", "en", "ar", "tlh", "qfz"];

    // language-subtag-with-more-than-three-letters.js
    shouldBe(direction("abcdefgh"), undefined, "abcdefgh");
    shouldBe(direction("abcdefgh-Latn"), "ltr", "abcdefgh-Latn");
    shouldBe(direction("abcdefgh-Arab"), "rtl", "abcdefgh-Arab");
    shouldBe(direction("abcdefgh-Zzzz"), undefined, "abcdefgh-Zzzz");

    // script-metadata-rtl-is-unknown.js
    for (let script of ["Zyyy", "Zinh", "Zzzz", "Brai"]) {
        for (let language of languages)
            shouldBe(direction(`${language}-${script}`), undefined, `${language}-${script}`);
    }

    // script-metadata-unlisted-language-script-is-rtl.js accepts "rtl" and undefined. This engine gives undefined.
    shouldBe(direction("ar-Zxxx"), undefined, "ar-Zxxx");
    shouldBe(direction("ur-Aran"), undefined, "ur-Aran");

    // script-subtag-missing.js
    for (let tag of ["und", "en", "ar", "tlh", "qfz", "pa", "pa-PK"]) {
        let locale = new Intl.Locale(tag);
        let maximized = locale.maximize();
        shouldBe(locale.getTextInfo().direction, maximized.getTextInfo().direction, `${tag} and its maximized locale`);
        if (maximized.script === undefined)
            shouldBe(locale.getTextInfo().direction, undefined, `${tag} has no likely script`);
    }
    shouldBe(new Intl.Locale("tlh").maximize().script, undefined, "tlh has no likely script");
    shouldBe(new Intl.Locale("qfz").maximize().script, undefined, "qfz has no likely script");
    shouldBe(direction("pa"), "ltr", "pa");
    shouldBe(direction("pa-PK"), "rtl", "pa-PK");

    // script-subtag-present.js
    for (let [script, expected] of [["Latn", "ltr"], ["Arab", "rtl"]]) {
        for (let language of languages)
            shouldBe(direction(`${language}-${script}`), expected, `${language}-${script}`);
    }

    // script-unregistered-or-privateuse.js
    for (let script of ["Aaaa", "Qaaq", "True"]) {
        for (let language of languages)
            shouldBe(direction(`${language}-${script}`), undefined, `${language}-${script}`);
    }

    // output-object-keys.js
    let result = new Intl.Locale("en").getTextInfo();
    shouldBe(JSON.stringify(Reflect.ownKeys(result)), `["direction"]`, "own keys of en");
    shouldBe(result.direction, "ltr", "en");
}
