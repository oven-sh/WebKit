//@ requireOptions("--useDollarVM=1")
//@ runDefault

// Which locales are available is kept in one place for all threads of a process: an index of the locale IDs of
// ICU's list, which the first thread that asks for a locale makes, and which no thread writes to after that. The
// segmenter is answered from the index of ICU's list of locales, whichever of the two is asked for first. Threads
// that ask the services for the same locales at the same time, each beginning with another service, get the same
// answers.
//
// The index of a list is made once: ICU converts each locale ID that is more than a language, a script and a region
// once for the list of locales and once for the collator's, however many threads ask first.
//
// What goes wrong here goes wrong in some runs only: the test is a chance to see it, not a proof.

const threadCount = 8;

const source = `
var tags = [
    "en", "en-US", "en-GB", "en-XX", "en-001", "en-150", "en-Latn-US", "en-Dsrt-US", "en-US-u-va-posix", "en-US-x-private",
    "de", "de-DE", "de-AT", "de-CH", "de-XX", "fr", "fr-FR", "fr-CA", "fr-CH", "es", "es-ES", "es-MX", "es-419", "pt",
    "pt-BR", "pt-PT", "it", "it-IT", "nl", "nl-NL", "nl-BE", "sv", "sv-SE", "da", "da-DK", "nb", "nb-NO", "no", "fi",
    "pl", "cs", "sk", "hu", "ro", "ro-MD", "bg", "el", "tr", "ru", "ru-RU", "uk", "he", "he-IL", "iw", "ar", "ar-EG",
    "ar-SA", "ar-001", "fa", "fa-AF", "hi", "hi-IN", "hi-Latn", "hi-Latn-IN", "bn", "bn-BD", "ta", "te", "th", "vi",
    "id", "ms", "ms-Arab", "ms-Arab-BN", "ja", "ja-JP", "ko", "ko-KR", "zh", "zh-CN", "zh-TW", "zh-HK", "zh-MO", "zh-SG",
    "zh-Hans", "zh-Hant", "zh-Hans-CN", "zh-Hant-TW", "zh-Hant-HK", "zh-Hans-TW", "zh-Latn", "sr", "sr-RS", "sr-ME",
    "sr-BA", "sr-XK", "sr-Latn", "sr-Cyrl", "sr-Latn-RS", "sr-Cyrl-RS", "sr-Latn-ME", "pa", "pa-IN", "pa-PK", "pa-Arab",
    "pa-Guru", "pa-Arab-PK", "pa-Guru-IN", "uz", "uz-AF", "uz-Arab", "uz-Cyrl", "uz-Latn-UZ", "az", "az-AZ", "az-Cyrl",
    "az-Latn-AZ", "bs", "bs-BA", "bs-Cyrl", "bs-Cyrl-BA", "ff", "ff-Adlm", "ff-Adlm-GN", "ff-GN", "ks", "ks-IN",
    "ks-Deva-IN", "yue", "yue-CN", "yue-HK", "yue-Hans", "yue-Hant-HK", "shi", "shi-MA", "shi-Tfng-MA", "vai", "vai-LR",
    "kok", "kok-IN", "kok-Latn-IN", "su", "su-ID", "mn", "mn-CN", "mn-Mong-CN", "kk", "kk-CN", "kk-Arab-CN", "ha",
    "ha-NE", "ha-Arab-NG", "sd", "sd-IN", "sd-PK", "sd-Deva-IN", "xx", "xx-YY", "qaa", "und", "und-US", "tlh", "mul",
];
var services = ["Collator", "DateTimeFormat", "ListFormat", "NumberFormat", "PluralRules", "Segmenter"];

function answers(rotation)
{
    const result = new Array(tags.length);
    for (let step = 0; step < tags.length; ++step) {
        const index = (step * 7 + rotation) % tags.length;
        const tag = tags[index];
        // The services are asked in an order that depends on the thread, and answer in the order of the list.
        const supported = new Array(services.length);
        for (let i = 0; i < services.length; ++i) {
            const serviceIndex = (i + rotation) % services.length;
            const service = Intl[services[serviceIndex]];
            supported[serviceIndex] = " " + service.supportedLocalesOf(tag).length + service.supportedLocalesOf(tag, { localeMatcher: "lookup" }).length;
        }
        result[index] = supported.join("") + " " + new Intl.PluralRules(tag).resolvedOptions().locale + " " + new Intl.Collator(tag).resolvedOptions().locale + " " + new Intl.Segmenter(tag).resolvedOptions().locale;
    }
    return result.join("|");
}
`;

