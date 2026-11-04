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
    // Zero: the read has no site. It is written columnDelta columns in front of the site at the return offset.
    uint32_t site;
    uint32_t columnDelta { 0 };
};

struct CodeRange {
    uint32_t begin;
    uint32_t end;
};

// The immediate of stubKeys[stubKey] is the number of mentions[mention]. Until Image puts it in, the key has the immediate 0.
struct NumberedStubKey {
    uint32_t stubKey;
    uint32_t mention;
};

struct CodeBody {
    uint32_t primaryEntryOffset() const { return entries[0].offset; }
    std::optional<uint32_t> offsetOfEntry(uint16_t id) const;

    ListOfUnit<uint8_t> bytes;
    // Ascending by id, each id once. The first is primaryEntry.
    ListOfUnit<CodeEntry, 1> entries;
    ListOfUnit<Relocation> relocations;
    ListOfUnit<ReturnSite> returnSites;
    ListOfUnit<FirstReadSite> firstReadSites;
    // A unit that a relocation names has to have code. A unit that only an inline frame or a site names need not.
    ListOfUnit<Typed::CodeUnit> callees;
    ListOfUnit<StubKey> stubKeys;
    // Ascending by stubKey.
    ListOfUnit<NumberedStubKey> numberedStubKeys;
    // With their two header words. A number that Image gives stands there as a mention: the immediate under immediateIsMention, a move as ArgumentSource::ThrowWordWithMention.
    // applyNumbersToFunctions() puts the numbers in before the lists of different functions are compared.
    ListOfUnit<ArgumentMoves> argumentMoves;
    ListOfUnit<ConstantData> constants;
    ListOfUnit<JumpTable> jumpTables;
    ListOfUnit<Mention> mentions;
    ListOfUnit<uint32_t> mentionWords;
    // The texts that the walk of the unit made and that a mention names, as canonical program strings one behind the other, in the order in which the mentions first name them.
    // Until internTextsMadeByFunctions() such a text stands among mentionWords as a StringID that liesInThread(), whose position is its place here. From then on both lists are empty.
    ListOfUnit<uint8_t> bytesOfMadeTexts;
    ListOfUnit<uint32_t> endsOfMadeTexts;
};

inline std::optional<uint32_t> CodeBody::offsetOfEntry(uint16_t id) const
{
    auto found = std::ranges::lower_bound(entries, id, { }, &CodeEntry::id);
    if (found == entries.end() || found->id != id)
        return std::nullopt;
    return found->offset;
}

// A RegisterAtOffsetList whose registers are a list of the unit.
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
    uint32_t constructionStartLine { 0 };
    uint32_t constructionStartColumn { 0 };
    bool startIsForCallers { false };
    bool startIsWhereWritten { false };
};

struct InlineFrame {
    uint32_t parent;
    uint32_t callSite;
    uint32_t callee;
    bool isTailCall;
    bool isConstruct;
    // Set for the frame of a built-in function whose steps were expanded into the callee.
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
    // Bit i: throwSite[i] is an index into CodeBody::mentions. applyNumbersToFunctions() puts ImageNumbers::valueOf() in its place and clears the bit, right behind the numbering: the fault table sees numbers alone.
    // An `a` that turns out narrow goes into the first word there, and its own word goes: ThrowSiteWord::withNarrowA().
    uint8_t wordsThatAreMentions { 0 };
};

template<>
inline constexpr bool holdsListsOfUnit<FaultingAccess> = true;

void forEachListOfUnitIn(FaultingAccess& access, auto& visit)
{
    visit(access.throwSite);
}

struct SiteDescription {
    bool hasEncodedInlineFrames() const { return inlineFrames.size() > 1 && positions.size() > 1 + numberOfSitesWithoutCode; }

    uint32_t numberOfSitesWithoutCode { 0 };
    ListOfUnit<SitePosition> positions;
    ListOfUnit<InlineFrame> inlineFrames;
    ListOfUnit<FlaggedSite> flaggedSites;
    // Sorted by site. A site without code stands for a call that an inline frame replaced.
    ListOfUnit<CalleeAtSite> calleesOfSites;
    // In rising order of the sites. The callee is an index into CodeBody::callees.
    ListOfUnit<ConstructedCallee> constructedCallees;
    ListOfUnit<SiteHandler> handlers;
    ListOfUnit<FaultingAccess> faultingAccesses;
    ListOfUnit<PollSite> pollSites;
    ListOfUnit<uint32_t> frameMapWords;
};

// Only a build that explains itself or writes a map fills these. Ascending by opcode, state, place and form, a row that is named in front of one that is not.
// `form`, `state` and `place` are what the emitter said (codegen/AOTFormsSaid.h), and what the schema names where it said nothing. `count`: how often the operation was handed to its emitter and got code.
struct BytesOfForm {
    Typed::Opcode opcode;
    Wire::CodeForm form;
    uint32_t count;
    uint32_t bytes;
    Typed::StateOfKnowledge state { Typed::StateOfKnowledge::Always };
    Typed::Place place { Typed::Place::Anywhere };
    // The schema names this form for the operation, in this state and at this place, and the operation stands at that place.
    bool isNamed { true };
    // Of `count`: the code in front of the rare code calls a stub or the run time where the form is Open, calls neither where it is not, or calls the other of the two.
    uint32_t countThatLooksOtherwise { 0 };
    // Of `count`, where the form is not Open: the code in front of the rare code is more than the call and what brings the words of the operation to it.
    uint32_t countLongerThanCall { 0 };
    // Of `count`: each site that is not named, looks otherwise or is longer than a call, once. StatisticID::FormsThatDifferFromSchema is the sum.
    uint32_t countThatDiffersFromSchema { 0 };
    // Of `count`, the sites that have code in front of the rare code, by what that code calls: nothing, a stub, the run time.
    uint32_t countFoundOpen { 0 };
    uint32_t countFoundClosed { 0 };
    uint32_t countFoundRuntime { 0 };

