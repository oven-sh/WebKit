//@ requireOptions("--useDollarVM=1")
//@ runDefault

// A program that asks an Intl service for a locale makes ICU list its locale IDs, and has ICU convert to a language
// tag only the IDs that are more than a language, a script and a region: en_US_POSIX, and no other in ICU 79. A
// program that asks for no locale does not make ICU list them. Upstream has ICU convert every locale ID of the list,
// about 900 of them, for the first service that is used, also when no locale is asked for, and does it a second time
// for the segmenter.
//
// $vm.intlLocaleIDsConverted() counts the locale IDs that ICU has converted for this.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

// What `ask` does has ICU convert this many locale IDs.
function expectConversions(label, count, ask)
{
    const before = $vm.intlLocaleIDsConverted();
    ask();
    expect(`${label}: locale IDs converted`, $vm.intlLocaleIDsConverted() - before, count);
}

// The first question of a list converts the few IDs that are more than a language, a script and a region.
function expectFirstQuestion(label, ask)
{
    const before = $vm.intlLocaleIDsConverted();
    ask();
    const converted = $vm.intlLocaleIDsConverted() - before;
    if (converted < 1 || converted > 8)
        throw new Error(`${label}: the first question converted ${converted} locale IDs`);
}

const services = [Intl.Collator, Intl.DateTimeFormat, Intl.DisplayNames, Intl.DurationFormat, Intl.ListFormat, Intl.NumberFormat, Intl.PluralRules, Intl.RelativeTimeFormat, Intl.Segmenter];

if ($vm.intlAvailableLocalesTable("locale").length < 500)
    throw new Error("the set of available locales is too small");
expect("after the set was made for the test", $vm.intlLocaleIDsConverted(), 0);

// No locale is asked for: nothing is looked up, and the answer is the default locale.
const defaultLocale = new Intl.NumberFormat().resolvedOptions().locale;
expectConversions("no requested locale", 0, () => {
    expect("NumberFormat", new Intl.NumberFormat(undefined, { style: "percent" }).resolvedOptions().locale, defaultLocale);
    expect("DateTimeFormat", new Intl.DateTimeFormat(undefined, { dateStyle: "short" }).resolvedOptions().locale, defaultLocale);
    expect("Collator", typeof new Intl.Collator().compare("a", "b"), "number");
    expect("Segmenter", new Intl.Segmenter([]).resolvedOptions().locale, defaultLocale);
    expect("toLocaleString", typeof (1234.5).toLocaleString(), "string");
    expect("toLocaleDateString", typeof new Date(0).toLocaleDateString(), "string");
    expect("localeCompare", "a".localeCompare("b"), -1);
    expect("Array toLocaleString", typeof [1, 2].toLocaleString(), "string");
    for (const service of services) {
        expect(`${service.name}.supportedLocalesOf()`, service.supportedLocalesOf().length, 0);
        expect(`${service.name}.supportedLocalesOf([])`, service.supportedLocalesOf([]).length, 0);
        let error;
        try {
            service.supportedLocalesOf("!!");
        } catch (thrown) {
            error = thrown;
        }
        expect(`${service.name}.supportedLocalesOf("!!")`, error instanceof RangeError, true);
    }
});

// The first locale that is asked for lists ICU's locale IDs.
expectFirstQuestion("locale", () => expect("supportedLocalesOf('en')", Intl.DateTimeFormat.supportedLocalesOf("en").join(), "en"));

