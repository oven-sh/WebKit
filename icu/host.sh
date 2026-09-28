#!/bin/sh
# Run in a Dockerfile's `base` stage, with the container's own compiler and flags in the environment.
#
# Makes, of /icu.tgz, what of ICU is the same for every lane:
#   /icudt.dat  Its data, which ICU's own compilers make of its text sources, as bun/data/icu-data.ts there has them do.
#   /icu-host   Those compilers, built for this container, and nothing else of ICU. config/icucross.* is what ICU's
#               configure asks of a build for the machine it runs on before it configures one for another.
set -eu
mkdir -p /icu-host
cd /icu-host
tar -xf /icu.tgz --strip-components=1
cd icu4c/source
./configure --disable-shared --enable-static --disable-samples --disable-tests --disable-extras --disable-icuio --disable-layoutex
mkdir -p lib bin
for dir in stubdata common i18n tools/toolutil tools/gencnval tools/genbrk tools/gendict tools/genrb; do
    make -j"$(nproc)" -C $dir
done
make config/icucross.mk config/icucross.inc
node --experimental-strip-types /icu-host/bun/data/icu-data.ts --tools "$PWD/bin" --work /tmp/icu-data --out /icudt.dat
rm -rf /tmp/icu-data lib/*.a stubdata/*.a
find . \( -name '*.o' -o -name '*.ao' -o -name '*.d' \) -delete
