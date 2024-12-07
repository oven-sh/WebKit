ARG MARCH_FLAG=""
ARG WEBKIT_RELEASE_TYPE=Release
ARG CPU=native
ARG LTO_FLAG="-flto=full -fwhole-program-vtables -fforce-emit-vtables "
ARG RELEASE_FLAGS="-O2 -DNDEBUG=1"
ARG LLVM_VERSION="18"
ARG DEFAULT_CFLAGS="-mno-omit-leaf-frame-pointer -fno-omit-frame-pointer -ffunction-sections -fdata-sections -faddrsig -fno-unwind-tables -fno-asynchronous-unwind-tables -DU_STATIC_IMPLEMENTATION=1 "

FROM ubuntu:18.04

ARG MARCH_FLAG
ARG WEBKIT_RELEASE_TYPE
ARG CPU
ARG LTO_FLAG
ARG RELEASE_FLAGS
ARG LLVM_VERSION
ARG DEFAULT_CFLAGS

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install basic build dependencies
RUN apt-get update && apt-get install -y \
    wget \
    curl \
    git \
    python3 \
    python3-pip \
    ninja-build \
    software-properties-common \
    apt-transport-https \
    ca-certificates \
    gnupg \
    lsb-release \
    && rm -rf /var/lib/apt/lists/*

# Install modern CMake
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null \
    && apt-add-repository 'deb https://apt.kitware.com/ubuntu/ bionic main' \
    && apt-get update \
    && apt-get install -y cmake \
    && rm -rf /var/lib/apt/lists/*

# Install GCC 13 toolchain
RUN add-apt-repository ppa:ubuntu-toolchain-r/test \
    && apt-get update \
    && apt-get install -y \
        gcc-13 \
        g++-13 \
        libgcc-13-dev \
        libstdc++-13-dev \
        libasan6 \
        libubsan1 \
        libatomic1 \
        libtsan0 \
        liblsan0 \
        libquadmath0 \
        libgfortran5 \
        lib32gcc-13-dev \
        libc6-dev \
    && rm -rf /var/lib/apt/lists/*

# Ensure GCC 13 is the default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 130 \
    --slave /usr/bin/g++ g++ /usr/bin/g++-13 \
    --slave /usr/bin/gcc-ar gcc-ar /usr/bin/gcc-ar-13 \
    --slave /usr/bin/gcc-nm gcc-nm /usr/bin/gcc-nm-13 \
    --slave /usr/bin/gcc-ranlib gcc-ranlib /usr/bin/gcc-ranlib-13

# Configure library paths
RUN mkdir -p /usr/lib/gcc/x86_64-linux-gnu/13 && \
    ln -sf /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /usr/lib/gcc/x86_64-linux-gnu/13/ && \
    echo "/usr/lib/gcc/x86_64-linux-gnu/13" > /etc/ld.so.conf.d/gcc-13.conf && \
    echo "/usr/lib/x86_64-linux-gnu" >> /etc/ld.so.conf.d/gcc-13.conf && \
    ldconfig

# Install LLVM 18
RUN wget https://apt.llvm.org/llvm.sh && chmod +x llvm.sh && ./llvm.sh 18 all \
    && rm llvm.sh \
    && rm -rf /var/lib/apt/lists/*

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

ENV CFLAGS="${DEFAULT_CFLAGS} $CFLAGS -stdlib=libstdc++"
ENV CXXFLAGS="${DEFAULT_CFLAGS} $CXXFLAGS -stdlib=libstdc++"
ENV LDFLAGS="-fuse-ld=lld -L/usr/lib/gcc/x86_64-linux-gnu/13 -L/usr/lib/x86_64-linux-gnu"

# Verify toolchain setup
RUN echo "#include <iostream>\n#include <numbers>\nint main() { std::cout << std::numbers::pi << std::endl; return 0; }" > test.cpp && \
    ${CXX} -std=c++20 test.cpp -o test && \
    ./test && \
    rm test.cpp test

# Download and build ICU
ADD https://github.com/unicode-org/icu/releases/download/release-75-1/icu4c-75_1-src.tgz /icu.tgz
RUN --mount=type=tmpfs,target=/icu \
    export CFLAGS="$CFLAGS -Os -std=c17 $LTO_FLAG" && \
    export CXXFLAGS="$CXXFLAGS -Os -DUCONFIG_NO_LEGACY_CONVERSION=1 -std=c++20 -fno-exceptions $LTO_FLAG -fno-c++-static-destructors " && \
    export LDFLAGS="-fuse-ld=lld " && \
    cd /icu && \
    tar -xf /icu.tgz --strip-components=1 && \
    rm /icu.tgz && \
    cd source && \
    ./configure --enable-static --disable-shared --disable-layoutex --disable-layout --with-data-packaging=static --disable-samples --disable-debug --disable-tests --disable-extras --disable-icuio && \
    make -j$(nproc) && \
    make install && cp -r /icu/source/lib/* /output/lib && cp -r /icu/source/i18n/unicode/* /icu/source/common/unicode/* /output/include/unicode

# Copy WebKit source and build
COPY . /webkit
WORKDIR /webkit

ENV CPU=${CPU}
ENV MARCH_FLAG=${MARCH_FLAG}
ENV RELEASE_FLAGS=${RELEASE_FLAGS}

RUN --mount=type=tmpfs,target=/webkitbuild \
    export CFLAGS="$CFLAGS $LTO_FLAG -ffile-prefix-map=/webkit/Source=vendor/WebKit/Source  -ffile-prefix-map=/webkitbuild/=. " && \
    export CXXFLAGS="$CXXFLAGS $LTO_FLAG -fno-c++-static-destructors -ffile-prefix-map=/webkit/Source=vendor/WebKit/Source -ffile-prefix-map=/webkitbuild/=. " && \
    export LDFLAGS="-fuse-ld=lld $LDFLAGS " && \
    cd /webkitbuild && \
    cmake \
    -DPORT="JSCOnly" \
    -DENABLE_STATIC_JSC=ON \
    -DENABLE_BUN_SKIP_FAILING_ASSERTIONS=ON \
    -DCMAKE_BUILD_TYPE=$WEBKIT_RELEASE_TYPE \
    -DUSE_THIN_ARCHIVES=OFF \
    -DUSE_BUN_JSC_ADDITIONS=ON \
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
COPY --from=0 /output /