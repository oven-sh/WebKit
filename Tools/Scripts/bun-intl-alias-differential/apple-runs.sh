#!/bin/bash
# Differential of the engine against icu::Locale::createCanonical with the alias tables of each macOS release in place of ICU 79.1's own.
export ASAN_OPTIONS=detect_leaks=0
cd /tmp/alias-harness
declare -A label=( [ICU-70127.1]="macOS 13.0" [ICU-70205]="macOS 13.3-13.5" [ICU-72122]="macOS 14.0" [ICU-72123.15]="macOS 14.1-14.3" [ICU-74000.403]="macOS 14.5-14.6" [ICU-74222.203]="macOS 15.2-15.3" [ICU-76104.3]="macOS 15.4-15.5" [ICU-76142.3.1.1]="macOS 26.3" [ICU-76142.5.1.200]="macOS 26.5" )
for v in ICU-70127.1 ICU-70205 ICU-72122 ICU-72123.15 ICU-74000.403 ICU-74222.203 ICU-76104.3 ICU-76142.3.1.1 ICU-76142.5.1.200; do
  total=0; mism=0; changed=0
  for c in /tmp/jsc-alias-probe/corpus.txt /tmp/jsc-alias-probe/long.txt /tmp/jsc-alias-probe/long2.txt /tmp/alias-harness/prefilter-corpus.txt; do
    line=$(ICU_DATA=/tmp/apple-data/$v/ ./harness diff 0 < $c 2>&1 | tail -1)
    t=$(echo "$line" | sed -n 's/.*total=\([0-9]*\).*/\1/p'); n=$(echo "$line" | sed -n 's/.*not-a-tag=\([0-9]*\).*/\1/p'); m=$(echo "$line" | sed -n 's/.*mismatches=\([0-9]*\).*/\1/p'); ch=$(echo "$line" | sed -n 's/.*changed-by-alias-pass=\([0-9]*\).*/\1/p')
    total=$((total + t - n)); mism=$((mism + m)); changed=$((changed + ch))
  done
  echo "$v (${label[$v]}): compared=$total changed-by-alias-pass=$changed mismatches=$mism"
done
