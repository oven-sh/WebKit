/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCodeSites.h"
#include "AOTCodeUnits.h"
#include "AOTImageFormat.h"
#include "AOTImageForward.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTStubCalls.h"
#include "AOTStubKey.h"
#include "AOTVector.h"
#include "HandlerInfo.h"
#include "RegisterAtOffsetList.h"
#include <optional>
#include <span>
#include <wtf/BitSet.h>
#include <wtf/OptionSet.h>
#include <wtf/text/WTFString.h>

namespace JSC::AOT {

struct CodeEntry {
    uint32_t offset;
    uint16_t id;
};

struct ConstantData {
    friend bool operator==(const ConstantData&, const ConstantData&) = default;

    Vector<uint8_t> bytes;
    uint32_t alignment { 1 };
};

struct JumpTable {
    friend bool operator==(const JumpTable&, const JumpTable&) = default;

    Vector<uint32_t> targets;
};

struct ReturnSite {
    uint32_t offset;
    OptionalIndex site;
};

struct FirstReadSite {
    uint32_t returnOffset;
    uint32_t site;
};

struct CodeBody {
    uint32_t primaryEntryOffset() const { return entries[0].offset; }
    std::optional<uint32_t> offsetOfEntry(uint16_t id) const;

    Vector<uint8_t> bytes;
    uint32_t paddingStart { 0 };
    uint32_t sizeOfHotBlocks { 0 };
    Vector<CodeEntry, 1> entries;
    PackedCodeUnits units;
    Vector<LinkedSite> localSites;
    Vector<Relocation> relocations;
    Vector<ReturnSite> returnSites;
    Vector<FirstReadSite> firstReadSites;
    Vector<FunctionID> callees;
    Vector<StubKey> stubKeys;
    Vector<ArgumentMoves> argumentMoves;
    Vector<ConstantData> constants;
    Vector<JumpTable> jumpTables;
};

inline std::optional<uint32_t> CodeBody::offsetOfEntry(uint16_t id) const
{
    for (const CodeEntry& entry : entries) {
        if (entry.id == id)
            return entry.offset;
    }
    return std::nullopt;
}

struct CodeRange {
    uint32_t begin;
    uint32_t end;
};

struct FrameDescription {
    bool hasFrame() const { return !frameRanges.isEmpty(); }
    bool hasFrameAt(uint32_t offset) const;

    unsigned frameSizeInBytes { 0 };
    bool hasFrameRecord { true };
    bool spillsArgumentRegisters { false };
    RegisterAtOffsetList calleeSaveRegisters;
    Vector<CodeRange, 1> frameRanges;
    uint32_t stackUsedAtCalls { 0 };
    bool isEnteredWithoutStackCheck { false };
    bool isProvenToFitOnStack { false };
    Vector<PrologueCheck, 1> prologueChecks;
};

inline bool FrameDescription::hasFrameAt(uint32_t offset) const
{
    for (const CodeRange& range : frameRanges) {
        if (offset >= range.begin && offset < range.end)
            return true;
    }
    return false;
}

struct EntryDescription {
    uint8_t numberOfParameters { 0 };
    // ClosureWord::borrowsArgumentShift.
    uint8_t borrowedArguments { 0 };
    bool endsItsTailCalls { false };
    bool isGenericEntrypoint { false };
};

struct SitePosition {
    uint32_t line;
    uint32_t column;
    ModuleID source;
    uint32_t sourceOffset;
    uint32_t inlineFrame;
};

struct InlineFrame {
    uint32_t parent;
    uint32_t callSite;
    uint32_t callee;
    bool isTailCall;
    bool isConstruct;
};

enum class SiteFlag : uint8_t {
    Constructs = 1 << 0,
    IsTailCall = 1 << 1,
};

struct FlaggedSite {
    uint32_t site;
    OptionSet<SiteFlag> flags;
};

struct SiteHandler {
    uint32_t site;
    uint32_t codeOffset;
    HandlerType type;
};

struct FaultingAccess {
    uint32_t codeOffset;
    OptionalIndex callSite;
    Vector<uint32_t, 3> throwSite;
};

struct SiteDescription {
    bool hasEncodedInlineFrames() const { return inlineFrames.size() > 1 && positions.size() > 1 + spreadSites.size() + numberOfSitesWithoutCode; }

