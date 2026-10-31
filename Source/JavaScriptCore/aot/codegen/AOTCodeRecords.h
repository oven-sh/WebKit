/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTCodeUnits.h"
#include "AOTCompiledCode.h"
#include "AOTFrameMaps.h"
#include "AOTID.h"
#include "AOTMutatorFenceMerging.h"
#include "AOTOperationData.h"
#include "AOTRelocation.h"
#include "AOTSchemaRecords.h"
#include "AOTClaimChecks.h"
#include "AOTStubCalls.h"
#include "AOTVector.h"
#include "AirInstObserver.h"
#include "CCallHelpers.h"
#include "HandlerInfo.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Box.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace B3 { namespace Air {
class StackSlot;
} }

namespace AOT { namespace Typed {

struct FrameRequirements {
    bool readsCallerFrame { false };
    bool savesFrameOnSuspend { false };
    bool needsStackCheckForTailCalls { false };
    bool dropsArgumentListFrames { false };
    bool spillsArgumentRegisters { false };
    unsigned stackUsedAtCalls { 0 };
};

struct RecordedEntry {
    uint16_t id;
    CCallHelpers::Label label;
};

struct RecordedSite {
    friend bool operator==(const RecordedSite&, const RecordedSite&) = default;

    Origin origin;
    CallSiteKind kind { CallSiteKind::Plain };
    BuiltinID calledBuiltin { };
    FunctionID constructedCallee { };
    // The function has made the function object that it calls here itself and has handed it to nobody.
    bool callsOwnFunctionObject { false };
    // The function object called here is kept in the frame: an index into SiteRecords::keptCallees, plus one.
    uint32_t keptCalleePlusOne { 0 };
};

struct KeptCallee {
    // An index into SiteRecords::wordsOfCallees.
    uint32_t word { 0 };
    // The position and inline frame of the call, which are those of the operation that keeps its callee.
    uint64_t call { 0 };
};

struct SiteRecords {
    Vector<RecordedSite> positions;
    Vector<uint32_t> inlineCallSites;
    uint32_t numberOfSitesWithoutCode { 0 };
    // Indexed by the call's inline depth. An inlined call keeps its word while the inlined body runs, and calls inside that body use the next word.
    Vector<B3::Air::StackSlot*, 2> wordsOfCallees;
    Vector<KeptCallee, 4> keptCallees;
    // Indexed by KeptCallee::call, for words that hold an environment: SlotData::encoded() of the field that holds the function object. nullopt if the field has no storage, so no function object is created.
    UncheckedKeyHashMap<uint64_t, std::optional<uint32_t>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fieldsOfCallees;

    static uint32_t wordOfCalleeCalledFrom(const Function& function, uint32_t inlineFrame)
    {
        uint32_t depth = 0;
        for (; inlineFrame; inlineFrame = function.inlineFrames[inlineFrame].parent)
            ++depth;
        return depth;
    }
    static uint64_t keyOfCall(uint32_t position, uint32_t inlineFrame) { return static_cast<uint64_t>(position) << 32 | inlineFrame; }

    // RecordedSite::keptCalleePlusOne.
    uint32_t addKeptCallee(const Function& function, uint32_t position, uint32_t inlineFrame)
    {
        keptCallees.append({ wordOfCalleeCalledFrom(function, inlineFrame), keyOfCall(position, inlineFrame) });
        return keptCallees.size();
    }

    uint32_t addSite(const RecordedSite& site)
    {
        positions.append(site);
        return positions.size();
    }
};

struct SiteLandingPad {
    uint32_t site;
    uint32_t landingPad;
    HandlerType type;
};

struct ExceptionRecords {
    WTF::Vector<Box<CCallHelpers::Label>> landingPads;
    Vector<SiteLandingPad> sites;
};

struct ThrowSite {
    friend bool operator==(const ThrowSite&, const ThrowSite&) = default;

    ThrowKind kind;
    uint32_t a;
    std::optional<uint32_t> b;
    bool calleeIsReadFromReceiver { false };
};

struct TrapSite {
    ValueID value;
    ThrowSite throwSite;
    Representation representation;
    uint32_t callSite;
};

struct TrappingAccess {
    MacroAssembler::Label instruction;
    ValueID value;
    GPRReg reference;
    bool referenceIsNarrow;
};

struct CallThrowSite {
    uint32_t returnSite;
    Vector<uint32_t, 3> words;
};

struct StackCheckLabels {
    CCallHelpers::Label call;
    CCallHelpers::Label resume;
};

struct TrapRecords {
    Vector<TrapSite> trapSites;
    Vector<TrappingAccess> trappingAccesses;
    Vector<CallThrowSite> callThrowSites;
    Vector<PollSite> pollSites;
    Vector<StackCheckLabels, 1> stackChecks;
};

class InstructionObserver final : public B3::Air::InstObserver {
    WTF_MAKE_NONCOPYABLE(InstructionObserver);
public:
    InstructionObserver(CodeUnitRecorder&, TrapRecords&);

    void willGenerate(const B3::Air::BasicBlock&) final;
    void didGenerate(const B3::Air::Inst&, MacroAssembler::Label begin, MacroAssembler::Label end) final;

private:
    CodeUnitRecorder& m_units;
    TrapRecords& m_traps;
};

struct CodeRecords {
    WTF_MAKE_NONCOPYABLE(CodeRecords);

    CodeRecords(const StubCatalog& catalog, RelocationRecorder& relocations, CodeGenerationTotals& totals)
        : relocations(relocations)
        , stubCalls(catalog, relocations, units)
        , observer(units, traps)
        , totals(totals)
    {
    }

    CodeUnitRecorder units;
    RelocationRecorder& relocations;
    StubCalls stubCalls;
    Vector<RecordedEntry, 1> entries;
    FrameRequirements frame;
    SiteRecords sites;
    ExceptionRecords exceptions;
    FrameMapRecords frameMaps;
    TrapRecords traps;
    InstructionObserver observer;
    CodeGenerationTotals& totals;
    // By the index of the operation: the code generator has compressed an operand in front of it, so the narrow reference has the origin of the operation.
    BitVector operationsWithCompressedOperand;
    SlotsOfCells slotsOfFrameCells;
    // Only with --aot-check=MovedLoads.
    GuardTests guardTests;
    bool hasBirthsOfFrameCells { false };
    MutatorFenceRecords mutatorFences;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
