function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldThrow(run, errorType, message) {
    let error;
    try {
        run();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`${message}: expected a ${errorType.name} but got ${error}`);
}

function collations(tag, options) {
    return JSON.stringify(new Intl.Locale(tag, options).getCollations());
}

function numberingSystems(tag, options) {
    return JSON.stringify(new Intl.Locale(tag, options).getNumberingSystems());
}

const rootCollations = `["emoji","eor"]`;
const latn = `["latn"]`;

// The locale that Intl.Collator and Intl.NumberFormat find for a tag with the lookup matcher, which is ECMA-402's
// LookupMatchingLocaleByPrefix. Undefined when no prefix of the tag is an available locale of the service.
function matchOf(service, tag) {
    if (!service.supportedLocalesOf(tag, { localeMatcher: "lookup" }).length)
        return undefined;
    return new Intl.Locale(new service(tag, { localeMatcher: "lookup" }).resolvedOptions().locale).baseName;
}

// CollationsOfLocale and NumberingSystemsOfLocale, from what the two services say.
function shouldAnswerAsTheServices(tag) {
    let locale = new Intl.Locale(tag);

    let collatorMatch = matchOf(Intl.Collator, locale.baseName);
    let expectedCollations = collatorMatch === undefined ? rootCollations : collations(collatorMatch);
    shouldBe(JSON.stringify(locale.getCollations()), expectedCollations, `getCollations() of ${tag}, which Intl.Collator matches with ${collatorMatch}`);
    if (collatorMatch !== undefined) {
        for (let collation of locale.getCollations())
            shouldBe(new Intl.Collator(tag, { collation }).resolvedOptions().collation, collation, `Intl.Collator for ${tag} with a collation from getCollations()`);
    }

    let numberFormatMatch = matchOf(Intl.NumberFormat, locale.baseName);
    let expectedNumberingSystems = numberFormatMatch === undefined ? latn : JSON.stringify([new Intl.NumberFormat(numberFormatMatch).resolvedOptions().numberingSystem]);
    shouldBe(JSON.stringify(locale.getNumberingSystems()), expectedNumberingSystems, `getNumberingSystems() of ${tag}, which Intl.NumberFormat matches with ${numberFormatMatch}`);
}

// No prefix of these tags is a locale of the services on any ICU, so the answer is the one that ECMA-402 writes out.
// ICU has no data for them, and answers from the data of its default locale, which comes from the host.
for (let tag of ["xx", "xx-XX", "xx-Latn-XX", "tlh", "qfz", "qga-DE", "qtz-CN", "abcdefgh", "abcdefgh-Arab-EG", "und", "und-US", "und-Latn", "und-Latn-US", "und-BD", "und-SA", "und-419", "xx-fonipa", "und-x-private"]) {
    shouldBe(Intl.Collator.supportedLocalesOf(tag).length, 0, `Intl.Collator has no locale for ${tag}`);
    shouldBe(Intl.NumberFormat.supportedLocalesOf(tag).length, 0, `Intl.NumberFormat has no locale for ${tag}`);
    shouldBe(collations(tag), rootCollations, `getCollations() of ${tag}`);
    shouldBe(numberingSystems(tag), latn, `getNumberingSystems() of ${tag}`);
    shouldAnswerAsTheServices(new Intl.Locale(tag).maximize().toString());
}

// A prefix of these tags is a locale of the services, and ICU has no data for the tag as a whole. ICU looks no
// further than the tag where its script is not the likely script of the language.
shouldBe(collations("de-Cyrl"), `["emoji","eor","phonebk"]`, "getCollations() of de-Cyrl");
shouldBe(collations("de-Arab-XX"), `["emoji","eor","phonebk"]`, "getCollations() of de-Arab-XX");
shouldBe(collations("es-Arab"), `["emoji","eor","trad"]`, "getCollations() of es-Arab");
shouldBe(collations("en-Arab"), rootCollations, "getCollations() of en-Arab");
shouldBe(numberingSystems("fa-Latn"), `["arabext"]`, "getNumberingSystems() of fa-Latn");
shouldBe(numberingSystems("bn-Latn"), `["beng"]`, "getNumberingSystems() of bn-Latn");
shouldBe(numberingSystems("en-Arab"), latn, "getNumberingSystems() of en-Arab");
shouldBe(numberingSystems("de-Cyrl"), latn, "getNumberingSystems() of de-Cyrl");

