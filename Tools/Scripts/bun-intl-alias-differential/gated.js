const canonical = {
  "sgn-DE": "gsg", "hy-arevmda": "hyw", "zh-guoyu": "zh", "no-bokmal": "nb", cnr: "sr-ME", "cnr-BA": "sr-BA", "en-840": "en-US", "sr-YU": "sr-RS", "nl-AN": "nl-CW",
  "und-Qaai": "und-Zinh", "el-polytoni": "el-polyton", "ja-Latn-hepburn-heploc": "ja-Latn-alalc97", "en-u-sd-cn11": "en-u-sd-cnbj", "en-u-rg-no23": "en-u-rg-no50",
  "und-t-iw-m0-names": "und-t-he-m0-prprname", "en-t-hi-h0-hybrid-d0-fwidth": "en-t-hi-d0-fwidth-h0-hybrid",
  "sh-Latn-YU-polytoni-u-ca-gregory-nu-latn-sd-cn11": "sr-Latn-RS-polyton-u-ca-gregory-nu-latn-sd-cnbj",
};
const bad = [];
for (const [tag, expected] of Object.entries(canonical)) {
  const actual = new Intl.Locale(tag).toString();
  if (actual !== expected) bad.push(tag + " => " + actual + " (79.1: " + expected + ")");
}
print(bad.length ? bad.join("; ") : "ok");
