/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileQueue.h"

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTInliningAcrossFiles.h"
#include "AOTLinkTables.h"
#include "AOTSort.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/HashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/simde/simde.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT {

static constexpr uint32_t bitsInWordOfUnitQueue = 32;

static String describeUnitOfQueue(const Typed::Program& program, Typed::CodeUnit unit)
{
    const Typed::Function& function = program.at(unit.function);
    StringBuilder text;
    text.append("function "_s, unit.function.index());
    if (function.name && function.name != program.strings.emptyString())
        text.append(" '"_s, program.strings.toWTFString(function.name), '\'');
    if (function.module) {
        if (StringID path = program.at(function.module).displayPath)
            text.append(" of "_s, program.strings.toWTFString(path), ':', program.lineColumnFor(function.module, function.position).line);
    }
    text.append(", "_s, nameOf(unit.role));
    return text.toString();
}

[[noreturn]] static NEVER_INLINE void crashOnMentionOfUnplannedUnit(const Typed::Program& program, Typed::CodeUnit unit)
{
    dataLogLn("AOT: finished code mentions a unit that is not in the plan: ", describeUnitOfQueue(program, unit), ", ", unit.placeOfSelector(), " bound arguments");
    CRASH();
}

[[noreturn]] static NEVER_INLINE void crashOnMentionOfDroppedUnit(const Typed::Program& program, Typed::CodeUnit unit, Typed::CodeUnit mentionedBy)
{
    dataLogLn("AOT: the code of ", describeUnitOfQueue(program, mentionedBy), ", mentions ", describeUnitOfQueue(program, unit), ", whose cycle is done: LinkTables::referrers lacks whoever leads there");
    CRASH();
}

[[noreturn]] static NEVER_INLINE void crashOnMentionWithoutReferrer(const Typed::Program& program, Typed::CodeUnit unit, Typed::CodeUnit mentionedBy)
{
    dataLogLn("AOT: the code of ", describeUnitOfQueue(program, mentionedBy), ", mentions ", describeUnitOfQueue(program, unit), ", and LinkTables::referrers does not lead from the one to the other");
    CRASH();
}

[[noreturn]] static NEVER_INLINE void crashOnMentionByUnitOutsideCycles(const Typed::Program& program, Typed::CodeUnit unit, Typed::CodeUnit mentionedBy)
{
    dataLogLn("AOT: the code of ", describeUnitOfQueue(program, mentionedBy), ", mentions ", describeUnitOfQueue(program, unit), ", and no cycle waits for the unit that mentions: LinkTables::standsOutsideCycles()");
    CRASH();
}

[[noreturn]] static NEVER_INLINE void crashOnRanksOutOfOrder(uint32_t awaited, uint32_t cycle)
{
    dataLogLn("AOT: cycle ", cycle, " of LinkTables::cyclesAwaited waits for cycle ", awaited, ", which has no lower rank");
    CRASH();
}

static constexpr Seconds timeThreadOfUnitQueueLooksBeforeSleep = 50_us;
static constexpr unsigned pausesBetweenLooksOfUnitQueue = 16;

static uint32_t numberOfCyclesOfUnitQueue(const Typed::LinkTables* link)
{
    if (!link)
        return 1;
    RELEASE_ASSERT(!link->rankOfCycle.isEmpty() && link->firstCycleAwaited.size() == link->rankOfCycle.size() + 1);
    return static_cast<uint32_t>(link->rankOfCycle.size());
}

static void sumCountsOfUnitQueue(Vector<uint32_t>& firstRowOfOwner)
{
    for (size_t owner = 1; owner < firstRowOfOwner.size(); ++owner)
        firstRowOfOwner[owner] += firstRowOfOwner[owner - 1];
}

UnitQueue::UnitQueue(const Typed::Program& program, const Typed::CallingConventions& conventions, unsigned numberOfThreads)
    : UnitQueue(program, conventions, nullptr, nullptr, numberOfThreads)
{
}

UnitQueue::UnitQueue(const Typed::Program& program, const Typed::CallingConventions& conventions, const Typed::LinkTables& link, Typed::Arrivals& arrivals, unsigned numberOfThreads)
    : UnitQueue(program, conventions, &link, &arrivals, numberOfThreads)
{
}

