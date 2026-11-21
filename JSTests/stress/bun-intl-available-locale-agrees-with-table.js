//@ requireOptions("--useDollarVM=1")
//@ runDefault

// Bun answers whether a language tag is available to an Intl service from an index of the locale IDs of ICU's list
// as ICU spells them ("zh_Hant_TW"), with "-" for "_". ICU converts only the IDs that are more than a language, a
// script and a region. Upstream has ICU convert every ID of the list, and keeps the tags in a set. That set is what
// says which tags are available, and in Bun only this test and a build with assertions make it. This holds the
// index against the set: for every tag of the set, for other spellings of each, and for tags that are not in it.
//
// $vm.intlAvailableLocalesTable(list) is the set and $vm.intlIsAvailableLocale(list, tag) the answer of the index.
// The lists are ICU's: "locale" for every service but two, "collator" and "segmenter".

function otherCase(tag)
{
    let result = "";
    for (const character of tag)
        result += character === character.toLowerCase() ? character.toUpperCase() : character.toLowerCase();
    return result;
}

// Spellings that are near a tag. A tag of the set has the case that ICU gives it, and only that one is in the set.
function spellingsNear(tag)
{
    const subtags = tag.split("-");
    return [
        tag.toLowerCase(),
        tag.toUpperCase(),
        otherCase(tag),
        tag.replaceAll("-", "_"),
        tag.replaceAll("-", "--"),
        tag.replaceAll("-", ""),
        tag + "-",
        "-" + tag,
        tag + "_",
        tag + " ",
        " " + tag,
        tag + "\0",
        tag + "\0x",
        tag + "x",
        "x" + tag,
        tag.slice(1),
        tag.slice(0, -1),
        tag + "-XX",
        tag + "-Zzzz",
        tag + "-u-va-posix",
        tag + "-POSIX",
        tag + "-posix",
        tag + "-x-private",
        tag + "-t-en",
        tag + "-1996",
        subtags.slice(0, -1).join("-"),
        subtags.slice(1).join("-"),
        subtags.toReversed().join("-"),
        [subtags[0], "Zzzz", ...subtags.slice(1)].join("-"),
        [subtags[0], ...subtags.slice(2)].join("-"),
        [subtags[0], ...subtags.slice(1).map(subtag => subtag.length === 4 ? "Zzzz" : subtag)].join("-"),
        [subtags[0], ...subtags.slice(1).map(subtag => subtag.length === 2 ? "ZZ" : subtag)].join("-"),
        tag.replace("a", "\u0430"), // CYRILLIC SMALL LETTER A
        tag.replace("k", "\u212A"), // KELVIN SIGN, which lower-cases to "k" outside ASCII
        tag.replace("s", "\u017F"), // LATIN SMALL LETTER LONG S, which upper-cases to "S" outside ASCII
        tag.replace("i", "\u0131"), // LATIN SMALL LETTER DOTLESS I
        tag + "\uD800",
    ];
}

const otherTags = [
    "", " ", "\0", "-", "_", "a", "aa", "aaa", "aaaa", "aaaaaaaa", "aaaaaaaaa", "und", "und-US", "und-Latn-US", "root", "mul", "zxx",
    "en-US-POSIX", "en-US-posix", "en-US-u-va-posix", "en-u-va-posix", "en-POSIX", "en_US_POSIX", "en_US", "POSIX", "C",
    "iw", "iw-IL", "in", "in-ID", "ji", "mo", "jw", "no", "no-NO", "nb", "nb-NO", "nn", "nn-NO", "sh", "sh-YU", "tl", "tl-PH",
    "en-BU", "de-DD", "fr-FX", "pt-TP", "ar-YD", "fr-ZR", "sr-CS", "sr-YU", "ru-SU", "en-UK",
    "ja-JP-TRADITIONAL", "th-TH-TRADITIONAL", "de--PHONEBOOK", "es--TRADITIONAL", "ca-ES-VALENCIA", "ca-ES-valencia",
    "zh-GAN", "zh-MIN-NAN", "art-LOJBAN", "i-klingon", "en-GB-oed", "x-private", "en-x-private", "en-u-ca-gregory",
    "en-US@calendar=gregorian", "en-US.UTF-8", "en-Latn", "en-Latn-US", "en-Latn-US-x", "en-Zzzz", "en-Zzzz-US", "en-US-Latn",
    "zh-TW", "zh-HK", "zh-MO", "zh-CN", "zh-SG", "zh-Hant-CN", "zh-Hans-TW", "zh-Hant", "zh-Hans", "zh-Hant-TW", "zh-Hans-CN",
    "sr-RS", "sr-ME", "sr-BA", "sr-XK", "sr-Latn-RS", "sr-Cyrl-RS", "pa-PK", "pa-IN", "pa-Arab-PK", "pa-Guru-IN", "uz-AF",
    "az-AZ", "bs-BA", "ks-IN", "kok-IN", "yue-CN", "yue-HK", "shi-MA", "vai-LR", "su-ID", "mn-CN", "kk-CN", "ha-NE", "ff-GN",
    "es-419", "en-001", "en-150", "ar-001", "en-419", "es-001", "en-0001", "en-01", "en-1", "en-99", "en-999", "en-A1", "en-1A",
    "en-Latn-001", "en-Dsrt-US", "en-Shaw-GB", "en-US-US", "en-US-001", "en-001-US", "e", "e-US", "engl-US", "english",
    "a".repeat(110), "a".repeat(255), "a".repeat(256), "a".repeat(65536), "en-".repeat(80) + "US", "\uFF45\uFF4E", "en\uFEFF", "\uD83D\uDE00",
];

