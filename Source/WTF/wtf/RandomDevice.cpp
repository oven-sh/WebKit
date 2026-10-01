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

#include <stdlib.h>

#if !OS(DARWIN) && !OS(FUCHSIA) && OS(UNIX)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if OS(LINUX)
#include <sys/syscall.h>
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 0x0001
#endif
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

#if !OS(DARWIN) && !OS(FUCHSIA) && !OS(WINDOWS)
RandomDevice::RandomDevice()
{
#if !OS(LINUX)
    openURandom();
#endif
}

void RandomDevice::openURandom()
{
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
#if OS(LINUX)
#if defined(SYS_getrandom)
    // getrandom(2) reads the pool /dev/urandom reads, but needs neither the path nor a free descriptor.
    // Any failure reads the rest from /dev/urandom: EAGAIN (pool not initialized, where /dev/urandom
    // does not block), ENOSYS, EPERM (a seccomp filter, which can be installed at any time).
    while (static_cast<size_t>(amountRead) < buffer.size()) {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        ssize_t currentRead = syscall(SYS_getrandom, buffer.data() + amountRead, buffer.size() - amountRead, GRND_NONBLOCK);
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        if (currentRead > 0)
            amountRead += currentRead;
        else if (currentRead != -1 || errno != EINTR)
            break;
    }
    if (static_cast<size_t>(amountRead) == buffer.size())
        return;
#endif
    if (m_fd < 0)
        openURandom();
#endif
    while (static_cast<size_t>(amountRead) < buffer.size()) {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        ssize_t currentRead = read(m_fd, buffer.data() + amountRead, buffer.size() - amountRead);
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
