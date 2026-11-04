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
#include "AOTIRProgram.h"
#include "AOTMachineDescription.h"
#include "AOTMachineFrame.h"
#include "AOTSort.h"
#include "AOTStackCheckEmitter.h"
#include "AOTStubDescriptor.h"
#include "AOTThrowEmitter.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/EnumTraits.h>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DescribedUnit {
    BuildContext& build;
    const Program& program;
    const CallingConventions& conventions;
    const Code& code;
};

struct SiteReference {
    uint32_t* site;
    bool isFirstRead { false };
};

class SiteReferencesInOrder {
public:
    SiteReferencesInOrder(CodeBody& body, SiteDescription& sites)
        : m_firstReadSites(body.firstReadSites.mutableSpan())
        , m_returnSites(body.returnSites.mutableSpan())
        , m_faultingAccesses(sites.faultingAccesses.mutableSpan())
        , m_pollSites(sites.pollSites.mutableSpan())
    {
        findNextFirstReadSite();
        findNextReturnSite();
        findNextFaultingAccess();
        findNextPollSite();
    }

    size_t mostReferences() const { return m_firstReadSites.size() + m_returnSites.size() + m_faultingAccesses.size() + m_pollSites.size(); }
    bool isAtEnd() const { return std::min(std::min(m_offsetOfFirstReadSite, m_offsetOfReturnSite), std::min(m_offsetOfFaultingAccess, m_offsetOfPollSite)) == atEnd; }

    SiteReference takeNext()
    {
        if (m_offsetOfFirstReadSite <= std::min(m_offsetOfReturnSite, std::min(m_offsetOfFaultingAccess, m_offsetOfPollSite))) {
            FirstReadSite& site = m_firstReadSites[m_nextFirstReadSite++];
            findNextFirstReadSite();
            return { &site.site, true };
        }
        if (m_offsetOfReturnSite <= std::min(m_offsetOfFaultingAccess, m_offsetOfPollSite)) {
            ReturnSite& site = m_returnSites[m_nextReturnSite++];
            findNextReturnSite();
            return { &site.site.bits };
        }
        if (m_offsetOfFaultingAccess <= m_offsetOfPollSite) {
            FaultingAccess& access = m_faultingAccesses[m_nextFaultingAccess++];
            findNextFaultingAccess();
            return { &access.callSite.bits };
        }
        PollSite& poll = m_pollSites[m_nextPollSite++];
        findNextPollSite();
        return { &poll.callSite.bits };
    }

private:
    static constexpr uint64_t atEnd = std::numeric_limits<uint64_t>::max();

    void findNextFirstReadSite()
    {
        m_offsetOfFirstReadSite = m_nextFirstReadSite < m_firstReadSites.size() ? m_firstReadSites[m_nextFirstReadSite].returnOffset : atEnd;
    }

    void findNextReturnSite()
    {
        while (m_nextReturnSite < m_returnSites.size() && !m_returnSites[m_nextReturnSite].site)
            ++m_nextReturnSite;
        m_offsetOfReturnSite = m_nextReturnSite < m_returnSites.size() ? m_returnSites[m_nextReturnSite].offset : atEnd;
    }

    void findNextFaultingAccess()
    {
        while (m_nextFaultingAccess < m_faultingAccesses.size() && !m_faultingAccesses[m_nextFaultingAccess].callSite)
            ++m_nextFaultingAccess;
        m_offsetOfFaultingAccess = m_nextFaultingAccess < m_faultingAccesses.size() ? m_faultingAccesses[m_nextFaultingAccess].codeOffset + static_cast<uint32_t>(sizeOfNearCall) : atEnd;
    }

    void findNextPollSite()
    {
        while (m_nextPollSite < m_pollSites.size() && !m_pollSites[m_nextPollSite].callSite)
            ++m_nextPollSite;
        m_offsetOfPollSite = m_nextPollSite < m_pollSites.size() ? m_pollSites[m_nextPollSite].returnOffset() : atEnd;
    }

    std::span<FirstReadSite> m_firstReadSites;
    std::span<ReturnSite> m_returnSites;
    std::span<FaultingAccess> m_faultingAccesses;
    std::span<PollSite> m_pollSites;
    size_t m_nextFirstReadSite { 0 };
    size_t m_nextReturnSite { 0 };
    size_t m_nextFaultingAccess { 0 };
    size_t m_nextPollSite { 0 };
    uint64_t m_offsetOfFirstReadSite { atEnd };
    uint64_t m_offsetOfReturnSite { atEnd };
    uint64_t m_offsetOfFaultingAccess { atEnd };
    uint64_t m_offsetOfPollSite { atEnd };
};

struct PlaceAmongPositions {
    static constexpr size_t unknown = std::numeric_limits<size_t>::max();

    size_t nextLine { unknown };
    size_t nextAdjustment { unknown };
    uint64_t beginOfStretch { 0 };
    uint64_t endOfStretch { 0 };
    uint32_t lineOfStretch { 0 };
    uint32_t originOfColumns { 0 };
    uint32_t columnAtOrigin { 0 };
};

struct ShownFunction {
    const Function* owner;
    std::optional<LineColumn> positionInBuiltin;
    PlaceAmongPositions lastPlace { };
};

using ShownFunctions = Vector<ShownFunction, 4>;

// A function that only an inline frame or a site names stands in CodeBody::callees as its Body, which need not have code.
// The Body of a coroutine is its ramp: what is walked at a call is the body as written, which is shown as the CoroutineBody is.
class DescribedCallees {
public:
    explicit DescribedCallees(CodeBody& body)
        : m_body(body)
    {
    }

    uint32_t indexOf(FunctionID function, CodeRole role = CodeRole::Body)
    {
        RELEASE_ASSERT(function);
        CodeUnit unit { function, role };
        if (m_indexes.isEmpty() && m_body.callees.size() < fewestCalleesWithIndexes) {
            size_t index = m_body.callees.find(unit);
            if (index == notFound) {
                index = m_body.callees.size();
                m_body.callees.append(unit);
            }
            return static_cast<uint32_t>(index);
        }
        if (m_indexes.isEmpty()) {
            for (uint32_t index = 0; index < m_body.callees.size(); ++index)
                m_indexes.add(keyOf(m_body.callees[index]), index);
        }
        auto result = m_indexes.add(keyOf(unit), static_cast<uint32_t>(m_body.callees.size()));
        if (result.isNewEntry)
            m_body.callees.append(unit);
        return result.iterator->value;
    }

private:
    static constexpr size_t fewestCalleesWithIndexes = 16;

