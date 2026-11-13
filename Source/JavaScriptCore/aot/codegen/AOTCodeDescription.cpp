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
#include "AOTUnitPlacement.h"
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
    {
        if (RareListsOfSites* rareLists = sites.rareLists.ifExists())
            m_pollSites = rareLists->pollSites.mutableSpan();
        findNextFirstReadSite();
        findNextReturnSite();
        findNextFaultingAccess();
        findNextPollSite();
    }

    size_t mostReferences() const { return m_firstReadSites.size() + m_returnSites.size() + m_faultingAccesses.size() + m_pollSites.size(); }
    bool isAtEnd() const { return std::min(std::min(m_offsetOfFirstReadSite, m_offsetOfReturnSite), std::min(m_offsetOfFaultingAccess, m_offsetOfPollSite)) == atEnd; }

    ALWAYS_INLINE SiteReference takeNextUnlessAtEnd()
    {
        uint64_t offsetOfFaultingAccessOrPollSite = std::min(m_offsetOfFaultingAccess, m_offsetOfPollSite);
        if (m_offsetOfFirstReadSite <= std::min(m_offsetOfReturnSite, offsetOfFaultingAccessOrPollSite)) {
            if (m_offsetOfFirstReadSite == atEnd)
                return { nullptr };
            FirstReadSite& site = m_firstReadSites[m_nextFirstReadSite++];
            findNextFirstReadSite();
            return { &site.site, true };
        }
        if (m_offsetOfReturnSite <= offsetOfFaultingAccessOrPollSite) {
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
    uint64_t offsetOfNextAdjustment { 0 };
    uint32_t offsetOfAdjustment { 0 };
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
        return static_cast<uint64_t>(unit.function.index()) << 32 | static_cast<uint64_t>(unit.role) << 24 | unit.placeOfSelector();
    }

    CodeBody& m_body;
    UncheckedKeyHashMap<uint64_t, uint32_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_indexes;
};

struct DescribedCodeOfOperation {
    uint32_t bytes { 0 };
    Form form { Form::Open };
    bool hasCodeOutsideRareCode { false };
    uint32_t bytesOutsideRareCode { 0 };
};

} // anonymous namespace

