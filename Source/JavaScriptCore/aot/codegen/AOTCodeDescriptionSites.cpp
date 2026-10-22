/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeDescription.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCodeRecords.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionKinds.h"
#include "AOTThrowEmitter.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct SiteReference {
    uint32_t returnOffset;
    uint32_t* site;
    bool isFirstRead { false };
};

struct RenumberedSites {
    Vector<RecordedSite> sites;
    size_t numberOfSitesBeforeItems { 0 };
};

// B3 may copy a site, and each copy has an edge to a landing pad of its own. The copies record their landing pads in the order in which their code is emitted.
class LandingPadsOfCopies {
public:
    LandingPadsOfCopies(const ExceptionRecords&, size_t numberOfSites, std::span<const SiteReference>);

    // Called once for each reference, by ascending return offset.
    const SiteLandingPad* takeNext(uint32_t recordedNumber);

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

    std::ranges::sort(traps.trapSites, { }, &TrapSite::value);
    Vector<FaultingAccess> accesses;
    accesses.reserveInitialCapacity(traps.trappingAccesses.size() + traps.callThrowSites.size());
    for (const TrappingAccess& access : traps.trappingAccesses) {
        auto site = std::ranges::lower_bound(traps.trapSites, access.value, { }, &TrapSite::value);
        if (site == traps.trapSites.end() || site->value != access.value) {
            reportUndeclaredTrap(described, access.value);
            continue;
        }
        accesses.append({ linkBuffer.offsetOf(access.instruction), OptionalIndex { site->callSite }, throwSiteWords(site->throwSite, access.referenceIsNarrow ? Representation::Cell32 : site->representation, static_cast<unsigned>(access.reference)) });
    }
    for (const CallThrowSite& site : traps.callThrowSites)
        accesses.append({ body.returnSites[site.returnSite].offset - static_cast<uint32_t>(sizeOfNearCall), { }, site.words });
    std::ranges::sort(accesses, { }, &FaultingAccess::codeOffset);
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
    std::ranges::stable_sort(references, { }, &SiteReference::returnOffset);
    return references;
}

LandingPadsOfCopies::LandingPadsOfCopies(const ExceptionRecords& exceptions, size_t numberOfSites, std::span<const SiteReference> references)
    : m_landingPads(exceptions.sites)
{
    if (m_landingPads.isEmpty())
        return;
    std::ranges::stable_sort(m_landingPads, { }, &SiteLandingPad::site);
    m_copies.grow(numberOfSites + 1);
    for (uint32_t index = m_landingPads.size(); index--;) {
        Copies& copies = m_copies[m_landingPads[index].site];
        copies.firstLandingPad = index;
        ++copies.numberOfLandingPads;
    }
    for (const SiteReference& reference : references)
        ++m_copies[*reference.site].numberOfReferences;
}

const SiteLandingPad* LandingPadsOfCopies::takeNext(uint32_t recordedNumber)
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

