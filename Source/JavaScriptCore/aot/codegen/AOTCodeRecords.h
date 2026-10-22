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
#include "AOTOperationData.h"
#include "AOTRelocation.h"
#include "AOTSchemaRecords.h"
#include "AOTStubCalls.h"
#include "AirInstObserver.h"
#include "CCallHelpers.h"
#include "HandlerInfo.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Box.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

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
    bool constructs { false };
    bool isTailCall { false };
};

struct SiteRecords {
    Vector<RecordedSite> positions;
    Vector<SpreadSite> itemSites;
    Vector<uint32_t> inlineCallSites;
    uint32_t numberOfSitesWithoutCode { 0 };

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
    Vector<Box<CCallHelpers::Label>> landingPads;
    Vector<SiteLandingPad> sites;
};

struct ThrowSite {
    friend bool operator==(const ThrowSite&, const ThrowSite&) = default;

    ThrowKind kind;
    uint32_t a;
    std::optional<uint32_t> b;
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
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