UnitQueue::UnitQueue(const Typed::Program& program, const Typed::CallingConventions& conventions, const Typed::LinkTables* link, Typed::Arrivals* arrivals, unsigned numberOfThreads)
    : m_program(program)
    , m_conventions(conventions)
    , m_link(link)
    , m_arrivals(arrivals)
    , m_cycles(numberOfCyclesOfUnitQueue(link))
    , m_isMentioned((static_cast<size_t>(conventions.numberOfUnits()) + bitsInWordOfUnitQueue - 1) / bitsInWordOfUnitQueue)
    , m_whatIsKnownOfEntries(link ? conventions.numberOfFunctions() : 0)
    , m_selectorsWhoseUnitsAreMentioned(conventions.methodSelectors().size())
    , m_unitsThatAskAmongMentionedUnitsOfClasses(program.layouts.size())
    , m_unitsOfThreads(numberOfThreads)
{
    RELEASE_ASSERT(numberOfThreads);
    for (std::atomic<uint32_t>& word : m_isMentioned)
        word.store(0, std::memory_order_relaxed);
    for (std::atomic<uint8_t>& known : m_whatIsKnownOfEntries)
        known.store(0, std::memory_order_relaxed);
    for (std::atomic<bool>& areMentioned : m_selectorsWhoseUnitsAreMentioned)
        areMentioned.store(false, std::memory_order_relaxed);
    for (std::atomic<const Vector<Typed::CodeUnit>*>& unitsThatAsk : m_unitsThatAskAmongMentionedUnitsOfClasses)
        unitsThatAsk.store(nullptr, std::memory_order_relaxed);
    for (Cycle& cycle : m_cycles) {
        cycle.numberOfCyclesAwaited.store(0, std::memory_order_relaxed);
        cycle.numberOfOpenUnits.store(0, std::memory_order_relaxed);
        cycle.state.store(CycleState::Waiting, std::memory_order_relaxed);
    }
    if (link)
        orderCycles();

    for (uint32_t number = 0; number < m_cycles.size(); ++number) {
        Cycle& cycle = m_cycles[number];
        bool awaitsNobody = !cycle.numberOfCyclesAwaited.load(std::memory_order_relaxed);
        if (!awaitsNobody && cycle.state.load(std::memory_order_relaxed) != CycleState::Ready)
            continue;
        cycle.numberOfOpenUnits.store(1, std::memory_order_relaxed);
        cycle.state.store(CycleState::Ready, std::memory_order_relaxed);
        if (awaitsNobody)
            m_cyclesReadyFromStart.append(number);
    }
}

UnitQueue::~UnitQueue()
{
    for (std::atomic<const Vector<Typed::CodeUnit>*>& unitsThatAsk : m_unitsThatAskAmongMentionedUnitsOfClasses)
        delete unitsThatAsk.load(std::memory_order_relaxed);
}

void UnitQueue::didMentionUnitsOfClass(LayoutID classLayout, Vector<Typed::CodeUnit>&& thoseThatAskWhoMentions)
{
    auto kept = makeUnique<Vector<Typed::CodeUnit>>(WTF::move(thoseThatAskWhoMentions));
    const Vector<Typed::CodeUnit>* nobodyWasFirst = nullptr;
    if (m_unitsThatAskAmongMentionedUnitsOfClasses[classLayout.index()].compare_exchange_strong(nobodyWasFirst, kept.get(), std::memory_order_release, std::memory_order_relaxed))
        (void)kept.release();
}

