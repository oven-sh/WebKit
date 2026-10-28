/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeDescription.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCallSiteTable.h"
#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionKinds.h"
#include "AOTHeat.h"
#include "AOTOperationData.h"
#include "AOTSort.h"
#include "AOTStackCheckEmitter.h"
#include "AOTThreshold.h"
#include "AOTThrowEmitter.h"
#include "AOTVector.h"
#include "AirCode.h"
#include "B3Procedure.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <bit>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DescribedFunction {
    BuildContext& build;
    const Program& program;
    FunctionID id;
    const Function& function;
};

struct SiteReference {
    uint32_t returnOffset;
    uint32_t* site;
    FirstReadSite* firstRead { nullptr };
};

// B3 may copy a site, and each copy has an edge to a landing pad of its own. The copies record their landing pads in the order in which their code is emitted.
class LandingPadsOfCopies {
public:
    LandingPadsOfCopies(const ExceptionRecords& exceptions, size_t numberOfSites, std::span<const SiteReference> references)
        : m_landingPads(exceptions.sites)
    {
        if (m_landingPads.isEmpty())
            return;
        AOT::stableSort(m_landingPads, { }, &SiteLandingPad::site);
        m_copies.grow(numberOfSites + 1);
        for (uint32_t index = m_landingPads.size(); index--;) {
            Copies& copies = m_copies[m_landingPads[index].site];
            copies.firstLandingPad = index;
            ++copies.numberOfLandingPads;
        }
        for (const SiteReference& reference : references)
            ++m_copies[*reference.site].numberOfReferences;
    }

    // Called once for each reference, by ascending return offset.
    const SiteLandingPad* takeNext(uint32_t recordedNumber)
    {
        if (m_landingPads.isEmpty())
            return nullptr;
        Copies& copies = m_copies[recordedNumber];
        if (!copies.numberOfLandingPads)
            return nullptr;
        RELEASE_ASSERT(!(copies.numberOfReferences % copies.numberOfLandingPads));
        uint32_t referencesPerCopy = copies.numberOfReferences / copies.numberOfLandingPads;
        return &m_landingPads[copies.firstLandingPad + copies.numberOfReferencesTaken++ / referencesPerCopy];
    }

private:
    struct Copies {
        uint32_t firstLandingPad { 0 };
        uint32_t numberOfLandingPads { 0 };
        uint32_t numberOfReferences { 0 };
        uint32_t numberOfReferencesTaken { 0 };
    };

    Vector<SiteLandingPad> m_landingPads;
    Vector<Copies> m_copies;
};

struct ShownFunction {
    FunctionID id;
    std::optional<LineColumn> positionInBuiltin;
};

using ShownFunctions = Vector<ShownFunction, 4>;

} // anonymous namespace

static constexpr LineColumn hostFunctionPosition { 0, 0 };
static constexpr LineColumn superCallOfDefaultConstructor { 1, 28 };
static_assert(sizeof("(function (...args) { super") == superCallOfDefaultConstructor.column, "JSC makes a default constructor from a text of its own");
static constexpr LineColumn startOfSuperCallOfDefaultConstructor { 1, 23 };
static_assert(sizeof("(function (...args) { ") == startOfSuperCallOfDefaultConstructor.column);

static void reportUndeclaredTrap(const DescribedFunction& described, ValueID value)
{
    described.build.diagnostics().add(InternalError {
        .kind = InternalErrorKind::InvalidCodeUnits,
        .function = findFunctionAsRead(&described.program, described.id),
        .location = value ? described.function.locationOf(value) : described.function.declarationLocation(),
    });
}

