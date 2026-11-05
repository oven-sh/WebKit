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
    bool callsOwnFunctionObject { false };
    uint32_t keptCalleePlusOne { 0 };
};

struct KeptCallee {
    uint32_t word { 0 };
    uint64_t call { 0 };
};

struct SiteRecords {
    Vector<RecordedSite> positions;
    Vector<uint32_t> inlineCallSites;
    uint32_t numberOfSitesWithoutCode { 0 };
    Vector<Machine::StackSlot*, 2> wordsOfCallees;
    Vector<KeptCallee, 4> keptCallees;
    UncheckedKeyHashMap<uint64_t, std::optional<uint32_t>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fieldsOfCallees;

    static uint32_t wordOfCalleeCalledFrom(std::span<const InlineFrame> inlineFrames, uint32_t inlineFrame)
    {
        uint32_t depth = 0;
        for (; inlineFrame; inlineFrame = inlineFrames[inlineFrame].parent)
            ++depth;
        return depth;
    }
    static uint64_t keyOfCall(uint32_t position, uint32_t inlineFrame) { return static_cast<uint64_t>(position) << 32 | inlineFrame; }

    uint32_t addKeptCallee(std::span<const InlineFrame> inlineFrames, uint32_t position, uint32_t inlineFrame)
    {
        keptCallees.append({ wordOfCalleeCalledFrom(inlineFrames, inlineFrame), keyOfCall(position, inlineFrame) });
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
