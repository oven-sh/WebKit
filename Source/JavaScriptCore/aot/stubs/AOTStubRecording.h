/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTRecordTypes.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTRuntimeWords.h"
#include "AOTStubFrames.h"
#include "AOTStubKey.h"
#include "AOTStubRegisters.h"
#include "AOTVector.h"
#include "RegisterSet.h"
#include <wtf/BitSet.h>

namespace JSC { namespace AOT {

constexpr Typed::Effects effectsOf(Entry entry)
{
    if (std::optional<RuntimeOperation> operation = runtimeOperationOf(entry))
        return Typed::declaredBoundOf(*operation);
    // Every stub that may throw reaches it. It finds the handler of an exception and runs no JavaScript.
    if (entry == Entry::operationAOTLookupExceptionHandler) {
        Typed::Effects effects = Typed::Effects::none();
        effects.reads = Typed::LocationMasks(Typed::AbstractHeapSet { Typed::AbstractHeap::RuntimeState });
        effects.writes = effects.reads;
        effects.mayThrow = true;
        effects.mayCollect = true;
        return effects;
    }
    // It looks a return address up in the tables of the image.
    if (entry == Entry::operationAOTFindLandingOfMisfit) {
        Typed::Effects effects = Typed::Effects::none();
        effects.reads = Typed::LocationMasks(Typed::AbstractHeapSet { Typed::AbstractHeap::RuntimeState });
        effects.writes = effects.reads;
        return effects;
    }
    if (entry == Entry::operationAOTBytesAllocatedAtPrologueCheck)
        return Typed::Effects::none();
    if (entry == Entry::operationAOTCopyDataPropertiesByFieldMap)
        return Typed::declaredBoundOf(RuntimeOperation::CopyDataProperties);
    return Typed::Effects::unknownCode();
}

struct StubReference {
    enum class Kind : uint8_t { Call, Jump, Address, ReturnedAddress };

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

struct StubExitRecord {
    StubExit exit;
    Entry operation;
    RuntimeCounterID reason;

    friend bool operator==(const StubExitRecord&, const StubExitRecord&) = default;
};

// What generating one body recorded. Nothing here is transitive.
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
    Vector<StubExitRecord, 2> exits;
    Vector<CodeSymbol, 2> symbols;
    WTF::BitSet<numberOfEntries> entriesUsed;
    WTF::BitSet<numberOfEntries> operationsCalled;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
