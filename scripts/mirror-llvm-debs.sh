#!/usr/bin/env bash
# Mirror the LLVM .debs that the Dockerfiles install from apt.llvm.org to a
# GitHub release on oven-sh/WebKit, so the docker builds stop depending on
# apt.llvm.org being reachable. Same idea as scripts/mirror-gcc13-debs.sh.
#
# One tarball per (Ubuntu release, arch) the Dockerfiles build on:
#   focal  amd64, arm64  -> Dockerfile          (llvm.sh $LLVM_VERSION all)
#   noble  amd64         -> Dockerfile.{android,freebsd,macos,windows}
#                                                (llvm.sh $LLVM_VERSION)
# Only packages served from apt.llvm.org go in; their Ubuntu-archive
# dependencies are resolved by `apt-get install ./*.deb` at image build time
# as before. apt verifies every download against the signed repo index.
#
# Needs docker and gh. Run .github/workflows/mirror-llvm-debs.yml to do this
# on a runner. Afterwards paste the printed SHA-256s into the Dockerfiles.
set -euo pipefail

REPO="${REPO:-oven-sh/WebKit}"
LLVM_VERSION="${LLVM_VERSION:-21}"
TAG="${TAG:-llvm-${LLVM_VERSION}-debs}"
OUT="${OUT:-/tmp/llvm-debs}"
V="$LLVM_VERSION"

# What `llvm.sh $V` and `llvm.sh $V all` install (see PKG= in llvm.sh).
BASE_PKGS="clang-$V lldb-$V lld-$V clangd-$V"
ALL_PKGS="$BASE_PKGS clang-tidy-$V clang-format-$V clang-tools-$V llvm-$V-dev \
  llvm-$V-tools libomp-$V-dev libc++-$V-dev libc++abi-$V-dev \
  libclang-common-$V-dev libclang-$V-dev libclang-cpp$V-dev liblldb-$V-dev \
  libunwind-$V-dev libclang-rt-$V-dev libpolly-$V-dev"

rm -rf "$OUT" && mkdir -p "$OUT"

# mirror <distro> <image> <pkgs> <arch>...
mirror() {
  local distro="$1" image="$2" pkgs="$3"; shift 3
  local arches="$*"
  mkdir -p "$OUT/$distro"
  docker run --rm -v "$OUT/$distro":/out -e V="$V" -e DISTRO="$distro" \
    -e PKGS="$pkgs" -e ARCHES="$arches" "$image" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq ca-certificates curl gnupg >/dev/null
    mkdir -p /etc/apt/keyrings
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /etc/apt/keyrings/llvm.gpg
    native=$(dpkg --print-architecture)
    for a in $ARCHES; do
      [ "$a" = "$native" ] || dpkg --add-architecture "$a"
    done
    # Keep the Ubuntu archive native-only: the foreign arch is served from
    # ports.ubuntu.com and we only need its apt.llvm.org debs.
    if [ -f /etc/apt/sources.list ]; then
      sed -i "s|^deb |deb [arch=$native] |" /etc/apt/sources.list
    fi
    if [ -f /etc/apt/sources.list.d/ubuntu.sources ]; then
      sed -i "/^Types:/a Architectures: $native" /etc/apt/sources.list.d/ubuntu.sources
    fi
    echo "deb [arch=$(echo $ARCHES | tr " " ,) signed-by=/etc/apt/keyrings/llvm.gpg] https://apt.llvm.org/$DISTRO/ llvm-toolchain-$DISTRO-$V main" \
      > /etc/apt/sources.list.d/llvm.list
    apt-get update -qq
    # Resolve the install set on the native arch and keep the names that come
    # from apt.llvm.org; the same names exist for every arch in that repo.
    names=$(apt-get install -y -qq --print-uris $PKGS \
      | grep -o "https://apt.llvm.org/[^ '"'"']*\.deb" | xargs -n1 basename | cut -d_ -f1 | sort -u)
    echo "$(echo "$names" | wc -l) packages from apt.llvm.org"
    for a in $ARCHES; do
      mkdir -p /out/$a && cd /out/$a
      for n in $names; do
        apt-get download -qq "$n:$a"
      done
      ls *.deb | wc -l
    done
  '
}

mirror focal ubuntu:20.04 "$ALL_PKGS" amd64 arm64
mirror noble ubuntu:24.04 "$BASE_PKGS" amd64

NOTES="$OUT/notes.md"
{
  echo "LLVM $V .debs from apt.llvm.org, mirrored so the Dockerfiles do not depend on apt.llvm.org at build time. Regenerate with scripts/mirror-llvm-debs.sh (or the mirror-llvm-debs workflow)."
  echo
  echo "| tarball | clang-$V version | files | sha256 |"
  echo "|---|---|---|---|"
} > "$NOTES"
ASSETS=()
for dir in "$OUT"/*/*/; do
  distro=$(basename "$(dirname "$dir")"); arch=$(basename "$dir")
  ls "$dir"/clang-${V}_*.deb >/dev/null || { echo "!! $distro/$arch missing clang-$V"; exit 1; }
  tarball="$OUT/llvm-$V-$distro-$arch.tar.gz"
  # Sorted names + fixed mtime/owner so the SHA-256 is reproducible.
  tar --sort=name --mtime='UTC 2020-01-01' --owner=0 --group=0 --numeric-owner \
      -czf "$tarball" -C "$dir" .
  sha=$(sha256sum "$tarball" | cut -d' ' -f1)
  ver=$(dpkg-deb -f "$(ls "$dir"/clang-${V}_*.deb)" Version)
  echo "| llvm-$V-$distro-$arch.tar.gz | $ver | $(ls "$dir" | wc -l) | \`$sha\` |" >> "$NOTES"
  echo "LLVM_DEBS_SHA256_${distro}_${arch}=$sha  ($(du -h "$tarball" | cut -f1))"
  ASSETS+=("$tarball")
done

if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" -R "$REPO" --clobber "${ASSETS[@]}"
  gh release edit "$TAG" -R "$REPO" --notes-file "$NOTES"
else
  gh release create "$TAG" -R "$REPO" --title "LLVM $V .debs (mirror of apt.llvm.org)" \
    --notes-file "$NOTES" "${ASSETS[@]}"
fi