    static uint64_t keyOf(CodeUnit unit)
    {
        return static_cast<uint64_t>(unit.function.index()) << 32 | static_cast<uint64_t>(unit.role) << 16 | unit.numberOfBoundArguments;
    }

    CodeBody& m_body;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_indexes;
};

struct DescribedCodeOfOperation {
    uint32_t bytes { 0 };
    // What the calls in front of the rare code show.
    Form form { Form::Open };
    bool hasCodeOutsideRareCode { false };
    uint32_t bytesOutsideRareCode { 0 };
};

} // anonymous namespace

static constexpr LineColumn hostFunctionPosition { 0, 0 };
static constexpr LineColumn superCallOfDefaultConstructor { 1, 28 };
static_assert(sizeof("(function (...args) { super") == superCallOfDefaultConstructor.column, "JSC makes a default constructor from a text of its own");
static constexpr LineColumn startOfSuperCallOfDefaultConstructor { 1, 23 };
static_assert(sizeof("(function (...args) { ") == startOfSuperCallOfDefaultConstructor.column);

static void reportUndeclaredTrap(const DescribedUnit& described, ValueID value)
{
    const Code& code = described.code;
    InternalError error {
        .kind = InternalErrorKind::TrapWithoutSite,
        .function = code.function(),
        .location = described.program.at(code.function()).declarationLocation(),
    };
    if (value) {
        const Operation& operation = code.at(value);
        Wire::SourceLocation location = code.locationOf(described.program, operation.origin());
        if (hasModule(location))
            error.location = location;
        // A record without a location is looked up in the body as the front end wrote it, where the numbers of Code mean nothing.
        if (hasModule(error.location)) {
            error.value = value;
            error.operation = nameOf(operation.opcode);
        }
    }
    described.build.diagnostics().add(error);
}

