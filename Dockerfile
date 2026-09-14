ARG MARCH_FLAG=""
ARG WEBKIT_RELEASE_TYPE=Release
ARG LTO_FLAG="-flto=thin -fno-split-lto-unit -fwhole-program-vtables -fforce-emit-vtables "
ARG RELEASE_FLAGS="-O3 -DNDEBUG=1"
ARG LLVM_VERSION="21"
# The -lto variants append -g1 (line tables only) after this, see $G below; every other variant keeps full -g.
ARG DEFAULT_CFLAGS="-mno-omit-leaf-frame-pointer -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -faddrsig -fno-unwind-tables -fno-asynchronous-unwind-tables -DU_STATIC_IMPLEMENTATION=1 "
ARG ENABLE_SANITIZERS=""
ARG USE_MIMALLOC="OFF"
ARG USE_EXTERNAL_MIMALLOC="OFF"
ARG USE_SYSTEM_MALLOC="OFF"
# What the lane is built for: x86_64, which is what this container is, or aarch64, cross-compiled against the sysroot
# `base` carries. The container itself is always linux/amd64.
ARG LINUX_ARCH="x86_64"

# The arm64 ubuntu:20.04, only ever copied from (see the aarch64 sysroot in `base`): nothing of it is run.
FROM --platform=linux/arm64 ubuntu:20.04 as rootfs-arm64

# `base` is the toolchain and nothing else: it takes no lane setting (LTO_FLAG, MARCH_FLAG, WEBKIT_RELEASE_TYPE, ...), so
# it is the same for every lane of an architecture. CI builds it once per change, keeps it in ghcr.io, and hands it to
# the lanes as `--build-context base=docker-image://...`, which replaces this stage (.github/workflows/ci.yml, the
# `image` job). Without that, it is built here like any other stage. Lane settings belong in `lane` below.
FROM ubuntu:20.04 as base

ARG LLVM_VERSION

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Both archive.ubuntu.com and azure.archive.ubuntu.com have intermittently
# timed out from inside the GitHub-hosted docker-buildx network at different
# times. Prefer Azure (faster on Azure-hosted runners) but fall back to the
# canonical mirror if `apt-get update` can't reach it.
RUN sed -i 's|http://archive.ubuntu.com/ubuntu|http://azure.archive.ubuntu.com/ubuntu|g' /etc/apt/sources.list

