/*
 * Copyright (C) 2016 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
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
#include <wtf/linux/CurrentProcessMemoryStatus.h>

#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <wtf/PageBlock.h>

namespace WTF {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
void currentProcessMemoryStatus(ProcessMemoryStatus& memoryStatus)
{
    // This is on the GC's heap-growth path (Heap::proportionalHeapSize via
    // memoryFootprint()), so open /proc/self/statm once and pread() it rather
    // than fopen/fclose on every call. If it can't be opened, latch to "no data".
    static int statmFd = -1;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        do {
            statmFd = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
        } while (statmFd == -1 && errno == EINTR);
    });
    if (statmFd < 0)
        return;

    char buffer[128];
    ssize_t length;
    do {
        length = pread(statmFd, buffer, sizeof(buffer) - 1, 0);
    } while (length == -1 && errno == EINTR);
    if (length <= 0)
        return;
    buffer[length] = '\0';

    size_t pageSize = WTF::pageSize();
    char* end = nullptr;
    unsigned long long intValue = strtoull(buffer, &end, 10);
    memoryStatus.size = intValue * pageSize;
    intValue = strtoull(end, &end, 10);
    memoryStatus.resident = intValue * pageSize;
    intValue = strtoull(end, &end, 10);
    memoryStatus.shared = intValue * pageSize;
    intValue = strtoull(end, &end, 10);
    memoryStatus.text = intValue * pageSize;
    intValue = strtoull(end, &end, 10);
    memoryStatus.lib = intValue * pageSize;
    intValue = strtoull(end, &end, 10);
    memoryStatus.data = intValue * pageSize;
    intValue = strtoull(end, &end, 10);
    memoryStatus.dt = intValue * pageSize;
}
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} // namespace WTF