static void collectFaultingAccesses(const DescribedUnit& described, CodeRecords& records, const Machine::Procedure& procedure, LinkBuffer& linkBuffer, const CodeBody& body, decltype(SiteDescription::faultingAccesses)& accesses)
{
    TrapRecords& traps = records.traps;
    Vector<Machine::LinkedTrappingAccess> trappingAccesses;
    Machine::describeTrappingAccesses(procedure, linkBuffer, trappingAccesses);

    AOT::sortInLine(traps.trapSites, { }, &TrapSite::value);
    accesses.reserveInitialCapacity(trappingAccesses.size() + traps.callThrowSites.size());
    for (const Machine::LinkedTrappingAccess& access : trappingAccesses) {
        ValueID value = valueOfOrigin(access.origin);
        auto site = std::ranges::lower_bound(traps.trapSites, value, { }, &TrapSite::value);
        if (site == traps.trapSites.end() || site->value != value) {
            reportUndeclaredTrap(described, value);
            continue;
        }
#if CPU(ARM64)
        // The handler of faults finds a site by the address of the instruction that faulted, so the record names the load or the store itself.
        uint32_t instruction;
        memcpy(&instruction, body.bytes.subspan(access.codeOffset, sizeof(instruction)).data(), sizeof(instruction));
        RELEASE_ASSERT((instruction & 0x0a000000) == 0x08000000, instruction);
#endif
        Vector<uint32_t, 3> words = throwSiteWords(site->throwSite, access.referenceIsNarrow ? Representation::Cell32 : site->representation, static_cast<unsigned>(access.reference));
        uint8_t wordsThatAreMentions = mentionsAmongThrowSiteWords(site->throwSite, words.size());
        accesses.append({ access.codeOffset, OptionalIndex { site->callSite }, WTF::move(words), wordsThatAreMentions });
    }
    for (const CallThrowSite& site : traps.callThrowSites)
        accesses.append({ body.returnSites[site.returnSite].offset - static_cast<uint32_t>(sizeOfNearCall), { }, site.words, site.wordsThatAreMentions });
    AOT::sortInLine(accesses, { }, &FaultingAccess::codeOffset);
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

// A class that writes no constructor gets one from the front end, at the position of the class.
static bool isDefaultConstructor(const Program& program, const Function& function)
{
    if (function.kind != FunctionKind::BaseConstructor && function.kind != FunctionKind::DerivedConstructor)
        return false;
    return function.constructedClass && program.at(function.constructedClass).position == function.position;
}

static std::optional<LineColumn> positionInBuiltin(const Program& program, const CallingConventions& conventions, FunctionID shown)
{
    const Function& owner = program.at(shown);
    if (isDefaultConstructor(program, owner))
        return owner.kind == FunctionKind::DerivedConstructor ? superCallOfDefaultConstructor : startOfBuiltinFunction;
    if (owner.syntax.contains(FunctionSyntax::IsShownAsNativeCode))
        return program.lineColumnFor(owner.module, owner.position);
    // Its frames name the module of the compiler's helpers, and the host holds the text of that module.
    if (owner.syntax.contains(FunctionSyntax::IsShownInModuleOfHelpers))
        return startOfBuiltinFunction;
    if (!owner.enclosingRuntimeFunction)
        return std::nullopt;
    switch (program.builtins().at(owner.enclosingRuntimeFunction).role) {
    case RuntimeFunctionRole::CompilerFunction: {
        auto trace = functionTraceOf(program, conventions, CodeUnit { shown, CodeRole::Body });
        if (trace.isHelperOfCompiler())
            return startOfBuiltinFunction;
        return positionOfImplementedBuiltin(trace.builtinKind());
    }
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

template<typename T, typename Projection>
static size_t findFirstBehindPosition(std::span<const T> ascending, uint32_t position, size_t nearby, Projection projection)
{
    constexpr size_t mostLinearSteps = 8;
    auto liesBehind = [&](size_t index) {
        return position < std::invoke(projection, ascending[index]);
    };
    auto searchBetween = [&](size_t first, size_t end) {
        std::span<const T> candidates = ascending.subspan(first, end - first);
        return first + static_cast<size_t>(std::ranges::upper_bound(candidates, position, { }, projection) - candidates.begin());
    };
    if (nearby == PlaceAmongPositions::unknown)
        return searchBetween(0, ascending.size());

    size_t index = nearby;
    if (index && liesBehind(index - 1)) {
        size_t limit = index > mostLinearSteps ? index - mostLinearSteps : 0;
        do {
            --index;
        } while (index > limit && liesBehind(index - 1));
        if (index && liesBehind(index - 1))
            return searchBetween(0, index - 1);
        return index;
    }
    size_t limit = std::min(ascending.size(), index + mostLinearSteps);
    while (index < limit && !liesBehind(index))
        ++index;
    if (index == limit)
        return searchBetween(limit, ascending.size());
    return index;
}

static NEVER_INLINE LineColumn lineColumnInOtherStretch(const Program& program, ModuleID id, uint32_t position, PlaceAmongPositions& place)
{
    constexpr uint64_t endOfPositions = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    place.beginOfStretch = 0;
    place.endOfStretch = 0;
    if (!id || id.index() >= program.modules.size())
        return { 1, 1 };
    const SourcePositions& positions = program.modules[id.index()].positions;
    std::span<const uint32_t> lineStarts = positions.lineStarts;
    place.nextLine = findFirstBehindPosition(lineStarts, position, place.nextLine, std::identity { });
    bool hasLine = place.nextLine;
    uint32_t line = hasLine ? static_cast<uint32_t>(place.nextLine - 1) : 0;
    uint32_t lineStart = hasLine ? lineStarts[line] : 0;

    std::span<const ColumnAdjustment> adjustments = positions.columnAdjustments.span();
    place.nextAdjustment = findFirstBehindPosition(adjustments, position, place.nextAdjustment, &ColumnAdjustment::offset);
    uint32_t offsetOfAdjustment = place.nextAdjustment ? adjustments[place.nextAdjustment - 1].offset : 0;
    bool isAdjusted = place.nextAdjustment && (!hasLine || offsetOfAdjustment > lineStart);
    if (!isAdjusted && !hasLine)
        return { 1, 1 };

    place.beginOfStretch = std::max(lineStart, offsetOfAdjustment);
    place.endOfStretch = std::min<uint64_t>(place.nextLine < lineStarts.size() ? lineStarts[place.nextLine] : endOfPositions, place.nextAdjustment < adjustments.size() ? adjustments[place.nextAdjustment].offset : endOfPositions);
    if (isAdjusted) {
        const ColumnAdjustment& adjustment = adjustments[place.nextAdjustment - 1];
        place.lineOfStretch = adjustment.line + 1;
        place.originOfColumns = adjustment.offset;
        place.columnAtOrigin = adjustment.column + 1;
    } else {
        place.lineOfStretch = line + 1;
        place.originOfColumns = lineStart;
        place.columnAtOrigin = 1;
    }
    return { place.lineOfStretch, place.columnAtOrigin + (position - place.originOfColumns) };
}

static ALWAYS_INLINE LineColumn lineColumnNear(const Program& program, ModuleID id, uint32_t position, PlaceAmongPositions& place)
{
    if (position >= place.beginOfStretch && position < place.endOfStretch) [[likely]]
        return { place.lineOfStretch, place.columnAtOrigin + (position - place.originOfColumns) };
    return lineColumnInOtherStretch(program, id, position, place);
}

static SitePosition positionIn(const Program& program, ShownFunction& shown, uint32_t position, uint32_t frame)
{
    if (shown.positionInBuiltin)
        return { shown.positionInBuiltin->line, shown.positionInBuiltin->column, ModuleID(), 0, frame };
    const Function& owner = *shown.owner;
    if (position == Instruction::noPosition || (!position && !owner.isModuleBody()))
        position = owner.position;
    if (position == Instruction::noPosition)
        position = 0;
    LineColumn lineColumn = lineColumnNear(program, owner.module, position, shown.lastPlace);
    return { lineColumn.line, lineColumn.column, owner.module, position, frame };
}

static ShownFunctions findShownFunctions(const DescribedUnit& described)
{
    std::span<const InlineFrame> frames = described.code.inlineFrames();
    uint32_t numberOfFrames = std::max<uint32_t>(frames.size(), 1);
    ShownFunctions shownFunctions;
    shownFunctions.reserveInitialCapacity(numberOfFrames);
    for (uint32_t frame = 0; frame < numberOfFrames; ++frame) {
        FunctionID shown = frame ? frames[frame].callee : described.code.function();
        RELEASE_ASSERT(shown);
        shownFunctions.append({ &described.program.at(shown), positionInBuiltin(described.program, described.conventions, shown) });
    }
    return shownFunctions;
}

static void describeConstructionStart(const Program& program, ShownFunction& shown, const RecordedSite& site, SitePosition& position)
{
    if (!site.origin.isPartOfConstruction && !isConstructOf(site.kind))
        return;
    const Function& owner = *shown.owner;
    if (owner.kind == FunctionKind::DerivedConstructor && isDefaultConstructor(program, owner)) {
        position.constructionStartLine = startOfSuperCallOfDefaultConstructor.line;
        position.constructionStartColumn = startOfSuperCallOfDefaultConstructor.column;
        return;
    }
    if (shown.positionInBuiltin)
        return;
    ModuleID module = owner.module;
    const ConstructionStart* construction = program.findConstructionReportedAt(module, position.sourceOffset);
    if (!construction || (construction->isWrittenAsCall && !isConstructOf(site.kind)))
        return;
    LineColumn lineColumn = lineColumnNear(program, module, construction->start, shown.lastPlace);
    if (construction->isShownAtStart) {
        position.constructionStartLine = std::exchange(position.line, lineColumn.line);
        position.constructionStartColumn = std::exchange(position.column, lineColumn.column);
        position.sourceOffset = construction->start;
        position.startIsWhereWritten = true;
        return;
    }
    position.constructionStartLine = lineColumn.line;
    position.constructionStartColumn = lineColumn.column;
    position.startIsForCallers = construction->isForCallers;
}

static SitePosition describePosition(const Program& program, ShownFunctions& shownFunctions, const RecordedSite& site)
{
    ShownFunction& shown = shownFunctions[site.origin.inlineFrame];
    SitePosition position = positionIn(program, shown, site.origin.position, site.origin.inlineFrame);
    describeConstructionStart(program, shown, site, position);
    return position;
}

// Indexed by the number the site was recorded under. Empty where no site has a landing pad.
static Vector<const SiteLandingPad*> findLandingPadsOfSites(const ExceptionRecords& exceptions, size_t numberOfSites)
{
    Vector<const SiteLandingPad*> landingPads;
    if (exceptions.sites.isEmpty())
        return landingPads;
    landingPads.fill(nullptr, numberOfSites + 1);
    for (const SiteLandingPad& landingPad : exceptions.sites) {
        const SiteLandingPad*& known = landingPads[landingPad.site];
        RELEASE_ASSERT(!known || (known->landingPad == landingPad.landingPad && known->type == landingPad.type));
        known = &landingPad;
    }
    return landingPads;
}

static void describeCalleeOfSite(const RecordedSite& site, uint32_t number, const SiteRecords& records, SiteDescription& sites)
{
    if (site.callsOwnFunctionObject) {
        sites.calleesOfSites.append({ number, CalleeOfSite::ownFunctionObject() });
        return;
    }
    if (!site.keptCalleePlusOne)
        return;
    const KeptCallee& kept = records.keptCallees[site.keptCalleePlusOne - 1];
    auto field = records.fieldsOfCallees.find(kept.call);
    if (field != records.fieldsOfCallees.end() && !field->value) {
        sites.calleesOfSites.append({ number, CalleeOfSite::ownFunctionObject() });
        return;
    }
    // Nothing writes the word of an unreachable call.
    if (kept.word >= records.wordsOfCallees.size() || !records.wordsOfCallees[kept.word])
        return;
    intptr_t offset = records.wordsOfCallees[kept.word]->offsetFromFP();
    RELEASE_ASSERT(!(offset % static_cast<intptr_t>(sizeof(uint64_t))));
    int32_t word = safeCast<int32_t>(offset / static_cast<intptr_t>(sizeof(uint64_t)));
    sites.calleesOfSites.append({ number, field != records.fieldsOfCallees.end() ? CalleeOfSite::inEnvironmentInFrame(word, *field->value) : CalleeOfSite::inFrame(word) });
}

static void describeSitesInOrder(const DescribedUnit& described, const CodeRecords& records, LinkBuffer& linkBuffer, SiteReferencesInOrder& references, DescribedCallees& callees, bool describesCalleesOfSites, SiteDescription& sites)
{
    const Program& program = described.program;
    const Vector<RecordedSite>& recorded = records.sites.positions;
    uint32_t numberOfSitesWithoutCode = records.sites.numberOfSitesWithoutCode;
    ShownFunctions shownFunctions = findShownFunctions(described);

    sites.positions.reserveInitialCapacity(1 + numberOfSitesWithoutCode + references.mostReferences());
    const Function& outermost = *shownFunctions[0].owner;
    sites.positions.append(positionIn(program, shownFunctions[0], outermost.position, 0));
    if (isDefaultConstructor(program, outermost))
        sites.positions[0].column = startOfBuiltinFunction.column;

    uint32_t numberOfSites = 0;
    auto addSite = [&](const RecordedSite& site) {
        uint32_t number = ++numberOfSites;
        sites.positions.append(describePosition(program, shownFunctions, site));
        if (number > numberOfSitesWithoutCode && site.kind != CallSiteKind::Plain)
            sites.flaggedSites.append({ number, site.kind, site.calledBuiltin });
        if (site.constructedCallee)
            sites.constructedCallees.append({ number, callees.indexOf(site.constructedCallee) });
        if (describesCalleesOfSites)
            describeCalleeOfSite(site, number, records.sites, sites);
        return number;
    };
    for (const RecordedSite& site : recorded.span().first(numberOfSitesWithoutCode))
        addSite(site);

    // One operation of the program is often several calls in a row.
    struct LastSite {
        const RecordedSite* record { nullptr };
        const SiteLandingPad* landingPad { nullptr };
    };
    LastSite last;
    auto leadToSameHandler = [](const SiteLandingPad* a, const SiteLandingPad* b) {
        return a && b ? a->landingPad == b->landingPad && a->type == b->type : a == b;
    };

    Vector<const SiteLandingPad*> landingPads = findLandingPadsOfSites(records.exceptions, recorded.size());
    while (!references.isAtEnd()) {
        SiteReference reference = references.takeNext();
        uint32_t recordedNumber = *reference.site;
        RELEASE_ASSERT(recordedNumber && recordedNumber <= recorded.size());
        const RecordedSite& record = recorded[recordedNumber - 1];
        const SiteLandingPad* landingPad = landingPads.isEmpty() ? nullptr : landingPads[recordedNumber];
        bool recordsOnlyPosition = !reference.isFirstRead && record.kind == CallSiteKind::Plain && !record.callsOwnFunctionObject && !record.keptCalleePlusOne;
        if (recordsOnlyPosition && last.record && *last.record == record && leadToSameHandler(last.landingPad, landingPad)) {
            *reference.site = numberOfSites;
            continue;
        }
        uint32_t number = addSite(record);
        *reference.site = number;
        if (landingPad) {
            const MacroAssembler::Label* label = records.exceptions.landingPads[landingPad->landingPad];
            RELEASE_ASSERT(label && label->isSet());
            sites.handlers.append({ number, linkBuffer.offsetOf(*label), landingPad->type });
        }
        last = recordsOnlyPosition ? LastSite { &record, landingPad } : LastSite { };
    }
}

static Vector<AOT::InlineFrame> describeInlineFrames(const Program& program, const Code& code, const SiteRecords& sites, DescribedCallees& callees)
{
    std::span<const InlineFrame> frames = code.inlineFrames();
    const Vector<uint32_t>& inlineCallSites = sites.inlineCallSites;
    RELEASE_ASSERT(inlineCallSites.size() == frames.size());
    Vector<AOT::InlineFrame> inlineFrames;
    if (frames.size() <= 1)
        return inlineFrames;
    inlineFrames.reserveInitialCapacity(frames.size());
    inlineFrames.append({ 0, 0, 0, false, false });
    for (size_t index = 1; index < frames.size(); ++index) {
        const InlineFrame& frame = frames[index];
        CodeRole roleShown = program.at(frame.callee).isCoroutine() ? CodeRole::CoroutineBody : CodeRole::Body;
        inlineFrames.append({ frame.parent, inlineCallSites[index], callees.indexOf(frame.callee, roleShown), frame.isTailCall, frame.isConstruction, frame.calledBuiltin });
    }
    return inlineFrames;
}

static void describeSites(const DescribedUnit& described, CodeRecords& records, EmittedCode& emitted, CodeBody& body, SiteDescription& sites)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    sites.numberOfSitesWithoutCode = records.sites.numberOfSitesWithoutCode;
    sites.frameMapWords = WTF::move(records.frameMaps.words);
    collectFaultingAccesses(described, records, emitted.procedure, linkBuffer, body, sites.faultingAccesses);
    sites.pollSites = WTF::move(records.traps.pollSites);
    AOT::sortInLine(sites.pollSites, { }, &PollSite::offset);

    SiteReferencesInOrder references(body, sites);
    // A stack walk only stops at a site that has code. A body without one is never asked for its inline frames.
    size_t numberOfFrames = described.code.inlineFrames().size();
    size_t numberOfInlineCallSites = numberOfFrames ? numberOfFrames - 1 : 0;
    bool hasOnlyInlineCallSites = numberOfInlineCallSites && sites.numberOfSitesWithoutCode == numberOfInlineCallSites && references.isAtEnd();
    DescribedCallees callees(body);
    describeSitesInOrder(described, records, linkBuffer, references, callees, !hasOnlyInlineCallSites, sites);
    if (hasOnlyInlineCallSites) {
        sites.numberOfSitesWithoutCode = 0;
        sites.positions.shrink(1);
        return;
    }
    sites.inlineFrames = describeInlineFrames(described.program, described.code, records.sites, callees);
}

// The code of a preamble is entered in place of the entrypoint of the procedure that has its id.
static Vector<CodeEntry, 1> describeEntriesOfUnit(const CodeRecords& records, const Machine::Procedure& procedure, LinkBuffer& linkBuffer)
{
    Vector<CodeEntry, 1> entries;
    Machine::describeEntries(procedure, linkBuffer, entries);
    for (const RecordedEntry& recorded : records.entries) {
        uint32_t offset = linkBuffer.offsetOf(recorded.label);
        size_t index = entries.findIf([&](const CodeEntry& entry) {
            return entry.id == recorded.id;
        });
        if (index == notFound)
            entries.append({ offset, recorded.id });
        else
            entries[index].offset = offset;
    }
    AOT::sort(entries, { }, &CodeEntry::id);
    RELEASE_ASSERT(!entries.isEmpty() && entries[0].id == primaryEntry);
    RELEASE_ASSERT(std::ranges::adjacent_find(entries, { }, &CodeEntry::id) == entries.end());
    return entries;
}

static void describeBody(CodeRecords& records, EmittedCode& emitted, CodeBody& body)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    body.relocations = WTF::move(emitted.relocations);
    records.stubCalls.describeCalls(body.relocations.span(), body, &linkBuffer);
    records.mentions.moveTo(body);
    std::span<const uint8_t> bytes = emitted.buffer.bytes();
    body.bytes.reserveInitialCapacity(bytes.size());
    body.bytes.append(bytes);
    body.entries = describeEntriesOfUnit(records, emitted.procedure, linkBuffer);
    Machine::describeConstants(emitted.procedure, body.constants);
    Machine::describeJumpTables(emitted.procedure, linkBuffer, body.jumpTables);
}

static void describeFrameOfUnit(const CodeRecords& records, EmittedCode& emitted, FrameDescription& frame)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    Machine::describeFrame(emitted.procedure, linkBuffer, frame);
    RELEASE_ASSERT(emitted.procedure.frame().hasFrame() || !frame.hasFrame());
    // A landing pad takes every register that an ordinary call clobbers as clobbered, so unwinding restores the callee-saved registers of the VM alone. Such a function writes none of them.
    if (emitted.procedure.traits().savesRegistersOfCallers) {
        RegisterSet restoredByUnwinding = RegisterSet::vmCalleeSaveRegisters();
        for (const RegisterAtOffset& save : frame.calleeSaveRegisters)
            RELEASE_ASSERT(!restoredByUnwinding.contains(save.reg(), IgnoreVectors));
        frame.calleeSaveRegisters = { };
    }
    // bytesAboveReturnAddressOf() adds the spilled registers where this is set, so bytesAboveFrameRecord does not count them.
    frame.spillsArgumentRegisters = records.frame.spillsArgumentRegisters;
    frame.stackUsedAtCalls = records.frame.stackUsedAtCalls;
    frame.isEnteredWithoutStackCheck = emitted.stackChecks.isEnteredWithoutStackCheck;
    frame.isProvenToFitOnStack = emitted.stackChecks.isProvenToFitOnStack;
    frame.prologueChecks = linkStackChecks(linkBuffer, records);
    // A frame is allocated whole when such a check calls, and without a record only its size leads from there to the caller.
    if (!frame.hasFrameRecord) {
        for (const PrologueCheck& check : frame.prologueChecks) {
            RELEASE_ASSERT(frame.frameRanges.isEmpty() || frame.frameRanges.last().end <= check.callOffset);
            frame.frameRanges.append({ check.callOffset, check.callOffset + static_cast<uint32_t>(sizeOfNearCall) });
        }
    }
}