// Every later question of that list is answered from what the first left: a tag that is there, a tag that is not,
// xx-ZZ for an xx-Yyyy-ZZ, and a list of tags.
expectConversions("later questions", 0, () => {
    expect("NumberFormat", new Intl.NumberFormat("en").resolvedOptions().locale, "en");
    expect("PluralRules", new Intl.PluralRules("en-US").resolvedOptions().locale, "en-US");
    expect("ListFormat", new Intl.ListFormat("de-AT").resolvedOptions().locale, "de-AT");
    expect("RelativeTimeFormat", new Intl.RelativeTimeFormat("pt-BR").resolvedOptions().locale, "pt-BR");
    expect("DisplayNames", new Intl.DisplayNames("es-419", { type: "region" }).resolvedOptions().locale, "es-419");
    expect("DurationFormat", new Intl.DurationFormat("ja-JP").resolvedOptions().locale, "ja-JP");
    expect("DateTimeFormat", new Intl.DateTimeFormat("fr-CA", { dateStyle: "short" }).resolvedOptions().locale, "fr-CA");
    expect("toLocaleString", (1234.5).toLocaleString("en"), "1,234.5");
    for (let i = 0; i < 3; ++i) {
        expect("en-XX", new Intl.NumberFormat("en-XX").resolvedOptions().locale, "en");
        expect("en-Zzzz-US", new Intl.NumberFormat("en-Zzzz-US").resolvedOptions().locale, "en");
        expect("xx", Intl.NumberFormat.supportedLocalesOf(["xx", "xx-YY", "qaa-Qaaa-QM", "en-XX-u-nu-latn"]).join(), "en-XX-u-nu-latn");
    }
    expect("zh-Hant-TW", new Intl.NumberFormat("zh-Hant-TW").resolvedOptions().locale, "zh-Hant-TW");
    expect("zh-TW", new Intl.NumberFormat("zh-TW").resolvedOptions().locale, "zh-TW");
    expect("sr-RS", new Intl.PluralRules("sr-RS").resolvedOptions().locale, "sr-RS");
    expect("pa-PK", new Intl.PluralRules("pa-PK").resolvedOptions().locale, "pa-PK");
    expect("zh-Hans-TW", new Intl.PluralRules("zh-Hans-TW").resolvedOptions().locale, "zh-Hans");
    expect("en-US-u-nu-latn", new Intl.NumberFormat("en-US-u-nu-latn").resolvedOptions().locale, "en-US-u-nu-latn");
    expect("en-US-x-private", Intl.PluralRules.supportedLocalesOf("en-US-x-private").join(), "en-US-x-private");
    expect("a list", Intl.ListFormat.supportedLocalesOf(["de-DE", "fr", "ja-JP-u-ca-japanese", "xx", "de"]).join(), "de-DE,fr,ja-JP-u-ca-japanese,de");
    expect("the first of a list that is available", new Intl.PluralRules(["xx", "pt-BR", "es-MX"]).resolvedOptions().locale, "pt-BR");
});

// The tag of en_US_POSIX is in the set. No requested locale reaches it: an extension is taken off a tag before it is
// looked up. en-US-POSIX, which is the ID with "-" for "_", is not in the set.
expectConversions("en-US-u-va-posix", 0, () => {
    expect("the tag of en_US_POSIX", $vm.intlIsAvailableLocale("locale", "en-US-u-va-posix"), true);
    expect("en-US-POSIX", $vm.intlIsAvailableLocale("locale", "en-US-POSIX"), false);
    expect("en-US-u-va-posix", new Intl.NumberFormat("en-US-u-va-posix").resolvedOptions().locale, "en-US");
});

// ICU's list for segmenters is its list of locales, string for string: the segmenter is answered from the same IDs,
// and ICU converts none a second time.
expectConversions("segmenter", 0, () => {
    expect("Segmenter: en", new Intl.Segmenter("en").resolvedOptions().locale, "en");
    expect("Segmenter: zh-TW", new Intl.Segmenter("zh-TW").resolvedOptions().locale, "zh-TW");
    expect("Segmenter: en-XX", Intl.Segmenter.supportedLocalesOf(["en", "en-XX", "xx"]).join(), "en,en-XX");
});

// The collator has a list of its own, with its own first question.
expectFirstQuestion("collator", () => expect("Collator: en", new Intl.Collator("en").resolvedOptions().locale, "en"));
expectConversions("collator: later questions", 0, () => {
    expect("Collator: zh-TW", new Intl.Collator("zh-TW").resolvedOptions().locale, "zh-TW");
    expect("Collator: en-XX", Intl.Collator.supportedLocalesOf(["en", "en-XX", "xx"]).join(), "en,en-XX");
    expect("localeCompare", "a".localeCompare("b", "sv"), -1);
});

// Each service asks the list that ICU has for it. ICU has collation data for few regions: en-GB and ja-JP are
// locales of the list of locales and of the segmenter's list, and not of the collator's.
expectConversions("each service and its list", 0, () => {
    for (const tag of ["en-GB", "ja-JP"]) {
        const language = tag.split("-")[0];
        expect(`NumberFormat: ${tag}`, new Intl.NumberFormat(tag).resolvedOptions().locale, tag);
        expect(`Segmenter: ${tag}`, new Intl.Segmenter(tag).resolvedOptions().locale, tag);
        expect(`Collator: ${tag}`, new Intl.Collator(tag).resolvedOptions().locale, language);
        expect(`Collator.supportedLocalesOf: ${tag}`, Intl.Collator.supportedLocalesOf(tag).join(), tag);
    }
});

// All of that is far from a list of 900.
if ($vm.intlLocaleIDsConverted() > 16)
    throw new Error(`${$vm.intlLocaleIDsConverted()} locale IDs were converted`);
