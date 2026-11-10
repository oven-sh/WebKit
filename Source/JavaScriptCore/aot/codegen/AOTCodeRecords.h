/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTCompiledCode.h"
#include "AOTFormsSaid.h"
#include "AOTFrameMaps.h"
#include "AOTID.h"
#include "AOTMachine.h"
#include "AOTNumbering.h"
#include "AOTOperationData.h"
#include "AOTRelocation.h"
#include "AOTSchemaRecords.h"
#include "AOTStubCalls.h"
#include "CCallHelpers.h"
#include "HandlerInfo.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>

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
    CallSiteKind kind { CallSiteKind::Plain };
    BuiltinID calledBuiltin { };
    FunctionID constructedCallee { };
};

struct SiteRecords {
    Vector<RecordedSite> positions;
    Vector<uint32_t> inlineCallSites;
    uint32_t numberOfSitesWithoutCode { 0 };

    uint32_t addSite(const RecordedSite& site)
    {
        if (positions.size() == positions.capacity()) [[unlikely]] {
            RecordedSite added = site;
            positions.reserveCapacity(std::max<size_t>(16, 2 * positions.capacity()));
            positions.unsafeAppendWithoutCapacityCheck(added);
            return positions.size();
        }
        positions.unsafeAppendWithoutCapacityCheck(site);
        return positions.size();
    }
};

struct SiteLandingPad {
    uint32_t site;
    uint32_t landingPad;
    HandlerType type;
};

struct ExceptionRecords {
    Vector<MacroAssembler::Label*> landingPads;
    Vector<SiteLandingPad> sites;
};

struct ThrowSite {
    friend bool operator==(const ThrowSite&, const ThrowSite&) = default;

    ThrowKind kind;
    uint32_t a;
    std::optional<uint32_t> b;
    bool calleeIsReadFromReceiver { false };
    bool aIsMention { false };
    bool bIsMention { false };
    std::optional<ProofSource> sourceOfFalseProof { };
    uint32_t expectedProofType { 0 };
};

struct TrapSite {
    ValueID value;
    ThrowSite throwSite;
    Representation representation;
    uint32_t callSite;
};

struct CallThrowSite {
    uint32_t returnSite;
    Vector<uint32_t, 3> words;
    uint8_t wordsThatAreMentions { 0 };
};

struct StackCheckLabels {
    CCallHelpers::Label call;
    CCallHelpers::Label resume;
};

struct TrapRecords {
    Vector<TrapSite> trapSites;
    Vector<CallThrowSite> callThrowSites;
    Vector<PollSite> pollSites;
    Vector<StackCheckLabels, 1> stackChecks;
};

struct CodeRecords {
    WTF_MAKE_NONCOPYABLE(CodeRecords);

    CodeRecords(const StubCatalog& catalog, RelocationRecorder& relocations, CodeGenerationTotals& totals, RememberedStubKeys* rememberedStubKeys = nullptr)
        : relocations(relocations)
        , stubCalls(catalog, relocations, rememberedStubKeys)
        , totals(totals)
    {
    }

    RelocationRecorder& relocations;
    StubCalls stubCalls;
    MentionList mentions;
    Vector<RecordedEntry, 1> entries;
    FrameRequirements frame;
    SiteRecords sites;
    ExceptionRecords exceptions;
    FrameMapRecords frameMaps;
    TrapRecords traps;
    FormsSaid formsSaid;
    CodeGenerationTotals& totals;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