# Install basic build dependencies
RUN ( apt-get update || \
      ( sed -i 's|http://azure.archive.ubuntu.com/ubuntu|http://archive.ubuntu.com/ubuntu|g' /etc/apt/sources.list && apt-get update ) \
    ) && apt-get install -y \
    wget \
    curl \
    git \
    python3 \
    python3-pip \
    xz-utils \
    ninja-build \
    software-properties-common \
    apt-transport-https \
    ca-certificates \
    gnupg \
    lsb-release \
    && rm -rf /var/lib/apt/lists/*

# Install zstd (for icu/compress-data.ts). Pinned: focal's apt has 1.4.4 which
# compresses meaningfully worse than 1.5.x; this matches Bun's vendored decoder.
# Its lib/ sources stay, at /zstd/lib: the jsc shell builds the decoder from them to read that data
# (shell/CMakeLists.txt, BUN_ICU_ZSTD_SOURCE_DIR).
ARG ZSTD_VERSION=1.5.7
RUN curl -fsSL "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" | tar xz -C /tmp \
    && make -C /tmp/zstd-${ZSTD_VERSION}/programs zstd -j$(nproc) \
    && cp /tmp/zstd-${ZSTD_VERSION}/programs/zstd /usr/local/bin/ \
    && mkdir /zstd && cp -r /tmp/zstd-${ZSTD_VERSION}/lib /zstd/lib && test -f /zstd/lib/zstd.h \
    && rm -rf /tmp/zstd-${ZSTD_VERSION} \
    && zstd --version

# Install Node (for icu/compress-data.ts; needs >=23.6 for default type stripping)
ARG NODE_VERSION=24.16.0
RUN curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-$(uname -m | sed 's/x86_64/x64/;s/aarch64/arm64/').tar.xz" \
    | tar -xJ -C /usr/local --strip-components=1 \
    && node --version

# Install modern CMake for Ubuntu
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null \
    && apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
    && apt-get update \
    && apt-get install -y cmake \
    && rm -rf /var/lib/apt/lists/*

# Install GCC 13 toolchain
# Mirrored from ppa:ubuntu-toolchain-r/test to a GitHub release so this image
# doesn't depend on Launchpad availability (single-IP 185.125.190.80; went
# hard-down 2026-05-01, taking the API + keyserver.ubuntu.com with it). The
# tarball is SHA-256-pinned. Regenerate via scripts/mirror-gcc13-debs.sh.
ARG GCC13_DEBS_SHA256_amd64=a2b3b6e10b175bbaaefeb3e9e703ca26a97ed6c1f19ca842d3e0b0c8f941e65b
ARG GCC13_DEBS_SHA256_arm64=be19db90d94c52c6061280bbadcaad9b09db1e9f2e77a12f8c18c5425d2eb056
RUN curl -fsSL --retry 5 --retry-connrefused \
        "https://github.com/oven-sh/WebKit/releases/download/gcc-13-focal-debs/gcc-13-focal-amd64.tar.gz" \
        -o /tmp/gcc13.tar.gz \
    && echo "${GCC13_DEBS_SHA256_amd64}  /tmp/gcc13.tar.gz" | sha256sum -c - \
    && mkdir -p /tmp/gcc13 && tar xzf /tmp/gcc13.tar.gz -C /tmp/gcc13 \
    && apt-get update \
    && apt-get install -y libc6-dev binutils libisl22 libmpc3 libmpfr6 \
    && (dpkg -i /tmp/gcc13/*.deb || apt-get install -f -y) \
    && dpkg -l gcc-13 g++-13 libstdc++-13-dev >/dev/null \
    && rm -rf /tmp/gcc13 /tmp/gcc13.tar.gz /var/lib/apt/lists/*

# Ensure GCC 13 is the default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 130 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-13 \
    --slave /usr/bin/gcc-ar gcc-ar /usr/bin/gcc-ar-13 \
    --slave /usr/bin/gcc-nm gcc-nm /usr/bin/gcc-nm-13 \
    --slave /usr/bin/gcc-ranlib gcc-ranlib /usr/bin/gcc-ranlib-13

# Install LLVM
# The `llvm.sh <version> all` package set, mirrored from apt.llvm.org to a
# GitHub release so the image doesn't depend on apt.llvm.org at build time.
# Ubuntu-archive dependencies still come from apt. Regenerate via
# scripts/mirror-llvm-debs.sh (or the mirror-llvm-debs workflow).
ARG LLVM_DEBS_SHA256_amd64=759ea9d6d50de9b6062cf40161a24a3a9d70aaf11aa1a544074d126590eb55f7
ARG LLVM_DEBS_SHA256_arm64=4d4923baa663cb2e1be67e8e7097220604489b0da8b7a4ab5911ac2baf1e0ba6
RUN curl -fsSL --retry 5 --retry-connrefused \
        "https://github.com/oven-sh/WebKit/releases/download/llvm-${LLVM_VERSION}-debs/llvm-${LLVM_VERSION}-focal-amd64.tar.gz" \
        -o /tmp/llvm.tar.gz \
    && echo "${LLVM_DEBS_SHA256_amd64}  /tmp/llvm.tar.gz" | sha256sum -c - \
    && mkdir -p /tmp/llvm && tar xzf /tmp/llvm.tar.gz -C /tmp/llvm \
    && apt-get update \
    && apt-get install -y /tmp/llvm/*.deb \
    && rm -rf /tmp/llvm /tmp/llvm.tar.gz /var/lib/apt/lists/*

# Configure library paths
RUN export ARCH_PATH="x86_64-linux-gnu" \
    && mkdir -p /usr/lib/gcc/${ARCH_PATH}/13 \
    && ln -sf /usr/lib/${ARCH_PATH}/libstdc++.so.6 /usr/lib/gcc/${ARCH_PATH}/13/ \
    && echo "/usr/lib/gcc/${ARCH_PATH}/13" > /etc/ld.so.conf.d/gcc-13.conf \
    && echo "/usr/lib/${ARCH_PATH}" >> /etc/ld.so.conf.d/gcc-13.conf \
    && ldconfig


# Install additional WebKit build dependencies
RUN apt-get update && apt-get install -y \
    libxml2-dev \
    ruby \
    ruby-dev \
    bison \
    gawk \
    perl \
    make \
    && rm -rf /var/lib/apt/lists/*

# Set up LLVM toolchain symlinks. After the last package install, which could put /usr/bin/ld and /usr/bin/cc back.
RUN for f in /usr/lib/llvm-${LLVM_VERSION}/bin/*; do ln -sf "$f" /usr/bin; done \
    && ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/bin/clang \
    && ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/bin/clang++ \
    && ln -sf /usr/bin/lld-${LLVM_VERSION} /usr/bin/lld \
    && ln -sf /usr/bin/lldb-${LLVM_VERSION} /usr/bin/lldb \
    && ln -sf /usr/bin/clangd-${LLVM_VERSION} /usr/bin/clangd \
    && ln -sf /usr/bin/llvm-ar-${LLVM_VERSION} /usr/bin/llvm-ar \
    && ln -sf /usr/bin/ld.lld /usr/bin/ld \
    && ln -sf /usr/bin/clang /usr/bin/cc \
    && ln -sf /usr/bin/clang++ /usr/bin/c++

ENV WEBKIT_OUT_DIR=/webkitbuild
RUN mkdir -p /output/lib /output/include /output/include/JavaScriptCore /output/include/glibc /output/include/wtf /output/include/bmalloc /output/include/unicode

# Set environment variables for toolchain
ENV CC="clang-${LLVM_VERSION}"
ENV CXX="clang++-${LLVM_VERSION}"
ENV AR="llvm-ar-${LLVM_VERSION}"
ENV RANLIB="llvm-ranlib-${LLVM_VERSION}"
ENV LD="lld-${LLVM_VERSION}"
ENV LD_LIBRARY_PATH="/usr/lib/gcc/x86_64-linux-gnu/13:/usr/lib/x86_64-linux-gnu"
ENV LIBRARY_PATH="/usr/lib/gcc/x86_64-linux-gnu/13:/usr/lib/x86_64-linux-gnu"
ENV CPLUS_INCLUDE_PATH="/usr/include/c++/13:/usr/include/x86_64-linux-gnu/c++/13"
ENV C_INCLUDE_PATH="/usr/lib/gcc/x86_64-linux-gnu/13/include"

ENV LDFLAGS="-fuse-ld=lld -L/usr/lib/gcc/x86_64-linux-gnu/13 -L/usr/lib/x86_64-linux-gnu"

# Verify toolchain setup
RUN echo "#include <iostream>\n#include <numbers>\nint main() { std::cout << std::numbers::pi << std::endl; return 0; }" > test.cpp && \
    ${CXX} -std=c++20 test.cpp -o test && \
    ./test && \
    rm test.cpp test

# ───────────────────────────────────────────────────────────────────────────
# aarch64. This container also builds the arm64 lanes, as a cross-compiler: clang --target=aarch64-unknown-linux-gnu
# --sysroot=$SYSROOT_AARCH64.
#
# The sysroot is what an arm64 ubuntu:20.04 with the packages above installed has where clang looks, from the same
# places: glibc 2.31 from focal, and libstdc++ and libgcc from the arm64 half of the gcc-13-focal-debs mirror. Nothing
# here can run arm64 code, so packages are unpacked over the arm64 ubuntu:20.04 image (which brings the merged-/usr
# layout: --keep-directory-symlink keeps /lib -> usr/lib a symlink) rather than installed. The packages' absolute
# symlinks (libc6-dev's libm.so -> /lib/aarch64-linux-gnu/libm.so.6 and the like) are re-pointed into the sysroot:
# left alone they dangle here, and the linker quietly takes libm.a, libpthread.a and libdl.a instead.
#
# The glibc an artifact is built against decides where it runs. It must be the container's own, 2.31: checked here, and
# again on every jsc that is linked (the WebKit step below).
# ───────────────────────────────────────────────────────────────────────────
ENV SYSROOT_AARCH64=/opt/sysroot-aarch64
COPY --from=rootfs-arm64 / ${SYSROOT_AARCH64}/
RUN set -eu; \
    dpkg --add-architecture arm64; \
    find /etc/apt/sources.list /etc/apt/sources.list.d -name '*.list' -exec sed -i 's/^deb http/deb [arch=amd64] http/' {} +; \
    for suite in focal focal-updates focal-security; do \
      echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports $suite main universe"; \
    done > /etc/apt/sources.list.d/arm64.list; \
    apt-get update; \
    mkdir -p /tmp/arm64 && cd /tmp/arm64; \
    apt-get download libc6:arm64 libc6-dev:arm64 linux-libc-dev:arm64 libcrypt1:arm64 libcrypt-dev:arm64; \
    curl -fsSL --retry 5 --retry-connrefused \
        "https://github.com/oven-sh/WebKit/releases/download/gcc-13-focal-debs/gcc-13-focal-arm64.tar.gz" -o gcc13.tar.gz; \
    echo "${GCC13_DEBS_SHA256_arm64}  gcc13.tar.gz" | sha256sum -c -; \
    tar xzf gcc13.tar.gz && rm gcc13.tar.gz; \
    for deb in *.deb; do \
      dpkg-deb --fsys-tarfile "$deb" | tar -xf - -C "$SYSROOT_AARCH64" --keep-directory-symlink; \
    done; \
    find "$SYSROOT_AARCH64" -type l -lname '/*' | while read -r link; do \
      ln -snf "$SYSROOT_AARCH64$(readlink "$link")" "$link"; \
    done; \
    container=$(dpkg-query -W -f='${Version}' libc6:amd64); \
    sysroot=$(dpkg-deb -f libc6_*_arm64.deb Version); \
    echo "glibc: container $container, aarch64 sysroot $sysroot"; \
    [ "${container%%-*}" = 2.31 ] || { echo "error: the container's glibc is not 2.31" >&2; exit 1; }; \
    [ "${sysroot%%-*}" = 2.31 ] || { echo "error: the aarch64 sysroot's glibc is not 2.31" >&2; exit 1; }; \
    test -L "$SYSROOT_AARCH64/lib"; \
    test -f "$SYSROOT_AARCH64/usr/lib/aarch64-linux-gnu/libc.so"; \
    for lib in m pthread dl rt resolv util; do test -e "$SYSROOT_AARCH64/usr/lib/aarch64-linux-gnu/lib$lib.so"; done; \
    test -f "$SYSROOT_AARCH64/usr/lib/gcc/aarch64-linux-gnu/13/libstdc++.a"; \
    test -d "$SYSROOT_AARCH64/usr/include/aarch64-linux-gnu/c++/13"; \
    cd / && rm -rf /tmp/arm64 /var/lib/apt/lists/*

# The sanitizer runtimes for aarch64. An x86_64 LLVM install only has its own; these are the ones the arm64 LLVM
# packages (the arm64 half of the llvm debs mirror) put in the same place.
RUN set -eu; \
    mkdir -p /tmp/llvm-arm64 && cd /tmp/llvm-arm64; \
    curl -fsSL --retry 5 --retry-connrefused \
        "https://github.com/oven-sh/WebKit/releases/download/llvm-${LLVM_VERSION}-debs/llvm-${LLVM_VERSION}-focal-arm64.tar.gz" -o llvm.tar.gz; \
    echo "${LLVM_DEBS_SHA256_arm64}  llvm.tar.gz" | sha256sum -c -; \
    tar xzf llvm.tar.gz --wildcards --no-anchored 'libclang-rt-*-dev_*_arm64.deb' && rm llvm.tar.gz; \
    for deb in $(find . -name 'libclang-rt-*-dev_*_arm64.deb'); do \
      installed=$(dpkg-query -W -f='${Version}' "$(dpkg-deb -f "$deb" Package):amd64"); \
      [ "$(dpkg-deb -f "$deb" Version)" = "$installed" ] || { echo "error: $deb is not version $installed, the one installed here" >&2; exit 1; }; \
      dpkg-deb -x "$deb" unpacked; \
    done; \
    cp -rn unpacked/usr/lib/llvm-${LLVM_VERSION}/lib/clang/. /usr/lib/llvm-${LLVM_VERSION}/lib/clang/; \
    find "$(clang -print-resource-dir)/" -name 'libclang_rt.asan*aarch64*' | grep -q .; \
    cd / && rm -rf /tmp/llvm-arm64

# Verify the cross toolchain: C++20 against the sysroot's libstdc++, plain and with the sanitizers, is an AArch64 ELF.
RUN set -eu; \
    printf '#include <iostream>\n#include <numbers>\nint main() { std::cout << std::numbers::pi << std::endl; return 0; }\n' > /tmp/t.cpp; \
    for san in "" "-fsanitize=address,undefined"; do \
      env -u LIBRARY_PATH -u CPLUS_INCLUDE_PATH -u C_INCLUDE_PATH -u LDFLAGS \
        ${CXX} --target=aarch64-unknown-linux-gnu --sysroot=${SYSROOT_AARCH64} -std=c++20 -fuse-ld=lld $san /tmp/t.cpp -o /tmp/t; \
      llvm-readelf -h /tmp/t | grep -q AArch64; \
      llvm-readelf -d /tmp/t | grep -q 'NEEDED.*libm\.so\.6'; \
    done; \
    rm /tmp/t.cpp /tmp/t

# ICU's sources (/icu.tgz, which the lanes build from) and its tools for this container. ICU runs those while it
# builds (pkgdata, genrb, ...), and the lanes filter and repack its data with icupkg: a lane that builds ICU for
# aarch64 cannot run the ones it builds. LDFLAGS without this stage's -L/usr/lib/x86_64-linux-gnu, as in the lanes' own
# ICU step: that is where the distribution's ICU is, and the tools would be linked against it instead of this one.
# Which ICU: icu/source.json, by way of lanes.mjs.
ARG ICU_VERSION
ARG ICU_SHA256
ADD --checksum=sha256:${ICU_SHA256} https://github.com/unicode-org/icu/releases/download/release-${ICU_VERSION}/icu4c-${ICU_VERSION}-sources.tgz /icu.tgz
RUN mkdir -p /icu-host && cd /icu-host && tar -xf /icu.tgz --strip-components=1 && cd source && \
    CFLAGS="-Os" CXXFLAGS="-Os" LDFLAGS="-fuse-ld=lld" ./configure --disable-shared --enable-static --disable-samples --disable-tests && \
    make -j$(nproc) && test -x bin/icupkg && test -f config/icucross.mk

# What is different about building for one architecture or the other. The lane picks one by LINUX_ARCH.
FROM base as lane-x86_64

ARG MARCH_FLAG
ARG DEFAULT_CFLAGS

ENV CFLAGS="${DEFAULT_CFLAGS} ${MARCH_FLAG} $CFLAGS -stdlib=libstdc++"
ENV CXXFLAGS="${DEFAULT_CFLAGS} ${MARCH_FLAG} $CXXFLAGS -stdlib=libstdc++"

FROM base as lane-aarch64

ARG MARCH_FLAG
ARG DEFAULT_CFLAGS

# `base` points clang at the container's own GCC, headers and libraries, which are x86_64. With a sysroot clang finds
# the aarch64 ones there by itself, the way it does in an arm64 container, where those paths do not exist.
ENV LIBRARY_PATH=""
ENV CPLUS_INCLUDE_PATH=""
ENV C_INCLUDE_PATH=""
ENV LDFLAGS="-fuse-ld=lld"
ENV CFLAGS="--sysroot=${SYSROOT_AARCH64} ${DEFAULT_CFLAGS} ${MARCH_FLAG} $CFLAGS -stdlib=libstdc++"
ENV CXXFLAGS="--sysroot=${SYSROOT_AARCH64} ${DEFAULT_CFLAGS} ${MARCH_FLAG} $CXXFLAGS -stdlib=libstdc++"

# The lane: its settings, then ICU and WebKit built with them.
FROM lane-${LINUX_ARCH} as lane

ARG MARCH_FLAG
ARG WEBKIT_RELEASE_TYPE
ARG LTO_FLAG
ARG RELEASE_FLAGS
ARG DEFAULT_CFLAGS
ARG ENABLE_SANITIZERS
ARG USE_MIMALLOC
ARG USE_EXTERNAL_MIMALLOC
ARG USE_SYSTEM_MALLOC
ARG LINUX_ARCH
ARG ICU_VERSION

ENV LTO_FLAG="${LTO_FLAG}"

# Download and build ICU.
#
# For aarch64 this is a cross build: ICU uses the container's tools (/icu-host) where it would run its own, and so do
# the data filtering and repacking; "$@" is what tells compress-data.ts to assemble for aarch64.
#
# After tar, patch udata.cpp with a per-item decompression hook (a weak extern
# Bun defines; null in ICU's own tools).
#
# After the first `make` (which produces bin/icupkg), filter data/in/icudt<major>l.dat
# to drop converters/translit/stringprep/confusables/unames — Bun has zero
# ucnv_/utrans_/usprep_/uspoof_ consumers — then rebuild.
#
# Most of rbnf/ goes too, but NOT all of it. Nothing in bun calls the
# RuleBasedNumberFormat API, yet ICU reaches rbnf/ on its own: numberingSystems.res
# declares 19 algorithmic numbering systems whose rules live there, and
# SimpleDateFormat applies them via the number overrides CLDR attaches to calendar
# patterns. ja + the japanese calendar forces "y=jpanyear" (smpdtfmt.cpp hardcodes
# it), so dropping rbnf/ja.res makes
# Intl.DateTimeFormat("ja", { calendar: "japanese", year: "numeric" }) throw
# U_MISSING_RESOURCE_ERROR, and zh + chinese carries "d=hanidays".
#
# Only the locales those rulesets name are reachable: root (for the bare
# "%ruleset" descs), ja, zh, zh_Hant. Keeping those five items costs 35 KB raw
# (~8 KB after per-item zstd) instead of the 621 KB the whole tree costs. The
# guard below re-derives that list from the data and fails the build if a CLDR
# bump ever adds a locale we are not keeping.
#
# Finally, repack the filtered .dat with per-item zstd (icu/compress-data.ts).
# Items matching icu/keep-raw.txt stay uncompressed (too expensive to decode lazily).
# The repacked libicudata.a also embeds the trained zstd dictionary.
COPY icu/ /icu-bun/
RUN --mount=type=tmpfs,target=/icu \
    export G=$(if [ -n "${LTO_FLAG:-}" ]; then echo "-g1"; fi) && \
    export CFLAGS="$CFLAGS $G -Os -std=c17 $LTO_FLAG" && \
    export CXXFLAGS="$CXXFLAGS $G -Os -DUCONFIG_NO_LEGACY_CONVERSION=1 -std=c++20 -fno-exceptions $LTO_FLAG -fno-c++-static-destructors " && \
    export LDFLAGS="-fuse-ld=lld " && \
    if [ "$LINUX_ARCH" = aarch64 ]; then \
        ICU_CROSS="--host=aarch64-unknown-linux-gnu --with-cross-build=/icu-host/source"; \
        ICUPKG=/icu-host/source/bin/icupkg; \
        set -- --cc "$CC --target=aarch64-unknown-linux-gnu" --ar llvm-ar; \
    else \
        ICU_CROSS=""; \
        ICUPKG=bin/icupkg; \
        set --; \
    fi && \
    cd /icu && \
    tar -xf /icu.tgz --strip-components=1 && \
    rm /icu.tgz && \
    patch -p1 < /icu-bun/udata-decompress-hook.patch && \
    cd source && \
    ./configure $ICU_CROSS --enable-static --disable-shared --disable-layoutex --disable-layout --with-data-packaging=static --disable-samples --disable-debug --disable-tests --disable-extras --disable-icuio && \
    make -j$(nproc) && \
    mkdir -p /tmp/ns && $ICUPKG -x numberingSystems.res data/in/icudt${ICU_VERSION%%.*}l.dat -d /tmp/ns && \
    stale=$(strings -el /tmp/ns/numberingSystems.res | sed -n 's|^\([A-Za-z_][A-Za-z_]*\)/.*|\1|p' | sort -u | grep -vxE 'ja|zh|zh_Hant' | tr '\n' ' ') && \
    { [ -z "$stale" ] || { echo "rbnf keep-list is stale, also reachable: $stale" >&2; exit 1; }; } && \
    $ICUPKG -l data/in/icudt${ICU_VERSION%%.*}l.dat | grep -E '\.(cnv|spp|cfu)$|^cnvalias\.icu$|^translit/|^rbnf/|^unames\.icu$' | grep -vE '^rbnf/(root|res_index|ja|zh|zh_Hant)\.res$' > data/in/rm.lst && \
    $ICUPKG --auto_toc_prefix -r data/in/rm.lst data/in/icudt${ICU_VERSION%%.*}l.dat data/in/icudt${ICU_VERSION%%.*}l_filtered.dat && \
    mv -f data/in/icudt${ICU_VERSION%%.*}l_filtered.dat data/in/icudt${ICU_VERSION%%.*}l.dat && \
    rm -rf data/out lib/libicudata.a && make -j$(nproc) && \
    make install && cp -r /icu/source/lib/* /output/lib && cp -r /icu/source/i18n/unicode/* /icu/source/common/unicode/* /output/include/unicode && \
    node --experimental-strip-types /icu-bun/compress-data.ts data/in/icudt${ICU_VERSION%%.*}l.dat /output/lib/libicudata.a --skip /icu-bun/keep-raw.txt --icupkg $ICUPKG "$@"

# Copy WebKit source and build.
#
# ICU_ROOT is /output, where the ICU stage put the libraries Bun gets: jsc links the same ones, the repacked
# libicudata.a included, and reads it with the hook in jsc.cpp (BUN_ICU_ZSTD_SOURCE_DIR). That hook names the zstd
# dictionary in the repacked data, so the link fails if CMake finds another ICU (`make install` also left one in
# /usr/local).
COPY . /webkit
WORKDIR /webkit

ENV MARCH_FLAG=${MARCH_FLAG}
ENV RELEASE_FLAGS=${RELEASE_FLAGS}

# After linking, the newest glibc symbol version jsc needs is read off it: that is the oldest glibc the artifact runs
# on, and it must not be past the container's, 2.31.
#
# clang searches C_INCLUDE_PATH (gcc-13's builtin-header dir) before its own
# resource dir, so C TUs including <immintrin.h> (mimalloc static.c, -march=nehalem)
# pick up gcc's incompatible copy. clang ships its own; drop it for this step.
RUN --mount=type=tmpfs,target=/webkitbuild \
    unset C_INCLUDE_PATH && \
    export G=$(if [ -n "${LTO_FLAG:-}" ]; then echo "-g1"; fi) && \
    export CFLAGS="$CFLAGS $G $LTO_FLAG -ffile-prefix-map=/webkit/Source=vendor/WebKit/Source  -ffile-prefix-map=/webkitbuild/=. " && \
    export CXXFLAGS="$CXXFLAGS $G $LTO_FLAG -fno-c++-static-destructors -ffile-prefix-map=/webkit/Source=vendor/WebKit/Source -ffile-prefix-map=/webkitbuild/=. " && \
    export ENABLE_ASSERTS="AUTO" && \
    export LDFLAGS="-fuse-ld=lld $LDFLAGS " && \
    if [ -n "$ENABLE_SANITIZERS" ]; then \
        export ENABLE_ASSERTS="ON"; \
    fi && \
    # Programs (perl, ruby, python) are the container's, never the sysroot's: those are aarch64 and cannot run here.
    CROSS_CMAKE="" && \
    if [ "$LINUX_ARCH" = aarch64 ]; then \
        CROSS_CMAKE="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_SYSROOT=$SYSROOT_AARCH64 -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH"; \
    fi && \
    cd /webkitbuild && \
    cmake $CROSS_CMAKE \
    -DPORT="JSCOnly" \
    -DENABLE_STATIC_JSC=ON \
    -DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON \
    -DCMAKE_BUILD_TYPE=$WEBKIT_RELEASE_TYPE \
    -DUSE_THIN_ARCHIVES=OFF \
    -DUSE_BUN_JSC_ADDITIONS=ON \
    -DUSE_BUN_EVENT_LOOP=ON \
    -DENABLE_FTL_JIT=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DALLOW_LINE_AND_COLUMN_NUMBER_IN_BUILTINS=ON \
    -DENABLE_REMOTE_INSPECTOR=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_AR=$(which llvm-ar) \
    -DCMAKE_RANLIB=$(which llvm-ranlib) \
    -DCMAKE_C_FLAGS="$CFLAGS" \
    -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
    -DCMAKE_C_FLAGS_RELEASE="$RELEASE_FLAGS" \
    -DCMAKE_CXX_FLAGS_RELEASE="$RELEASE_FLAGS" \
    -DICU_ROOT=/output \
    -DBUN_ICU_ZSTD_SOURCE_DIR=/zstd/lib \
    -DENABLE_SANITIZERS="$ENABLE_SANITIZERS" \
    -DENABLE_ASSERTS="$ENABLE_ASSERTS" \
    -DUSE_MIMALLOC="$USE_MIMALLOC" \
    -DUSE_EXTERNAL_MIMALLOC="$USE_EXTERNAL_MIMALLOC" \
    -DUSE_SYSTEM_MALLOC="$USE_SYSTEM_MALLOC" \
    -G Ninja \
    /webkit && \
    cd /webkitbuild && \
    cmake --build /webkitbuild --config $WEBKIT_RELEASE_TYPE --target "jsc" --target "testFFI" && \
    python3 /webkit/Tools/Scripts/check-classinfo-uniqueness.py $WEBKIT_OUT_DIR/bin/jsc && \
    llvm-readelf -h $WEBKIT_OUT_DIR/bin/jsc | grep Machine: && \
    llvm-readelf -h $WEBKIT_OUT_DIR/bin/jsc | grep -q "Machine:.*$(if [ "$LINUX_ARCH" = aarch64 ]; then echo AArch64; else echo X86-64; fi)" && \
    llvm-readelf -d $WEBKIT_OUT_DIR/bin/jsc | grep NEEDED && \
    llvm-readelf -d $WEBKIT_OUT_DIR/bin/jsc | grep -q 'NEEDED.*libm\.so\.6' && \
    glibc=$(llvm-readelf --version-info $WEBKIT_OUT_DIR/bin/jsc | grep -o 'GLIBC_[0-9][0-9.]*' | sort -uV | tail -1) && \
    echo "jsc needs glibc symbols up to $glibc" && \
    { [ -n "$glibc" ] && [ "$(printf '%s\n' GLIBC_2.31 "$glibc" | sort -V | tail -1)" = GLIBC_2.31 ] || { echo "error: that is newer than GLIBC_2.31" >&2; exit 1; }; } && \
    cp -r $WEBKIT_OUT_DIR/lib/*.a /output/lib && \
    cp $WEBKIT_OUT_DIR/*.h /output/include && \
    cp -r $WEBKIT_OUT_DIR/bin /output/bin && \
    cp $WEBKIT_OUT_DIR/*.json /output && \
    find $WEBKIT_OUT_DIR/JavaScriptCore/DerivedSources/ -name "*.h" -exec sh -c 'cp "$1" "/output/include/JavaScriptCore/$(basename "$1")"' sh {} \; && \
    find $WEBKIT_OUT_DIR/JavaScriptCore/DerivedSources/ -name "*.json" -exec sh -c 'cp "$1" "/output/$(basename "$1")"' sh {} \; && \
    find $WEBKIT_OUT_DIR/JavaScriptCore/Headers/JavaScriptCore/ -name "*.h" -exec cp {} /output/include/JavaScriptCore/ \; && \
    find $WEBKIT_OUT_DIR/JavaScriptCore/PrivateHeaders/JavaScriptCore/ -name "*.h" -exec cp {} /output/include/JavaScriptCore/ \; && \
    cp -r $WEBKIT_OUT_DIR/WTF/Headers/wtf/ /output/include && \
    cp -r $WEBKIT_OUT_DIR/bmalloc/Headers/bmalloc/ /output/include && \
    mkdir -p /output/Source/JavaScriptCore && \
    cp -r /webkit/Source/JavaScriptCore/Scripts /output/Source/JavaScriptCore && \
    cp /webkit/Source/JavaScriptCore/create_hash_table /output/Source/JavaScriptCore

FROM scratch as artifact

COPY --from=lane /output /