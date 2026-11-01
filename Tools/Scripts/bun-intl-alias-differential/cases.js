const tags = [
 'fre','en-uk','sh','tl','cmn','no','nb','iw','in','mo','jw','ji','prs','swc','arb','sgn-BR','sgn-US','no-bokmal','no-nynorsk','aa-saaho',
 'und-Armn-SU','hy-SU','ru-SU','und-SU','az-Arab-SU','uk-SU','sr-YU','und-YU','sq-YU','nl-AN','und-AN','de-DD','en-BU','fr-FX','en-554','en-840','zz-TP','und-ZR','und-QU',
 'und-Qaai','ja-Latn-hepburn-heploc','hy-arevela','hy-arevmda','sv-aaland','en-polytoni','cel-gaulish','zh-guoyu','zh-hakka','zh-xiang','art-lojban','i-klingon','zh-min-nan','i-default','i-enochian','i-mingo','zh-min',
 'sl-rozaj-biske-1994','en-fonupa-fonipa','de-1996-1901','sl-ROZAJ-biske','en-scouse-fonipa',
 'und-t-iw','en-t-m0-names-d0-ascii','und-t-m0-names','en-t-sh-latn','und-t-en-uk','und-t-fre-h0-hybrid','en-t-iw-k0-qwerty','und-t-hy-arevmda','und-t-sl-rozaj-biske',
 'und-u-rg-no23','und-u-sd-frg','en-u-sd-cn11','und-u-rg-fi01','en-u-rg-gbzzzz','en-u-sd-usca','und-u-rg-no23-sd-no23','und-u-rg-jp13',
 'en-u-ca-islamicc','en-u-ca-gregorian','en-u-kn-yes','en-u-kn-true','en-u-ms-imperial','en-u-tz-cnckg','en-u-co-phonebk',
 'und','und-u-ca-gregory','und-x-private','en-x-foo','en-a-bbb-x-foo','fre-a-bbb','sh-a-bbb-u-ca-gregory','x-private',
 'SH-latn-ba','sh-Cyrl','sh-ME','sh-Cyrl-ME','cnr','cnr-BA','drw','tnf','bh','ccq','ggn','yos','zyb','kxe','sca','lmm',
 'zh-Hans-cn','zh-CN-u-nu-hanidec','ko-kore','ja-jp-u-ca-japanese-x-lvariant-JP','en-US-POSIX','en-us-posix','C','posix',
 'aar','heb','ara','zho','chi','ger','deu','dut','nld','fra','tib','bod','cze','ces','wel','cym','gre','ell','baq','eus','per','fas','ice','isl','arm','hye','geo','kat','mac','mkd','mao','mri','may','msa','bur','mya','rum','ron','slo','slk','alb','sqi','scc','scr',
 'en-Latn-US-u-hc-h23','de-latf','de-gaulish','sr-Cyrl-CS','sr-CS','und-CS','cs-CS','und-Latn-CS','und-Cyrl-YU','mn-Mong-SU','und-Mong-SU','kk-SU','ka-SU','und-Geor-SU','lt-SU','tg-SU','und-NT','ar-YD','de-AN','pap-AN','und-BQ','en-UM',
 'hbs','kor-KP','en-004','es-419','ar-001','und-830','und-172','und-200','und-062','und-890','und-230','und-280','und-532','und-582','und-886','und-720','und-736','und-810','und-891',
];
const out = {};
for (const t of tags) {
  let r;
  try { r = Intl.getCanonicalLocales(t)[0]; } catch (e) { r = 'ERR:' + e.constructor.name; }
  let l;
  try { l = new Intl.Locale(t).toString(); } catch (e) { l = 'ERR:' + e.constructor.name; }
  out[t] = r === l ? r : [r, l];
}
console.log(JSON.stringify(out, null, 0).replace(/","/g, '",\n"'));