static Vector<FaultingAccess> collectFaultingAccesses(const DescribedFunction& described, CodeRecords& records, LinkBuffer& linkBuffer, const CodeBody& body)
{
    TrapRecords& traps = records.traps;
    RELEASE_ASSERT(!described.build.validates() || records.units.takeTraps().size() == traps.trappingAccesses.size());

    AOT::sort(traps.trapSites, { }, &TrapSite::value);
    Vector<FaultingAccess> accesses;
    accesses.reserveInitialCapacity(traps.trappingAccesses.size() + traps.callThrowSites.size());
    for (const TrappingAccess& access : traps.trappingAccesses) {
        auto site = std::ranges::lower_bound(traps.trapSites, access.value, { }, &TrapSite::value);
        if (site == traps.trapSites.end() || site->value != access.value) {
            reportUndeclaredTrap(described, access.value);
            continue;
        }
#if CPU(ARM64)
        // The handler of faults finds a site by the address of the instruction that faulted, so the record names the load or the store itself.
        uint32_t instruction;
        memcpy(&instruction, body.bytes.subspan(linkBuffer.offsetOf(access.instruction), sizeof(instruction)).data(), sizeof(instruction));
        RELEASE_ASSERT((instruction & 0x0a000000) == 0x08000000, instruction);
#endif
        accesses.append({ linkBuffer.offsetOf(access.instruction), OptionalIndex { site->callSite }, throwSiteWords(site->throwSite, access.referenceIsNarrow ? Representation::Cell32 : site->representation, static_cast<unsigned>(access.reference)) });
    }
    for (const CallThrowSite& site : traps.callThrowSites)
        accesses.append({ body.returnSites[site.returnSite].offset - static_cast<uint32_t>(sizeOfNearCall), { }, site.words });
    AOT::sort(accesses, { }, &FaultingAccess::codeOffset);
    return accesses;
}

static Vector<SiteReference, 32> collectSiteReferences(CodeBody& body, SiteDescription& sites)
{
    Vector<SiteReference, 32> references;
    for (FirstReadSite& site : body.firstReadSites)
        references.append({ site.returnOffset, &site.site, &site });
    for (ReturnSite& site : body.returnSites) {
        if (site.site)
            references.append({ site.offset, &site.site.bits });
    }
    for (FaultingAccess& access : sites.faultingAccesses) {
        if (access.callSite)
            references.append({ access.codeOffset + static_cast<uint32_t>(sizeOfNearCall), &access.callSite.bits });
    }
    for (PollSite& poll : sites.pollSites) {
        if (poll.callSite)
            references.append({ poll.returnOffset(), &poll.callSite.bits });
    }
    AOT::stableSort(references, { }, &SiteReference::returnOffset);
    return references;
}