void UnitQueue::orderCycles()
{
    const Typed::LinkTables& link = *m_link;
    uint32_t numberOfFunctions = m_conventions.numberOfFunctions();
    uint32_t numberOfCycles = static_cast<uint32_t>(m_cycles.size());
    RELEASE_ASSERT(link.functions.size() == numberOfFunctions);

    m_cycleOfFunction = Vector<uint32_t>(numberOfFunctions, [&](size_t function) {
        RELEASE_ASSERT(link.functions[function].cycle < numberOfCycles);
        return link.functions[function].cycle;
    });

    m_firstMemberOfCycle.fill(0, numberOfCycles + 1);
    for (uint32_t cycle : m_cycleOfFunction)
        ++m_firstMemberOfCycle[cycle + 1];
    sumCountsOfUnitQueue(m_firstMemberOfCycle);
    m_membersOfCycles.grow(numberOfFunctions);
    Vector<uint32_t> nextPlaceOfCycle = m_firstMemberOfCycle;
    for (uint32_t function = 0; function < numberOfFunctions; ++function)
        m_membersOfCycles[nextPlaceOfCycle[m_cycleOfFunction[function]]++] = FunctionID(function);

    RELEASE_ASSERT(link.firstCycleNamed.size() == numberOfCycles + 1 && link.cyclesNamed.size() == link.cyclesAwaited.size());
    for (uint32_t cycle = 0; cycle < numberOfCycles; ++cycle) {
        std::span<const uint32_t> awaited = link.cyclesAwaitedBy(cycle);
        for (uint32_t other : awaited) {
            if (link.rankOfCycle[other] >= link.rankOfCycle[cycle]) [[unlikely]]
                crashOnRanksOutOfOrder(other, cycle);
        }
        m_cycles[cycle].numberOfCyclesAwaited.store(static_cast<uint32_t>(awaited.size()), std::memory_order_relaxed);

        bool takesWhatWasLeft = std::ranges::any_of(m_membersOfCycles.span().subspan(m_firstMemberOfCycle[cycle], m_firstMemberOfCycle[cycle + 1] - m_firstMemberOfCycle[cycle]), [&](FunctionID member) {
            return m_arrivals->takesWhatWasLeft(member);
        });
        if (!takesWhatWasLeft)
            m_cycles[cycle].state.store(CycleState::Ready, std::memory_order_relaxed);
    }

    for (uint32_t unit = numberOfFunctions * Typed::numberOfCodeRolesOfOneFunction; unit < m_conventions.numberOfUnits(); ++unit)
        m_unitsBehindRoles.append({ m_conventions.unitAt(unit).function.index(), unit });
    AOT::stableSort(m_unitsBehindRoles, { }, &UnitBehindRoles::function);
}

std::span<const uint32_t> UnitQueue::cyclesNamedBy(uint32_t cycle) const
{
    return m_link ? m_link->cyclesNamedBy(cycle) : std::span<const uint32_t>();
}

bool UnitQueue::cycleLeadsTo(uint32_t referrer, uint32_t named) const
{
    return !m_link || m_link->cycleLeadsTo(referrer, named);
}

bool UnitQueue::isInQueueFromStart(FunctionID function) const
{
    return m_link && m_link->functions[function.index()].isInQueueFromStart;
}

bool UnitQueue::leadsTo(FunctionID referrer, FunctionID function) const
{
    return cycleLeadsTo(cycleOf(referrer), cycleOf(function));
}

bool UnitQueue::isSeedThatWaitsForNobody(Typed::CodeUnit unit) const
{
    uint32_t index = m_conventions.indexOf(unit);
    return !m_isSeed.isEmpty() && (m_isSeed[index / bitsInWordOfUnitQueue] >> (index % bitsInWordOfUnitQueue) & 1) && !m_arrivals->takesWhatWasLeft(unit.function);
}

ALWAYS_INLINE_INTO_OTHER_FILES void UnitQueue::checkMentionByTakenUnit(unsigned thread, Typed::CodeUnit unit) const
{
    if (m_isTakingSeeds)
        return;
    const UnitsOfThread& unitsOfThread = m_unitsOfThreads[thread];
    if (unitsOfThread.takenUnitStandsOutsideCycles) [[unlikely]]
        crashOnMentionByUnitOutsideCycles(m_program, unit, m_conventions.unitAt(unitsOfThread.takenUnit));
    if (!cycleLeadsTo(unitsOfThread.cycleOfTakenUnit, cycleOf(unit.function)) && !isSeedThatWaitsForNobody(unit)) [[unlikely]]
        crashOnMentionWithoutReferrer(m_program, unit, m_conventions.unitAt(unitsOfThread.takenUnit));
}

Vector<Typed::CodeUnit> UnitQueue::mentionedFusedCallEntries() const
{
    RELEASE_ASSERT(!m_numberOfOpenUnits.load(std::memory_order_acquire));
    Vector<Typed::CodeUnit> entries;
    for (uint32_t index = 0; index < m_conventions.numberOfFunctions(); ++index) {
        Typed::CodeUnit entry = m_conventions.closureEntryOf(FunctionID(index));
        if (m_conventions.hasFusedCallEntry(entry.function) && isMentioned(m_conventions.indexOf(entry)) && (m_conventions.callEntryIsNumberOfModuleStart(entry.function) || isMentioned(m_conventions.indexOf(m_conventions.genericEntry(entry).target))))
            entries.append(entry);
    }
    return entries;
}

