/*
 * Copyright (C) 2012 University of Szeged. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY UNIVERSITY OF SZEGED ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL UNIVERSITY OF SZEGED OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/NumberOfCores.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringToIntegerConversion.h>

#if OS(DARWIN)
#include <sys/sysctl.h>
#elif OS(LINUX)
#include <sched.h>
#include <unistd.h>
#include "uv_get_constrained_memory.h"
#elif OS(AIX) || OS(OPENBSD) || OS(NETBSD) || OS(FREEBSD) || OS(HAIKU)
#include <unistd.h>
#elif OS(QNX)
#include <sys/syspage.h>
#elif OS(WINDOWS)
#include <windows.h>
#endif

namespace WTF {

static constexpr unsigned maxNumberOfProcessorCoresOverride = 1024;

int numberOfProcessorCores()
{
    const int defaultIfUnavailable = 1;
    static int s_numberOfCores = -1;

    if (s_numberOfCores > 0)
        return s_numberOfCores;
    
    ASCIILiteral coresEnvName = "WTF_numberOfProcessorCores";
    CString coresEnv = getenv(coresEnvName);
#if !USE(BUN_JSC_ADDITIONS)
    // Bun reports this value as navigator.hardwareConcurrency and os.availableParallelism(), which
    // (as in Node) do not follow NUMBER_OF_PROCESSORS.
    if (coresEnv.isNull()) {
        coresEnvName = "NUMBER_OF_PROCESSORS";
        coresEnv = getenv(coresEnvName);
    }
#endif
    if (!coresEnv.isNull()) {
        if (auto numberOfCores = parseInteger<unsigned>(coresEnv.span())) {
            // The value sizes thread pools and is reported as navigator.hardwareConcurrency
            // and os.availableParallelism(), so it must be at least 1. The cap keeps it in
            // `int` range; it matches the UV_THREADPOOL_SIZE cap.
            s_numberOfCores = static_cast<int>(std::clamp<unsigned>(*numberOfCores, 1, maxNumberOfProcessorCoresOverride));
            return s_numberOfCores;
        }
        SAFE_FPRINTF(stderr, "WARNING: failed to parse %s=%s\n", coresEnvName, coresEnv);
    }

#if OS(DARWIN)
    unsigned result;
    size_t length = sizeof(result);
    std::array name {
            CTL_HW,
            HW_AVAILCPU
    };
    int sysctlResult = sysctl(name.data(), name.size(), &result, &length, 0, 0);

    s_numberOfCores = sysctlResult < 0 ? defaultIfUnavailable : result;
#elif OS(LINUX)
    long result = sysconf(_SC_NPROCESSORS_ONLN);

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        long affinity = CPU_COUNT(&set);
        if (affinity > 0 && (result < 1 || affinity < result))
            result = affinity;
    }

    if (int constrained = uv_get_constrained_cpu(); constrained > 0 && (result < 1 || constrained < result))
        result = constrained;

    s_numberOfCores = result < 1 ? defaultIfUnavailable : static_cast<int>(result);
#elif OS(AIX) || OS(OPENBSD) || OS(NETBSD) || OS(FREEBSD) || OS(HAIKU)
    long sysconfResult = sysconf(_SC_NPROCESSORS_ONLN);

    s_numberOfCores = sysconfResult < 0 ? defaultIfUnavailable : static_cast<int>(sysconfResult);
#elif OS(QNX)
    int numCpuQNX = _syspage_ptr->num_cpu;
    s_numberOfCores = numCpuQNX < 0 ? defaultIfUnavailable : numCpuQNX;
#elif OS(WINDOWS)
    UNUSED_PARAM(defaultIfUnavailable);
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    s_numberOfCores = sysInfo.dwNumberOfProcessors;
#else
    s_numberOfCores = defaultIfUnavailable;
#endif
    return s_numberOfCores;
}

#if OS(DARWIN)
int numberOfPhysicalProcessorCores()
{
    static int32_t numCores = [] {
        constexpr int32_t defaultIfUnavailable = 1;
        size_t valueSize = sizeof(numCores);
        int32_t numCores = 0;
        int result = sysctlbyname("hw.physicalcpu_max", &numCores, &valueSize, nullptr, 0);
        if (result < 0)
            return defaultIfUnavailable;
        return numCores;
    }();

    return numCores;
}
#endif

}
