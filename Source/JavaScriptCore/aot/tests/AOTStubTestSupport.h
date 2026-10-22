/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTNativeTest.h"
#include "AOTStubAssembler.h"
#include "AOTStubBlob.h"
#include "AOTStubCatalog.h"
#include "AOTStubDescriptor.h"
#include "AOTStubFrontEnds.h"
#include "AOTStubRegisters.h"
#include <algorithm>
#include <wtf/Vector.h>

#if OS(UNIX)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace JSC { namespace AOT {

inline Vector<StubKey> allStubKeys()
{
    Vector<StubKey> keys;
    forEachKeyWithoutImmediate([&](StubKey key) {
        keys.append(key);
    });
    AOT_TEST_CHECK(keys.size() >= numberOfStubFamilies / 2);
    return keys;
}

inline Vector<StubKey> allStubBodies()
{
    Vector<StubKey> bodies;
    for (StubKey key : allStubKeys())
        bodies.append(bodyOf(key));
    std::sort(bodies.begin(), bodies.end());
    bodies.shrink(std::unique(bodies.begin(), bodies.end()) - bodies.begin());
    return bodies;
}

inline Vector<StubKey> stubKeysWithLinkage(StubLinkage linkage)
{
    Vector<StubKey> keys;
    for (StubKey key : allStubKeys()) {
        if (descriptorOf(key.family()).linkage == linkage)
            keys.append(key);
    }
    return keys;
}

inline Vector<StubKey> entriesOf(StubKey body)
{
    Vector<StubKey> entries;
    forEachEntryOf(body, [&](StubKey entry) {
        entries.append(entry);
    });
    return entries;
}

inline Vector<StubOptions> stubOptionVariants()
{
    Vector<StubOptions> variants { StubOptions { } };
    for (bool StubOptions::* member : { &StubOptions::useWideReferences, &StubOptions::forceGCSlowPaths, &StubOptions::imageHasWebAssembly, &StubOptions::hasOneInstance, &StubOptions::validates }) {
        StubOptions options;
        options.*member = !(options.*member);
        variants.append(options);
    }
    variants.append(StubOptions { .targetFeatures = CPUFeatureSet::buildTarget() });
    return variants;
}

inline StubSignature signatureFor(StubKey key) { return signatureOf(key); }
inline size_t indexOf(Entry entry) { return static_cast<size_t>(entry); }
inline bool containsRegister(const RegisterSet& set, Reg reg) { return set.contains(reg, IgnoreVectors); }

inline bool namesRegister(const StubSignature& signature, Reg reg)
{
    AOT_TEST_CHECK(reg.isSet());
    for (unsigned i = 0; i < signature.numberOfOperands; ++i) {
        if (signature.operands[i] == reg)
            return true;
    }
    return signature.firstResult == reg || signature.secondResult == reg || (signature.immediate && Reg(*signature.immediate) == reg);
}

inline RegisterSet pinnedRegisters()
{
    RegisterSet pinned { instanceGPR, cellCageBaseGPR, MacroAssembler::stackPointerRegister, GPRInfo::callFrameRegister };
#if CPU(ARM64)
    pinned.add(GPRInfo::numberTagRegister, IgnoreVectors);
    pinned.add(GPRInfo::notCellMaskRegister, IgnoreVectors);
#endif
    return pinned;
}

#if OS(UNIX)

enum class ChildOutcome : uint8_t { Finished, Crashed };

template<typename Functor>
ChildOutcome runInChild(const Functor& functor)
{
    constexpr int crashedExitCode = 42;
    constexpr unsigned timeoutInSeconds = 60;

    fflush(nullptr);
    pid_t child = fork();
    AOT_TEST_CHECK(child >= 0);
    if (!child) {
        dup2(open("/dev/null", O_WRONLY), STDERR_FILENO);
        alarm(timeoutInSeconds);
        for (int number : { SIGTRAP, SIGILL, SIGABRT, SIGSEGV, SIGBUS })
            signal(number, [](int) { _exit(crashedExitCode); });
        functor();
        _exit(0);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    AOT_TEST_CHECK(waited == child);
    AOT_TEST_CHECK(WIFEXITED(status));
    int exitCode = WEXITSTATUS(status);
    AOT_TEST_CHECK(!exitCode || exitCode == crashedExitCode);
    return exitCode ? ChildOutcome::Crashed : ChildOutcome::Finished;
}

#endif // OS(UNIX)

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