    uint32_t numberOfSitesWithoutCode { 0 };
    Vector<SitePosition> positions;
    Vector<InlineFrame> inlineFrames;
    Vector<SpreadSite> spreadSites;
    Vector<FlaggedSite> flaggedSites;
    Vector<SiteHandler> handlers;
    Vector<FaultingAccess> faultingAccesses;
    Vector<PollSite> pollSites;
    Vector<uint32_t> frameMapWords;
};

struct CompiledCode {
    FunctionID function;
    CodeBody body;
    FrameDescription frame;
    EntryDescription entry;
    SiteDescription sites;
    String sizesByOperation;
};

struct CodeGenerationTotals {
    WTF::BitSet<numberOfEntries> entriesUsed;
};

enum class CodeOffsetRole : uint8_t {
    Call,
    Site,
    Target,
    ReturnAddress,
    Boundary,
};

template<typename Code, typename Functor>
void forEachCodeOffset(Code& code, const Functor& functor)
{
    auto& body = code.body;
    auto& sites = code.sites;
    for (auto& relocation : body.relocations)
        functor(relocation.offset, relocation.kind == RelocationKind::Call ? CodeOffsetRole::Call : CodeOffsetRole::Site, MacroAssembler::relocationSize(relocation.kind));
    for (auto& site : body.localSites) {
        functor(site.offset, CodeOffsetRole::Site, MacroAssembler::relocationSize(site.kind));
        functor(site.target, CodeOffsetRole::Target, 0u);
    }
    for (auto& entry : body.entries)
        functor(entry.offset, CodeOffsetRole::Target, 0u);
    for (auto& site : body.returnSites)
        functor(site.offset, CodeOffsetRole::ReturnAddress, 0u);
    for (auto& site : body.firstReadSites)
        functor(site.returnOffset, CodeOffsetRole::ReturnAddress, 0u);
    for (auto& table : body.jumpTables) {
        for (auto& target : table.targets)
            functor(target, CodeOffsetRole::Target, 0u);
    }
    functor(body.paddingStart, CodeOffsetRole::Boundary, 0u);
    for (auto& handler : sites.handlers)
        functor(handler.codeOffset, CodeOffsetRole::Target, 0u);
    for (auto& access : sites.faultingAccesses)
        functor(access.codeOffset, CodeOffsetRole::Site, 1u);
    for (auto& poll : sites.pollSites)
        functor(poll.offset, CodeOffsetRole::Site, static_cast<unsigned>(sizeOfLoopPoll));
    for (auto& range : code.frame.frameRanges) {
        functor(range.begin, CodeOffsetRole::Boundary, 0u);
        functor(range.end, CodeOffsetRole::Boundary, 0u);
    }
    for (auto& check : code.frame.prologueChecks) {
        functor(check.callOffset, CodeOffsetRole::Site, static_cast<unsigned>(sizeOfNearCall));
        functor(check.resumeOffset, CodeOffsetRole::Target, 0u);
    }
}

template<typename Functor>
void forEachReturnAddress(const CompiledCode& code, const Functor& functor)
{
    std::span<const PrologueCheck> prologueChecks = code.frame.prologueChecks.span();
    for (const ReturnSite& site : code.body.returnSites) {
        while (!prologueChecks.empty() && prologueChecks.front().callOffset + sizeOfNearCall < site.offset)
            prologueChecks = prologueChecks.subspan(1);
        // The stub that a prologue check calls returns to PrologueCheck::resumeOffset.
        if (!prologueChecks.empty() && prologueChecks.front().callOffset + sizeOfNearCall == site.offset)
            continue;
        functor(site.offset, site.site);
    }
    for (const FaultingAccess& access : code.sites.faultingAccesses)
        functor(access.codeOffset + static_cast<uint32_t>(sizeOfNearCall), access.callSite);
    for (const PollSite& poll : code.sites.pollSites)
        functor(poll.returnOffset(), poll.callSite);
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
