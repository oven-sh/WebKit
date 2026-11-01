const fs = require('fs');
const rd = f => fs.readFileSync(f, 'utf8').split('\n').slice(1).filter(Boolean).map(l => l.split('\t'));
const lang = rd('t_language.tsv'), terr = rd('t_territory.tsv'), subd = rd('t_subdivision.tsv');
const out = new Set();
const add = t => out.add(t);
const A = 'abcdefghijklmnopqrstuvwxyz';
const langs2 = [], langs3 = [];
for (const a of A) for (const b of A) { langs2.push(a + b); for (const c of A) langs3.push(a + b + c); }
// 1. every 2- and 3-letter language subtag
for (const l of langs2) add(l);
for (const l of langs3) add(l);
// 2. every region code under several languages/scripts
const regions = [];
for (const a of A) for (const b of A) regions.push((a + b).toUpperCase());
for (let i = 0; i < 1000; i++) regions.push(String(i).padStart(3, '0'));
for (const r of regions) for (const p of ['und', 'en', 'ru', 'sr', 'sh', 'und-Armn', 'und-Cyrl', 'zh-Hant', 'hy', 'sgn', 'cnr', 'fre']) add(p + '-' + r);
// 3. multi-territory aliases x every 2-letter language x scripts
const multi = terr.filter(([k, v]) => v.includes(' ')).map(([k]) => k).filter(k => /^([A-Z]{2}|[0-9]{3})$/.test(k));
const scripts = ['', 'Latn', 'Cyrl', 'Arab', 'Armn', 'Geor', 'Mong', 'Hans', 'Hebr', 'Deva', 'Qaai', 'Grek', 'Thai'];
for (const r of multi) for (const l of langs2.concat(['und', 'fil', 'tl', 'sh', 'cnr', 'mo', 'crh', 'kaa', 'sah', 'tt', 'ba', 'ce', 'cv', 'ab', 'os', 'hyw', 'xcl', 'lez', 'tkr', 'uz', 'kk', 'ky', 'tg', 'tk', 'az', 'ka', 'be', 'uk', 'et', 'lv', 'lt', 'ro', 'bs', 'hr', 'sl', 'mk', 'sq', 'hu', 'pap', 'nl', 'en', 'ku', 'ar', 'ckb', 'chk', 'mh', 'pau', 'cs', 'sk', 'fr', 'gsw']))
  for (const s of scripts) add(l + (s ? '-' + s : '') + '-' + r);
