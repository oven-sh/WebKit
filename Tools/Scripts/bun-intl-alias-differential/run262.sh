#!/bin/bash
# usage: run262.sh <engine-binary...> ; runs the 13 skipped test262 files + the repro + ICU-21367 one with the standard harness
T=/workspace/WebKit/JSTests/test262
files="intl402/Locale/constructor-apply-options-canonicalizes-twice.js
intl402/Intl/getCanonicalLocales/canonicalized-tags.js
intl402/Intl/getCanonicalLocales/complex-language-subtag-replacement.js
intl402/Intl/getCanonicalLocales/complex-region-subtag-replacement.js
intl402/Intl/getCanonicalLocales/grandfathered.js
intl402/Intl/getCanonicalLocales/preferred-grandfathered.js
intl402/Intl/getCanonicalLocales/transformed-ext-canonical.js
intl402/Intl/getCanonicalLocales/unicode-ext-canonicalize-region.js
intl402/Intl/getCanonicalLocales/unicode-ext-canonicalize-subdivision.js
intl402/Locale/constructor-options-region-valid.js
intl402/Locale/extensions-grandfathered.js
intl402/Locale/getters-grandfathered.js
intl402/Locale/likely-subtags-grandfathered.js
intl402/Intl/getCanonicalLocales/unicode-ext-canonicalize-yes-to-true.js"
[ -n "$FILES" ] && files="$FILES"
pass=0; fail=0
for f in $files; do
  inc=$(sed -n 's/^includes: *\[\(.*\)\]/\1/p' $T/test/$f | tr ',' ' ')
  tmp=$(mktemp /tmp/refute-t262/t262-XXXXXX.js)
  cat $T/harness/assert.js $T/harness/sta.js > $tmp
  for i in $inc; do cat $T/harness/$i >> $tmp; done
  cat $T/test/$f >> $tmp
  out=$("$@" $tmp 2>&1); rc=$?
  if [ $rc -eq 0 ]; then pass=$((pass+1)); echo "PASS $f"; else fail=$((fail+1)); echo "FAIL $f :: $(echo "$out" | grep -E "message|Expected|Test262Error|Error" | head -2 | tr "\n" " " | cut -c1-400)"; fi
  rm -f $tmp
done
echo "== $*: pass=$pass fail=$fail"