static constexpr LineColumn hostFunctionPosition { 0, 0 };
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
    if (!procedure.trappingAccesses().empty())
        Machine::describeTrappingAccesses(procedure, linkBuffer, trappingAccesses);

    AOT::sortInLine(traps.trapSites, { }, &TrapSite::value);
    accesses.reserveInitialCapacity(trappingAccesses.size() + traps.callThrowSites.size());
    std::span<const CallThrowSite> callThrowSites = traps.callThrowSites.span();
    auto codeOffsetOfCallThatThrows = [&](const CallThrowSite& site) {
        return body.returnSites[site.returnSite].offset - static_cast<uint32_t>(sizeOfNearCall);
    };
    for (const Machine::LinkedTrappingAccess& access : trappingAccesses) {
        for (; !callThrowSites.empty() && codeOffsetOfCallThatThrows(callThrowSites.front()) < access.codeOffset; callThrowSites = callThrowSites.subspan(1))
            accesses.appendWithinCapacity(codeOffsetOfCallThatThrows(callThrowSites.front()), OptionalIndex { }, ThrowSiteWords(callThrowSites.front().words.span()), callThrowSites.front().wordsThatAreMentions);
        ValueID value = valueOfOrigin(access.origin);
        auto site = std::ranges::lower_bound(traps.trapSites, value, { }, &TrapSite::value);
        if (site == traps.trapSites.end() || site->value != value) {
            reportUndeclaredTrap(described, value);
            continue;
        }
#if CPU(ARM64)
        uint32_t instruction;
        memcpy(&instruction, body.bytes.subspan(access.codeOffset, sizeof(instruction)).data(), sizeof(instruction));
        RELEASE_ASSERT((instruction & 0x0a000000) == 0x08000000, instruction);
#endif
        Vector<uint32_t, 3> words = throwSiteWords(site->throwSite, access.referenceIsNarrow ? Representation::Cell32 : site->representation, static_cast<unsigned>(access.reference));
        uint8_t wordsThatAreMentions = mentionsAmongThrowSiteWords(site->throwSite, words.size());
        accesses.appendWithinCapacity(access.codeOffset, OptionalIndex { site->callSite }, ThrowSiteWords(words.span()), wordsThatAreMentions);
    }
    for (const CallThrowSite& site : callThrowSites)
        accesses.appendWithinCapacity(codeOffsetOfCallThatThrows(site), OptionalIndex { }, ThrowSiteWords(site.words.span()), site.wordsThatAreMentions);
    if (accesses.size() > 1)
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
    if (owner.syntax.contains(FunctionSyntax::IsShownInHelperModule))
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
        if (index && liesBehind(index - 1)) {
            size_t end = index - 1;
            size_t step = mostLinearSteps;
            while (end > step && liesBehind(end - step)) {
                end -= step;
                step *= 2;
            }
            return searchBetween(end > step ? end - step + 1 : 0, end);
        }
        return index;
    }
    size_t limit = std::min(ascending.size(), index + mostLinearSteps);
    while (index < limit && !liesBehind(index))
        ++index;
    if (index == limit) {
        size_t step = mostLinearSteps;
        while (ascending.size() - index > step && !liesBehind(index + step)) {
            index += step + 1;
            step *= 2;
        }
        return searchBetween(index, std::min(ascending.size(), index + step));
    }
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
    if (position < place.offsetOfAdjustment || position >= place.offsetOfNextAdjustment) {
        place.nextAdjustment = findFirstBehindPosition(adjustments, position, place.nextAdjustment, &ColumnAdjustment::offset);
        place.offsetOfAdjustment = place.nextAdjustment ? adjustments[place.nextAdjustment - 1].offset : 0;
        place.offsetOfNextAdjustment = place.nextAdjustment < adjustments.size() ? adjustments[place.nextAdjustment].offset : endOfPositions;
    }
    uint32_t offsetOfAdjustment = place.offsetOfAdjustment;
    bool isAdjusted = place.nextAdjustment && (!hasLine || offsetOfAdjustment > lineStart);
    if (!isAdjusted && !hasLine)
        return { 1, 1 };

    place.beginOfStretch = std::max(lineStart, offsetOfAdjustment);
    place.endOfStretch = std::min<uint64_t>(place.nextLine < lineStarts.size() ? lineStarts[place.nextLine] : endOfPositions, place.offsetOfNextAdjustment);
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

static ALWAYS_INLINE SitePosition positionIn(const Program& program, ShownFunction& shown, uint32_t position, uint32_t frame)
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

static ALWAYS_INLINE void describeConstructionStart(const Program& program, ShownFunction& shown, const RecordedSite& site, uint32_t number, SitePosition& position, SiteDescription& sites)
{
    if (!site.origin.isPartOfConstruction && !isConstructOf(site.kind))
        return;
    const Function& owner = *shown.owner;
    if (owner.kind == FunctionKind::DerivedConstructor && isDefaultConstructor(program, owner)) {
        sites.rareLists.ensure().constructionStarts.append(SiteConstructionStart { number, startOfSuperCallOfDefaultConstructor.line, startOfSuperCallOfDefaultConstructor.column });
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
        uint32_t lineWhereWritten = std::exchange(position.line, lineColumn.line);
        uint32_t columnWhereWritten = std::exchange(position.column, lineColumn.column);
        position.sourceOffset = construction->start;
        sites.rareLists.ensure().constructionStarts.append(SiteConstructionStart { number, lineWhereWritten, columnWhereWritten, false, true });
        return;
    }
    sites.rareLists.ensure().constructionStarts.append(SiteConstructionStart { number, lineColumn.line, lineColumn.column, construction->isForCallers });
}

static ALWAYS_INLINE SitePosition describePosition(const Program& program, ShownFunctions& shownFunctions, const RecordedSite& site, uint32_t number, SiteDescription& sites)
{
    ShownFunction& shown = shownFunctions[site.origin.inlineFrame];
    SitePosition position = positionIn(program, shown, site.origin.position, site.origin.inlineFrame);
    describeConstructionStart(program, shown, site, number, position, sites);
    return position;
}

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