// 4. every language-alias key as a tag, with suffixes
const valid = t => { try { Intl.getCanonicalLocales(t); return true; } catch { return false; } };
const sfx = ['', '-Latn', '-Cyrl', '-US', '-Latn-BA', '-SU', '-fonipa', '-u-ca-gregory', '-t-en-uk', '-x-priv', '-Qaai-YU-heploc', '-u-rg-no23-sd-cn11', '-a-bbb', '-ME', '-Hans', '-Hant-TW', '-1994', '-polytoni', '-hepburn-heploc'];
for (const [k] of lang) { const t = k.replace(/_/g, '-'); for (const s of sfx) add(t + s); add('und-t-' + t.toLowerCase()); add('en-t-' + t.toLowerCase() + '-h0-hybrid-d0-ascii-m0-names'); }
// 5. variants
const variants = ['aaland', 'arevela', 'arevmda', 'bokmal', 'hakka', 'heploc', 'hepburn', 'lojban', 'nynorsk', 'saaho', 'xiang', 'polytoni', 'polyton', 'oed', 'oxendict', 'fonipa', '1994', 'rozaj', 'biske', 'gaulish', 'guoyu', 'posix', 'alalc97', 'bok', 'nyn', 'cmn', 'gan', 'min', 'nan', 'wuu', 'yue'];
const vlangs = ['und', 'en', 'sv', 'hy', 'ja', 'no', 'zh', 'art', 'cel', 'aa', 'de', 'sl', 'ja-Latn', 'en-GB', 'sgn-BE', 'sgn-CH', 'zh-Hans', 'i', 'sgn', 'und-Latn', 'und-SU', 'sh-YU'];
for (const l of vlangs) for (const a of variants) { add(l + '-' + a); for (const b of variants) if (a !== b) { add(l + '-' + a + '-' + b); } }
for (const l of ['und', 'ja-Latn', 'no', 'hy', 'zh', 'sgn-BE']) for (const a of variants.slice(0, 14)) for (const b of variants.slice(0, 14)) for (const c of variants.slice(0, 14)) if (a !== b && b !== c && a !== c) add(l + '-' + a + '-' + b + '-' + c);
// 6. subdivisions
for (const [k] of subd) { add('und-u-sd-' + k); add('en-u-rg-' + k); add('fre-SU-u-rg-' + k + '-sd-' + k); add('en-u-ca-gregory-rg-' + k + '-nu-latn'); }
for (const r of regions.slice(0, 676)) { add('und-u-rg-' + r.toLowerCase() + 'zzzz'); add('und-u-sd-' + r.toLowerCase() + '01'); add('und-u-rg-' + r.toLowerCase() + '23'); }
// 7. t extension
const tl = ['iw', 'sh', 'fre', 'en-uk', 'en-latn-su', 'und-armn-su', 'hy-arevmda', 'sl-rozaj-biske', 'ja-latn-hepburn-heploc', 'zh-cmn', 'no-bokmal', 'cel-gaulish', 'und-qaai', 'en-554', 'sgn-br', 'tl', 'cmn-hans', 'und', 'en'];
const tf = ['', '-h0-hybrid', '-m0-names', '-d0-ascii', '-m0-names-d0-ascii', '-k0-qwerty-h0-hybrid', '-s0-ascii-d0-fwidth', '-x0-foo', '-i0-handwrit', '-t0-und', '-m0-ungegn-2007', '-d0-fwidth-m0-bgn'];
for (const a of tl) for (const b of tf) for (const base of ['und', 'en', 'fre', 'sh-SU']) { add(base + '-t-' + a + b); add(base + '-t-' + a + b + '-u-rg-no23'); add(base + '-a-foo-t-' + a + b); }
for (const b of tf) if (b) add('und-t' + b);
// 8. explicit cases from the earlier probe
const cases = fs.readFileSync('cases.js', 'utf8').match(/'[^']+'/g).map(s => s.slice(1, -1));
for (const c of cases) add(c);
// 9. tags quoted in the 13 skipped test262 files
const T = '/workspace/WebKit/JSTests/test262/test/intl402/';
for (const f of ['Locale/constructor-apply-options-canonicalizes-twice.js', 'Intl/getCanonicalLocales/canonicalized-tags.js', 'Intl/getCanonicalLocales/complex-language-subtag-replacement.js', 'Intl/getCanonicalLocales/complex-region-subtag-replacement.js', 'Intl/getCanonicalLocales/grandfathered.js', 'Intl/getCanonicalLocales/preferred-grandfathered.js', 'Intl/getCanonicalLocales/transformed-ext-canonical.js', 'Intl/getCanonicalLocales/unicode-ext-canonicalize-region.js', 'Intl/getCanonicalLocales/unicode-ext-canonicalize-subdivision.js', 'Locale/constructor-options-region-valid.js', 'Locale/extensions-grandfathered.js', 'Locale/getters-grandfathered.js', 'Locale/likely-subtags-grandfathered.js']) {
  for (const m of fs.readFileSync(T + f, 'utf8').matchAll(/["']([A-Za-z0-9-]{2,})["']/g)) add(m[1]);
}
// 10. bun's ICU locale fixture
for (const l of fs.readFileSync('/workspace/bun/test/js/web/intl/icu-locales.txt', 'utf8').split('\n')) if (l.trim()) add(l.trim().replace(/_/g, '-'));
// 11. pseudo-random structurally valid tags built from alias-heavy pieces
let seed = 12345; const rnd = n => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const pick = a => a[rnd(a.length)];
const lkeys = lang.map(([k]) => k.split('_')[0]).filter(k => /^[a-z]{2,3}$/.test(k)).concat(['en', 'und', 'fr', 'de', 'zh', 'sr', 'ru', 'hy', 'ja', 'no', 'sv', 'nl', 'az', 'uz']);
const rkeys = terr.map(([k]) => k).filter(k => /^([A-Z]{2}|[0-9]{3})$/.test(k)).concat(['US', 'GB', 'RS', 'ME', 'RU', 'AM', 'CN', 'TW', '419', '001']);
const skeys = ['Latn', 'Cyrl', 'Arab', 'Armn', 'Qaai', 'Hans', 'Hant', 'Geor', 'Zinh'];
const uext = ['ca-gregory', 'ca-islamicc', 'nu-latn', 'kn', 'kn-true', 'kn-yes', 'rg-no23', 'sd-cn11', 'rg-fi01', 'sd-frg', 'hc-h23', 'co-phonebk', 'tz-cnckg', 'ms-imperial', 'rg-gbzzzz', 'sd-usca', 'ka-yes', 'attr'];
for (let i = 0; i < 60000; i++) {
  let t = pick(lkeys);
  if (rnd(3) === 0) t += '-' + pick(skeys);
  if (rnd(2) === 0) t += '-' + pick(rkeys);
  const nv = rnd(6) === 0 ? 1 + rnd(3) : 0; const vs = new Set(); for (let j = 0; j < nv; j++) vs.add(pick(variants)); for (const v of vs) t += '-' + v;
  if (rnd(5) === 0) t += '-a-' + pick(['foo', 'bar-baz']);
  if (rnd(4) === 0) t += '-t-' + pick(tl) + pick(tf);
  if (rnd(3) === 0) { const n = 1 + rnd(3); const ks = new Map(); for (let j = 0; j < n; j++) { const e = pick(uext); ks.set(e.slice(0, 2), e); } t += '-u-' + [...ks.values()].join('-'); }
  if (rnd(8) === 0) t += '-x-' + pick(['priv', 'a-b', 'lvariant-jp']);
  add(t);
}
const all = [...out];
fs.writeFileSync('corpus.txt', all.join('\n') + '\n');
console.log('corpus', all.length);
