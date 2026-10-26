/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeDescription.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeGenerator.h"
#include "AOTCodeRecords.h"
#include "AOTEntryEmitter.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionKinds.h"
#include "AOTSort.h"
#include "AOTStackCheckEmitter.h"
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
    bool isFirstRead { false };
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
static constexpr LineColumn startOfBuiltinFunction { 1, 11 };
static_assert(sizeof("function (") == startOfBuiltinFunction.column, "JSC reports every frame of a built-in written in JavaScript at the start of its parameters");
static constexpr LineColumn superCallOfDefaultConstructor { 1, 28 };
static_assert(sizeof("(function (...args) { super") == superCallOfDefaultConstructor.column, "JSC makes a default constructor from a text of its own");

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
        references.append({ site.returnOffset, &site.site, true });
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

static Vector<RecordedSite> renumberSites(const CodeRecords& records, LinkBuffer& linkBuffer, std::span<const SiteReference> references, SiteDescription& sites)
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

    Vector<RecordedSite> result;
    result.reserveInitialCapacity(records.sites.numberOfSitesWithoutCode + references.size());
    result.append(recorded.span().first(records.sites.numberOfSitesWithoutCode));
    for (const SiteReference& reference : references) {
        uint32_t recordedNumber = *reference.site;
        const RecordedSite& record = recordOf(recordedNumber);
        const SiteLandingPad* landingPad = landingPads.takeNext(recordedNumber);
        bool saysOnlyWhereItIs = !reference.isFirstRead && record.kind == CallSiteKind::Plain;
        if (saysOnlyWhereItIs && last.record && *last.record == record && leadToSameHandler(last.landingPad, landingPad)) {
            *reference.site = result.size();
            continue;
        }
        result.append(record);
        uint32_t number = result.size();
        *reference.site = number;
        if (landingPad)
            sites.handlers.append({ number, linkBuffer.offsetOf(*records.exceptions.landingPads[landingPad->landingPad]), landingPad->type });
        last = saysOnlyWhereItIs ? LastSite { &record, landingPad } : LastSite { };
    }
    return result;
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
    // The lines of a text that the program evaluates are counted in the text. The file is the one that evaluates it.
    ModuleID shownModule = owner.module;
    if (shownModule && program.at(shownModule).kind == ModuleKind::CommonJs && program.at(shownModule).referrer)
        shownModule = program.at(shownModule).referrer;
    return { lineColumn.line, lineColumn.column, shownModule, position, frame };
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

static void describeConstructionStart(const Program& program, const ShownFunction& shown, bool siteConstructs, SitePosition& position)
{
    if (shown.positionInBuiltin)
        return;
    ModuleID module = program.at(shown.id).module;
    const ConstructionStart* construction = program.findConstructionReportedAt(module, position.sourceOffset);
    // A construction may have become an allocation and a call of the body.
    if (!construction || (construction->isWrittenAsCall && !siteConstructs))
        return;
    LineColumn lineColumn = program.lineColumnFor(module, construction->start);
    position.constructionStartLine = lineColumn.line;
    position.constructionStartColumn = lineColumn.column;
}

static Vector<SitePosition> describePositions(const DescribedFunction& described, const Vector<RecordedSite>& renumbered)
{
    const Program& program = described.program;
    ShownFunctions shownFunctions = findShownFunctions(described);
    Vector<SitePosition> positions;
    positions.reserveInitialCapacity(renumbered.size() + 1);
    const Function& outermost = program.at(shownFunctions[0].id);
    positions.append(positionIn(program, shownFunctions[0], outermost.position, 0));
    if (isDefaultConstructor(program, outermost))
        positions[0].column = startOfBuiltinFunction.column;
    for (const RecordedSite& site : renumbered) {
        positions.append(positionIn(program, shownFunctions[site.origin.inlineFrame], site.origin.position, site.origin.inlineFrame));
        describeConstructionStart(program, shownFunctions[site.origin.inlineFrame], isConstructOf(site.kind), positions.last());
    }
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

static uint32_t ensureCalleeIndex(CodeBody& body, FunctionID callee)
{
    size_t index = body.callees.find(callee);
    if (index != notFound)
        return static_cast<uint32_t>(index);
    body.callees.append(callee);
    return static_cast<uint32_t>(body.callees.size() - 1);
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
        inlineFrames.append({ frame.parent, inlineCallSites[index], ensureCalleeIndex(body, frame.callee), frame.isTailCall, frame.isConstruction });
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

    Vector<SiteReference, 32> references = collectSiteReferences(body, sites);
    Vector<RecordedSite> renumbered = renumberSites(records, linkBuffer, references.span(), sites);
    sites.positions = describePositions(described, renumbered);
    sites.flaggedSites = collectFlaggedSites(renumbered, sites.numberOfSitesWithoutCode);
    // No walk of the stack can stand in a body without a site that has code, so nobody asks which functions it took in.
    size_t numberOfInlineCallSites = described.function.inlineFrames.isEmpty() ? 0 : described.function.inlineFrames.size() - 1;
    if (numberOfInlineCallSites && sites.numberOfSitesWithoutCode == numberOfInlineCallSites && sites.positions.size() == 1 + numberOfInlineCallSites) {
        sites.numberOfSitesWithoutCode = 0;
        sites.positions.shrink(1);
        return sites;
    }
    sites.inlineFrames = describeInlineFrames(described.function, records.sites, body);
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
    result.frame = describeFrame(records, emitted, id, function);
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