    // The design lets a loop of the unit pick a form, and nothing else about where the operation stands. StatisticID::FormsPickedByPlaceOtherThanLoop is the sum of `count` over such rows.
    bool isPickedByPlaceOtherThanLoop() const { return place != Typed::Place::Anywhere && place != Typed::Place::Loop; }
};

// The rest of the code, filled with them. `origin` says what it is: an origin of the back end (machine/AOTMachineDescription.h), one of the end of a block (codegen/AOTCodeGenerator.h),
// or zero for what the emitters append outside every operation and every end of a block. Ascending by origin, the code in front of the rare code first.
struct BytesOutsideOperations {
    uint32_t origin;
    bool isRare;
    uint32_t count;
    uint32_t bytes;
};

// All the code range by range, filled with them. Ascending and apart, and together they are CodeBody::bytes. A range that was written for an operation names its value and its opcode.
// Any other has no value, and `origin` says what it is, as in BytesOutsideOperations.
struct RangeOfCode {
    uint32_t begin;
    uint32_t end;
    uint32_t origin;
    Typed::ValueID value;
    Typed::Opcode opcode;
    bool isRare;
};

// Where the code of a unit lies among the units of its function: an entry lies in front of what it goes on to, so that a jump to the next body can go.
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
    case Typed::CodeRole::BoundFunction:
        return 1;
    case Typed::CodeRole::GenericEntryOfBoundFunction:
        return 0;
    // Each has an index of its own, and goes on to no unit that lies beside it.
    case Typed::CodeRole::TableEntry:
    case Typed::CodeRole::TableLookup:
    case Typed::CodeRole::Serializer:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

// unitIndex is CallingConventions::indexOf(unit): function * numberOfCodeRolesOfOneFunction + role, and the two units of each planned bound function behind all of those.
// Behind those come the units of the method table, which lie in the order of their indexes.
// The answer is as dense, and tells all units apart.
constexpr uint32_t placeInCodeOf(Typed::CodeUnit unit, uint32_t unitIndex)
{
    if (unit.role == Typed::CodeRole::Serializer)
        return unitIndex;
    if (Typed::isTableRole(unit.role))
        return unitIndex;
    unsigned firstRole = Typed::isBoundRole(unit.role) ? Typed::numberOfCodeRolesOfOneFunction : 0;
    return unitIndex - (static_cast<unsigned>(unit.role) - firstRole) + rankInCodeOf(unit.role);
}

// Code::calleesOfIndirectCalls() of the calls that the sweep has kept. The lists and the functions ascend, each once. The queue of units reads them and lets them go.
struct CalleesOfIndirectCalls {
    Vector<uint32_t> lists;
    Vector<FunctionID> functions;
    uint32_t numberOfCallsThatMayEnterAnyClosure { 0 };
};

// All that is left of a unit once its thread goes on to the next.
struct CompiledCode {
    Typed::CodeUnit unit;
    // CallingConventions::indexOf(unit).
    uint32_t unitIndex { UINT32_MAX };
    // placeInCodeOf(unit, unitIndex).
    uint32_t placeInCode { UINT32_MAX };
    // Image's: FunctionNumbers::idOf(unitIndex).
    FunctionID function;
    CodeBody body;
    FrameDescription frame;
    EntryDescription entry;
    SiteDescription sites;
    ListOfUnit<BytesOfForm> bytesByForm;
    // Of the bytes in bytesByForm, those of operations that stand in an inline frame. Of these, where the callee is text of the library, and where the frame holds the steps of a built-in function.
    uint32_t bytesInInlineFrames { 0 };
    uint32_t bytesInInlineFramesOfLibrary { 0 };
    uint32_t bytesInFramesOfBuiltins { 0 };
    ListOfUnit<BytesOutsideOperations> bytesOutsideOperations;
    ListOfUnit<RangeOfCode> ranges;
    CalleesOfIndirectCalls calleesOfIndirectCalls;
    // CompileWorker::compileClosedGenericEntry() has made it.
    bool isClosedGenericEntry { false };
};

// Every list but those of calleesOfIndirectCalls, which the queue lets go before the record is kept.
void forEachListOfUnitIn(CompiledCode& code, auto& visit)
{
    auto visitEach = [&](auto&... lists) {
        (visit(lists), ...);
    };
    CodeBody& body = code.body;
    SiteDescription& sites = code.sites;
    visitEach(body.bytes, body.entries, body.relocations, body.returnSites, body.firstReadSites, body.callees, body.stubKeys, body.numberedStubKeys, body.argumentMoves, body.constants, body.jumpTables, body.mentions, body.mentionWords, body.bytesOfMadeTexts, body.endsOfMadeTexts);
    visitEach(code.frame.calleeSaveRegisters.list(), code.frame.frameRanges, code.frame.prologueChecks);
    visitEach(sites.positions, sites.inlineFrames, sites.flaggedSites, sites.calleesOfSites, sites.constructedCallees, sites.handlers, sites.faultingAccesses, sites.pollSites, sites.frameMapWords);
    visitEach(code.bytesByForm, code.bytesOutsideOperations, code.ranges);
}

// One for each thread of Compile. The driver unites them.
struct CodeGenerationTotals {
    WTF::BitSet<numberOfEntries> entriesUsed;
};

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
