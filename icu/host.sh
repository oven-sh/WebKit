#!/bin/sh
# Run in a Dockerfile's `base` stage, with the container's own compiler and flags in the environment.
#
# Makes, of /icu.tgz, what of ICU is the same for every lane:
#   /icudt.dat  Its data, which ICU's own compilers make of its text sources, as bun/data/icu-data.ts there has them do.
#               It is the same whatever makes it, so which bytes it is (ICU_DATA_SHA256) is known beforehand.
#   /icu-host   config/icucross.*, which is what ICU's configure asks of a build for the machine it runs on
#               before it configures one for another.
set -eu
mkdir -p /tmp/icu-host
cd /tmp/icu-host
tar -xf /icu.tgz --strip-components=1
cd icu4c/source
./configure --disable-shared --enable-static --disable-samples --disable-tests --disable-extras --disable-icuio --disable-layoutex
mkdir -p lib bin
for dir in stubdata common i18n tools/toolutil tools/gencnval tools/genbrk tools/gendict tools/genrb; do
    make -j"$(nproc)" -C $dir
done
make config/icucross.mk config/icucross.inc
node --experimental-strip-types /tmp/icu-host/bun/data/icu-data.ts --tools "$PWD/bin" --work /tmp/icu-data --out /icudt.dat
# Said first: the check says only that it failed, and a new commit of ICU needs the new one.
sha256sum /icudt.dat
echo "$ICU_DATA_SHA256  /icudt.dat" | sha256sum -c -
mkdir -p /icu-host/config
cp config/icucross.mk config/icucross.inc /icu-host/config
rm -rf /tmp/icu-data /tmp/icu-host
