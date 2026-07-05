ARG MARCH_FLAG=""
ARG WEBKIT_RELEASE_TYPE=Release
ARG CPU=native
ARG LTO_FLAG="-flto=full -fwhole-program-vtables -fforce-emit-vtables "
ARG RELEASE_FLAGS="-O3 -DNDEBUG=1"
ARG LLVM_VERSION="21"
ARG DEFAULT_CFLAGS="-mno-omit-leaf-frame-pointer -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -faddrsig -fno-unwind-tables -fno-asynchronous-unwind-tables -DU_STATIC_IMPLEMENTATION=1 "
ARG ENABLE_SANITIZERS=""

# Use different base images for ARM64 vs x86_64
FROM --platform=$BUILDPLATFORM ubuntu:20.04 as base-arm64
FROM --platform=$BUILDPLATFORM ubuntu:20.04 as base-amd64
FROM base-$TARGETARCH as base

ARG MARCH_FLAG
ARG WEBKIT_RELEASE_TYPE
ARG CPU
ARG LTO_FLAG
ARG RELEASE_FLAGS
ARG LLVM_VERSION
ARG DEFAULT_CFLAGS
ARG TARGETARCH
ARG ENABLE_SANITIZERS

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Both archive.ubuntu.com and azure.archive.ubuntu.com have intermittently
# timed out from inside the GitHub-hosted docker-buildx network at different
# times. Prefer Azure (faster on Azure-hosted runners) but fall back to the
# canonical mirror if `apt-get update` can't reach it. arm64 uses
# ports.ubuntu.com which has been reachable, so leave it alone.
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
ARG ZSTD_VERSION=1.5.7
RUN curl -fsSL "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" | tar xz -C /tmp \
    && make -C /tmp/zstd-${ZSTD_VERSION}/programs zstd -j$(nproc) \
    && cp /tmp/zstd-${ZSTD_VERSION}/programs/zstd /usr/local/bin/ \
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
        "https://github.com/oven-sh/WebKit/releases/download/gcc-13-focal-debs/gcc-13-focal-${TARGETARCH}.tar.gz" \
        -o /tmp/gcc13.tar.gz \
    && eval "expected=\$GCC13_DEBS_SHA256_${TARGETARCH}" \
    && echo "${expected}  /tmp/gcc13.tar.gz" | sha256sum -c - \
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