// Tags with data of their own, with a prefix that has data, and without data. Some have data only as an alias in
// ICU ("ars", "yue") or only without their likely script ("ar-Arab-EG"): the services do not know those locales, so
// neither do the getters.
const tags = [
    "en", "en-US", "en-GB", "en-001", "en-150", "en-JP", "en-Latn-US", "en-Dsrt", "de", "de-DE", "de-AT", "de-Latn-DE", "de-Cyrl",
    "es", "es-ES", "es-419", "es-MX", "es-Arab", "fr-CA", "ja", "ja-JP", "ko-KR", "sv", "sv-FI", "fi", "ln", "kn", "si", "vi", "bn", "bn-Latn",
    "ar", "ar-EG", "ar-SA", "ar-AE", "ar-001", "ar-Arab-EG", "ar-Latn", "ar-Latn-EG", "fa", "fa-AF", "fa-Latn", "ur-IN", "ur-Arab-IN", "ks", "ckb", "ps",
    "zh", "zh-CN", "zh-TW", "zh-HK", "zh-Hans", "zh-Hant", "zh-Hant-TW", "zh-Hans-SG", "zh-Latn", "yue", "yue-HK", "yue-Hans-CN",
    "sr", "sr-RS", "sr-Latn", "sr-Latn-BA", "sr-Cyrl-ME", "pa", "pa-PK", "pa-Arab", "pa-Guru-IN", "uz-Arab", "uz-AF", "az-Cyrl", "az-AZ", "ff-Adlm", "ff-CM",
    "mni", "mni-Mtei", "sat", "sat-Deva", "ars", "ars-SA", "nb", "no", "nn", "fil", "haw", "eu", "eu-ES", "ga", "cy", "la", "cnr", "tok", "ie",
    "hi", "mr", "ne", "th", "my", "my-MM", "dz", "km", "lo", "ta", "te",
];
for (let tag of tags) {
    shouldAnswerAsTheServices(tag);
    let locale = new Intl.Locale(tag);
    shouldAnswerAsTheServices(locale.maximize().toString());
    shouldAnswerAsTheServices(locale.minimize().toString());
}

// Variants are not part of a locale of the services, apart from en-US-u-va-posix.
for (let [tag, without] of [["ca-ES-valencia", "ca-ES"], ["ca-valencia", "ca"], ["de-1996", "de"], ["de-DE-1901", "de-DE"], ["sl-rozaj-biske-1994", "sl"], ["ar-EG-fonipa", "ar-EG"], ["fa-Latn-fonipa", "fa"], ["xx-fonipa", "xx"]]) {
    shouldBe(collations(tag), collations(without), `getCollations() of ${tag} and ${without}`);
    shouldBe(numberingSystems(tag), numberingSystems(without), `getNumberingSystems() of ${tag} and ${without}`);
    shouldAnswerAsTheServices(tag);
}
shouldAnswerAsTheServices("en-US-posix");
shouldAnswerAsTheServices("en-US-u-va-posix");
shouldAnswerAsTheServices("en-posix");

// Extensions other than -u-co and -u-nu change nothing, and neither does what follows -x-.
for (let [tag, without] of [["de-u-ca-buddhist", "de"], ["ar-EG-u-ca-islamic-hc-h23", "ar-EG"], ["de-Cyrl-u-kn-kf-upper", "de-Cyrl"], ["fa-Latn-t-fa-arab", "fa-Latn"], ["de-x-private", "de"], ["xx-u-ca-gregory-x-de-phonebk", "xx"], ["und-u-ca-gregory", "und"], ["ar-EG-a-bbb-u-fw-mon-x-ccc", "ar-EG"]]) {
    shouldBe(collations(tag), collations(without), `getCollations() of ${tag} and ${without}`);
    shouldBe(numberingSystems(tag), numberingSystems(without), `getNumberingSystems() of ${tag} and ${without}`);
}

