/*
 * Copyright (c) 1996, David Mazieres <dm@uun.org>
 * Copyright (c) 2008, Damien Miller <djm@openbsd.org>
 * Copyright (C) 2017 Apple Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Arc4 random number generator for OpenBSD.
 *
 * This code is derived from section 17.1 of Applied Cryptography,
 * second edition, which describes a stream cipher allegedly
 * compatible with RSA Labs "RC4" cipher (the actual description of
 * which is a trade secret).  The same algorithm is used as a stream
 * cipher called "arcfour" in Tatu Ylonen's ssh package.
 *
 * RC4 is a registered trademark of RSA Laboratories.
 */

#include "CryptoRandom.h"

#include "BAssert.h"
#include "BPlatform.h"
#include "Mutex.h"
#include "StaticPerProcess.h"
#include "VMAllocate.h"

#if !BOS(DARWIN) && !BOS(WINDOWS)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if BOS(LINUX)
#include <sys/syscall.h>
#if defined(SYS_getrandom)
#define CRYPTO_RANDOM_USE_GETRANDOM 1
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif
#endif
#endif
#ifndef CRYPTO_RANDOM_USE_GETRANDOM
#define CRYPTO_RANDOM_USE_GETRANDOM 0
#endif

#if BOS(DARWIN)
#include <CommonCrypto/CommonCryptoError.h>
#include <CommonCrypto/CommonRandom.h>
#endif

namespace bmalloc {

class ARC4Stream {
public:
    ARC4Stream();

    uint8_t i;
    uint8_t j;
    uint8_t s[256];
};

class ARC4RandomNumberGenerator : public StaticPerProcess<ARC4RandomNumberGenerator> {
public:
    ARC4RandomNumberGenerator(const LockHolder&);

    uint32_t randomNumber();
    void randomValues(void* buffer, size_t length);
    void setUsesGetrandom(bool);

private:
    inline void addRandomData(unsigned char *data, int length);
    void stir();
    void stirIfNeeded();
    inline uint8_t getByte();
#if !BOS(DARWIN) && !BOS(WINDOWS)
    void chooseSource();
#endif