void UnitQueue::push(unsigned thread, uint32_t unitIndex, Cycle* cycle)
{
    if (cycle)
        cycle->numberOfOpenUnits.fetch_add(1, std::memory_order_relaxed);
    m_numberOfOpenUnits.fetch_add(1, std::memory_order_relaxed);

    UnitsOfThread& unitsOfThread = m_unitsOfThreads[thread];
    size_t numberOfUnits;
    {
        Locker locker { unitsOfThread.lock };
        unitsOfThread.units.append(unitIndex);
        numberOfUnits = unitsOfThread.units.size();
        unitsOfThread.numberOfUnits.store(static_cast<uint32_t>(numberOfUnits), std::memory_order_seq_cst);
    }
    if (numberOfUnits > 1 && !m_isTakingSeeds)
        wakeThreadUnlessOneIsLooking();
}

bool UnitQueue::asksWhoMentions(Typed::CodeUnit unit) const
{
    return !isInQueueFromStart(unit.function) || (unit.role == Typed::CodeRole::SlowBody && !m_conventions.isSeedOfFunctionInQueueFromStart(unit));
}

void UnitQueue::mentionEntriesOf(unsigned thread, FunctionID function)
{
    Typed::CodeUnit entry = m_conventions.closureEntryOf(function);
    mention(thread, entry);
    if (Typed::CodeUnit constructEntry = m_conventions.genericEntry(entry).constructEntry)
        mention(thread, constructEntry);
}

void UnitQueue::noteEntryByValue(unsigned thread, FunctionID function, bool asksEveryTeller)
{
    if (!isEntryThatMayBeClosed(m_conventions.closureEntryOf(function)))
        return;
    std::atomic<uint8_t>& known = m_whatIsKnownOfEntries[function.index()];
    bool isToldAlready = known.load(std::memory_order_relaxed) & isEnteredByValue;
    if (isToldAlready && !asksEveryTeller)
        return;
    checkMentionByTakenUnit(thread, m_conventions.closureEntryOf(function));
    if (isToldAlready)
        return;
    if (known.fetch_or(isEnteredByValue, std::memory_order_acq_rel) == wordIsNamed)
        mentionEntriesOf(thread, function);
}

void UnitQueue::mention(unsigned thread, Typed::CodeUnit unit, MentionKind kind)
{
    if (!m_conventions.exists(unit)) [[unlikely]]
        crashOnMentionOfUnplannedUnit(m_program, unit);

    if (isEntryThatMayBeClosed(unit)) {
        std::atomic<uint8_t>& known = m_whatIsKnownOfEntries[unit.function.index()];
        if (kind == MentionKind::ClosureWord) {
            if (known.load(std::memory_order_relaxed) & wordIsNamed)
                return;
            checkMentionByTakenUnit(thread, unit);
            if (m_cycles[cycleOf(unit.function)].state.load(std::memory_order_acquire) == CycleState::Done) [[unlikely]]
                crashOnMentionOfDroppedUnit(m_program, unit, m_conventions.unitAt(m_unitsOfThreads[thread].takenUnit));
            if (known.fetch_or(wordIsNamed, std::memory_order_acq_rel) == isEnteredByValue)
                mentionEntriesOf(thread, unit.function);
            return;
        }
        if (known.fetch_or(isEnteredByValue, std::memory_order_acq_rel) == wordIsNamed)
            mentionEntriesOf(thread, unit.function);
    }

    bool isOfFunctionInQueueFromStart = !asksWhoMentions(unit);
    if (!isOfFunctionInQueueFromStart)
        checkMentionByTakenUnit(thread, unit);

    uint32_t index = m_conventions.indexOf(unit);
    std::atomic<uint32_t>& word = m_isMentioned[index / bitsInWordOfUnitQueue];
    uint32_t bit = 1u << (index % bitsInWordOfUnitQueue);
    if (word.load(std::memory_order_relaxed) & bit || word.fetch_or(bit, std::memory_order_relaxed) & bit)
        return;

    if (unit.role == Typed::CodeRole::GenericCallEntry && m_conventions.hasFusedCallEntry(unit.function)) {
        if (!m_conventions.callEntryIsNumberOfModuleStart(unit.function))
            mention(thread, m_conventions.genericEntry(unit).target);
        return;
    }

    if (standsOutsideCycles(unit)) {
        push(thread, index, nullptr);
        return;
    }
    RELEASE_ASSERT(!isOfFunctionInQueueFromStart || m_isTakingSeeds);

    Cycle& cycle = m_cycles[cycleOf(unit.function)];
    switch (cycle.state.load(std::memory_order_acquire)) {
    case CycleState::Waiting:
        break;
    case CycleState::Ready:
        if (unit.role != Typed::CodeRole::SlowBody)
            push(thread, index, &cycle);
        break;
    case CycleState::ReadyForSlowVersions:
    case CycleState::ReadyForClosedEntries:
        push(thread, index, &cycle);
        break;
    case CycleState::Done:
        crashOnMentionOfDroppedUnit(m_program, unit, m_conventions.unitAt(m_unitsOfThreads[thread].takenUnit));
    }
}

