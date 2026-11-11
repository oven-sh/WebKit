/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTCodeRole.h"
#include "AOTCodeSites.h"
#include "AOTImageFormat.h"
#include "AOTImageForward.h"
#include "AOTListOfUnit.h"
#include "AOTNumbering.h"
#include "AOTOpcode.h"
#include "AOTRelocation.h"
#include "AOTRuntimeTable.h"
#include "AOTSchemaPackedRecords.h"
#include "AOTStubCalls.h"
#include "AOTStubKey.h"
#include "AOTWire.h"
#include "HandlerInfo.h"
#include "RegisterAtOffsetList.h"
#include <algorithm>
#include <wtf/BitSet.h>

namespace JSC::AOT {

struct CodeEntry {
    uint32_t offset;
    uint16_t id;
};

struct ConstantData {
    friend bool operator==(const ConstantData&, const ConstantData&) = default;

    ListOfUnit<uint8_t, 16> bytes;
    uint32_t alignment { 1 };
    ConstantKind kind { ConstantKind::Unlabeled };
};

template<>
inline constexpr bool holdsListsOfUnit<ConstantData> = true;

void forEachListOfUnitIn(ConstantData& constant, auto& visit)
{
    visit(constant.bytes);
}

struct JumpTable {
    friend bool operator==(const JumpTable&, const JumpTable&) = default;

    ListOfUnit<uint32_t> targets;
};

template<>
inline constexpr bool holdsListsOfUnit<JumpTable> = true;

void forEachListOfUnitIn(JumpTable& table, auto& visit)
{
    visit(table.targets);
}

struct ReturnSite {
    uint32_t offset;
    OptionalIndex site;
};

struct FirstReadSite {
    uint32_t returnOffset;
    uint32_t site;
    uint32_t columnDelta { 0 };
};

struct CodeRange {
    uint32_t begin;
    uint32_t end;
};

struct NumberedStubKey {
    uint32_t stubKey;
    uint32_t mention;
};

struct RareListsOfBody {
    ListOfUnit<NumberedStubKey> numberedStubKeys;
    ListOfUnit<ConstantData> constants;
    ListOfUnit<JumpTable> jumpTables;
    ListOfUnit<uint8_t> bytesOfMadeTexts;
    ListOfUnit<uint32_t> firstsOfMadeTexts;
    ListOfUnit<uint32_t> endsOfMadeTexts;
};

template<>
inline constexpr bool holdsListsOfUnit<RareListsOfBody> = true;

void forEachListOfUnitIn(RareListsOfBody& lists, auto& visit)
{
    visit(lists.numberedStubKeys);
    visit(lists.constants);
    visit(lists.jumpTables);
    visit(lists.bytesOfMadeTexts);
    visit(lists.firstsOfMadeTexts);
    visit(lists.endsOfMadeTexts);
}

struct CodeBody {
    uint32_t primaryEntryOffset() const { return entries[0].offset; }
    std::optional<uint32_t> offsetOfEntry(uint16_t id) const;

    ListOfUnit<uint8_t> bytes;
    ListOfUnit<CodeEntry, 1> entries;
    ListOfUnit<Relocation> relocations;
    ListOfUnit<ReturnSite> returnSites;
    ListOfUnit<FirstReadSite> firstReadSites;
    ListOfUnit<Typed::CodeUnit> callees;
    ListOfUnit<StubKey> stubKeys;
    ArgumentMovesOfUnit argumentMoves;
    ListOfUnit<Mention> mentions;
    ListOfUnit<uint32_t> mentionWords;
    RarePartOfUnit<RareListsOfBody> rareLists;
};

inline std::optional<uint32_t> CodeBody::offsetOfEntry(uint16_t id) const
{
    if (!entries.isEmpty() && entries.first().id == id)
        return entries.first().offset;
    auto found = std::ranges::lower_bound(entries, id, { }, &CodeEntry::id);
    if (found == entries.end() || found->id != id)
        return std::nullopt;
    return found->offset;
}

class SavedRegisters {
public:
    SavedRegisters() = default;
    SavedRegisters(const RegisterAtOffsetList& registers)
    {
        m_registers.reserveCapacity(registers.registerCount());
        for (const RegisterAtOffset& saved : registers)
            m_registers.append(saved);
    }

    size_t registerCount() const { return m_registers.size(); }
    const RegisterAtOffset& at(size_t index) const { return m_registers[index]; }
    const RegisterAtOffset* begin() const { return m_registers.begin(); }
    const RegisterAtOffset* end() const { return m_registers.end(); }
    ListOfUnit<RegisterAtOffset>& list() { return m_registers; }

private:
    ListOfUnit<RegisterAtOffset> m_registers;
};

struct FrameDescription {
    bool hasFrame() const { return !frameRanges.isEmpty(); }
    bool hasFrameAt(uint32_t offset) const;