static RenumberedSites renumberSites(const CodeRecords& records, LinkBuffer& linkBuffer, std::span<const SiteReference> references, SiteDescription& sites)
{
    const Vector<RecordedSite>& recorded = records.sites.positions;
    const Vector<SpreadSite>& itemSites = records.sites.itemSites;
    ASSERT(std::ranges::is_sorted(itemSites, { }, &SpreadSite::callSite));
    LandingPadsOfCopies landingPads(records.exceptions, recorded.size(), references);
    auto recordOf = [&](uint32_t recordedNumber) -> const RecordedSite& {
        RELEASE_ASSERT(recordedNumber && recordedNumber <= recorded.size());
        return recorded[recordedNumber - 1];
    };

    // One operation of the program is often several calls in a row. A return address that says nothing but what the one in front of it says shares its site:
    // the image finds the site at or in front of a return address.
    struct LastSite {
        const RecordedSite* record { nullptr };
        const SiteLandingPad* landingPad { nullptr };
    };
    LastSite last;
    auto leadToSameHandler = [](const SiteLandingPad* a, const SiteLandingPad* b) {
        return a && b ? a->landingPad == b->landingPad && a->type == b->type : a == b;
    };

    RenumberedSites result;
    result.sites.reserveInitialCapacity(records.sites.numberOfSitesWithoutCode + references.size() + itemSites.size());
    result.sites.append(recorded.span().first(records.sites.numberOfSitesWithoutCode));
    for (const SiteReference& reference : references) {
        uint32_t recordedNumber = *reference.site;
        const RecordedSite& record = recordOf(recordedNumber);
        const SiteLandingPad* landingPad = landingPads.takeNext(recordedNumber);
        auto firstItem = std::ranges::lower_bound(itemSites, recordedNumber, { }, &SpreadSite::callSite);
        bool saysOnlyWhereItIs = !reference.isFirstRead && !record.constructs && !record.isTailCall && (firstItem == itemSites.end() || firstItem->callSite != recordedNumber);
        if (saysOnlyWhereItIs && last.record && *last.record == record && leadToSameHandler(last.landingPad, landingPad)) {
            *reference.site = result.sites.size();
            continue;
        }
        result.sites.append(record);
        uint32_t number = result.sites.size();
        *reference.site = number;
        if (landingPad)
            sites.handlers.append({ number, linkBuffer.offsetOf(*records.exceptions.landingPads[landingPad->landingPad]), landingPad->type });
        for (auto entry = firstItem; entry != itemSites.end() && entry->callSite == recordedNumber; ++entry)
            sites.spreadSites.append({ number, entry->item, entry->site });
        last = saysOnlyWhereItIs ? LastSite { &record, landingPad } : LastSite { };
    }
    result.numberOfSitesBeforeItems = result.sites.size();
    for (SpreadSite& entry : sites.spreadSites) {
        result.sites.append(recordOf(entry.site));
        entry.site = result.sites.size();
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

static std::optional<LineColumn> positionInBuiltin(const Program& program, FunctionID shown)
{
    const Function& owner = program.at(shown);
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

static FunctionID functionShownFor(const Program& program, FunctionID id, const Function& function, uint32_t frame)
{
    return firstSourceOf(program, frame ? function.inlineFrames[frame].callee : id);
}

static ShownFunctions findShownFunctions(const DescribedFunction& described)
{
    uint32_t numberOfFrames = std::max<uint32_t>(described.function.inlineFrames.size(), 1);
    ShownFunctions shownFunctions;
    shownFunctions.reserveInitialCapacity(numberOfFrames);
    for (uint32_t frame = 0; frame < numberOfFrames; ++frame) {
        FunctionID shown = functionShownFor(described.program, described.id, described.function, frame);
        shownFunctions.append({ shown, positionInBuiltin(described.program, shown) });
    }
    return shownFunctions;
}

static Vector<SitePosition> describePositions(const DescribedFunction& described, const RenumberedSites& renumbered)
{
    const Program& program = described.program;
    ShownFunctions shownFunctions = findShownFunctions(described);
    Vector<SitePosition> positions;
    positions.reserveInitialCapacity(renumbered.sites.size() + 1);
    positions.append(positionIn(program, shownFunctions[0], program.at(shownFunctions[0].id).position, 0));
    for (size_t i = 0; i < renumbered.sites.size(); ++i) {
        Origin origin = renumbered.sites[i].origin;
        positions.append(positionIn(program, shownFunctions[origin.inlineFrame], origin.position, i >= renumbered.numberOfSitesBeforeItems ? 0 : origin.inlineFrame));
    }
    return positions;
}

static Vector<FlaggedSite> collectFlaggedSites(const RenumberedSites& renumbered, uint32_t numberOfSitesWithoutCode)
{
    Vector<FlaggedSite> flaggedSites;
    for (uint32_t i = numberOfSitesWithoutCode; i < renumbered.sites.size(); ++i) {
        OptionSet<SiteFlag> flags;
        if (renumbered.sites[i].constructs)
            flags.add(SiteFlag::Constructs);
        if (renumbered.sites[i].isTailCall)
            flags.add(SiteFlag::IsTailCall);
        if (!flags.isEmpty())
            flaggedSites.append({ i + 1, flags });
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

SiteDescription describeSites(const DescribedFunction& described, CodeRecords& records, LinkBuffer& linkBuffer, CodeBody& body)
{
    SiteDescription sites;
    sites.numberOfSitesWithoutCode = records.sites.numberOfSitesWithoutCode;
    sites.frameMapWords = WTF::move(records.frameMaps.words);
    sites.faultingAccesses = collectFaultingAccesses(described, records, linkBuffer, body);
    sites.pollSites = WTF::move(records.traps.pollSites);
    std::ranges::sort(sites.pollSites, { }, &PollSite::offset);

    Vector<SiteReference, 32> references = collectSiteReferences(body, sites);
    RenumberedSites renumbered = renumberSites(records, linkBuffer, references.span(), sites);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
