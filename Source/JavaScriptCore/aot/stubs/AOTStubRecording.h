/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubFrames.h"
#include "AOTStubKey.h"
#include "AOTStubRegisters.h"
#include "AOTVector.h"
#include "RegisterSet.h"
#include <wtf/BitSet.h>

namespace JSC { namespace AOT {

constexpr Typed::Effects effectsOf(Entry entry)
{
    if (std::optional<RuntimeOperation> operation = runtimeOperationOf(operationBehindFrontEnd(entry)))
        return Typed::declaredBoundOf(*operation);
    if (entry == Entry::operationAOTLookupExceptionHandler) {
        Typed::Effects effects = Typed::Effects::none();
        effects.reads = Typed::LocationMasks(Typed::AbstractHeapSet { Typed::AbstractHeap::RuntimeState });
        effects.writes = effects.reads;
        effects.mayThrow = true;
        effects.mayCollect = true;
        return effects;
    }
    if (entry == Entry::operationAOTBytesAllocatedAtPrologueCheck)
        return Typed::Effects::none();
    if (entry == Entry::operationAOTCopyDataPropertiesByFieldMap)
        return Typed::declaredBoundOf(RuntimeOperation::CopyDataProperties);
    return Typed::Effects::unknownCode();
}

struct StubReference {
    enum class Kind : uint8_t { Call, PollAfterOperation, Jump, Address, ReturnedAddress };

    StubKey target;
    Kind kind;
    RegisterSet savedAround;

    friend bool operator==(const StubReference&, const StubReference&) = default;
};

enum class StubCallee : uint8_t { Operation, PreserveMostLeaf, HostFunction, CompiledCode };

inline RegisterSet registersClobberedBy(StubCallee callee)
{
    switch (callee) {
    case StubCallee::Operation:
    case StubCallee::HostFunction:
        return registersClobberedByOperations();
    case StubCallee::PreserveMostLeaf:
        return registersClobberedByPreserveMostLeaves();
    case StubCallee::CompiledCode:
        return registersClobberedByCompiledCalls();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct StubRecording {
    RegisterSet clobbered() const { return RegisterSet(clobberedInline).merge(clobberedByCallees); }

    StubKey body;
    RegisterSet clobberedInline;
    RegisterSet clobberedByCallees;
    bool hasUnknownEffects { false };
    bool leavesStubs { false };
    Vector<StubKey, 4> entries;
    Vector<StubReference, 8> references;
    Vector<StubFrameKind, 2> returnLabels;
    WTF::BitSet<numberOfEntries> entriesUsed;
    WTF::BitSet<numberOfEntries> operationsCalled;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