// The threads wait for one signal, so that their first questions come at the same time.
const signal = new Int32Array(new SharedArrayBuffer(8));
const waiting = 0, go = 1;

for (let thread = 1; thread < threadCount; ++thread) {
    $.agent.start(source + `
        $.agent.receiveBroadcast(buffer => {
            const signal = new Int32Array(buffer);
            Atomics.add(signal, ${waiting}, 1);
            Atomics.wait(signal, ${go}, 0);
            let report = "";
            for (let round = 0; round < 3; ++round)
                report += answers(${thread} * 11 + round) + "\\n";
            $.agent.report(report);
            $.agent.leaving();
        });
    `);
}

eval(source);

$.agent.broadcast(signal.buffer);
while (Atomics.load(signal, waiting) < threadCount - 1)
    $.agent.sleep(1);
Atomics.store(signal, go, 1);
Atomics.notify(signal, go);

let expected = "";
for (let round = 0; round < 3; ++round)
    expected += answers(round) + "\n";
const rounds = expected.split("\n");
if (rounds[0] !== rounds[1] || rounds[0] !== rounds[2])
    throw new Error("the answers of one thread changed between rounds");
const first = rounds[0].split("|");
// en, en-US and zh-TW are available to every service, en-XX falls back to en, and xx is not available.
const defaultLocale = new Intl.PluralRules().resolvedOptions().locale;
const all = " 11".repeat(services.length);
const none = " 00".repeat(services.length);
function expectAnswer(tag, answer)
{
    const got = first[tags.indexOf(tag)];
    if (got !== answer)
        throw new Error(`${tag}: expected "${answer}", got "${got}"`);
}
expectAnswer("en", `${all} en en en`);
expectAnswer("en-US", `${all} en-US en-US en-US`);
expectAnswer("en-XX", `${all} en en en`);
expectAnswer("zh-TW", `${all} zh-TW zh-TW zh-TW`);
expectAnswer("zh-Hant-TW", `${all} zh-Hant-TW zh-Hant-TW zh-Hant-TW`);
expectAnswer("xx", `${none} ${defaultLocale} ${defaultLocale} ${defaultLocale}`);

for (let received = 1; received < threadCount;) {
    const report = $.agent.getReport();
    if (report === null) {
        $.agent.sleep(1);
        continue;
    }
    if (report !== expected)
        throw new Error(`a thread got other answers:\n${report}\nexpected:\n${expected}`);
    ++received;
}

// A locale ID that ICU converted has a tag that is not the ID with "-" for "_": en-US-u-va-posix.
function tagsOfConvertedLocaleIDs(list)
{
    return $vm.intlAvailableLocalesTable(list).filter(tag => !/^[a-z]{2,3}(-[A-Z][a-z]{3})?(-([A-Z]{2}|[0-9]{3}))?$/.test(tag)).length;
}
const converted = $vm.intlLocaleIDsConverted();
const expectedConverted = tagsOfConvertedLocaleIDs("locale") + tagsOfConvertedLocaleIDs("collator");
if (converted !== expectedConverted)
    throw new Error(`ICU converted ${converted} locale IDs for ${threadCount} threads, and ${expectedConverted} make one index of each list`);