bool UnitQueue::beginCycle(unsigned thread, uint32_t number)
{
    Cycle& cycle = m_cycles[number];
    Vector<uint32_t, 16> mentionedUnits;
    for (uint32_t place = m_firstMemberOfCycle[number]; place < m_firstMemberOfCycle[number + 1]; ++place) {
        FunctionID member = m_membersOfCycles[place];
        uint32_t firstUnit = member.index() * Typed::numberOfCodeRolesOfOneFunction;
        bool hasFusedCallEntry = m_conventions.hasFusedCallEntry(member);
        for (uint32_t role = 0; role < Typed::numberOfCodeRolesOfOneFunction; ++role) {
            if (!isMentioned(firstUnit + role))
                continue;
            if (hasFusedCallEntry && static_cast<Typed::CodeRole>(role) == Typed::CodeRole::GenericCallEntry)
                continue;
            if (static_cast<Typed::CodeRole>(role) == Typed::CodeRole::SlowBody)
                continue;
            mentionedUnits.append(firstUnit + role);
        }
        if (m_unitsBehindRoles.isEmpty())
            continue;
        for (const UnitBehindRoles& other : std::ranges::equal_range(m_unitsBehindRoles, member.index(), { }, &UnitBehindRoles::function)) {
            if (isMentioned(other.unit))
                mentionedUnits.append(other.unit);
        }
    }

    cycle.numberOfOpenUnits.store(1, std::memory_order_relaxed);
    cycle.state.store(CycleState::Ready, std::memory_order_release);
    for (uint32_t unit : mentionedUnits)
        push(thread, unit, &cycle);
    return cycle.numberOfOpenUnits.fetch_sub(1, std::memory_order_acq_rel) > 1;
}

bool UnitQueue::beginSlowVersionsOfCycle(unsigned thread, uint32_t number)
{
    Vector<uint32_t, 16> mentionedUnits;
    auto addMember = [&](FunctionID member) {
        uint32_t unit = member.index() * Typed::numberOfCodeRolesOfOneFunction + static_cast<uint32_t>(Typed::CodeRole::SlowBody);
        if (isMentioned(unit))
            mentionedUnits.append(unit);
    };
    if (m_cycleOfFunction.isEmpty()) {
        for (uint32_t function = 0; function < m_conventions.numberOfFunctions(); ++function)
            addMember(FunctionID(function));
    } else {
        for (uint32_t place = m_firstMemberOfCycle[number]; place < m_firstMemberOfCycle[number + 1]; ++place)
            addMember(m_membersOfCycles[place]);
    }
    if (mentionedUnits.isEmpty())
        return false;

    Cycle& cycle = m_cycles[number];
    cycle.numberOfOpenUnits.store(1, std::memory_order_relaxed);
    cycle.state.store(CycleState::ReadyForSlowVersions, std::memory_order_release);
    for (uint32_t unit : mentionedUnits)
        push(thread, unit, &cycle);
    return cycle.numberOfOpenUnits.fetch_sub(1, std::memory_order_acq_rel) > 1;
}