static void describeSitesInOrder(const DescribedUnit& described, const CodeRecords& records, LinkBuffer& linkBuffer, SiteReferencesInOrder references, DescribedCallees& callees, SiteDescription& sites)
{
    const Program& program = described.program;
    const Vector<RecordedSite>& recorded = records.sites.positions;
    uint32_t numberOfSitesWithoutCode = records.sites.numberOfSitesWithoutCode;
    ShownFunctions shownFunctions = findShownFunctions(described);

    sites.positions.reserveInitialCapacity(1 + numberOfSitesWithoutCode + references.mostReferences());
    const Function& outermost = *shownFunctions[0].owner;
    sites.positions.appendWithinCapacity(positionIn(program, shownFunctions[0], outermost.position, 0));
    if (isDefaultConstructor(program, outermost))
        sites.positions[0].column = startOfBuiltinFunction.column;

    uint32_t numberOfSites = 0;
    auto addSite = [&](const RecordedSite& site) ALWAYS_INLINE_LAMBDA {
        uint32_t number = ++numberOfSites;
        sites.positions.appendWithinCapacity(describePosition(program, shownFunctions, site, number, sites));
        if (number > numberOfSitesWithoutCode && site.kind != CallSiteKind::Plain)
            sites.flaggedSites.append({ number, site.kind, site.calledBuiltin });
        if (site.constructedCallee)
            sites.rareLists.ensure().constructedCallees.append({ number, callees.indexOf(site.constructedCallee) });
        return number;
    };
    for (const RecordedSite& site : recorded.span().first(numberOfSitesWithoutCode))
        addSite(site);

    struct LastSite {
        const RecordedSite* record { nullptr };
        const SiteLandingPad* landingPad { nullptr };
    };
    LastSite last;
    auto leadToSameHandler = [](const SiteLandingPad* a, const SiteLandingPad* b) {
        return a && b ? a->landingPad == b->landingPad && a->type == b->type : a == b;
    };

    Vector<const SiteLandingPad*> landingPads = findLandingPadsOfSites(records.exceptions, recorded.size());
    for (SiteReference reference = references.takeNextUnlessAtEnd(); reference.site; reference = references.takeNextUnlessAtEnd()) {
        uint32_t recordedNumber = *reference.site;
        RELEASE_ASSERT(recordedNumber && recordedNumber <= recorded.size());
        const RecordedSite& record = recorded[recordedNumber - 1];
        const SiteLandingPad* landingPad = landingPads.isEmpty() ? nullptr : landingPads[recordedNumber];
        bool recordsOnlyPosition = !reference.isFirstRead && record.kind == CallSiteKind::Plain;
        if (recordsOnlyPosition && last.record && *last.record == record && leadToSameHandler(last.landingPad, landingPad)) {
            *reference.site = numberOfSites;
            continue;
        }
        uint32_t number = addSite(record);
        *reference.site = number;
        if (landingPad) {
            const MacroAssembler::Label* label = records.exceptions.landingPads[landingPad->landingPad];
            RELEASE_ASSERT(label && label->isSet());
            sites.rareLists.ensure().handlers.append({ number, linkBuffer.offsetOf(*label), landingPad->type });
        }
        last = recordsOnlyPosition ? LastSite { &record, landingPad } : LastSite { };
    }
}

static void describeInlineFrames(const Program& program, const Code& code, const SiteRecords& sites, DescribedCallees& callees, decltype(SiteDescription::inlineFrames)& inlineFrames)
{
    std::span<const InlineFrame> frames = code.inlineFrames();
    const Vector<uint32_t>& inlineCallSites = sites.inlineCallSites;
    RELEASE_ASSERT(inlineCallSites.size() == frames.size());
    if (frames.size() <= 1)
        return;
    inlineFrames.reserveInitialCapacity(frames.size());
    inlineFrames.appendWithinCapacity(AOT::InlineFrame { 0, 0, 0, false, false });
    for (size_t index = 1; index < frames.size(); ++index) {
        const InlineFrame& frame = frames[index];
        CodeRole roleShown = program.at(frame.callee).isCoroutine() ? CodeRole::CoroutineBody : CodeRole::Body;
        inlineFrames.appendWithinCapacity(AOT::InlineFrame { frame.parent, inlineCallSites[index], callees.indexOf(frame.callee, roleShown), frame.isTailCall, frame.isConstruction, frame.calledBuiltin });
    }
}