static std::optional<StubKey> describedStubEnteredBy(const Relocation& relocation, const CodeBody& body)
{
    if (!transfersControl(relocation.kind))
        return std::nullopt;
    switch (relocation.space) {
    case SymbolSpace::Stub:
        return body.stubKeys[relocation.symbol];
    case SymbolSpace::ArgumentThunk:
        return StubKey::fromBits(body.argumentMoves[relocation.symbol][0]);
    case SymbolSpace::Function:
    case SymbolSpace::Constant:
    case SymbolSpace::JumpTable:
    case SymbolSpace::Number:
    case SymbolSpace::InstanceWord:
    case SymbolSpace::RegExp:
    case SymbolSpace::WebAssembly:
    case SymbolSpace::FFI:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The head of an operation call only calls the run-time function, which stands behind the open form and the closed form alike.
static Form describedFormOfCallTo(StubKey stub)
{
    return isOperationCallHead(stub) ? Form::Runtime : Form::Closed;
}

static std::atomic<uint32_t> describedFormSaidFalsely { 0 };

void setFormSaidFalsely(std::optional<FormSaidFalsely> said)
{
    describedFormSaidFalsely.store(said ? 1u << 31 | static_cast<uint32_t>(said->opcode) << 8 | static_cast<uint32_t>(said->form) : 0);
}

std::optional<FormSaidFalsely> formSaidFalsely()
{
    uint32_t said = describedFormSaidFalsely.load();
    if (!said)
        return std::nullopt;
    return FormSaidFalsely { static_cast<Opcode>(said >> 8 & 0xFFFF), static_cast<Form>(said & 0xFF) };
}

static bool describedSiteStandsWhereItSays(const Code& code, const SiteOfForm& site)
{
    if (site.place == Place::Anywhere)
        return true;
    if (!site.block)
        return false;
    const Block& block = code.at(site.block);
    switch (site.place) {
    case Place::Loop:
        return block.isInLoop;
    case Place::UnlikelyBlock:
        return block.frequency == BlockFrequency::Unlikely;
    case Place::RareBlock:
        return block.frequency == BlockFrequency::Rare;
    case Place::CodeThatRunsOnce:
        return !block.isInLoop;
    case Place::Anywhere:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What a form that calls may write in front of the rare code: the call, an instruction for each word of the payload that brings the word to where the callee takes it, and one for the answer.
// The moves of the operands are the allocator's, and are counted with the back end. A test in line in front of the call, or stores in line behind it, are more than that.
static uint32_t describedBytesOfCallFor(const Operation& operation)
{
    constexpr uint32_t bytesOfInstructionThatBringsWord = isX86_64() ? 10 : 2 * sizeof(uint32_t);
    return static_cast<uint32_t>(sizeOfNearCall) + bytesOfInstructionThatBringsWord * (1 + static_cast<uint32_t>(operation.payload().size()));
}

namespace {

// Machine::describeRareCode().
class DescribedRareCode {
public:
    explicit DescribedRareCode(EmittedCode& emitted)
    {
        Machine::describeRareCode(emitted.procedure, emitted.buffer.linkBuffer(), m_ranges);
    }

    bool contains(uint32_t offset) const
    {
        auto behind = std::ranges::upper_bound(m_ranges, offset, { }, &Machine::LinkedRangeOfRareCode::begin);
        return behind != m_ranges.begin() && offset < (behind - 1)->end;
    }

    // functor(begin, end, isRare) for each piece of the range that lies in rare code or outside it altogether.
    template<typename Functor>
    void forEachPieceOf(uint32_t begin, uint32_t end, const Functor& functor) const
    {
        auto range = std::ranges::upper_bound(m_ranges, begin, { }, &Machine::LinkedRangeOfRareCode::end);
        for (; begin < end; ++range) {
            uint32_t beginOfRareCode = range == m_ranges.end() ? end : std::min(end, range->begin);
            if (begin < beginOfRareCode)
                functor(begin, beginOfRareCode, false);
            if (range == m_ranges.end())
                return;
            begin = std::max(begin, beginOfRareCode);
            uint32_t endOfRareCode = std::min(end, range->end);
            if (begin < endOfRareCode)
                functor(begin, endOfRareCode, true);
            begin = std::max(begin, endOfRareCode);
        }
    }

private:
    Vector<Machine::LinkedRangeOfRareCode> m_ranges;
};

} // anonymous namespace

// The form of a site is what its emitter said, and what the schema names where it said nothing. A row is named where the two agree.
// What the bytes show is a second witness. The slow arm of an open form lies among the rare code, and it calls, so a call there says nothing, and a site that lies among the rare code altogether shows nothing.
static Vector<BytesOfForm> describeBytesByForm(const Program& program, const Code& code, const FormsSaid& said, const CodeBody& body, EmittedCode& emitted, CompiledCode& result)
{
    static_assert(Form::Open < Form::Closed && Form::Closed < Form::Runtime);
    constexpr uint32_t noSite = FormsSaid::noSite;

    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    Vector<Machine::LinkedRangeOfOrigin> ranges;
    Machine::describeRangesOfOperations(emitted.procedure, linkBuffer, ranges);
    DescribedRareCode rareCode(emitted);

    // What is appended for a value outside its emitter, as a move that Order puts on an edge, goes with the first site of the value. A value that no emitter was handed gets a site that says nothing.
    Vector<SiteOfForm> sites(said.sites());
    Vector<uint32_t> firstSiteOfValue;
    firstSiteOfValue.fill(noSite, code.numberOfValues());
    for (uint32_t index = sites.size(); index--;)
        firstSiteOfValue[sites[index].value.index()] = index;
    Vector<uint32_t> siteOfRange(ranges.size(), [&](size_t index) {
        const Machine::LinkedRangeOfOrigin& range = ranges[index];
        ValueID value = valueOfOrigin(range.origin);
        if (!value)
            return noSite;
        uint32_t site = range.operation == UINT32_MAX ? noSite : said.siteOf(range.operation);
        if (site != noSite && sites[site].value == value)
            return site;
        uint32_t& first = firstSiteOfValue[value.index()];
        if (first == noSite) {
            first = sites.size();
            sites.append(SiteOfForm { value, code.at(value).block(), code.at(value).opcode });
        }
        return first;
    });

    // Indexed by the site.
    Vector<DescribedCodeOfOperation> operations;
    operations.fill(DescribedCodeOfOperation { }, sites.size());
    for (size_t index = 0; index < ranges.size(); ++index) {
        if (siteOfRange[index] == noSite)
            continue;
        DescribedCodeOfOperation& operation = operations[siteOfRange[index]];
        operation.bytes += ranges[index].end - ranges[index].begin;
        rareCode.forEachPieceOf(ranges[index].begin, ranges[index].end, [&](uint32_t begin, uint32_t end, bool isRare) {
            if (isRare)
                return;
            operation.hasCodeOutsideRareCode = true;
            operation.bytesOutsideRareCode += end - begin;
        });
    }

    // Relocations and ranges both ascend.
    size_t range = 0;
    for (const Relocation& relocation : body.relocations) {
        std::optional<StubKey> stub = describedStubEnteredBy(relocation, body);
        if (!stub)
            continue;
        while (range < ranges.size() && ranges[range].end <= relocation.offset)
            ++range;
        if (range == ranges.size())
            break;
        if (relocation.offset < ranges[range].begin || siteOfRange[range] == noSite || rareCode.contains(relocation.offset))
            continue;
        DescribedCodeOfOperation& operation = operations[siteOfRange[range]];
        operation.form = std::max(operation.form, describedFormOfCallTo(*stub));
    }

    Vector<BytesOfForm> bytesOfSites;
    for (uint32_t index = 0; index < sites.size(); ++index) {
        const DescribedCodeOfOperation& operation = operations[index];
        if (!operation.bytes)
            continue;
        const SiteOfForm& site = sites[index];
        std::optional<Form> named = namedFormOf(site.opcode, site.state, site.place);
        Form form = site.hasSaid ? site.form : named.value_or(formOf(site.opcode));
        bool isNamed = named == form && !site.hasContradictedItself && describedSiteStandsWhereItSays(code, site);
        bool looksOtherwise = operation.hasCodeOutsideRareCode && operation.form != form;
        bool isLongerThanCall = form != Form::Open && operation.bytesOutsideRareCode > describedBytesOfCallFor(code.at(site.value));
        bool differsFromSchema = !isNamed || looksOtherwise || isLongerThanCall;
        bytesOfSites.append({ site.opcode, reportedFormOf(form), 1, operation.bytes, site.state, site.place, isNamed, looksOtherwise, isLongerThanCall, differsFromSchema });
        if (operation.hasCodeOutsideRareCode) {
            bytesOfSites.last().countFoundOpen = operation.form == Form::Open;
            bytesOfSites.last().countFoundClosed = operation.form == Form::Closed;
            bytesOfSites.last().countFoundRuntime = operation.form == Form::Runtime;
        }
        uint32_t inlineFrame = code.at(site.value).inlineFrame;
        if (!inlineFrame)
            continue;
        result.bytesInInlineFrames += operation.bytes;
        const InlineFrame& frame = code.inlineFrames()[inlineFrame];
        if (frame.calledBuiltin)
            result.bytesInFramesOfBuiltins += operation.bytes;
        else if (isLibraryCode(program, program.at(frame.callee)))
            result.bytesInInlineFramesOfLibrary += operation.bytes;
    }
    auto keyOf = [](const BytesOfForm& entry) {
        return std::tuple { entry.opcode, entry.state, entry.place, entry.form, !entry.isNamed };
    };
    AOT::sort(bytesOfSites, { }, keyOf);

    Vector<BytesOfForm> bytesByForm;
    for (const BytesOfForm& entry : bytesOfSites) {
        if (bytesByForm.isEmpty() || keyOf(bytesByForm.last()) != keyOf(entry)) {
            bytesByForm.append(entry);
            continue;
        }
        bytesByForm.last().count += entry.count;
        bytesByForm.last().bytes += entry.bytes;
        bytesByForm.last().countThatLooksOtherwise += entry.countThatLooksOtherwise;
        bytesByForm.last().countLongerThanCall += entry.countLongerThanCall;
        bytesByForm.last().countThatDiffersFromSchema += entry.countThatDiffersFromSchema;
        bytesByForm.last().countFoundOpen += entry.countFoundOpen;
        bytesByForm.last().countFoundClosed += entry.countFoundClosed;
        bytesByForm.last().countFoundRuntime += entry.countFoundRuntime;
    }
    return bytesByForm;
}

static Vector<BytesOutsideOperations> describeBytesOutsideOperations(const CodeBody& body, EmittedCode& emitted)
{
    static_assert(firstOriginOfTerminators + numberOfEnumerators<TerminatorKind> <= Machine::firstOriginOfBackEnd);

    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    Vector<Machine::LinkedRangeOfOrigin> ranges;
    Machine::describeRangesOfOrigins(emitted.procedure, linkBuffer, ranges);
    DescribedRareCode rareCode(emitted);

    Vector<BytesOutsideOperations> pieces;
    auto add = [&](uint32_t origin, uint32_t begin, uint32_t end) {
        rareCode.forEachPieceOf(begin, end, [&](uint32_t beginOfPiece, uint32_t endOfPiece, bool isRare) {
            pieces.append({ origin, isRare, 1, endOfPiece - beginOfPiece });
        });
    };
    uint32_t endOfPreviousRange = 0;
    for (const Machine::LinkedRangeOfOrigin& range : ranges) {
        add(0, endOfPreviousRange, range.begin);
        endOfPreviousRange = range.end;
        if (!valueOfOrigin(range.origin))
            add(range.origin.bits(), range.begin, range.end);
    }
    add(0, endOfPreviousRange, safeCast<uint32_t>(body.bytes.size()));
    AOT::sort(pieces, { }, [](const BytesOutsideOperations& piece) {
        return std::pair { piece.origin, piece.isRare };
    });

    Vector<BytesOutsideOperations> rows;
    for (const BytesOutsideOperations& piece : pieces) {
        if (rows.isEmpty() || rows.last().origin != piece.origin || rows.last().isRare != piece.isRare) {
            rows.append(piece);
            continue;
        }
        rows.last().count += piece.count;
        rows.last().bytes += piece.bytes;
    }
    return rows;
}

static Vector<RangeOfCode> describeRangesOfCode(const Code& code, const CodeBody& body, EmittedCode& emitted)
{
    Vector<Machine::LinkedRangeOfOrigin> rangesOfOrigins;
    Machine::describeRangesOfOrigins(emitted.procedure, emitted.buffer.linkBuffer(), rangesOfOrigins);
    DescribedRareCode rareCode(emitted);

    Vector<RangeOfCode> ranges;
    auto add = [&](Machine::Origin origin, uint32_t begin, uint32_t end) {
        ValueID value = valueOfOrigin(origin);
        rareCode.forEachPieceOf(begin, end, [&](uint32_t beginOfPiece, uint32_t endOfPiece, bool isRare) {
            ranges.append({ beginOfPiece, endOfPiece, value ? 0 : origin.bits(), value, value ? code.at(value).opcode : Opcode::Undefined, isRare });
        });
    };
    uint32_t endOfPreviousRange = 0;
    for (const Machine::LinkedRangeOfOrigin& range : rangesOfOrigins) {
        add(Machine::Origin(0), endOfPreviousRange, range.begin);
        endOfPreviousRange = range.end;
        add(range.origin, range.begin, range.end);
    }
    add(Machine::Origin(0), endOfPreviousRange, safeCast<uint32_t>(body.bytes.size()));
    return ranges;
}

void printOriginOutsideOperations(PrintStream& out, uint32_t origin)
{
    if (!origin) {
        out.print("Other Nowhere");
        return;
    }
    if (Machine::isOriginOfBackEnd(Machine::Origin(origin))) {
        out.print(WTF::enumName(Machine::codeOfBackEndAt(Machine::Origin(origin))), " ", WTF::enumName(Machine::placeOfCodeOfBackEndAt(Machine::Origin(origin))));
        return;
    }
    RELEASE_ASSERT(origin >= firstOriginOfTerminators);
    out.print("End ", nameOf(static_cast<TerminatorKind>(origin - firstOriginOfTerminators)));
}

static std::optional<uint32_t> endOfLastReturnSiteOfDescribedUnit(const CompiledCode& code)
{
    std::optional<uint32_t> end;
    auto add = [&](uint32_t returnOffset) {
        end = end ? std::max(*end, returnOffset) : returnOffset;
    };
    std::span<const ReturnSite> returnSites = code.body.returnSites.span();
    for (size_t index = returnSites.size(); index--;) {
        uint32_t returnOffset = returnSites[index].offset;
        bool returnsToPrologueCheck = code.frame.prologueChecks.containsIf([&](const PrologueCheck& check) {
            return check.callOffset + sizeOfNearCall == returnOffset;
        });
        if (returnsToPrologueCheck)
            continue;
        add(returnOffset);
        break;
    }
    if (!code.sites.faultingAccesses.isEmpty())
        add(code.sites.faultingAccesses.last().codeOffset + static_cast<uint32_t>(sizeOfNearCall));
    if (!code.sites.pollSites.isEmpty())
        add(code.sites.pollSites.last().returnOffset());
    ASSERT(end == endOfLastReturnSite(code));
    return end;
}

// Image::locate would take a return address at the very end of a body for the next body's. Padding is needed for that alone.
static void fitPaddingOfDescribedUnit(CompiledCode& code)
{
    auto& bytes = code.body.bytes;
    RELEASE_ASSERT(bytes.size() >= codeOffsetUnit);
    size_t paddingStart = bytes.size() - codeOffsetUnit;
    std::array<uint8_t, codeOffsetUnit> padding;
    memcpySpan(std::span { padding }, bytes.subspan(paddingStart));

    std::optional<uint32_t> lastReturnSite = endOfLastReturnSiteOfDescribedUnit(code);
    size_t size = std::max<size_t>(paddingStart, lastReturnSite ? *lastReturnSite + codeOffsetUnit : 0);
    RELEASE_ASSERT(!((size - paddingStart) % codeOffsetUnit));
    bytes.shrink(paddingStart);
    while (bytes.size() < size)
        bytes.append(std::span<const uint8_t> { padding });
}

CompiledCode describeCode(BuildContext& build, const CodeGenerationInputs& inputs, const Code& code, CodeRecords& records, EmittedCode&& emitted)
{
    const CallingConventions& conventions = inputs.plan.conventions();
    CompiledCode result;
    result.unit = code.unit();
    result.unitIndex = conventions.indexOf(result.unit);
    result.placeInCode = placeInCodeOf(result.unit, result.unitIndex);
    result.entry = describeEntry(inputs, result.unit);
    describeBody(records, emitted, result.body);
    describeSites({ build, inputs.program, conventions, code }, records, emitted, result.body, result.sites);
    describeFrameOfUnit(records, emitted, result.frame);
    fitPaddingOfDescribedUnit(result);
    if (emitted.procedure.traits().notesRangesOfOrigins) {
        result.bytesByForm = describeBytesByForm(inputs.program, code, records.formsSaid, result.body, emitted, result);
        uint64_t formsThatDifferFromSchema = 0;
        uint64_t formsPickedByPlaceOtherThanLoop = 0;
        for (const BytesOfForm& row : result.bytesByForm) {
            formsThatDifferFromSchema += row.countThatDiffersFromSchema;
            formsPickedByPlaceOtherThanLoop += row.isPickedByPlaceOtherThanLoop() ? row.count : 0;
        }
        build.statistics().count(StatisticID::FormsThatDifferFromSchema, formsThatDifferFromSchema);
        build.statistics().count(StatisticID::FormsPickedByPlaceOtherThanLoop, formsPickedByPlaceOtherThanLoop);
        result.bytesOutsideOperations = describeBytesOutsideOperations(result.body, emitted);
        result.ranges = describeRangesOfCode(code, result.body, emitted);
    }
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