bool UnitQueue::beginClosedEntriesOfCycle(unsigned thread, uint32_t number)
{
    if (m_whatIsKnownOfEntries.isEmpty() || m_cycleOfFunction.isEmpty())
        return false;
    Vector<FunctionID, 16> functionsWhoseWordAloneIsNamed;
    for (uint32_t place = m_firstMemberOfCycle[number]; place < m_firstMemberOfCycle[number + 1]; ++place) {
        FunctionID member = m_membersOfCycles[place];
        if (m_whatIsKnownOfEntries[member.index()].load(std::memory_order_acquire) == wordIsNamed)
            functionsWhoseWordAloneIsNamed.append(member);
    }
    if (functionsWhoseWordAloneIsNamed.isEmpty())
        return false;

    Cycle& cycle = m_cycles[number];
    cycle.numberOfOpenUnits.store(1, std::memory_order_relaxed);
    cycle.state.store(CycleState::ReadyForClosedEntries, std::memory_order_release);
    auto pushUnlessMentioned = [&](Typed::CodeUnit unit) {
        uint32_t index = m_conventions.indexOf(unit);
        uint32_t bit = 1u << (index % bitsInWordOfUnitQueue);
        bool liesInBodyThatIsThere = unit.role == Typed::CodeRole::GenericCallEntry && m_conventions.hasFusedCallEntry(unit.function) && isMentioned(m_conventions.indexOf(m_conventions.genericEntry(unit).target));
        if (!(m_isMentioned[index / bitsInWordOfUnitQueue].fetch_or(bit, std::memory_order_relaxed) & bit) && !liesInBodyThatIsThere)
            push(thread, index, &cycle);
    };
    for (FunctionID member : functionsWhoseWordAloneIsNamed) {
        m_arrivals->closeEntriesOf(member);
        Typed::CodeUnit entry = m_conventions.closureEntryOf(member);
        pushUnlessMentioned(entry);
        if (Typed::CodeUnit constructEntry = m_conventions.genericEntry(entry).constructEntry)
            pushUnlessMentioned(constructEntry);
    }
    return cycle.numberOfOpenUnits.fetch_sub(1, std::memory_order_acq_rel) > 1;
}

void UnitQueue::finishCycle(unsigned thread, uint32_t first)
{
    Vector<uint32_t, 16> cyclesToFinish;
    cyclesToFinish.append(first);
    while (!cyclesToFinish.isEmpty()) {
        uint32_t number = cyclesToFinish.takeLast();
        if (m_cycles[number].state.load(std::memory_order_relaxed) == CycleState::Ready && beginSlowVersionsOfCycle(thread, number))
            continue;
        if (m_cycles[number].state.load(std::memory_order_relaxed) != CycleState::ReadyForClosedEntries && beginClosedEntriesOfCycle(thread, number))
            continue;
        m_cycles[number].state.store(CycleState::Done, std::memory_order_release);
        for (uint32_t named : cyclesNamedBy(number)) {
            if (m_cycles[named].numberOfCyclesAwaited.fetch_sub(1, std::memory_order_acq_rel) > 1)
                continue;
            bool hasOpenUnits = m_cycles[named].state.load(std::memory_order_relaxed) == CycleState::Ready ? m_cycles[named].numberOfOpenUnits.fetch_sub(1, std::memory_order_acq_rel) > 1 : beginCycle(thread, named);
            if (!hasOpenUnits)
                cyclesToFinish.append(named);
        }
    }
}

Vector<uint32_t> UnitQueue::inRunsOfUnitQueue(std::span<const uint32_t> lightestFirst)
{
    size_t numberOfUnits = lightestFirst.size();
    size_t endOfHeaviest = numberOfUnits;
    size_t endOfOthers = numberOfUnits - (numberOfUnits + mostUnitsTakenAtOnce - 1) / mostUnitsTakenAtOnce;
    Vector<uint32_t> inRuns(numberOfUnits);
    for (size_t place = numberOfUnits; place--;) {
        bool isNewestOfRun = !((numberOfUnits - 1 - place) % mostUnitsTakenAtOnce);
        inRuns[place] = lightestFirst[isNewestOfRun ? --endOfHeaviest : --endOfOthers];
    }
    return inRuns;
}