static void describeSites(const DescribedUnit& described, CodeRecords& records, EmittedCode& emitted, CodeBody& body, SiteDescription& sites)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    sites.numberOfSitesWithoutCode = records.sites.numberOfSitesWithoutCode;
    if (!records.frameMaps.words.isEmpty())
        sites.rareLists.ensure().frameMapWords = WTF::move(records.frameMaps.words);
    collectFaultingAccesses(described, records, emitted.procedure, linkBuffer, body, sites.faultingAccesses);
    if (!records.traps.pollSites.isEmpty()) {
        ListOfUnit<PollSite>& pollSites = sites.rareLists.ensure().pollSites;
        pollSites = WTF::move(records.traps.pollSites);
        AOT::sortInLine(pollSites, { }, &PollSite::offset);
    }

    SiteReferencesInOrder references(body, sites);
    size_t numberOfFrames = described.code.inlineFrames().size();
    size_t numberOfInlineCallSites = numberOfFrames ? numberOfFrames - 1 : 0;
    bool hasOnlyInlineCallSites = numberOfInlineCallSites && sites.numberOfSitesWithoutCode == numberOfInlineCallSites && references.isAtEnd();
    DescribedCallees callees(body);
    describeSitesInOrder(described, records, linkBuffer, references, callees, sites);
    if (hasOnlyInlineCallSites) {
        sites.numberOfSitesWithoutCode = 0;
        sites.positions.shrink(1);
        if (RareListsOfSites* rareLists = sites.rareLists.ifExists())
            rareLists->constructionStarts.clear();
        return;
    }
    describeInlineFrames(described.program, described.code, records.sites, callees, sites.inlineFrames);
}

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
    if (!records.mentions.isEmpty())
        records.mentions.moveTo(body);
    body.bytes = emitted.buffer.takeBytes();
    body.entries = describeEntriesOfUnit(records, emitted.procedure, linkBuffer);
    if (!emitted.procedure.constantData().empty())
        Machine::describeConstants(emitted.procedure, body.rareLists.ensure().constants);
    if (!emitted.procedure.jumpTables().empty())
        Machine::describeJumpTables(emitted.procedure, linkBuffer, body.rareLists.ensure().jumpTables);
    if (!emitted.procedure.shortLoops().empty()) {
        uint64_t placesOfPrimaryEntry = std::rotr(alignedPlacesInLine(imageFunctionAlignment), static_cast<int>(body.primaryEntryOffset() % imageFunctionAlignment));
        if (uint64_t places = Machine::describePlacesInLine(emitted.procedure, linkBuffer, placesOfPrimaryEntry); places != UINT64_MAX)
            body.rareLists.ensure().placesInLine = places;
    }
}