# Install LLVM 21
RUN wget https://apt.llvm.org/llvm.sh \
    && chmod +x llvm.sh \
    && ./llvm.sh 21 all \
    && rm llvm.sh \
    && rm -rf /var/lib/apt/lists/*

# Configure library paths
RUN if [ "$TARGETARCH" = "arm64" ]; then \
        export ARCH_PATH="aarch64-linux-gnu"; \
    else \
        export ARCH_PATH="x86_64-linux-gnu"; \
    fi \
    && mkdir -p /usr/lib/gcc/${ARCH_PATH}/13 \
    && ln -sf /usr/lib/${ARCH_PATH}/libstdc++.so.6 /usr/lib/gcc/${ARCH_PATH}/13/ \
    && echo "/usr/lib/gcc/${ARCH_PATH}/13" > /etc/ld.so.conf.d/gcc-13.conf \
    && echo "/usr/lib/${ARCH_PATH}" >> /etc/ld.so.conf.d/gcc-13.conf \
    && ldconfig

# Set up LLVM toolchain symlinks
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


# Set up architecture-specific library paths
RUN if [ "$(uname -m)" = "aarch64" ]; then \
        export ARCH_PATH="aarch64-linux-gnu"; \
    else \
        export ARCH_PATH="x86_64-linux-gnu"; \
    fi \
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

# Set up LLVM toolchain symlinks
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
ENV LTO_FLAG="${LTO_FLAG}"
ENV LD_LIBRARY_PATH="/usr/lib/gcc/x86_64-linux-gnu/13:/usr/lib/x86_64-linux-gnu"
ENV LIBRARY_PATH="/usr/lib/gcc/x86_64-linux-gnu/13:/usr/lib/x86_64-linux-gnu"
ENV CPLUS_INCLUDE_PATH="/usr/include/c++/13:/usr/include/x86_64-linux-gnu/c++/13"
ENV C_INCLUDE_PATH="/usr/lib/gcc/x86_64-linux-gnu/13/include"

ENV CFLAGS="${DEFAULT_CFLAGS} ${MARCH_FLAG} $CFLAGS -stdlib=libstdc++"
ENV CXXFLAGS="${DEFAULT_CFLAGS} ${MARCH_FLAG} $CXXFLAGS -stdlib=libstdc++"
ENV LDFLAGS="-fuse-ld=lld -L/usr/lib/gcc/x86_64-linux-gnu/13 -L/usr/lib/x86_64-linux-gnu"

# Verify toolchain setup
RUN echo "#include <iostream>\n#include <numbers>\nint main() { std::cout << std::numbers::pi << std::endl; return 0; }" > test.cpp && \
    ${CXX} -std=c++20 test.cpp -o test && \
    ./test && \
    rm test.cpp test

# Download and build ICU.
#
# After tar, patch udata.cpp with a per-item decompression hook (a weak extern
# Bun defines; null in ICU's own tools).
#
# After the first `make` (which produces bin/icupkg), filter data/in/icudt75l.dat
# to drop converters/translit/rbnf/stringprep/confusables/unames — Bun has zero
# ucnv_/utrans_/usprep_/uspoof_ consumers — plus the individually-unreachable
# items in icu/remove-items.txt, then rebuild. The `test` asserts every listed
# name still exists in the upstream .dat so an ICU upgrade can't silently
# re-ship one (or leave a stale name behind).
#
# Finally, repack the filtered .dat with per-item zstd (icu/compress-data.ts).
# Items matching icu/keep-raw.txt stay uncompressed (too expensive to decode lazily).
# The repacked libicudata.a also embeds the trained zstd dictionary.
COPY icu/ /icu-bun/
ADD https://github.com/unicode-org/icu/releases/download/release-75-1/icu4c-75_1-src.tgz /icu.tgz
RUN --mount=type=tmpfs,target=/icu \
    export CFLAGS="$CFLAGS -Os -std=c17 $LTO_FLAG" && \
    export CXXFLAGS="$CXXFLAGS -Os -DUCONFIG_NO_LEGACY_CONVERSION=1 -std=c++20 -fno-exceptions $LTO_FLAG -fno-c++-static-destructors " && \
    export LDFLAGS="-fuse-ld=lld " && \
    cd /icu && \
    tar -xf /icu.tgz --strip-components=1 && \
    rm /icu.tgz && \
    patch -p1 < /icu-bun/udata-decompress-hook.patch && \
    patch -p1 < /icu-bun/ucmndata-toc.patch && \
    cd source && \
    ./configure --enable-static --disable-shared --disable-layoutex --disable-layout --with-data-packaging=static --disable-samples --disable-debug --disable-tests --disable-extras --disable-icuio && \
    make -j$(nproc) && \
    bin/icupkg -l data/in/icudt75l.dat > data/in/all.lst && \
    grep -vE '^#|^$' /icu-bun/remove-items.txt > data/in/rm-named.lst && \
    test "$(grep -Fxc -f data/in/rm-named.lst data/in/all.lst)" = "$(grep -c . data/in/rm-named.lst)" && \
    { grep -E '\.(cnv|spp|cfu)$|^cnvalias\.icu$|^translit/|^rbnf/|^unames\.icu$' data/in/all.lst; grep -Fx -f data/in/rm-named.lst data/in/all.lst; } > data/in/rm.lst && \
    bin/icupkg --auto_toc_prefix -r data/in/rm.lst data/in/icudt75l.dat data/in/icudt75l_filtered.dat && \
    mv -f data/in/icudt75l_filtered.dat data/in/icudt75l.dat && \
    rm -rf data/out lib/libicudata.a && make -j$(nproc) && \
    make install && cp -r /icu/source/lib/* /output/lib && cp -r /icu/source/i18n/unicode/* /icu/source/common/unicode/* /output/include/unicode && \
    node --experimental-strip-types /icu-bun/compress-data.ts data/in/icudt75l.dat /output/lib/libicudata.a --skip /icu-bun/keep-raw.txt --icupkg bin/icupkg

# Copy WebKit source and build
COPY . /webkit
WORKDIR /webkit

ENV CPU=${CPU}
ENV MARCH_FLAG=${MARCH_FLAG}
ENV RELEASE_FLAGS=${RELEASE_FLAGS}

RUN --mount=type=tmpfs,target=/webkitbuild \
    export CFLAGS="$CFLAGS $LTO_FLAG -ffile-prefix-map=/webkit/Source=vendor/WebKit/Source  -ffile-prefix-map=/webkitbuild/=. " && \
    export CXXFLAGS="$CXXFLAGS $LTO_FLAG -fno-c++-static-destructors -ffile-prefix-map=/webkit/Source=vendor/WebKit/Source -ffile-prefix-map=/webkitbuild/=. " && \
    export ENABLE_ASSERTS="AUTO" && \
    export LDFLAGS="-fuse-ld=lld $LDFLAGS " && \
    if [ -n "$ENABLE_SANITIZERS" ]; then \
        export ENABLE_ASSERTS="ON"; \
    fi && \
    cd /webkitbuild && \
    cmake \
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
    -DICU_ROOT=/icu \
    -DENABLE_SANITIZERS="$ENABLE_SANITIZERS" \
    -DENABLE_ASSERTS="$ENABLE_ASSERTS" \
    -G Ninja \
    /webkit && \
    cd /webkitbuild && \
    cmake --build /webkitbuild --config $WEBKIT_RELEASE_TYPE --target "jsc" && \
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
ARG TARGETARCH

COPY --from=base /output /