void UnitQueue::addSeeds(std::span<const Typed::CodeUnit> lightestFirst, const ParallelLoops& loops)
{
    RELEASE_ASSERT(loops.width() <= m_unitsOfThreads.size());
    ChunksOfIndices division = divideIntoChunks(static_cast<unsigned>(lightestFirst.size()));
    Vector<Vector<uint32_t>> queuedInChunks(division.size());
    m_isTakingSeeds = true;
    forEachIndexInParallel(loops, division.size(), [&](unsigned chunk, unsigned thread) {
        UnitsOfThread& unitsOfThread = m_unitsOfThreads[thread];
        RELEASE_ASSERT(!unitsOfThread.numberOfUnits.load(std::memory_order_relaxed));
        for (unsigned index = division.firstIndexOf[chunk]; index < division.firstIndexOf[chunk + 1]; ++index)
            mentionUnitNamedOutsideCode(*this, thread, lightestFirst[index]);
        Locker locker { unitsOfThread.lock };
        queuedInChunks[chunk].appendVector(unitsOfThread.units);
        unitsOfThread.units.shrink(0);
        unitsOfThread.numberOfUnits.store(0, std::memory_order_relaxed);
    });
    Vector<uint32_t> queued = joinChunks<uint32_t>(loops, WTF::move(queuedInChunks));
    m_isSeed = m_isMentioned.map([](const std::atomic<uint32_t>& word) {
        return word.load(std::memory_order_relaxed);
    });

    constexpr unsigned thisThread = 0;
    for (uint32_t cycle : std::exchange(m_cyclesReadyFromStart, { })) {
        if (m_cycles[cycle].numberOfOpenUnits.fetch_sub(1, std::memory_order_acq_rel) == 1)
            finishCycle(thisThread, cycle);
    }
    m_isTakingSeeds = false;
    Vector<uint32_t> sharedUnits;
    {
        UnitsOfThread& unitsOfThread = m_unitsOfThreads[thisThread];
        Locker locker { unitsOfThread.lock };
        sharedUnits = std::exchange(unitsOfThread.units, { });
        unitsOfThread.numberOfUnits.store(0, std::memory_order_relaxed);
    }
    sharedUnits.appendVector(inRunsOfUnitQueue(queued.span()));

    Locker locker { m_lock };
    RELEASE_ASSERT(m_sharedUnits.isEmpty());
    m_sharedUnits = WTF::move(sharedUnits);
    m_numberOfSharedUnits.store(m_sharedUnits.size(), std::memory_order_relaxed);
}

std::optional<uint32_t> UnitQueue::takeNewestUnitOf(UnitsOfThread& unitsOfThread)
{
    Locker locker { unitsOfThread.lock };
    if (unitsOfThread.units.isEmpty())
        return std::nullopt;
    uint32_t unit = unitsOfThread.units.takeLast();
    unitsOfThread.numberOfUnits.store(static_cast<uint32_t>(unitsOfThread.units.size()), std::memory_order_relaxed);
    return unit;
}

bool UnitQueue::takeRunOfSharedUnits(unsigned thread)
{
    if (!m_numberOfSharedUnits.load(std::memory_order_relaxed))
        return false;
    Vector<uint32_t, mostUnitsTakenAtOnce> run;
    {
        Locker locker { m_lock };
        size_t count = std::min((m_sharedUnits.size() + 1) / 2, mostUnitsTakenAtOnce);
        run.append(m_sharedUnits.span().last(count));
        m_sharedUnits.shrink(m_sharedUnits.size() - count);
        m_numberOfSharedUnits.store(m_sharedUnits.size(), std::memory_order_relaxed);
    }
    if (run.isEmpty())
        return false;
    UnitsOfThread& unitsOfThread = m_unitsOfThreads[thread];
    Locker locker { unitsOfThread.lock };
    unitsOfThread.units.appendVector(run);
    unitsOfThread.numberOfUnits.store(static_cast<uint32_t>(unitsOfThread.units.size()), std::memory_order_seq_cst);
    return true;
}

bool UnitQueue::takeOlderHalfOfAnotherThread(unsigned thread)
{
    unsigned numberOfThreads = static_cast<unsigned>(m_unitsOfThreads.size());
    Vector<uint32_t, mostUnitsTakenAtOnce> taken;
    for (unsigned step = 1; step < numberOfThreads && taken.isEmpty(); ++step) {
        UnitsOfThread& other = m_unitsOfThreads[(thread + step) % numberOfThreads];
        if (!other.numberOfUnits.load(std::memory_order_seq_cst) || !other.lock.tryLock())
            continue;
        Locker locker { AdoptLock, other.lock };
        size_t count = (other.units.size() + 1) / 2;
        taken.append(other.units.span().first(count));
        other.units.removeAt(0, count);
        other.numberOfUnits.store(static_cast<uint32_t>(other.units.size()), std::memory_order_relaxed);
    }
    if (taken.isEmpty())
        return false;
    UnitsOfThread& unitsOfThread = m_unitsOfThreads[thread];
    Locker locker { unitsOfThread.lock };
    unitsOfThread.units.appendVector(taken);
    unitsOfThread.numberOfUnits.store(static_cast<uint32_t>(unitsOfThread.units.size()), std::memory_order_seq_cst);
    return true;
}

