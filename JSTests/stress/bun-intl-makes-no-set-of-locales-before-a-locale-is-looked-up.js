//@ requireOptions("--useDollarVM=1")
//@ runDefault

// The set of the language tags of ICU's locales is made when an Intl service first looks a locale up in it, and the
// segmenter has the set of the other services where ICU has one list of locale IDs for both. Upstream makes the set
// for the first service that is used, also when no locale is requested, and a second one for the segmenter. To make
// a set, every locale ID of one of ICU's lists is converted to a language tag: 906 of them with ICU 79, and 146 for
// the collator.
//
// $vm.intlLocaleIDsPutIntoSets() counts the locale IDs that this process has put into a set.
// $vm.intlLookupsThatFoundNoSet() counts the lookups that found no set in the available locales of their service:
// one for each list, however many locales are looked up.

function expect(label, got, want)
{
    if (got !== want)
        throw new Error(`${label}: expected ${want}, got ${got}`);
}

// Both counts since the process started.
function expectSoFar(label, localeIDsPutIntoSets, lookupsThatFoundNoSet)
{
    expect(`locale IDs put into a set, ${label}`, $vm.intlLocaleIDsPutIntoSets(), localeIDsPutIntoSets);
    expect(`lookups that found no set, ${label}`, $vm.intlLookupsThatFoundNoSet(), lookupsThatFoundNoSet);
}

// What the locale IDs put into a set were before, and how many the list of ICU 79 has. With another ICU a list has
// some: what comes after is held to the count that it has.
function expectList(label, before, sizeWithICU79)
{
    const size = $vm.intlLocaleIDsPutIntoSets() - before;
    if ($vm.icuVersion() === 79 ? size !== sizeWithICU79 : size <= 0)
        throw new Error(`${label}: ${size} locale IDs were put into a set, expected ${sizeWithICU79} with ICU 79`);
    return size;
}

expectSoFar("before the first use of Intl", 0, 0);

const services = [Intl.Collator, Intl.DateTimeFormat, Intl.DisplayNames, Intl.DurationFormat, Intl.ListFormat, Intl.NumberFormat, Intl.PluralRules, Intl.RelativeTimeFormat, Intl.Segmenter];

// No locale is requested, so no tag is looked up and no set is made.
const defaultLocale = new Intl.NumberFormat().resolvedOptions().locale;
expect("DateTimeFormat", new Intl.DateTimeFormat(undefined, { dateStyle: "short" }).resolvedOptions().locale, defaultLocale);
expect("Segmenter", new Intl.Segmenter([]).resolvedOptions().locale, defaultLocale);
expect("Collator", typeof new Intl.Collator().compare("a", "b"), "number");
expect("localeCompare", "a".localeCompare("b"), -1);
expect("toLocaleString", typeof (1234.5).toLocaleString(), "string");
expect("toLocaleDateString", typeof new Date(0).toLocaleDateString(), "string");
expect("Array toLocaleString", typeof [1, 2].toLocaleString(), "string");
for (const service of services) {
    const options = service === Intl.DisplayNames ? { type: "region" } : undefined;
    expect(`new ${service.name}()`, typeof new service(undefined, options).resolvedOptions().locale, "string");
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
expectSoFar("before a locale is looked up", 0, 0);

// The first locale that is looked up makes the set of ICU's list of locales. The services of that list have the one set.
expect("supportedLocalesOf('en')", Intl.DateTimeFormat.supportedLocalesOf("en").join(), "en");
const listOfLocales = expectList("the list of locales", 0, 906);
const tags = ["en-GB", "zh-TW", "zh-Hant-TW", "sr-RS", "xx", "en-XX", "qaa-Qaaa-QM", "de-DE-u-nu-latn"];
for (const service of [Intl.DateTimeFormat, Intl.DurationFormat, Intl.ListFormat, Intl.NumberFormat, Intl.PluralRules, Intl.RelativeTimeFormat]) {
    expect(`${service.name}: en-GB`, new service("en-GB").resolvedOptions().locale, "en-GB");
    expect(`${service.name}: zh-TW`, new service("zh-TW").resolvedOptions().locale, "zh-TW");
    expect(`${service.name}: list`, service.supportedLocalesOf(tags).join(), "en-GB,zh-TW,zh-Hant-TW,sr-RS,en-XX,de-DE-u-nu-latn");
}
expect("DisplayNames: en-GB", new Intl.DisplayNames("en-GB", { type: "region" }).resolvedOptions().locale, "en-GB");
expect("toLocaleString", (1234.5).toLocaleString("en-GB"), "1,234.5");
expectSoFar("after the lookups of the services of ICU's list of locales", listOfLocales, 1);

// ICU's list for the segmenter is its list of locales: the segmenter finds no set of its own, takes the set that is
// there, and makes none.
expect("Segmenter: en-GB", new Intl.Segmenter("en-GB").resolvedOptions().locale, "en-GB");
expect("Segmenter: list", Intl.Segmenter.supportedLocalesOf(tags).join(), "en-GB,zh-TW,zh-Hant-TW,sr-RS,en-XX,de-DE-u-nu-latn");
expectSoFar("after the lookups of the segmenter", listOfLocales, 2);

// The collator has a list of its own, which has few regions.
expect("Collator: en-GB", new Intl.Collator("en-GB").resolvedOptions().locale, "en");
const listOfCollators = expectList("the collator's list", listOfLocales, 146);
expect("Collator: zh-TW", new Intl.Collator("zh-TW").resolvedOptions().locale, "zh-TW");
expect("Collator: list", Intl.Collator.supportedLocalesOf(tags).join(), "en-GB,zh-TW,zh-Hant-TW,sr-RS,en-XX,de-DE-u-nu-latn");
expect("localeCompare", "a".localeCompare("b", "en-GB"), -1);
expectSoFar("after the lookups of the collator", listOfLocales + listOfCollators, 3);