    ARC4Stream m_stream;
    int m_count;
#if !BOS(DARWIN) && !BOS(WINDOWS)
    // /dev/urandom, or -1 while getrandom(2) is the source (or before chooseSource() ran).
    int m_fd { -1 };
    bool m_sourceChosen { false };
#if CRYPTO_RANDOM_USE_GETRANDOM
    bool m_usesGetrandom { true };
#endif
#endif
};
BALLOW_DEPRECATED_DECLARATIONS_BEGIN
DECLARE_STATIC_PER_PROCESS_STORAGE_WITH_LINKAGE(ARC4RandomNumberGenerator, BNOEXPORT);
BALLOW_DEPRECATED_DECLARATIONS_END
DEFINE_STATIC_PER_PROCESS_STORAGE(ARC4RandomNumberGenerator);

ARC4Stream::ARC4Stream()
{
    for (int n = 0; n < 256; n++)
        s[n] = n;
    i = 0;
    j = 0;
}

ARC4RandomNumberGenerator::ARC4RandomNumberGenerator(const LockHolder&)
    : m_count(0)
{
}

void ARC4RandomNumberGenerator::addRandomData(unsigned char* data, int length)
{
    m_stream.i--;
    for (int n = 0; n < 256; n++) {
        m_stream.i++;
        uint8_t si = m_stream.s[m_stream.i];
        m_stream.j += si + data[n % length];
        m_stream.s[m_stream.i] = m_stream.s[m_stream.j];
        m_stream.s[m_stream.j] = si;
    }
    m_stream.j = m_stream.i;
}

#if !BOS(DARWIN) && !BOS(WINDOWS)
// The caller holds mutex(). Runs before the first stir() and again from
// setUsesGetrandom(), so it has to be able to switch in both directions.
void ARC4RandomNumberGenerator::chooseSource()
{
    m_sourceChosen = true;
#if CRYPTO_RANDOM_USE_GETRANDOM
    if (m_usesGetrandom) {
        // Prefer getrandom(2): same kernel pool as /dev/urandom, but needs no
        // filesystem access and keeps no fd open. Probe once with GRND_NONBLOCK;
        // if the pool is already initialized use getrandom from here on (it can
        // never become uninitialized again). Otherwise -- EAGAIN (pool not ready,
        // where /dev/urandom would return without blocking), ENOSYS (pre-3.17
        // kernel) or EPERM (seccomp) -- keep the historical /dev/urandom behaviour.
        uint8_t probe;
        long probeResult;
        do {
            probeResult = syscall(SYS_getrandom, &probe, sizeof(probe), GRND_NONBLOCK);
        } while (probeResult == -1 && errno == EINTR);
        if (probeResult == 1) {
            if (m_fd >= 0) {
                close(m_fd);
                m_fd = -1;
            }
            return;
        }
    }
#endif
    if (m_fd >= 0)
        return;
    int ret = 0;
    do {
        ret = open("/dev/urandom", O_RDONLY | O_CLOEXEC, 0);
    } while (ret == -1 && errno == EINTR);
    RELEASE_BASSERT(ret >= 0);
    m_fd = ret;
}
#endif

void ARC4RandomNumberGenerator::stir()
{
    unsigned char randomness[128];
    size_t length = sizeof(randomness);

#if BOS(DARWIN)
    RELEASE_BASSERT(!CCRandomGenerateBytes(randomness, length));
#elif BOS(WINDOWS)
    // TODO Generate random bytes - this appears to be unused when running libpas
    BCRASH();
#else
    if (!m_sourceChosen)
        chooseSource();
    ssize_t amountRead = 0;
    while (static_cast<size_t>(amountRead) < length) {
        ssize_t currentRead;
#if CRYPTO_RANDOM_USE_GETRANDOM
        if (m_fd < 0)
            currentRead = syscall(SYS_getrandom, randomness + amountRead, length - amountRead, 0);
        else
#endif
            currentRead = read(m_fd, randomness + amountRead, length - amountRead);
        // We need to check for both EAGAIN and EINTR since on some systems /dev/urandom
        // is blocking and on others it is non-blocking.
        if (currentRead == -1)
            RELEASE_BASSERT(errno == EAGAIN || errno == EINTR);
        else
            amountRead += currentRead;
    }
#endif

    addRandomData(randomness, length);

    // Discard early keystream, as per recommendations in:
    // http://www.wisdom.weizmann.ac.il/~itsik/RC4/Papers/Rc4_ksa.ps
    for (int i = 0; i < 256; i++)
        getByte();
    m_count = 1600000;
}

void ARC4RandomNumberGenerator::stirIfNeeded()
{
    if (m_count <= 0)
        stir();
}

uint8_t ARC4RandomNumberGenerator::getByte()
{
    m_stream.i++;
    uint8_t si = m_stream.s[m_stream.i];
    m_stream.j += si;
    uint8_t sj = m_stream.s[m_stream.j];
    m_stream.s[m_stream.i] = sj;
    m_stream.s[m_stream.j] = si;
    return (m_stream.s[(si + sj) & 0xff]);
}

void ARC4RandomNumberGenerator::randomValues(void* buffer, size_t length)
{
    LockHolder lock(mutex());

    unsigned char* result = reinterpret_cast<unsigned char*>(buffer);
    stirIfNeeded();
    while (length--) {
        m_count--;
        stirIfNeeded();
        result[length] = getByte();
    }
}

void ARC4RandomNumberGenerator::setUsesGetrandom(bool usesGetrandom)
{
#if CRYPTO_RANDOM_USE_GETRANDOM
    LockHolder lock(mutex());
    if (m_usesGetrandom == usesGetrandom)
        return;
    m_usesGetrandom = usesGetrandom;
    // Switch now rather than at the next stir(), so that a /dev/urandom that
    // cannot be opened fails at the call that asked for it.
    if (m_sourceChosen)
        chooseSource();
#else
    BUNUSED_PARAM(usesGetrandom);
#endif
}

void cryptoRandom(void* buffer, size_t length)
{
    ARC4RandomNumberGenerator::get()->randomValues(buffer, length);
}

void setCryptoRandomUsesGetrandom(bool usesGetrandom)
{
    ARC4RandomNumberGenerator::get()->setUsesGetrandom(usesGetrandom);
}

} // namespace bmalloc

