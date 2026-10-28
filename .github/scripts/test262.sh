#!/usr/bin/env bash
# Runs test262 with a jsc shell and says whether the two files that say what this fork's shell does with it are true
# of that shell. The `test` job of .github/workflows/ci.yml runs this for every tested lane, and this is how to get
# CI's answer by hand:
#
#   bash .github/scripts/test262.sh <jsc> [<directory for what it writes>]
#
# JSTests/test262/expectations-linux.yaml is the record of the runs that fail, each with its exit code. It is what
# Tools/Scripts/test262-runner (upstream's, not changed here) reads on Linux to tell a failure that is known from one
# that is new, and the runner by itself fails only for a run that fails and is not in the record. This fails too for
# a run that is in the record and passes, or is no longer run: the runner writes the record again from a run of the
# whole suite (`--save`, into a copy), and the shell passes when that copy is the checked-in file, byte for byte.
#
# JSTests/test262/skipped-linux.txt is the list of the tests that JSTests/test262/config.yaml takes out of the run. A
# test that is not run is in no record, so a line of config.yaml could take any number of tests away and the record
# would stay true. The list is held in the same way: it is written again from what the runner skipped.
#
# When this fails it prints the difference between the two files and the shell, and what the runner printed above it
# (`! NEW FAIL` with the output of the test, `NEW PASS`) says why. Either the engine is fixed or the files are: the
# ones this run wrote are in <directory> (in CI, in test262/ of the leg's artifact), to be copied over the checked-in
# ones. Both are written by this script and never by hand. JSTests/BUN-TEST-DIFFERENCES.md says what to look at first
# when a leg is red, and is where the reason for a run that fails goes.

set -euo pipefail

