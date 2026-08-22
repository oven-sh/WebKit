/*
 * Copyright (C) 2011 Google Inc.
 * Copyright (C) 2017 Yusuke Suzuki <utatane.tea@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY GOOGLE, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/RandomDevice.h>

#include <atomic>
#include <stdlib.h>

#if !OS(DARWIN) && !OS(FUCHSIA) && OS(UNIX)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if OS(LINUX)
#include <sys/syscall.h>
#if defined(SYS_getrandom)
#define RANDOM_DEVICE_USE_GETRANDOM 1
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif
#endif
#endif
#ifndef RANDOM_DEVICE_USE_GETRANDOM
#define RANDOM_DEVICE_USE_GETRANDOM 0
#endif

#if OS(WINDOWS)
#include <windows.h>
#endif

#if OS(DARWIN)
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonRandom.h>
#endif

#if OS(FUCHSIA)
#include <zircon/syscalls.h>
#endif

namespace WTF {

#if OS(WINDOWS)
// ProcessPrng (bcryptprimitives.dll) is the primitive that BCryptGenRandom and
// RtlGenRandom bottom out in; calling it directly avoids loading the CNG/CryptoAPI
// provider stacks on first use. It has no import library, so resolve it once.
static void processPrng(std::span<uint8_t> buffer)
{
    using ProcessPrngFunction = BOOL (WINAPI*)(PBYTE, SIZE_T);
    static const ProcessPrngFunction function = [] {
        ProcessPrngFunction result = nullptr;
        if (HMODULE module = LoadLibraryExW(L"bcryptprimitives.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            result = reinterpret_cast<ProcessPrngFunction>(GetProcAddress(module, "ProcessPrng"));
        if (!result)
            CRASH();
        return result;
    }();
    // Documented to always return TRUE; checked to stay fail-closed regardless.
    if (!function(buffer.data(), buffer.size()))
        CRASH();
}
#endif

#if !OS(DARWIN) && !OS(FUCHSIA) && OS(UNIX)
NEVER_INLINE NO_RETURN_DUE_TO_CRASH static void crashUnableToOpenURandom()
{
    CRASH();
}

NEVER_INLINE NO_RETURN_DUE_TO_CRASH static void crashUnableToReadFromURandom()
{
    CRASH();
}
#endif

#if RANDOM_DEVICE_USE_GETRANDOM
static std::atomic<bool> s_useGetrandom { true };
#endif

void RandomDevice::setUseGetrandom(bool use)
{
#if RANDOM_DEVICE_USE_GETRANDOM
    s_useGetrandom.store(use, std::memory_order_relaxed);
#else
    UNUSED_PARAM(use);
#endif
}

#if !OS(DARWIN) && !OS(FUCHSIA) && !OS(WINDOWS)
RandomDevice::RandomDevice()
{
#if RANDOM_DEVICE_USE_GETRANDOM
    // Prefer getrandom(2): same kernel pool as /dev/urandom, but needs no
    // filesystem access and keeps no fd open. Probe once with GRND_NONBLOCK; if
    // the pool is already initialized use getrandom from here on (it can never
    // become uninitialized again). Otherwise -- EAGAIN (pool not ready, where
    // /dev/urandom would return without blocking), ENOSYS (pre-3.17 kernel) or
    // EPERM (seccomp) -- keep the historical /dev/urandom behaviour, which
    // setUseGetrandom(false) (JSC's useGetrandom=false) also selects outright.
    if (s_useGetrandom.load(std::memory_order_relaxed)) {
        uint8_t probe;
        long probeResult;
        do {
            probeResult = syscall(SYS_getrandom, &probe, sizeof(probe), GRND_NONBLOCK);
        } while (probeResult == -1 && errno == EINTR);
        if (probeResult == 1)
            return; // m_fd stays -1: cryptographicallyRandomValues() uses getrandom.
    }
#endif
    int ret = 0;
    do {
        ret = open("/dev/urandom", O_RDONLY | O_CLOEXEC, 0);
    } while (ret == -1 && errno == EINTR);
    m_fd = ret;
    if (m_fd < 0)
        crashUnableToOpenURandom(); // We need /dev/urandom for this API to work...
}
#endif

#if !OS(DARWIN) && !OS(FUCHSIA) && !OS(WINDOWS)
RandomDevice::~RandomDevice()
{
    if (m_fd >= 0)
        close(m_fd);
}
#endif

// FIXME: Make this call fast by creating the pool in RandomDevice.
// https://bugs.webkit.org/show_bug.cgi?id=170190
void RandomDevice::cryptographicallyRandomValues(std::span<uint8_t> buffer)
{
#if OS(DARWIN)
    RELEASE_ASSERT(!CCRandomGenerateBytes(buffer.data(), buffer.size()));
#elif OS(FUCHSIA)
    zx_cprng_draw(buffer.data(), buffer.size());
#elif OS(UNIX)
    ssize_t amountRead = 0;
    while (static_cast<size_t>(amountRead) < buffer.size()) {
        ssize_t currentRead;
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
#if RANDOM_DEVICE_USE_GETRANDOM
        if (m_fd < 0)
            currentRead = syscall(SYS_getrandom, buffer.data() + amountRead, buffer.size() - amountRead, 0);
        else
#endif
            currentRead = read(m_fd, buffer.data() + amountRead, buffer.size() - amountRead);
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        // We need to check for both EAGAIN and EINTR since on some systems /dev/urandom
        // is blocking and on others it is non-blocking.
        if (currentRead == -1) {
            if (!(errno == EAGAIN || errno == EINTR))
                crashUnableToReadFromURandom();
        } else
            amountRead += currentRead;
    }
#elif OS(WINDOWS)
    processPrng(buffer);
#else
#error "This configuration doesn't have a strong source of randomness."
// WARNING: When adding new sources of OS randomness, the randomness must
//          be of cryptographic quality!
#endif
}

}