// The tag of the locale decides, not how the locale was written: ICU keeps the language and the region as written
// ("iw", "DD"), and the tag has their replacements ("he", "DE").
for (let tag of ["iw", "iw-IL", "in", "in-ID", "ji", "jw", "mo", "mo-MD", "de-DD", "fr-FX", "pt-TP", "my-BU", "ar-YD", "fr-ZR", "ln-ZR", "iw-Arab-YD"]) {
    let canonical = new Intl.Locale(tag).toString();
    if (canonical === tag)
        throw new Error(`${tag} should not be its own canonical form`);
    shouldBe(collations(tag), collations(canonical), `getCollations() of ${tag} and ${canonical}`);
    shouldBe(numberingSystems(tag), numberingSystems(canonical), `getNumberingSystems() of ${tag} and ${canonical}`);
    shouldAnswerAsTheServices(tag);
}
{
    const letters = "abcdefghijklmnopqrstuvwxyz";
    for (let first of letters) {
        for (let second of letters) {
            let region = (first + second).toUpperCase();
            for (let language of ["ar", "my", "fr"]) {
                let tag = `${language}-${region}`;
                let canonical = new Intl.Locale(tag).toString();
                shouldBe(numberingSystems(tag), numberingSystems(canonical), `getNumberingSystems() of ${tag} and ${canonical}`);
                shouldBe(collations(tag), collations(canonical), `getCollations() of ${tag} and ${canonical}`);
            }
            let tag = `${first}${second}-SA`;
            let canonical = new Intl.Locale(tag).toString();
            shouldBe(numberingSystems(tag), numberingSystems(canonical), `getNumberingSystems() of ${tag} and ${canonical}`);
            shouldBe(collations(tag), collations(canonical), `getCollations() of ${tag} and ${canonical}`);
        }
    }
    shouldAnswerAsTheServices("ar-YE");
    shouldAnswerAsTheServices("ar-XX");
    shouldAnswerAsTheServices("my-MM");
}

// -u-co and -u-nu are the answer, whatever the rest of the tag is. The value "true" is the empty string.
// A long tag, and a long tag that ICU writes in another way, take the same path as a short one.
{
    let variants = Array.from({ length: 12 }, (_, index) => `v${1000000 + index}`).join("-");
    shouldBe(collations(`de-${variants}`), collations("de"), "getCollations() of de with 12 variants");
    shouldBe(numberingSystems(`ar-EG-${variants}`), numberingSystems("ar-EG"), "getNumberingSystems() of ar-EG with 12 variants");
    shouldBe(numberingSystems(`ar-YD-${variants}`), numberingSystems("ar-YE"), "getNumberingSystems() of ar-YD with 12 variants");
    shouldBe(collations(`xx-${variants}`), rootCollations, "getCollations() of xx with 12 variants");
    shouldBe(numberingSystems(`abcdefgh-Arab-EG-${variants}-u-ca-gregory-x-${variants}`), latn, "getNumberingSystems() of a tag of 250 characters");
}

