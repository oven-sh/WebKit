// The rows of bun's ungated tests (test/js/web/intl/intl.test.ts), to run with the alias tables of each macOS release.
const failures = [];
function same(name, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) failures.push(name + ": " + a + " != " + e);
}
const canonical = {
  fre: "fr", cmn: "zh", tl: "fil", aam: "aas", sh: "sr-Latn", "sgn-DE": "gsg", "zh-guoyu": "zh", "no-bokmal": "nb",
  "en-uk": "en-GB", "en-DD": "en-DE", "en-BU": "en-MM", "en-840": "en-US", "ru-SU": "ru-RU", "hy-SU": "hy-AM", "und-Armn-SU": "und-Armn-AM",
  "und-Qaai": "und-Zinh", "el-polytoni": "el-polyton", "ja-Latn-hepburn-heploc": "ja-Latn-alalc97", "und-t-iw": "und-t-he",
  "fre-u-ca-gregory": "fr-u-ca-gregory", "en-uk-u-ca-gregory": "en-GB-u-ca-gregory",
};
const tags = Object.keys(canonical);
same("Locale", Object.fromEntries(tags.map(tag => [tag, new Intl.Locale(tag).toString()])), canonical);
same("getCanonicalLocales", Intl.getCanonicalLocales(tags), [...new Set(Object.values(canonical))]);
same("dedupe", Intl.getCanonicalLocales(["fr", "fre", "FRE", "fra"]), ["fr"]);
same("legacy", Intl.getCanonicalLocales(["in", "iw", "mo", "ji"]), ["id", "he", "ro", "yi"]);
same("sh tl no", Intl.getCanonicalLocales(["sh", "tl", "no"]), ["sr-Latn", "fil", "no"]);
const plain = "und und-u-ca-gregory und-x-private und-Latn en en-US zh-Hant-TW sr-Latn-RS no".split(" ");
same("plain Locale", plain.map(tag => new Intl.Locale(tag).toString()), plain);
same("plain canonical", Intl.getCanonicalLocales(plain), plain);
same("options", [
  new Intl.Locale("und-Armn-SU", { language: "ru" }).toString(),
  new Intl.Locale("en", { region: "uk" }).toString(),
  new Intl.Locale("fre", { region: "uk" }).toString(),
  new Intl.Locale("und", { language: "sh" }).toString(),
], ["ru-Armn-AM", "en-GB", "fr-GB", "sr-Latn"]);
const uk = new Intl.Locale("en-uk"), sh = new Intl.Locale("sh");
same("getters", [uk.region, uk.baseName, sh.language, sh.script, sh.maximize().toString()], ["GB", "en-GB", "sr", "Latn", "sr-Latn-RS"]);
same("formatters", [
  new Intl.NumberFormat("fre").resolvedOptions().locale,
  new Intl.DateTimeFormat("fre").resolvedOptions().locale,
  new Intl.PluralRules("tl").resolvedOptions().locale,
  Intl.NumberFormat.supportedLocalesOf(["fre", "sh", "en-uk"]).join(),
], ["fr", "fr", "fil", "fr,sr-Latn,en-GB"]);
print(failures.length ? "FAIL\n" + failures.join("\n") : "ok");