    unsigned frameSizeInBytes { 0 };
    unsigned bytesAboveFrameRecord { 0 };
    unsigned bytesDeadAtSuspensions { 0 };
    bool hasFrameRecord { true };
    bool spillsArgumentRegisters { false };
    SavedRegisters calleeSaveRegisters;
    ListOfUnit<CodeRange, 1> frameRanges;
    uint32_t stackUsedAtCalls { 0 };
    bool isEnteredWithoutStackCheck { false };
    bool isProvenToFitOnStack { false };
    ListOfUnit<PrologueCheck, 1> prologueChecks;
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
static_assert(sizeof(SitePosition) == 20);

struct SiteConstructionStart {
    uint32_t site;
    uint32_t line;
    uint32_t column;
    bool isForCallers { false };
    bool isWhereWritten { false };
};

struct InlineFrame {
    uint32_t parent;
    uint32_t callSite;
    uint32_t callee;
    bool isTailCall;
    bool isConstruct;
    BuiltinID calledBuiltin { };
};

struct FlaggedSite {
    uint32_t site;
    CallSiteKind kind;
    BuiltinID calledBuiltin { };
};

struct ConstructedCallee {
    uint32_t site;
    uint32_t callee;
};

struct SiteHandler {
    uint32_t site;
    uint32_t codeOffset;
    HandlerType type;
};

struct FaultingAccess {
    uint32_t codeOffset;
    OptionalIndex callSite;
    ListOfUnit<uint32_t, 3> throwSite;
    uint8_t wordsThatAreMentions { 0 };
};

template<>
inline constexpr bool holdsListsOfUnit<FaultingAccess> = true;

void forEachListOfUnitIn(FaultingAccess& access, auto& visit)
{
    visit(access.throwSite);
}

struct RareListsOfSites {
    ListOfUnit<SiteConstructionStart> constructionStarts;
    ListOfUnit<ConstructedCallee> constructedCallees;
    ListOfUnit<SiteHandler> handlers;
    ListOfUnit<PollSite> pollSites;
    ListOfUnit<uint32_t> frameMapWords;
};

template<>
inline constexpr bool holdsListsOfUnit<RareListsOfSites> = true;

void forEachListOfUnitIn(RareListsOfSites& lists, auto& visit)
{
    visit(lists.constructionStarts);
    visit(lists.constructedCallees);
    visit(lists.handlers);
    visit(lists.pollSites);
    visit(lists.frameMapWords);
}

struct SiteDescription {
    bool hasEncodedInlineFrames() const { return inlineFrames.size() > 1 && positions.size() > 1 + numberOfSitesWithoutCode; }

    uint32_t numberOfSitesWithoutCode { 0 };
    ListOfUnit<SitePosition> positions;
    ListOfUnit<InlineFrame> inlineFrames;
    ListOfUnit<FlaggedSite> flaggedSites;
    ListOfUnit<FaultingAccess> faultingAccesses;
    RarePartOfUnit<RareListsOfSites> rareLists;
};

struct BytesOfForm {
    Typed::Opcode opcode;
    Wire::CodeForm form;
    uint32_t count;
    uint32_t bytes;
    Typed::StateOfKnowledge state { Typed::StateOfKnowledge::Always };
    Typed::Place place { Typed::Place::Anywhere };
    bool isNamed { true };
    uint32_t countThatLooksOtherwise { 0 };
    uint32_t countLongerThanCall { 0 };
    uint32_t schemaMismatchCount { 0 };
    uint32_t countFoundOpen { 0 };
    uint32_t countFoundClosed { 0 };
    uint32_t countFoundRuntime { 0 };