bool UnitQueue::someThreadHoldsUnits() const
{
    if (m_numberOfSharedUnits.load(std::memory_order_seq_cst))
        return true;
    return std::ranges::any_of(m_unitsOfThreads, [](const UnitsOfThread& unitsOfThread) {
        return unitsOfThread.numberOfUnits.load(std::memory_order_seq_cst);
    });
}

void UnitQueue::wakeThreadUnlessOneIsLooking()
{
    if (m_numberOfThreadsLooking.load(std::memory_order_seq_cst) || !m_numberOfThreadsAsleep.load(std::memory_order_seq_cst))
        return;
    {
        Locker locker { m_lock };
    }
    m_didChange.notifyOne();
}

bool UnitQueue::waitForUnits(unsigned thread)
{
    m_numberOfThreadsLooking.fetch_add(1, std::memory_order_seq_cst);
    MonotonicTime endOfLook = MonotonicTime::now() + timeThreadOfUnitQueueLooksBeforeSleep;
    for (;;) {
        if (takeRunOfSharedUnits(thread) || takeOlderHalfOfAnotherThread(thread)) {
            m_numberOfThreadsLooking.fetch_sub(1, std::memory_order_seq_cst);
            if (m_unitsOfThreads[thread].numberOfUnits.load(std::memory_order_relaxed) > 1)
                wakeThreadUnlessOneIsLooking();
            return true;
        }
        if (!m_numberOfOpenUnits.load(std::memory_order_acquire)) {
            m_numberOfThreadsLooking.fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        if (MonotonicTime::now() < endOfLook) {
            for (unsigned pause = 0; pause < pausesBetweenLooksOfUnitQueue; ++pause)
                simde_mm_pause();
            continue;
        }

        Locker locker { m_lock };
        m_numberOfThreadsAsleep.fetch_add(1, std::memory_order_seq_cst);
        m_numberOfThreadsLooking.fetch_sub(1, std::memory_order_seq_cst);
        if (!someThreadHoldsUnits() && m_numberOfOpenUnits.load(std::memory_order_acquire))
            m_didChange.wait(m_lock);
        m_numberOfThreadsLooking.fetch_add(1, std::memory_order_seq_cst);
        m_numberOfThreadsAsleep.fetch_sub(1, std::memory_order_seq_cst);
        endOfLook = MonotonicTime::now() + timeThreadOfUnitQueueLooksBeforeSleep;
    }
}

std::optional<Typed::CodeUnit> UnitQueue::take(unsigned thread)
{
    UnitsOfThread& unitsOfThread = m_unitsOfThreads[thread];
    std::optional<uint32_t> taken = takeNewestUnitOf(unitsOfThread);
    while (!taken) {
        if (!waitForUnits(thread))
            return std::nullopt;
        taken = takeNewestUnitOf(unitsOfThread);
    }

    unitsOfThread.takenUnit = *taken;
    Typed::CodeUnit unit = m_conventions.unitAt(unitsOfThread.takenUnit);
    unitsOfThread.cycleOfTakenUnit = cycleOf(unit.function);
    unitsOfThread.takenUnitStandsOutsideCycles = standsOutsideCycles(unit);
    return unit;
}

void UnitQueue::didFinish(unsigned thread)
{
    uint32_t cycle = m_unitsOfThreads[thread].cycleOfTakenUnit;
    if (!m_unitsOfThreads[thread].takenUnitStandsOutsideCycles && m_cycles[cycle].numberOfOpenUnits.fetch_sub(1, std::memory_order_acq_rel) == 1)
        finishCycle(thread, cycle);

    uint64_t numberOfOpenUnits = m_numberOfOpenUnits.fetch_sub(1, std::memory_order_release);
    RELEASE_ASSERT(numberOfOpenUnits);
    if (numberOfOpenUnits > 1)
        return;

    // A waiting thread read a nonzero count while holding the lock and released the lock only after it was queued to sleep, so a notification sent under the lock reaches it.
    Locker locker { m_lock };
    m_didChange.notifyAll();
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
