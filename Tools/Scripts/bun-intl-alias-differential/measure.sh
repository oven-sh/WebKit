#!/bin/bash
# Per-call cost, before and after, for a fixed list of ICU locale IDs. Each ID runs in its own process so that a first call is a first call.
B=/tmp/alias-harness/bench
ids=("en_US" "de" "en_CA" "fr_CA" "zh_Hant_TW" "sr_Latn_RS" "en_US@calendar=gregorian" "de_DE@calendar=gregorian;collation=phonebook;numbers=latn" "en__POSIX" "_Latn" "fre" "en_UK" "sh" "_Armn_SU" "en@rg=no23")
for id in "${ids[@]}"; do
  $B before 20000 de "$id" | tail -1
  $B after 20000 de "$id" | tail -1
done
echo "--- one-time load (first call of 'de' in a fresh process):"
$B before 1 de | tail -1
$B after 1 de | tail -1