    bool isChosenForColdCode() const { return place != Typed::Place::Anywhere && place != Typed::Place::Loop; }
};

struct BytesOutsideOperations {
    uint32_t origin;
    bool isRare;
    uint32_t count;
    uint32_t bytes;
};

struct RangeOfCode {
    uint32_t begin;
    uint32_t end;
    uint32_t origin;
    Typed::ValueID value;
    Typed::Opcode opcode;
    bool isRare;
    uint32_t inlineFrame { 0 };
    bool standsInRareBlock { false };
};

struct MeasurementsOfCode {
    ListOfUnit<BytesOfForm> bytesByForm;
    ListOfUnit<BytesOutsideOperations> bytesOutsideOperations;
    ListOfUnit<RangeOfCode> ranges;
    uint32_t bytesInInlineFrames { 0 };
    uint32_t bytesInInlineFramesOfLibrary { 0 };
    uint32_t bytesInFramesOfBuiltins { 0 };
};

template<>
inline constexpr bool holdsListsOfUnit<MeasurementsOfCode> = true;

void forEachListOfUnitIn(MeasurementsOfCode& measurements, auto& visit)
{
    visit(measurements.bytesByForm);
    visit(measurements.bytesOutsideOperations);
    visit(measurements.ranges);
}

constexpr unsigned rankInCodeOf(Typed::CodeRole role)
{
    switch (role) {
    case Typed::CodeRole::GenericConstructEntry:
        return 0;
    case Typed::CodeRole::AllocatingConstructEntry:
        return 1;
    case Typed::CodeRole::GenericCallEntry:
        return 2;
    case Typed::CodeRole::ModuleStart:
        return 3;
    case Typed::CodeRole::ParameterGuard:
        return 4;
    case Typed::CodeRole::Body:
        return 5;
    case Typed::CodeRole::CoroutineBody:
        return 6;
    case Typed::CodeRole::SlowBody:
        return 7;
    case Typed::CodeRole::TableEntry:
    case Typed::CodeRole::TableLookup:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr uint32_t placeInCodeOf(Typed::CodeUnit unit, uint32_t unitIndex)
{
    if (Typed::isTableRole(unit.role))
        return unitIndex;
    return unitIndex - static_cast<unsigned>(unit.role) + rankInCodeOf(unit.role);
}

struct CompiledCode {
    CompiledCode() = default;
    CompiledCode(const CompiledCode&) = default;
    CompiledCode(CompiledCode&&);
    CompiledCode& operator=(const CompiledCode&) = default;
    CompiledCode& operator=(CompiledCode&&) = default;

    Typed::CodeUnit unit;
    uint32_t unitIndex { UINT32_MAX };
    uint32_t placeInCode { UINT32_MAX };
    FunctionID function;
    EntryDescription entry;
    CodeBody body;
    FrameDescription frame;
    SiteDescription sites;
    RarePartOfUnit<MeasurementsOfCode> measurements;
};

void forEachListOfUnitWhateverItHoldsIn(CompiledCode& code, auto& visit)
{
    auto visitEach = [&](auto&... lists) {
        (visit(lists), ...);
    };
    CodeBody& body = code.body;
    SiteDescription& sites = code.sites;
    visitEach(body.bytes, body.entries, body.relocations, body.returnSites, body.firstReadSites, body.callees, body.stubKeys, body.argumentMoves.list(), body.mentions, body.mentionWords, body.rareLists);
    visitEach(code.frame.calleeSaveRegisters.list(), code.frame.frameRanges, code.frame.prologueChecks);
    visitEach(sites.positions, sites.inlineFrames, sites.flaggedSites, sites.faultingAccesses, sites.rareLists);
    visitEach(code.measurements);
}

void forEachListOfUnitIn(CompiledCode& code, auto& visit)
{
    auto visitIfItHoldsAnything = [&](auto& list) {
        if (!list.isEmpty() || list.liesOnHeap())
            visit(list);
    };
    forEachListOfUnitWhateverItHoldsIn(code, visitIfItHoldsAnything);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

inline CompiledCode::CompiledCode(CompiledCode&& other)
{
    memcpy(static_cast<void*>(this), static_cast<const void*>(&other), sizeof(CompiledCode));
    auto leaveEmpty = [](auto& list) {
        new (NotNull, &list) std::remove_reference_t<decltype(list)>;
    };
    forEachListOfUnitWhateverItHoldsIn(other, leaveEmpty);
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

struct CodeGenerationTotals {
    WTF::BitSet<numberOfEntries> entriesUsed;
};

template<typename Functor>
void forEachReturnAddress(const CompiledCode& code, const Functor& functor)
{
    std::span<const PrologueCheck> prologueChecks = code.frame.prologueChecks.span();
    std::span<const ReturnSite> returnSites = code.body.returnSites.span();
    while (!prologueChecks.empty() && !returnSites.empty()) {
        const ReturnSite& site = returnSites.front();
        returnSites = returnSites.subspan(1);
        while (!prologueChecks.empty() && prologueChecks.front().callOffset + sizeOfNearCall < site.offset)
            prologueChecks = prologueChecks.subspan(1);
        if (!prologueChecks.empty() && prologueChecks.front().callOffset + sizeOfNearCall == site.offset)
            continue;
        functor(site.offset, site.site);
    }
    for (const ReturnSite& site : returnSites)
        functor(site.offset, site.site);
    for (const FaultingAccess& access : code.sites.faultingAccesses)
        functor(access.codeOffset + static_cast<uint32_t>(sizeOfNearCall), access.callSite);
    for (const PollSite& poll : code.sites.rareLists->pollSites)
        functor(poll.returnOffset(), poll.callSite);
}

} // namespace JSC::AOT

#endif // ENABLE(AOT)