shouldBe(collations("xx-u-co-phonebk"), `["phonebk"]`, "getCollations() of xx-u-co-phonebk");
shouldBe(collations("de-u-co-phonebk"), `["phonebk"]`, "getCollations() of de-u-co-phonebk");
shouldBe(collations("abcdefgh-u-co-phonebk"), `["phonebk"]`, "getCollations() of abcdefgh-u-co-phonebk");
shouldBe(collations("xx", { collation: "trad" }), `["trad"]`, "getCollations() of xx with collation trad");
shouldBe(numberingSystems("xx-u-nu-thai"), `["thai"]`, "getNumberingSystems() of xx-u-nu-thai");
shouldBe(numberingSystems("ar-EG-u-nu-latn"), latn, "getNumberingSystems() of ar-EG-u-nu-latn");
shouldBe(numberingSystems("abcdefgh-u-nu-arab"), `["arab"]`, "getNumberingSystems() of abcdefgh-u-nu-arab");
shouldBe(numberingSystems("xx", { numberingSystem: "arab" }), `["arab"]`, "getNumberingSystems() of xx with numberingSystem arab");
for (let tag of ["und", "en", "xx", "ar-EG", "de-Cyrl"]) {
    shouldBe(new Intl.Locale(`${tag}-u-co-true`).collation, "", `collation of ${tag}-u-co-true`);
    shouldBe(collations(`${tag}-u-co-true`), `[""]`, `getCollations() of ${tag}-u-co-true`);
    shouldBe(numberingSystems(`${tag}-u-co-true`), numberingSystems(tag), `getNumberingSystems() of ${tag}-u-co-true`);
    shouldBe(new Intl.Locale(`${tag}-u-nu-true`).numberingSystem, "", `numberingSystem of ${tag}-u-nu-true`);
    shouldBe(numberingSystems(`${tag}-u-nu-true`), `[""]`, `getNumberingSystems() of ${tag}-u-nu-true`);
    shouldBe(collations(`${tag}-u-nu-true`), collations(tag), `getCollations() of ${tag}-u-nu-true`);
}

// The same first step in the two other getters that have it.
for (let tag of ["en", "xx", "ar-EG"]) {
    shouldBe(new Intl.Locale(`${tag}-u-ca-true`).calendar, "", `calendar of ${tag}-u-ca-true`);
    shouldBe(JSON.stringify(new Intl.Locale(`${tag}-u-ca-true`).getCalendars()), `[""]`, `getCalendars() of ${tag}-u-ca-true`);
    shouldBe(new Intl.Locale(`${tag}-u-hc-true`).hourCycle, "", `hourCycle of ${tag}-u-hc-true`);
    shouldBe(JSON.stringify(new Intl.Locale(`${tag}-u-hc-true`).getHourCycles()), `[""]`, `getHourCycles() of ${tag}-u-hc-true`);
    shouldBe(JSON.stringify(new Intl.Locale(`${tag}-u-ca-buddhist`).getCalendars()), `["buddhist"]`, `getCalendars() of ${tag}-u-ca-buddhist`);
    shouldBe(JSON.stringify(new Intl.Locale(`${tag}-u-hc-h23`).getHourCycles()), `["h23"]`, `getHourCycles() of ${tag}-u-hc-h23`);
}

// The options of the constructor, and a Locale made from a Locale.
shouldBe(collations("en", { language: "de", script: "Cyrl" }), collations("de-Cyrl"), "en with language de and script Cyrl");
shouldBe(numberingSystems("en", { language: "fa", script: "Latn" }), numberingSystems("fa-Latn"), "en with language fa and script Latn");
shouldBe(numberingSystems("ar", { region: "EG" }), numberingSystems("ar-EG"), "ar with region EG");
shouldBe(numberingSystems("ar-EG", { language: "xx" }), latn, "ar-EG with language xx");
shouldBe(collations(new Intl.Locale("de-Cyrl")), collations("de-Cyrl"), "a Locale of de-Cyrl");
shouldBe(numberingSystems(new Intl.Locale("xx"), { region: "EG" }), latn, "a Locale of xx with region EG");

