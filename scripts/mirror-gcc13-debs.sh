#!/usr/bin/env bash
# Mirror gcc-13 focal .debs from ppa:ubuntu-toolchain-r/test to a GitHub
# release on oven-sh/WebKit so the Dockerfile no longer depends on Launchpad
# availability. Run this once when Launchpad is reachable; it is idempotent.
#
# After this succeeds, update Dockerfile to fetch from the release (see the
# block at the bottom of this script for the exact RUN line + SHA-256 check).
set -euo pipefail

REPO="${REPO:-oven-sh/WebKit}"
TAG="${TAG:-gcc-13-focal-debs}"
OUT="${OUT:-/tmp/gcc13-debs}"

PKGS="gcc-13 g++-13 cpp-13 gcc-13-base libgcc-13-dev libstdc++-13-dev \
      libasan6 libubsan1 libatomic1 libtsan0 liblsan0 libgfortran5 \
      gcc-11-base gcc-16-base libasan8 libcc1-0 libgcc-s1 libgomp1 \
      libhwasan0 libitm1 libquadmath0 libstdc++6 libtsan2"

echo ">> waiting for Launchpad..."
until curl -sf --connect-timeout 8 \
  "https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/dists/focal/InRelease" \
  -o /dev/null; do sleep 30; done
echo ">> Launchpad reachable"

rm -rf "$OUT" && mkdir -p "$OUT/amd64" "$OUT/arm64"

docker run --rm -v "$OUT":/out ubuntu:20.04 bash -c '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq curl gnupg ca-certificates >/dev/null
  curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x60C317803A41BA51845E371A1E9377A2BA9EF27F" \
    | gpg --dearmor -o /etc/apt/trusted.gpg.d/tcr.gpg
  dpkg --add-architecture arm64
  sed -i "s|^deb |deb [arch=amd64] |" /etc/apt/sources.list
  echo "deb [arch=amd64,arm64] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu focal main" \
    > /etc/apt/sources.list.d/tcr.list
  for i in $(seq 1 60); do
    apt-get update -qq 2>/dev/null || true
    apt-cache policy gcc-13:amd64 | grep -q "13\." \
      && apt-cache policy gcc-13:arm64 | grep -q "13\." && break
    sleep 20
  done
  for arch in amd64 arm64; do
    cd /out/$arch
    for p in '"$PKGS"'; do
      for j in $(seq 1 20); do apt-get download "$p:$arch" 2>/dev/null && break; sleep 15; done
    done
  done
'

for arch in amd64 arm64; do
  test -n "$(ls -A "$OUT/$arch")" || { echo "!! $arch empty"; exit 1; }
  ls "$OUT/$arch"/gcc-13_*.deb >/dev/null || { echo "!! $arch missing gcc-13"; exit 1; }
  # tar with sorted names + zeroed mtime/owner so the SHA-256 is reproducible.
  tar --sort=name --mtime='UTC 2020-01-01' --owner=0 --group=0 --numeric-owner \
      -czf "$OUT/gcc-13-focal-$arch.tar.gz" -C "$OUT/$arch" .
done

AMD64_SHA=$(sha256sum "$OUT/gcc-13-focal-amd64.tar.gz" | cut -d' ' -f1)
ARM64_SHA=$(sha256sum "$OUT/gcc-13-focal-arm64.tar.gz" | cut -d' ' -f1)
echo "amd64 sha256: $AMD64_SHA  ($(du -h "$OUT/gcc-13-focal-amd64.tar.gz" | cut -f1))"
echo "arm64 sha256: $ARM64_SHA  ($(du -h "$OUT/gcc-13-focal-arm64.tar.gz" | cut -f1))"

if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" -R "$REPO" --clobber \
    "$OUT/gcc-13-focal-amd64.tar.gz" "$OUT/gcc-13-focal-arm64.tar.gz"
else
  gh release create "$TAG" -R "$REPO" \
    --title "gcc-13 focal .debs (mirror of ubuntu-toolchain-r PPA)" \
    --notes "Mirrored from ppa:ubuntu-toolchain-r/test for focal so Docker builds don't depend on Launchpad availability. Regenerate with scripts/mirror-gcc13-debs.sh." \
    "$OUT/gcc-13-focal-amd64.tar.gz" "$OUT/gcc-13-focal-arm64.tar.gz"
fi

cat <<EOF

>> Dockerfile RUN block (replace the existing gcc-13 install step):

# Install GCC 13 toolchain — mirrored to a GitHub release so builds don't
# depend on Launchpad PPA availability (single-IP, has gone hard-down).
# Tarballs produced by scripts/mirror-gcc13-debs.sh; SHA-256 pinned here so
# dpkg -i can't be subverted by a tampered release asset.
RUN set -e; \\
    case "\${TARGETARCH}" in \\
      amd64) sha="${AMD64_SHA}" ;; \\
      arm64) sha="${ARM64_SHA}" ;; \\
      *) echo "unsupported TARGETARCH=\${TARGETARCH}"; exit 1 ;; \\
    esac; \\
    curl -fsSL --retry 5 \\
      "https://github.com/${REPO}/releases/download/${TAG}/gcc-13-focal-\${TARGETARCH}.tar.gz" \\
      -o /tmp/gcc13.tar.gz; \\
    echo "\${sha}  /tmp/gcc13.tar.gz" | sha256sum -c -; \\
    mkdir -p /tmp/gcc13 && tar xzf /tmp/gcc13.tar.gz -C /tmp/gcc13; \\
    apt-get update && apt-get install -y libc6-dev; \\
    dpkg -i /tmp/gcc13/*.deb; \\
    rm -rf /tmp/gcc13 /tmp/gcc13.tar.gz /var/lib/apt/lists/*
EOF
