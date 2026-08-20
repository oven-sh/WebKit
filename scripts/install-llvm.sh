#!/usr/bin/env bash
# Install LLVM from apt.llvm.org inside the Docker builds. Usage, from a
# Dockerfile RUN step: install-llvm.sh <major version> [all]
#
# This runs apt.llvm.org's own llvm.sh, it only adds the retries llvm.sh does
# not have. Every docker build (about 25 per CI run) fetches llvm.sh, probes the
# repo and the signing key with HEAD requests, downloads the key, then pulls
# about 30 debs, all from apt.llvm.org and none of it retried. One failed
# request fails the build, and during a burst of builds that happens to a few
# variants per run: the connection to apt.llvm.org fails, or llvm.sh exits with
# "GPG key not reachable", or a repo probe fails and llvm.sh misreports it as
# "Distribution 'ubuntu' ... is not supported by this script".
#
# So the whole install is one attempt, retried with a growing pause: fetch the
# key, fetch the script, run it. llvm.sh only runs add-apt-repository, apt-get
# update and apt-get install, which are idempotent, and it skips its own
# unretried key download because the key file is already there. The retry is
# a shell loop rather than curl --retry: curl --retry covers timeouts, a few
# HTTP codes and (with --retry-connrefused) ECONNREFUSED, but not the other
# connect failures seen here, and focal's curl has no --retry-all-errors.
set -euo pipefail

if [ $# -lt 1 ]; then
  echo "usage: $0 <llvm major version> [all]" >&2
  exit 2
fi

# Both files are a few KB. The time limits turn a stalled server into a failed
# attempt instead of a job that sits there until its 90 minute timeout.
fetch() {
  curl -fsSL --connect-timeout 15 --max-time 60 "$1" -o "$2"
}

attempt() {
  fetch https://apt.llvm.org/llvm-snapshot.gpg.key /etc/apt/trusted.gpg.d/apt.llvm.org.asc &&
    fetch https://apt.llvm.org/llvm.sh /tmp/llvm.sh &&
    bash /tmp/llvm.sh "$@"
}

for n in 1 2 3 4 5; do
  if attempt "$@"; then
    rm -f /tmp/llvm.sh
    exit 0
  fi
  echo "installing LLVM from apt.llvm.org failed (attempt $n of 5)" >&2
  if [ "$n" -lt 5 ]; then
    sleep $((n * 5))
  fi
done
exit 1