// Each call makes a new array, and the receiver has to be an Intl.Locale.
for (let tag of ["en", "xx", "de-Cyrl", "ar-EG", "xx-u-co-phonebk-nu-thai"]) {
    let locale = new Intl.Locale(tag);
    for (let method of ["getCollations", "getNumberingSystems"]) {
        let first = locale[method]();
        let second = locale[method]();
        shouldBe(Array.isArray(first), true, `${method}() of ${tag} is an array`);
        shouldBe(Object.getPrototypeOf(first), Array.prototype, `prototype of ${method}() of ${tag}`);
        shouldBe(first === second, false, `${method}() of ${tag} makes a new array`);
        shouldBe(JSON.stringify(first), JSON.stringify(second), `the second ${method}() of ${tag}`);
        first.length = 0;
        shouldBe(JSON.stringify(locale[method]()), JSON.stringify(second), `${method}() of ${tag} after a change to its result`);
    }
}
for (let method of ["getCollations", "getNumberingSystems"]) {
    for (let [name, receiver] of [["undefined", undefined], ["null", null], ["a string", "en"], ["an object", {}], ["Intl.Locale.prototype", Intl.Locale.prototype], ["an Intl.Collator", new Intl.Collator("en")]])
        shouldThrow(() => Intl.Locale.prototype[method].call(receiver), TypeError, `${method}() on ${name}`);
}

// The assertions of test262's intl402/Locale/prototype/getCollations and getNumberingSystems that the vendored
// test262 does not have yet, by file.
{
    // getCollations/und-language.js
    for (let tag of ["und", "und-US", "und-Latn", "und-Latn-US", "und-u-ca-gregory", "und-US-u-nu-latn"]) {
        shouldBe(new Intl.Locale(tag).language, "und", `language of ${tag}`);
        shouldBe(collations(tag), rootCollations, `getCollations() of ${tag}`);
    }
    for (let tag of ["qfz", "qga-DE", "qgb-ES", "qgc-KR", "qtz-CN"])
        shouldBe(collations(tag), rootCollations, `getCollations() of ${tag}`);

    // getCollations/language-subtag-with-more-than-three-letters.js
    shouldBe(collations("abcdefgh"), rootCollations, "getCollations() of abcdefgh");
    shouldBe(collations("abcdefgh-u-co-phonebk"), `["phonebk"]`, "getCollations() of abcdefgh-u-co-phonebk");

    // getCollations/collations-supported-by-collator.js
    for (let tag of ["en", "ar", "bn", "de", "es", "fi", "ja", "kn", "ko", "ln", "si", "sv", "vi", "zh"]) {
        if (!Intl.Collator.supportedLocalesOf(tag).length)
            continue;
        for (let collation of new Intl.Locale(tag).getCollations())
            shouldBe(new Intl.Collator(tag, { collation }).resolvedOptions().collation, collation, `Intl.Collator for ${tag} with ${collation}`);
    }

    // getCollations/preferred-from-unicode-extension-true-empty.js and its getNumberingSystems twin, without their
    // rows for a key that has no value.
    for (let tag of ["und-u-co-true", "en-u-co-true"])
        shouldBe(collations(tag), `[""]`, `getCollations() of ${tag}`);
    for (let tag of ["und-u-nu-true", "en-u-nu-true"])
        shouldBe(numberingSystems(tag), `[""]`, `getNumberingSystems() of ${tag}`);

    // getNumberingSystems/unsupported-defaults-to-latn.js
    for (let tag of ["und", "tlh", "qfz"]) {
        if (Intl.NumberFormat.supportedLocalesOf(tag).length)
            continue;
        shouldBe(numberingSystems(tag), latn, `getNumberingSystems() of ${tag}`);
    }

    // getNumberingSystems/lookup-locale-by-prefix.js
    for (let tag of ["und-BD", "und-SA"]) {
        if (Intl.NumberFormat.supportedLocalesOf(tag, { localeMatcher: "lookup" }).length)
            continue;
        shouldBe(numberingSystems(tag), latn, `getNumberingSystems() of ${tag}`);
    }

    // getNumberingSystems/language-subtag-with-more-than-three-letters.js
    shouldBe(numberingSystems("abcdefgh"), latn, "getNumberingSystems() of abcdefgh");
    shouldBe(numberingSystems("abcdefgh-u-nu-arab"), `["arab"]`, "getNumberingSystems() of abcdefgh-u-nu-arab");
}