static void describeFrameOfUnit(const CodeRecords& records, EmittedCode& emitted, FrameDescription& frame)
{
    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    Machine::describeFrame(emitted.procedure, linkBuffer, frame);
    RELEASE_ASSERT(emitted.procedure.frame().hasFrame() || !frame.hasFrame());
    if (emitted.procedure.traits().savesRegistersOfCallers) {
        RegisterSet restoredByUnwinding = RegisterSet::vmCalleeSaveRegisters();
        for (const RegisterAtOffset& save : frame.calleeSaveRegisters)
            RELEASE_ASSERT(!restoredByUnwinding.contains(save.reg(), IgnoreVectors));
        frame.calleeSaveRegisters = { };
    }
    frame.spillsArgumentRegisters = records.frame.spillsArgumentRegisters;
    frame.stackUsedAtCalls = records.frame.stackUsedAtCalls;
    frame.isEnteredWithoutStackCheck = emitted.stackChecks.isEnteredWithoutStackCheck;
    frame.isProvenToFitOnStack = emitted.stackChecks.isProvenToFitOnStack;
    if (!records.traps.stackChecks.isEmpty())
        frame.prologueChecks = linkStackChecks(linkBuffer, records);
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
        return calleeOfArgumentMoves(body.argumentMoves[relocation.symbol]);
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

static bool describedSiteTakesLoopForms(const Code& code, const FormsSaid& said, const SiteOfForm& site)
{
    const Block& block = code.at(site.block);
    if (block.isInLoop)
        return true;
    if ((said.isTrapOfProxy() || code.callsItself()) && block.frequency != BlockFrequency::Rare)
        return true;
    return site.value && Operation::opcodesWithHasLeftLoop.contains(site.opcode) && code.at(site.value).hasLeftLoop();
}

static bool describedSiteStandsWhereItSays(const Code& code, const FormsSaid& said, const SiteOfForm& site)
{
    if (site.place == Place::Anywhere)
        return true;
    if (!site.block)
        return false;
    const Block& block = code.at(site.block);
    switch (site.place) {
    case Place::Loop:
        return describedSiteTakesLoopForms(code, said, site);
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

static uint32_t describedBytesOfCallFor(const Operation& operation)
{
    constexpr uint32_t bytesOfInstructionThatBringsWord = isX86_64() ? 10 : 2 * sizeof(uint32_t);
    return static_cast<uint32_t>(sizeOfNearCall) + bytesOfInstructionThatBringsWord * (1 + static_cast<uint32_t>(operation.payload().size()));
}

namespace {

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

static Vector<BytesOfForm> describeBytesByForm(const Program& program, const Code& code, const FormsSaid& said, const CodeBody& body, EmittedCode& emitted, MeasurementsOfCode& measurements)
{
    static_assert(Form::Open < Form::Closed && Form::Closed < Form::Runtime);
    constexpr uint32_t noSite = FormsSaid::noSite;

    LinkBuffer& linkBuffer = emitted.buffer.linkBuffer();
    Vector<Machine::LinkedRangeOfOrigin> ranges;
    Machine::describeRangesOfOperations(emitted.procedure, linkBuffer, ranges);
    DescribedRareCode rareCode(emitted);

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
        bool isNamed = named == form && !site.hasContradictedItself && describedSiteStandsWhereItSays(code, said, site);
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
        measurements.bytesInInlineFrames += operation.bytes;
        const InlineFrame& frame = code.inlineFrames()[inlineFrame];
        if (frame.calledBuiltin)
            measurements.bytesInFramesOfBuiltins += operation.bytes;
        else if (isLibraryCode(program, program.at(frame.callee)))
            measurements.bytesInInlineFramesOfLibrary += operation.bytes;
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
        bytesByForm.last().schemaMismatchCount += entry.schemaMismatchCount;
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
            BlockID block = value && !code.isConstant(value) ? code.blockOf(value) : BlockID();
            ranges.append({ beginOfPiece, endOfPiece, value ? 0 : origin.bits(), value, value ? code.at(value).opcode : Opcode::Undefined, isRare, value ? static_cast<uint32_t>(code.at(value).inlineFrame) : 0u, block && code.at(block).frequency == BlockFrequency::Rare });
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
    std::span<const PrologueCheck> prologueChecks = code.frame.prologueChecks.span();
    for (size_t index = returnSites.size(); index--;) {
        uint32_t returnOffset = returnSites[index].offset;
        while (!prologueChecks.empty() && prologueChecks.back().callOffset + sizeOfNearCall > returnOffset)
            prologueChecks = prologueChecks.first(prologueChecks.size() - 1);
        if (!prologueChecks.empty() && prologueChecks.back().callOffset + sizeOfNearCall == returnOffset)
            continue;
        add(returnOffset);
        break;
    }
    if (!code.sites.faultingAccesses.isEmpty())
        add(code.sites.faultingAccesses.last().codeOffset + static_cast<uint32_t>(sizeOfNearCall));
    if (!code.sites.rareLists->pollSites.isEmpty())
        add(code.sites.rareLists->pollSites.last().returnOffset());
    ASSERT(end == endOfLastReturnSite(code));
    return end;
}

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
        MeasurementsOfCode& measurements = result.measurements.ensure();
        measurements.bytesByForm = describeBytesByForm(inputs.program, code, records.formsSaid, result.body, emitted, measurements);
        uint64_t codeFormSchemaMismatches = 0;
        uint64_t coldCodeFormsChosen = 0;
        for (const BytesOfForm& row : measurements.bytesByForm) {
            codeFormSchemaMismatches += row.schemaMismatchCount;
            coldCodeFormsChosen += row.isChosenForColdCode() ? row.count : 0;
        }
        build.statistics().count(StatisticID::CodeFormSchemaMismatches, codeFormSchemaMismatches);
        build.statistics().count(StatisticID::ColdCodeFormsChosen, coldCodeFormsChosen);
        measurements.bytesOutsideOperations = describeBytesOutsideOperations(result.body, emitted);
        measurements.ranges = describeRangesOfCode(code, result.body, emitted);
    }
    if (!records.inlinedStrings.isEmpty())
        records.inlinedStrings.moveTo(result.measurements.ensure());
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