[ $# -ge 1 ] || { echo "usage: $0 <jsc> [<directory for what it writes> [<where a reader finds that directory>]]" >&2; exit 2; }
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
files=(expectations-linux.yaml skipped-linux.txt)

# The runner picks the record by platform. Off Linux it reads expectations.yaml, which is upstream's and which this
# fork does not edit: no lane is tested there.
if [ "$(uname -s)" != Linux ]; then
  echo "test262 is not run: off Linux the runner reads JSTests/test262/expectations.yaml, and no tested lane holds that file."
  exit 0
fi

case "$1" in /*) jsc=$1 ;; *) jsc="$PWD/$1" ;; esac
[ -x "$jsc" ] || { echo "::error::test262: there is no jsc at $jsc"; exit 1; }
out=${2:-$(mktemp -d "${TMPDIR:-/tmp}/test262.XXXXXX")}
mkdir -p "$out"
# The runner writes its reports (test262-results/) where it is run.
cd "$out"
out=$PWD
where=${3:-$out}
rm -rf "${files[@]}" test262.diff wip && mkdir wip

# The runner imports two modules that Python does not come with. Upstream leaves them to webkitcorepy's
# autoinstaller, which gets them, and the seven packages it installs them with, from pypi.org by version alone. Here
# the autoinstaller is off and nothing is downloaded. PyYAML is the machine's (python3-yaml: the images of CI's
# runners have it, and the runner writes the same bytes with every version of it). tblib, which webkitcorepy's
# TaskPool imports, is four files in .github/scripts/python.
python3 -c 'import yaml' 2> /dev/null || { echo "::error::test262: this python3 has no PyYAML (python3-yaml), which Tools/Scripts/test262-runner imports"; exit 1; }
export PYTHONPATH="$root/.github/scripts/python${PYTHONPATH:+:$PYTHONPATH}"
export DISABLE_WEBKITCOREPY_AUTOINSTALLER=1

# What a run by hand has, so that the files do not depend on where they were written: the runner's own time zone,
# the locale of CI's runners (jsc takes its default locale from it), and no limit on the shell's memory, which the
# `test` job sets for the other tests. Time stays the machine's: the runner ends a jsc after 300 s and calls that a
# failure.
export TZ=PST LC_ALL=C.UTF-8
unset JSCTEST_memoryLimit

# The runner names a test only when every batch of tests is back. Until then this names the runs that take long, so
# that the log of a step that did not end in the job's time has the tests that kept it. Like the runner's lines of
# progress, each of its lines starts with the line break.
long_runs() {
  while sleep 60 > /dev/null 2>&1; do
    ps -ww -eo etimes=,args= | awk -v jsc="$jsc" '$2 == jsc && $1 >= 120 { sub(/.*\/JSTests\/test262\//, "", $NF); printf "\ntest262: one jsc has run for %s s: %s", $1, $NF }'
  done
}

# No --test-only, --filter or --features: only from a run of everything does --save write the record from nothing.
# From a part of the suite it keeps every entry whose test did not run.
run() {
  long_runs &
  local watcher=$!
  python3 "$root/Tools/Scripts/test262-runner" --jsc "$jsc" --expectations wip/expectations-linux.yaml --save
  local status=$?
  pkill -P "$watcher" 2> /dev/null
  kill "$watcher" 2> /dev/null
  return "$status"
}
cp "$root/JSTests/test262/expectations-linux.yaml" wip/
set +e
TIMEFORMAT='test262-runner took %R s, %U s of it user and %S s system time'
time run 2>&1 | tee test262.log
status=${PIPESTATUS[0]}
set -e

# In CI the verdict is also on the run's page.
summary() { if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then cat >> "$GITHUB_STEP_SUMMARY"; else cat > /dev/null; fi; }
heading="### test262${LABEL:+: $LABEL}"
died() { # <what happened>
  echo "::error::test262: $1"
  { echo "$heading"; echo; echo "$1."; } | summary
  exit 1
}

# A runner that died (no test files, an exception of its own) wrote no record, and one that failed behind that has
# not finished its reports. With a record written its status is 0, or 1 for a failure that was not in the record.
grep -q '^Saved expectation file in: ' test262.log || died "test262-runner ended with status $status before it wrote the record"
[ "$status" -le 1 ] || died "test262-runner wrote the record and then ended with status $status"
mv wip/expectations-linux.yaml . && rmdir wip
# The tests that config.yaml took out of the run, from the runner's report: an entry has its mode before its path.
awk '/^(- |  )mode: / { skip = ($0 ~ /mode: skip$/) } /^(- |  )path: / { if (skip) print $NF; skip = 0 }' test262-results/results.yaml | sort > skipped-linux.txt
skipped=$(sed -n 's/^\([0-9][0-9]*\) test files skipped$/\1/p' test262.log)
[ "$(wc -l < skipped-linux.txt)" = "$skipped" ] || died "test262-runner skipped ${skipped:-a number it did not print of} tests, and its report names $(wc -l < skipped-linux.txt)"
totals=$(grep -E '^[0-9]+ (tests run|test files skipped|tests failed in total)$' test262.log | paste -sd, - | sed 's/,/, /g')

for file in "${files[@]}"; do
  diff -u --label "a/JSTests/test262/$file" --label "b/JSTests/test262/$file" "$root/JSTests/test262/$file" "$file" || true
done > test262.diff
if [ ! -s test262.diff ]; then
  rm test262.diff
  [ "$status" -eq 0 ] || died "test262-runner wrote the record as it is checked in and ended with status $status"
  echo "test262: expectations-linux.yaml and skipped-linux.txt are what this shell does ($totals)."
  { echo "$heading"; echo; echo "$totals. \`expectations-linux.yaml\` and \`skipped-linux.txt\` are what this shell does."; } | summary
  exit 0
fi

# A jsc that the runner ended at its limit is a failure with exit code 1 whose output is the word Timeout.
timeouts=$(awk '/^! NEW FAIL .* \(Exit code: 1\)$/ { run = $0; sub(/^! NEW FAIL /, "", run); sub(/ \(Exit code: 1\)$/, "", run); if ((getline output) > 0 && output == "Timeout") print "  " run }' test262.log)
what_to_do() {
  if [ -n "$timeouts" ]; then
    echo "These runs did not end in the 300 s that the runner gives one jsc. On a machine that is slow or busy that"
    echo "says nothing about the engine: run it again before anything is recorded."
    echo "$timeouts"
    echo
  fi
  echo "First look at the test262 step of main's latest run${GITHUB_REPOSITORY:+ (${GITHUB_SERVER_URL:-https://github.com}/$GITHUB_REPOSITORY/actions/workflows/ci.yml?query=branch%3Amain)}."
  echo "Lines that are there too are main's. They are repaired on main: do not record them on this branch. For the"
  echo "others, fix the engine, or copy the two files this run wrote over the checked-in ones. They are in: $where"
}
echo
echo "The two files and this shell differ. In expectations-linux.yaml a line with + is a run that fails and is not in"
echo "the record (or that fails with another exit code than the line with - above it), and a line with - alone is a"
echo "run that is in the record and passes, or is not run any more. In skipped-linux.txt a line with + is a test that"
echo "config.yaml takes out of the run and that is not in the list."
cat test262.diff
echo
what_to_do
echo "::error::test262: expectations-linux.yaml and skipped-linux.txt of JSTests/test262 are not what this shell does ($totals). The log of the step says what differs and what to do."
{
  echo "$heading"
  echo
  echo "$totals. \`expectations-linux.yaml\` and \`skipped-linux.txt\` are not what this shell does:"
  echo
  echo '```diff'
  cat test262.diff
  echo '```'
  echo
  what_to_do
} | summary
exit 1