// ICU has collation data for far fewer locales than it lists, and its list for segmenters is its list of locales.
if ($vm.intlAvailableLocalesTable("collator").length * 2 > $vm.intlAvailableLocalesTable("locale").length)
    throw new Error("the collator's set is not the set of ICU's list of collators");
if ($vm.intlAvailableLocalesTable("segmenter").toSorted().join() !== $vm.intlAvailableLocalesTable("locale").toSorted().join())
    throw new Error("the segmenter's set is not the set of ICU's list of locales");

let compared = 0;

for (const list of ["locale", "collator", "segmenter"]) {
    const table = $vm.intlAvailableLocalesTable(list);
    if (table.length < 100)
        throw new Error(`${list}: the set has only ${table.length} tags`);
    const inTable = new Set(table);
    if (inTable.size !== table.length)
        throw new Error(`${list}: a tag is in the set twice`);
    for (const expected of ["en", "en-US", "de", "fr", "ja", "zh", "zh-Hant", "zh-TW", "sr-Latn"]) {
        if (!inTable.has(expected))
            throw new Error(`${list}: the set has no ${expected}`);
    }

    function check(tag)
    {
        const bySet = inTable.has(tag);
        const byIndex = $vm.intlIsAvailableLocale(list, tag);
        if (bySet !== byIndex)
            throw new Error(`${list}, ${JSON.stringify(tag)}: the set says ${bySet}, the index says ${byIndex}`);
        ++compared;
    }

    for (const tag of otherTags) {
        check(tag);
        for (const spelling of spellingsNear(tag))
            check(spelling);
    }
    for (const tag of table) {
        check(tag);
        for (const spelling of spellingsNear(tag))
            check(spelling);
    }

    // The set has xx-ZZ for every xx-Yyyy-ZZ that ICU has, and for nothing else: each language that the set has with
    // a script, with every script and every region of the set.
    const scripts = new Set, regions = new Set, languagesWithScript = new Set;
    for (const tag of table) {
        const subtags = tag.split("-");
        for (const subtag of subtags.slice(1)) {
            if (/^[A-Z][a-z]{3}$/.test(subtag)) {
                scripts.add(subtag);
                languagesWithScript.add(subtags[0]);
            } else if (/^([A-Z]{2}|[0-9]{3})$/.test(subtag))
                regions.add(subtag);
        }
    }
    if (languagesWithScript.size < 5 || scripts.size < 2 || regions.size < 10)
        throw new Error(`${list}: ${languagesWithScript.size} languages with a script, ${scripts.size} scripts, ${regions.size} regions`);
    for (const language of languagesWithScript) {
        for (const region of regions) {
            check(language + "-" + region);
            check(language + "-Zzzz-" + region);
        }
        for (const script of scripts) {
            check(language + "-" + script);
            for (const region of ["CN", "TW", "RS", "IN", "US", "001", "XX"])
                check(language + "-" + script + "-" + region);
        }
    }
}

if (compared < 100000)
    throw new Error(`only ${compared} tags were compared`);
