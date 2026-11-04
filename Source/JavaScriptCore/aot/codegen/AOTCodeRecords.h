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
    Vector<Machine::StackSlot*, 2> wordsOfCallees;
    Vector<KeptCallee, 4> keptCallees;
    // Indexed by KeptCallee::call, for words that hold an environment: SlotData::encoded() of the field that holds the function object. nullopt if the field has no storage, so no function object is created.
    UncheckedKeyHashMap<uint64_t, std::optional<uint32_t>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> fieldsOfCallees;

    static uint32_t wordOfCalleeCalledFrom(std::span<const InlineFrame> inlineFrames, uint32_t inlineFrame)
    {
        uint32_t depth = 0;
        for (; inlineFrame; inlineFrame = inlineFrames[inlineFrame].parent)
            ++depth;
        return depth;
    }
    static uint64_t keyOfCall(uint32_t position, uint32_t inlineFrame) { return static_cast<uint64_t>(position) << 32 | inlineFrame; }

    // RecordedSite::keptCalleePlusOne.
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
    // The labels lie in the arena of the procedure and are set once the function is encoded.
    Vector<MacroAssembler::Label*> landingPads;
    Vector<SiteLandingPad> sites;
};

struct ThrowSite {
    friend bool operator==(const ThrowSite&, const ThrowSite&) = default;

    ThrowKind kind;
    uint32_t a;
    std::optional<uint32_t> b;
    bool calleeIsReadFromReceiver { false };
    // The word names a string: it is an index into the mentions of the function (NumberSpace::ShownString), and Image puts the number of the string in its place.
    // Such an `a` always has a word of its own, so that the length of the words does not depend on the number.
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
    // mentionsAmongThrowSiteWords()
    uint8_t wordsThatAreMentions { 0 };
};

struct StackCheckLabels {
    CCallHelpers::Label call;
    CCallHelpers::Label resume;
};

struct TrapRecords {
    // Of the operations whose loads trap. Machine::Procedure::trappingAccesses() has the instructions, each with the origin of its operation.
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
