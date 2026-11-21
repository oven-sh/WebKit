//@ requireOptions("--useDollarVM=1")
//@ runDefault

// The set of the available locales of an Intl service has the language tag of each locale ID of one of ICU's lists.
// For an ID that is a language, a script and a region ("en", "es_419", "zh_Hant_TW") the tag is the ID with "-" for
// "_", and ICU is not asked. ICU is asked about every other ID ("en_US_POSIX"). Upstream asks ICU about every ID.
//
// $vm.intlAvailableLocaleIDsWithAnotherTag() asks ICU about every such ID of its three lists and returns the IDs for
// which ICU's tag is another one. $vm.intlLocaleIDsOfListsConvertedByICU() counts the IDs that ICU has converted
// for a set.

const withAnotherTag = [...new Set($vm.intlAvailableLocaleIDsWithAnotherTag())];
if (withAnotherTag.length)
    throw new Error(`ICU converts these locale IDs of its lists to another tag than the ID with "-" for "_": ${withAnotherTag.join(", ")}`);

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

// To make a set, ICU converts the few IDs that are more than a language, a script and a region: en_US_POSIX in ICU 79.
function expectFewConversions(label, use)
{
    const before = $vm.intlLocaleIDsOfListsConvertedByICU();
    use();
    const converted = $vm.intlLocaleIDsOfListsConvertedByICU() - before;
    if (converted > 8)
        throw new Error(`${label}: ICU converted ${converted} locale IDs of the list, expected 8 or fewer`);
}

expectFewConversions("the list of locales", () => expect("en", Intl.DateTimeFormat.supportedLocalesOf("en").join(), "en"));
expectFewConversions("the collator's list", () => expect("de", Intl.Collator.supportedLocalesOf("de").join(), "de"));

// The tags are the ones ICU gives: with a script, with a region of digits, xx-ZZ for xx-Yyyy-ZZ, and en-US-u-va-posix.
expect("tags", Intl.NumberFormat.supportedLocalesOf(["zh-Hant-TW", "zh-TW", "es-419", "sr-Latn-RS", "sr-RS", "yue-Hant-HK", "en-US-u-va-posix", "xx"]).join(), "zh-Hant-TW,zh-TW,es-419,sr-Latn-RS,sr-RS,yue-Hant-HK,en-US-u-va-posix");
expect("en-US-u-va-posix", new Intl.NumberFormat("en-US-u-va-posix").resolvedOptions().locale, "en-US");