static std::optional<LineColumn> positionOfImplementedBuiltin(StackTraceBuiltinKind kind)
{
    switch (kind) {
    case StackTraceBuiltinKind::None:
        return std::nullopt;
    case StackTraceBuiltinKind::HostFunction:
        return hostFunctionPosition;
    case StackTraceBuiltinKind::JSBuiltin:
    case StackTraceBuiltinKind::JSBuiltinConstructor:
        return startOfBuiltinFunction;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The front end gives a constructor that no class member writes the position of its class.
static bool isDefaultConstructor(const Program& program, const Function& function)
{
    if (function.kind != FunctionKind::BaseConstructor && function.kind != FunctionKind::DerivedConstructor)
        return false;
    return function.constructedClass && program.at(function.constructedClass).position == function.position;
}

static std::optional<LineColumn> positionInBuiltin(const Program& program, FunctionID shown)
{
    const Function& owner = program.at(shown);
    if (isDefaultConstructor(program, owner))
        return owner.kind == FunctionKind::DerivedConstructor ? superCallOfDefaultConstructor : startOfBuiltinFunction;
    if (!owner.enclosingRuntimeFunction)
        return std::nullopt;
    switch (program.builtins().at(owner.enclosingRuntimeFunction).role) {
    case RuntimeFunctionRole::CompilerFunction:
        return positionOfImplementedBuiltin(functionTraceOf(program, shown).builtinKind());
    case RuntimeFunctionRole::Model:
        return std::nullopt;
    case RuntimeFunctionRole::EngineBuiltin:
    case RuntimeFunctionRole::HostBuiltin:
        break;
    }
    // In the engine each registered function is a source of its own. A helper outside all of them is shown like one.
    const RuntimeFunctionEntry* registered = findRuntimeFunction(program, owner.enclosingRuntimeFunction);
    if (!registered || !registered->function || registered->function == shown)
        return startOfBuiltinFunction;
    LineColumn start = program.lineColumnFor(owner.module, owner.position);
    unsigned firstLine = program.lineColumnFor(owner.module, program.at(registered->function).position).line;
    return LineColumn { start.line - firstLine + 1, start.column };
}

static SitePosition positionIn(const Program& program, const ShownFunction& shown, uint32_t position, uint32_t frame)
{
    if (shown.positionInBuiltin)
        return { shown.positionInBuiltin->line, shown.positionInBuiltin->column, ModuleID(), 0, frame };
    const Function& owner = program.at(shown.id);
    if (position == Instruction::noPosition || (!position && !owner.isModuleBody()))
        position = owner.position;
    if (position == Instruction::noPosition)
        position = 0;
    LineColumn lineColumn = program.lineColumnFor(owner.module, position);
    return { lineColumn.line, lineColumn.column, owner.module, position, frame };
}

static ShownFunctions findShownFunctions(const DescribedFunction& described)
{
    uint32_t numberOfFrames = std::max<uint32_t>(described.function.inlineFrames.size(), 1);
    ShownFunctions shownFunctions;
    shownFunctions.reserveInitialCapacity(numberOfFrames);
    for (uint32_t frame = 0; frame < numberOfFrames; ++frame) {
        FunctionID shown = firstSourceOf(described.program, frame ? described.function.inlineFrames[frame].callee : described.id);
        shownFunctions.append({ shown, positionInBuiltin(described.program, shown) });
    }
    return shownFunctions;
}

static void describeConstructionStart(const Program& program, const ShownFunction& shown, const RecordedSite& site, SitePosition& position)
{
    if (!site.origin.isPartOfConstruction && !isConstructOf(site.kind))
        return;
    const Function& owner = program.at(shown.id);
    if (owner.kind == FunctionKind::DerivedConstructor && isDefaultConstructor(program, owner)) {
        position.constructionStartLine = startOfSuperCallOfDefaultConstructor.line;
        position.constructionStartColumn = startOfSuperCallOfDefaultConstructor.column;
        return;
    }
    if (shown.positionInBuiltin)
        return;
    ModuleID module = program.at(shown.id).module;
    const ConstructionStart* construction = program.findConstructionReportedAt(module, position.sourceOffset);
    if (!construction || (construction->isWrittenAsCall && !isConstructOf(site.kind)))
        return;
    LineColumn lineColumn = program.lineColumnFor(module, construction->start);
    position.constructionStartLine = lineColumn.line;
    position.constructionStartColumn = lineColumn.column;
    position.startIsForCallers = construction->isForCallers;
}

static SitePosition describePosition(const Program& program, const ShownFunctions& shownFunctions, const RecordedSite& site)
{
    const ShownFunction& shown = shownFunctions[site.origin.inlineFrame];
    SitePosition position = positionIn(program, shown, site.origin.position, site.origin.inlineFrame);
    describeConstructionStart(program, shown, site, position);
    return position;
}

static std::optional<unsigned> firstReadColumnDelta(const SitePosition& read, const SitePosition& site)
{
    if (read.line != site.line || read.source != site.source || read.inlineFrame != site.inlineFrame || read.constructionStartLine || read.column > site.column)
        return std::nullopt;
    return site.column - read.column;
}

// A method is mostly read a few columns in front of its call. The throw site of the call has room for that number, and the read needs no site.
static void foldMethodReadSites(const DescribedFunction& described, const ShownFunctions& shownFunctions, const Vector<RecordedSite>& recorded, CodeBody& body, SiteDescription& sites)
{
    if (described.build.options().obfuscates())
        return;
    body.firstReadSites.removeAllMatching([&](const FirstReadSite& read) {
        uint32_t callOffset = read.returnOffset - static_cast<uint32_t>(sizeOfNearCall);
        auto access = std::ranges::lower_bound(sites.faultingAccesses, callOffset, { }, &FaultingAccess::codeOffset);
        if (access == sites.faultingAccesses.end() || access->codeOffset != callOffset || access->callSite)
            return false;
        ThrowSiteWord word = std::bit_cast<ThrowSiteWord>(access->throwSite[0]);
        if (!word.isCallSite())
            return false;
        auto call = std::ranges::lower_bound(body.returnSites, read.returnOffset, { }, &ReturnSite::offset);
        RELEASE_ASSERT(call != body.returnSites.end() && call->offset == read.returnOffset && call->site);
        std::optional<unsigned> delta = firstReadColumnDelta(describePosition(described.program, shownFunctions, recorded[read.site - 1]), describePosition(described.program, shownFunctions, recorded[*call->site - 1]));
        if (!delta || *delta > ThrowSiteWord::maxMethodReadColumnDelta)
            return false;
        word.registerNumber = *delta + 1;
        access->throwSite[0] = std::bit_cast<uint32_t>(word);
        return true;
    });
}

static Vector<RecordedSite> renumberSites(const DescribedFunction& described, const ShownFunctions& shownFunctions, const CodeRecords& records, LinkBuffer& linkBuffer, std::span<const SiteReference> references, SiteDescription& sites)
{
    const Vector<RecordedSite>& recorded = records.sites.positions;
    LandingPadsOfCopies landingPads(records.exceptions, recorded.size(), references);
    auto recordOf = [&](uint32_t recordedNumber) -> const RecordedSite& {
        RELEASE_ASSERT(recordedNumber && recordedNumber <= recorded.size());
        return recorded[recordedNumber - 1];
    };

    // One operation of the program is often several calls in a row.
    struct LastSite {
        const RecordedSite* record { nullptr };
        const SiteLandingPad* landingPad { nullptr };
    };
    LastSite last;
    auto leadToSameHandler = [](const SiteLandingPad* a, const SiteLandingPad* b) {
        return a && b ? a->landingPad == b->landingPad && a->type == b->type : a == b;
    };

    // The row of a plain site in the frame of the site in front has room for the columns from its first read. A read at the position of its site says nothing.
    bool mayOmitFirstReadSites = !described.build.options().obfuscates();
    unsigned frameOfLastSite = 0;
    auto columnDeltaWithoutSite = [&](const RecordedSite& read, const RecordedSite& site) -> std::optional<unsigned> {
        if (!mayOmitFirstReadSites)
            return std::nullopt;
        std::optional<unsigned> delta = firstReadColumnDelta(describePosition(described.program, shownFunctions, read), describePosition(described.program, shownFunctions, site));
        if (!delta || !*delta)
            return delta;
        if (*delta > maxFirstReadColumnDelta || site.kind != CallSiteKind::Plain || site.origin.inlineFrame != frameOfLastSite)
            return std::nullopt;
        return delta;
    };

    Vector<RecordedSite> result;
    result.reserveInitialCapacity(records.sites.numberOfSitesWithoutCode + references.size());
    result.append(recorded.span().first(records.sites.numberOfSitesWithoutCode));
    bool rowHoldsFirstRead = false;
    for (size_t index = 0; index < references.size(); ++index) {
        const SiteReference& reference = references[index];
        uint32_t recordedNumber = *reference.site;
        const RecordedSite& record = recordOf(recordedNumber);
        if (reference.firstRead) {
            RELEASE_ASSERT(index + 1 < references.size() && references[index + 1].returnOffset == reference.returnOffset && !references[index + 1].firstRead);
            if (std::optional<unsigned> delta = columnDeltaWithoutSite(record, recordOf(*references[index + 1].site))) {
                reference.firstRead->site = 0;
                reference.firstRead->columnDelta = *delta;
                rowHoldsFirstRead = !!*delta;
                continue;
            }
        }
        const SiteLandingPad* landingPad = landingPads.takeNext(recordedNumber);
        bool saysOnlyWhereItIs = !reference.firstRead && !std::exchange(rowHoldsFirstRead, false) && record.kind == CallSiteKind::Plain && !record.callsOwnFunctionObject && !record.keptCalleePlusOne;
        if (saysOnlyWhereItIs && last.record && *last.record == record && leadToSameHandler(last.landingPad, landingPad)) {
            *reference.site = result.size();
            continue;
        }
        result.append(record);
        frameOfLastSite = record.origin.inlineFrame;
        uint32_t number = result.size();
        *reference.site = number;
        if (landingPad)
            sites.handlers.append({ number, linkBuffer.offsetOf(*records.exceptions.landingPads[landingPad->landingPad]), landingPad->type });
        last = saysOnlyWhereItIs ? LastSite { &record, landingPad } : LastSite { };
    }
    return result;
}

static Vector<SitePosition> describePositions(const DescribedFunction& described, const ShownFunctions& shownFunctions, const Vector<RecordedSite>& renumbered)
{
    const Program& program = described.program;
    Vector<SitePosition> positions;
    positions.reserveInitialCapacity(renumbered.size() + 1);
    const Function& outermost = program.at(shownFunctions[0].id);
    positions.append(positionIn(program, shownFunctions[0], outermost.position, 0));
    if (isDefaultConstructor(program, outermost))
        positions[0].column = startOfBuiltinFunction.column;
    for (const RecordedSite& site : renumbered)
        positions.append(describePosition(program, shownFunctions, site));
    return positions;
}

static Vector<FlaggedSite> collectFlaggedSites(const Vector<RecordedSite>& renumbered, uint32_t numberOfSitesWithoutCode)
{
    Vector<FlaggedSite> flaggedSites;
    for (uint32_t i = numberOfSitesWithoutCode; i < renumbered.size(); ++i) {
        if (renumbered[i].kind != CallSiteKind::Plain)
            flaggedSites.append({ i + 1, renumbered[i].kind, renumbered[i].calledBuiltin });
    }
    return flaggedSites;
}

static Vector<std::pair<uint32_t, CalleeOfSite>> collectCalleesOfSites(const Vector<RecordedSite>& renumbered, const SiteRecords& records)
{
    Vector<std::pair<uint32_t, CalleeOfSite>> sites;
    for (uint32_t i = 0; i < renumbered.size(); ++i) {
        if (renumbered[i].callsOwnFunctionObject) {
            sites.append({ i + 1, CalleeOfSite::ownFunctionObject() });
            continue;
        }
        if (!renumbered[i].keptCalleePlusOne)
            continue;
        const KeptCallee& kept = records.keptCallees[renumbered[i].keptCalleePlusOne - 1];
        auto field = records.fieldsOfCallees.find(kept.call);
        if (field != records.fieldsOfCallees.end() && !field->value) {
            sites.append({ i + 1, CalleeOfSite::ownFunctionObject() });
            continue;
        }
        // Nothing writes the word of a call that no way leads to.
        if (kept.word >= records.wordsOfCallees.size() || !records.wordsOfCallees[kept.word])
            continue;
        intptr_t offset = records.wordsOfCallees[kept.word]->offsetFromFP();
        RELEASE_ASSERT(!(offset % static_cast<intptr_t>(sizeof(uint64_t))));
        int32_t word = safeCast<int32_t>(offset / static_cast<intptr_t>(sizeof(uint64_t)));
        sites.append({ i + 1, field != records.fieldsOfCallees.end() ? CalleeOfSite::inEnvironmentInFrame(word, *field->value) : CalleeOfSite::inFrame(word) });
    }
    return sites;
}

static uint32_t ensureCalleeIndex(CodeBody& body, FunctionID callee)
{
    size_t index = body.callees.find(callee);
    if (index != notFound)
        return static_cast<uint32_t>(index);
    body.callees.append(callee);
    return static_cast<uint32_t>(body.callees.size() - 1);
}

static Vector<ConstructedCallee> collectConstructedCallees(const Vector<RecordedSite>& renumbered, CodeBody& body)
{
    Vector<ConstructedCallee> callees;
    for (uint32_t i = 0; i < renumbered.size(); ++i) {
        if (renumbered[i].constructedCallee)
            callees.append({ i + 1, ensureCalleeIndex(body, renumbered[i].constructedCallee) });
    }
    return callees;
}

static Vector<AOT::InlineFrame> describeInlineFrames(const Function& function, const SiteRecords& sites, CodeBody& body)
{
    const Vector<uint32_t>& inlineCallSites = sites.inlineCallSites;
    RELEASE_ASSERT(inlineCallSites.size() == function.inlineFrames.size());
    Vector<AOT::InlineFrame> inlineFrames;
    if (function.inlineFrames.isEmpty())
        return inlineFrames;
    inlineFrames.reserveInitialCapacity(function.inlineFrames.size());
    inlineFrames.append({ 0, 0, 0, false, false });
    for (size_t index = 1; index < function.inlineFrames.size(); ++index) {
        const InlineFrame& frame = function.inlineFrames[index];
        inlineFrames.append({ frame.parent, inlineCallSites[index], ensureCalleeIndex(body, frame.callee), frame.isTailCall, frame.isConstruction, frame.calledBuiltin });
    }
    return inlineFrames;
}

static SiteDescription describeSites(const DescribedFunction& described, CodeRecords& records, LinkBuffer& linkBuffer, CodeBody& body)
{
    SiteDescription sites;
    sites.numberOfSitesWithoutCode = records.sites.numberOfSitesWithoutCode;
    sites.frameMapWords = WTF::move(records.frameMaps.words);
    sites.faultingAccesses = collectFaultingAccesses(described, records, linkBuffer, body);
    sites.pollSites = WTF::move(records.traps.pollSites);
    AOT::sort(sites.pollSites, { }, &PollSite::offset);

    ShownFunctions shownFunctions = findShownFunctions(described);
    foldMethodReadSites(described, shownFunctions, records.sites.positions, body, sites);
    Vector<SiteReference, 32> references = collectSiteReferences(body, sites);
    Vector<RecordedSite> renumbered = renumberSites(described, shownFunctions, records, linkBuffer, references.span(), sites);
    sites.positions = describePositions(described, shownFunctions, renumbered);
    sites.flaggedSites = collectFlaggedSites(renumbered, sites.numberOfSitesWithoutCode);
    sites.constructedCallees = collectConstructedCallees(renumbered, body);
    // No walk of the stack can stand in a body without a site that has code, so nobody asks which functions it took in.
    size_t numberOfInlineCallSites = described.function.inlineFrames.isEmpty() ? 0 : described.function.inlineFrames.size() - 1;
    if (numberOfInlineCallSites && sites.numberOfSitesWithoutCode == numberOfInlineCallSites && sites.positions.size() == 1 + numberOfInlineCallSites) {
        sites.numberOfSitesWithoutCode = 0;
        sites.positions.shrink(1);
        return sites;
    }
    sites.inlineFrames = describeInlineFrames(described.function, records.sites, body);
    sites.calleesOfSites = collectCalleesOfSites(renumbered, records.sites);
    return sites;
}

static Vector<CodeEntry, 1> describeEntries(CodeRecords& records, LinkBuffer& linkBuffer)
{
    Vector<RecordedEntry, 1>& recorded = records.entries;
    AOT::sort(recorded, { }, &RecordedEntry::id);
    RELEASE_ASSERT(!recorded.isEmpty() && recorded[0].id == primaryEntry);
    RELEASE_ASSERT(std::ranges::adjacent_find(recorded, { }, &RecordedEntry::id) == recorded.end());

    Vector<CodeEntry, 1> entries;
    entries.reserveInitialCapacity(recorded.size());
    for (const RecordedEntry& entry : recorded)
        entries.append({ linkBuffer.offsetOf(entry.label), entry.id });
    return entries;
}

Vector<ConstantData> copyConstants(const B3::Procedure& procedure)
{
    Vector<ConstantData> constants;
    constants.reserveInitialCapacity(procedure.dataSections().size());
    for (const B3::Procedure::DataSectionRecord& section : procedure.dataSections()) {
        RELEASE_ASSERT(section.size);
        constants.append({
            Vector<uint8_t>(unsafeMakeSpan(static_cast<const uint8_t*>(section.data), section.size)),
            alignmentOfConstant(section.size),
        });
    }
    return constants;
}

Vector<AOT::JumpTable> takeJumpTables(B3::Procedure& procedure)
{
    Vector<AOT::JumpTable> jumpTables;
    jumpTables.reserveInitialCapacity(procedure.numberOfJumpTables());
    for (unsigned index = 0; index < procedure.numberOfJumpTables(); ++index)
        jumpTables.append({ WTF::move(procedure.jumpTable(index).targetOffsets) });
    return jumpTables;
}

static CodeBody describeBody(CodeRecords& records, EmittedCode& emitted)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    CodeBody body;
    body.relocations = WTF::move(emitted.relocations);
    records.stubCalls.describeCalls(body.relocations.span(), body, &linkBuffer);
    body.units = records.units.link(emitted.buffer, body.relocations.span());
    body.sizeOfHotBlocks = records.units.sizeOfHotBlocks();
    body.localSites = linkBuffer.linkedSites();
    body.bytes = Vector<uint8_t>(emitted.buffer.bytes());

    uint32_t size = safeCast<uint32_t>(body.bytes.size());
    body.paddingStart = std::min(linkBuffer.offsetOf(emitted.paddingStart), size);
    body.entries = describeEntries(records, linkBuffer);
    if (emitted.procedure) {
        body.constants = copyConstants(*emitted.procedure);
        body.jumpTables = takeJumpTables(*emitted.procedure);
    }
    return body;
}

// A loop of a few instructions turns once a cycle where it lies in one line of the instruction cache, and once in two cycles where it lies in two. A longer one takes two cycles wherever it lies. Of loops in one another the innermost turns most.
// A jump back is also how a block that seldom runs gets back to where it was left for. A loop of the program has a poll.
static Vector<CodeRange, 0> findSmallLoops(const CodeBody& body, std::span<const PollSite> polls)
{
    AOT_THRESHOLD(size_t, maxLoopsKeptInLine, 4);
    constexpr uint32_t maxSizeOfLoopKeptInLine = sizeOfInstructionCacheLine / 2;
    Vector<CodeRange, 0> loops;
    for (const LinkedSite& site : body.localSites) {
        if (!transfersControl(site.kind) || site.kind == RelocationKind::Call || site.target > site.offset)
            continue;
        CodeRange loop { site.target, site.offset + MacroAssembler::relocationSize(site.kind) };
        if (loop.end > body.sizeOfHotBlocks || loop.end - loop.begin > maxSizeOfLoopKeptInLine)
            continue;
        bool hasPoll = std::ranges::any_of(polls, [&](const PollSite& poll) {
            return poll.offset >= loop.begin && poll.returnOffset() <= loop.end;
        });
        if (!hasPoll)
            continue;
        // The sites ascend, so a loop that was found earlier ends earlier.
        bool isKnown = false;
        for (CodeRange& earlier : loops) {
            if (earlier.begin < loop.begin)
                continue;
            isKnown = true;
            if (earlier.begin == loop.begin)
                earlier.end = loop.end;
        }
        if (!isKnown && loops.size() < maxLoopsKeptInLine)
            loops.append(loop);
    }
    return loops;
}

static Vector<CodeRange, 1> describeFrameRanges(const B3::Air::Code& code, LinkBuffer& linkBuffer)
{
    Vector<CodeRange, 1> ranges;
    for (const B3::Air::FrameRange& range : code.frameRanges()) {
        CodeRange linked { linkBuffer.offsetOf(range.begin), linkBuffer.offsetOf(range.end) };
        RELEASE_ASSERT(linked.begin <= linked.end && (ranges.isEmpty() || ranges.last().end <= linked.begin));
        if (linked.begin == linked.end)
            continue;
        if (!ranges.isEmpty() && ranges.last().end == linked.begin) {
            ranges.last().end = linked.end;
            continue;
        }
        ranges.append(linked);
    }
    return ranges;
}

// A landing pad takes every register that a call may clobber as clobbered, so unwinding restores only the callee-saved registers of the VM.
static RegisterAtOffsetList registersRestoredByUnwinding(const B3::Procedure& procedure, const Function& function)
{
    if (function.role != FunctionRole::StubBody)
        return procedure.calleeSaveRegisterAtOffsetList();
    RegisterSet restored = RegisterSet::vmCalleeSaveRegisters();
    for (const RegisterAtOffset& entry : procedure.calleeSaveRegisterAtOffsetList())
        RELEASE_ASSERT(!restored.contains(entry.reg(), IgnoreVectors));
    return { };
}

static FrameDescription describeFrame(const CodeRecords& records, EmittedCode& emitted, FunctionID id, const Function& function)
{
    FrameDescription frame;
    frame.spillsArgumentRegisters = records.frame.spillsArgumentRegisters;
    frame.stackUsedAtCalls = records.frame.stackUsedAtCalls;
    frame.isEnteredWithoutStackCheck = emitted.stackChecks.isEnteredWithoutStackCheck;
    frame.isProvenToFitOnStack = emitted.stackChecks.isProvenToFitOnStack;
    if (!emitted.procedure)
        return frame;

    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    const B3::Procedure& procedure = *emitted.procedure;
    const B3::Air::Code& code = procedure.code();
    frame.frameSizeInBytes = procedure.frameSize();
    frame.bytesAboveFrameRecord = code.frameLayout().bytesAboveFrameRecord;
    frame.bytesDeadAtSuspensions = code.frameLayout().bytesDeadAtSuspensions;
    frame.hasFrameRecord = !code.frameLayout().hasFrame || code.frameLayout().hasFrameRecord;
    frame.calleeSaveRegisters = registersRestoredByUnwinding(procedure, function);
    frame.frameRanges = describeFrameRanges(code, linkBuffer);
    // A frame that is set up for its stack check alone is torn down at once: no byte of code runs in it.
    RELEASE_ASSERT(code.frameLayout().hasFrame || !frame.hasFrame());
    frame.prologueChecks = linkStackChecks(linkBuffer, id, records);
    // A frame is allocated whole when such a check calls, and without a record only its size leads from there to the caller.
    if (!frame.hasFrameRecord) {
        for (const PrologueCheck& check : frame.prologueChecks) {
            RELEASE_ASSERT(frame.frameRanges.isEmpty() || frame.frameRanges.last().end <= check.callOffset);
            frame.frameRanges.append({ check.callOffset, check.callOffset + static_cast<uint32_t>(sizeOfNearCall) });
        }
    }
    return frame;
}

CompiledCode describeCode(BuildContext& build, const CodeGenerationInputs& inputs, FunctionID id, const Function& function, CodeRecords& records, EmittedCode&& emitted)
{
    CompiledCode result;
    result.function = id;
    result.entry = describeEntry(inputs, id, function);
    result.body = describeBody(records, emitted);
    result.sites = describeSites({ build, inputs.program, id, function }, records, emitted.buffer.linkBuffer(), result.body);
    if (keepsSmallLoopsInLines && !isCold(inputs.heat.of(inputs.program, id)).predicted())
        result.body.smallLoops = findSmallLoops(result.body, result.sites.pollSites.span());
    result.frame = describeFrame(records, emitted, id, function);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
