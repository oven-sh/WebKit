/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionAnalyses.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTThreshold.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <utility>
#include <wtf/DataLog.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct AnalysisWordHasher {
    void add(uint64_t word)
    {
        hash = (hash ^ word) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 32;
    }
    void add(uint32_t first, uint32_t second) { add(static_cast<uint64_t>(first) << 32 | second); }

    uint64_t hash { 0x243f6a8885a308d3ull };
};

struct DeclaredRowScan {
    void reads(const MemoryLocation& location) { readHeaps.add(location.heap()); }
    void writes(const MemoryLocation& location) { writtenHeaps.add(location.heap()); }
    void readsAtPayload(AbstractHeap heap) { addAtPayload(readHeaps, heap); }
    void writesAtPayload(AbstractHeap heap) { addAtPayload(writtenHeaps, heap); }
    void readsAllHeaps() { readHeaps = allHeaps; }
    void throws() { }
    void collects() { }
    void suspends() { }
    void terminates() { }
    void unknownCode() { writtenHeaps = allHeaps; }

    void addAtPayload(AbstractHeapSet& heaps, AbstractHeap heap)
    {
        hasEffectAtPayload = true;
        if (heap != AbstractHeap::ShapeWords)
            heaps.add(heap);
    }

    AbstractHeapSet readHeaps;
    AbstractHeapSet writtenHeaps;
    bool hasEffectAtPayload { false };
};

} // anonymous namespace

template<typename Entry>
static DeclaredRowScan scanDeclaredRow(Entry entry)
{
    DeclaredRowScan scan;
    forEachDeclaredEffect(entry, scan);
    return scan;
}

static EffectInputs inputsOfRule(Opcode opcode)
{
    if (familyOf(opcode) == OperationFamily::RuntimeValidation)
        return EffectInputs::Function;
    if (effectRuleOf(opcode) == EffectRule::Access)
        return EffectInputs::DefinitionsBehindCasts;
    return EffectInputs::Operands;
}

static OpcodeEffectFacts computeEffectFactsOf(Opcode opcode)
{
    OpcodeEffectFacts facts { .inputs = inputsOfRule(opcode) };
    if (effectRuleOf(opcode) != EffectRule::Declared)
        return facts;

    Effects bound = declaredBoundOf(opcode);
    bool runsNoOperandCode = !mayRunOperandCode(opcode);
    facts.staysWithinDeclaredBound = runsNoOperandCode && !bound.mayCallUnknownCode;
    if (facts.staysWithinDeclaredBound)
        facts.traits.atMost = EffectTraits(bound);
    if (isRefinedByValue(opcode, bound))
        return facts;

    DeclaredRowScan scan = scanDeclaredRow(opcode);
    if (!bound.mayCallUnknownCode) {
        Effects least = bound;
        least.reads = LocationMasks(scan.readHeaps);
        least.writes = LocationMasks(scan.writtenHeaps);
        facts.traits.atLeast = EffectTraits(least);
    }
    if (!bound.mayCallUnknownCode || bound.maySuspend) {
        facts.mayExitSideways = bound.mayExitSideways();
        facts.writtenHeaps = scan.writtenHeaps;
    }
    if (runsNoOperandCode)
        facts.inputs = scan.hasEffectAtPayload ? EffectInputs::Payload : EffectInputs::Row;
    return facts;
}

const OpcodeEffectFacts& effectFactsOf(Opcode opcode)
{
    static const std::array<OpcodeEffectFacts, numberOfOpcodes> factsOfOpcodes = [] {
        std::array<OpcodeEffectFacts, numberOfOpcodes> result { };
        for (Opcode member : allMembersOf<Opcode>)
            result[static_cast<unsigned>(member)] = computeEffectFactsOf(member);
        return result;
    }();
    return factsOfOpcodes[static_cast<unsigned>(opcode)];
}

static EffectInputs effectInputsOf(TerminatorKind kind)
{
    static const std::array<EffectInputs, numberOfTerminatorKinds> inputsOfKinds = [] {
        std::array<EffectInputs, numberOfTerminatorKinds> result { };
        for (TerminatorKind member : allMembersOf<TerminatorKind>) {
            bool isDescribedByRow = effectRuleOf(member) != EffectRule::Inline && !mayRunOperandCode(member) && !scanDeclaredRow(member).hasEffectAtPayload;
            result[static_cast<unsigned>(member)] = isDescribedByRow ? EffectInputs::Row : EffectInputs::Operands;
        }
        return result;
    }();
    return inputsOfKinds[static_cast<unsigned>(kind)];
}

static EffectInputs effectInputsOf(const Value& value) { return effectFactsOf(value.opcode).inputs; }
static EffectInputs effectInputsOf(const Terminator& terminator) { return effectInputsOf(terminator.kind); }

static void addPayload(AnalysisWordHasher& hasher, const Instruction& instruction)
{
    static_assert(numberOfInlinePayloadWords == 3);
    std::array<uint32_t, numberOfInlinePayloadWords> inlinePayload = instruction.inlinePayload();
    hasher.add(instruction.flags, inlinePayload[0]);
    hasher.add(inlinePayload[1], inlinePayload[2]);
    std::span<const uint32_t> outOfLinePayload = instruction.outOfLinePayload();
    hasher.add(outOfLinePayload.size());
    for (uint32_t word : outOfLinePayload)
        hasher.add(word);
}

static bool addDefinitionBehindCasts(AnalysisWordHasher& hasher, const Function& function, ValueID id)
{
    for (size_t remainingSteps = function.values.size(); remainingSteps; --remainingSteps) {
        const Value& definition = function.at(id);
        if (!isCastOrConversion(definition.opcode)) {
            hasher.add(static_cast<uint32_t>(definition.opcode));
            addPayload(hasher, definition);
            return true;
        }
        id = definition.operand(0);
        if (!id)
            return false;
    }
    return false;
}

static uint32_t firstPositionOf(const Instruction& instruction, uint32_t index)
{
    ValueID operand = instruction.operand(index);
    for (uint32_t earlier = 0; earlier < index; ++earlier) {
        if (instruction.operand(earlier) == operand)
            return earlier;
    }
    return index;
}

static bool addOperands(AnalysisWordHasher& hasher, const Function& function, const Instruction& instruction, EffectInputs inputs)
{
    AOT_THRESHOLD(uint32_t, maxComparedOperands, 16);

    uint32_t numberOfOperands = instruction.numberOfOperands();
    bool outlivesRenumbering = numberOfOperands <= maxComparedOperands;
    hasher.add(numberOfOperands);
    for (uint32_t index = 0; index < numberOfOperands; ++index) {
        ValueID operand = instruction.operand(index);
        hasher.add(outlivesRenumbering ? firstPositionOf(instruction, index) : operand.index(), !!operand);
        if (!operand)
            continue;
        const Value& definition = function.at(operand);
        hasher.add(definition.type.index(), definition.expected().index());
        if (inputs == EffectInputs::DefinitionsBehindCasts && !addDefinitionBehindCasts(hasher, function, operand))
            return false;
    }
    return true;
}

static uint64_t finishStamp(AnalysisWordHasher& hasher, const Function& function, const Instruction& instruction, EffectInputs inputs)
{
    switch (inputs) {
    case EffectInputs::Row:
    case EffectInputs::Function:
        return 0;
    case EffectInputs::Payload:
    case EffectInputs::Operands:
    case EffectInputs::DefinitionsBehindCasts:
        break;
    }
    addPayload(hasher, instruction);
    if (inputs != EffectInputs::Payload && !addOperands(hasher, function, instruction, inputs))
        return 0;
    return hasher.hash && hasher.hash < std::numeric_limits<uint64_t>::max() - 1 ? hasher.hash : 1;
}

static uint64_t stampOf(const Function& function, const Value& value, EffectInputs inputs)
{
    AnalysisWordHasher hasher;
    hasher.add(static_cast<uint32_t>(value.opcode));
    if (inputs != EffectInputs::Payload)
        hasher.add(value.type.index(), value.expected().index());
    return finishStamp(hasher, function, value, inputs);
}

static uint64_t stampOf(const Function& function, const Terminator& terminator, EffectInputs inputs)
{
    AnalysisWordHasher hasher;
    hasher.add(numberOfOpcodes + static_cast<uint32_t>(terminator.kind));
    return finishStamp(hasher, function, terminator, inputs);
}

static uint32_t hashOf(const EffectsOnEdges& effectsOnEdges)
{
    AnalysisWordHasher hasher;
    for (const Effects* effects : { &effectsOnEdges.normal, &effectsOnEdges.handler }) {
        hasher.add(effects->reads.mixedWords());
        hasher.add(effects->writes.mixedWords());
        hasher.add(WTF::packBools(static_cast<bool>(effects->mayThrow), effects->mayThrowByRule(), effects->mayCollect, effects->mayCallUnknownCode, effects->maySuspend, effects->mayBeTerminated));
    }
    return static_cast<uint32_t>(hasher.hash);
}

EffectsCache::EffectsCache(const EffectContext& context, const Function& function, bool verifiesAnswers)
    : m_context(context)
    , m_function(function)
    , m_verifiesAnswers(verifiesAnswers)
{
}

void EffectsCache::resizeToFunction()
{
    m_values.resize(m_function.values.size());
    m_terminators.resize(m_function.blocks.size());
    m_stampsOfUsers.resize(m_function.values.size());
}

template<typename Carrier>
EffectsCache::EntryID EffectsCache::compute(const Carrier& carrier)
{
    ++m_numberOfComputedAnswers;
    EffectsOnEdges effects = Typed::effectsOf(m_context, m_function, carrier);
    uint32_t hash = hashOf(effects);
    EffectTraitsOnEdges traits(effects);
    EntryID entry = m_entries.findOrAdd(WTF::move(effects), hash);
    if (entry.index() == m_traitsOfEntries.size())
        m_traitsOfEntries.append(traits);
    return entry;
}

template<typename Carrier>
void EffectsCache::answerAgain(Answer& answer, EntryID& entryOfRow, const Carrier& carrier)
{
    answer.round = m_round;
    EffectInputs inputs = effectInputsOf(carrier);
    if (inputs == EffectInputs::Row) {
        if (!entryOfRow)
            entryOfRow = compute(carrier);
        answer.stamp = 0;
        answer.entry = entryOfRow;
        return;
    }

    uint64_t stamp = stampOf(m_function, carrier, inputs);
    if (stamp && stamp == answer.stamp) {
        ++m_numberOfKeptAnswers;
        return;
    }
    answer.stamp = stamp;
    if (!stamp) {
        answer.entry = compute(carrier);
        return;
    }

    if (auto kept = m_entryOfStamp.find(stamp); kept != m_entryOfStamp.end()) {
        ++m_numberOfKeptAnswers;
        answer.entry = kept->value;
        return;
    }
    answer.entry = compute(carrier);
    m_entryOfStamp.add(stamp, answer.entry);
}

EffectsCache::EntryID EffectsCache::entryOf(ValueID id)
{
    RELEASE_ASSERT(id.index() < m_values.size());
    Answer& answer = m_values[id.index()];
    if (answer.round != m_round) {
        const Value& value = m_function.at(id);
        answerAgain(answer, m_entryOfOpcode[static_cast<unsigned>(value.opcode)], value);
    } else if (m_verifiesAnswers) [[unlikely]]
        RELEASE_ASSERT(m_entries.at(answer.entry) == Typed::effectsOf(m_context, m_function, m_function.at(id)));
    return answer.entry;
}

EffectsCache::EntryID EffectsCache::entryOfTerminator(BlockID block)
{
    RELEASE_ASSERT(block.index() < m_terminators.size());
    Answer& answer = m_terminators[block.index()];
    if (answer.round != m_round) {
        const Terminator& terminator = m_function.at(block).terminator;
        answerAgain(answer, m_entryOfTerminatorKind[static_cast<unsigned>(terminator.kind)], terminator);
    } else if (m_verifiesAnswers) [[unlikely]]
        RELEASE_ASSERT(m_entries.at(answer.entry) == Typed::effectsOf(m_context, m_function, m_function.at(block).terminator));
    return answer.entry;
}

const EffectsOnEdges& EffectsCache::of(ValueID id) { return m_entries.at(entryOf(id)); }
const EffectsOnEdges& EffectsCache::ofTerminator(BlockID block) { return m_entries.at(entryOfTerminator(block)); }
EffectTraitsOnEdges EffectsCache::traitsOf(ValueID id) { return m_traitsOfEntries[entryOf(id).index()]; }
EffectTraitsOnEdges EffectsCache::traitsOfTerminator(BlockID block) { return m_traitsOfEntries[entryOfTerminator(block).index()]; }

uint64_t EffectsCache::stampInThisRoundOf(ValueID id, const Value& value)
{
    RELEASE_ASSERT(id.index() < m_values.size() && id.index() < m_stampsOfUsers.size());
    if (const Answer& answer = m_values[id.index()]; answer.round == m_round)
        return answer.stamp;
    StampInRound& kept = m_stampsOfUsers[id.index()];
    if (kept.round != m_round) {
        kept.round = m_round;
        kept.stamp = stampOf(m_function, value, effectInputsOf(value));
    }
    return kept.stamp;
}

OperandSummary EffectsCache::summaryOfOperand(ValueID id, uint32_t operandIndex)
{
    const Value& user = m_function.at(id);
    switch (effectRuleOf(user.opcode)) {
    case EffectRule::Declared:
    case EffectRule::Operator:
        return summarizeOperand(m_context, m_function, user, operandIndex);
    case EffectRule::Access:
    case EffectRule::Call:
    case EffectRule::Builtin:
    case EffectRule::Inline:
        break;
    }

    uint64_t stamp = stampInThisRoundOf(id, user);
    if (!stamp)
        return summarizeOperand(m_context, m_function, user, operandIndex);
    uint64_t key = stamp ^ (0x9e3779b97f4a7c15ull * (operandIndex + 1));
    if (key >= std::numeric_limits<uint64_t>::max() - 1)
        key = 1;
    return m_operandSummaryOfStamp.ensure(key, [&] {
        return summarizeOperand(m_context, m_function, user, operandIndex);
    }).iterator->value;
}

template<typename Carrier>
const EffectsOnEdges* EffectsCache::findKept(const Vector<Answer>& answers, size_t index, EntryID entryOfRow, const Carrier& carrier) const
{
    EffectInputs inputs = effectInputsOf(carrier);
    if (inputs == EffectInputs::Row)
        return entryOfRow ? &m_entries.at(entryOfRow) : nullptr;
    if (index < answers.size() && answers[index].round == m_round)
        return &m_entries.at(answers[index].entry);

    uint64_t stamp = stampOf(m_function, carrier, inputs);
    if (!stamp)
        return nullptr;
    auto kept = m_entryOfStamp.find(stamp);
    return kept != m_entryOfStamp.end() ? &m_entries.at(kept->value) : nullptr;
}

const EffectsOnEdges* EffectsCache::findKept(ValueID id) const
{
    const Value& value = m_function.at(id);
    return findKept(m_values, id.index(), m_entryOfOpcode[static_cast<unsigned>(value.opcode)], value);
}

const EffectsOnEdges* EffectsCache::findKeptOfTerminator(BlockID block) const
{
    const Terminator& terminator = m_function.at(block).terminator;
    return findKept(m_terminators, block.index(), m_entryOfTerminatorKind[static_cast<unsigned>(terminator.kind)], terminator);
}

static AnalysisSet addDependentAnalyses(AnalysisSet set)
{
    if (set.contains(FunctionAnalysis::ControlFlow))
        set.add(allAnalyses);
    if (set.contains(FunctionAnalysis::Dominators))
        set.add({ FunctionAnalysis::NaturalLoops, FunctionAnalysis::Guards, FunctionAnalysis::FailedTests, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::NaturalLoops))
        set.add(FunctionAnalysis::Induction);
    if (set.contains(FunctionAnalysis::Cycles))
        set.add({ FunctionAnalysis::Escape, FunctionAnalysis::Frequencies });
    if (set.contains(FunctionAnalysis::UseLists))
        set.add({ FunctionAnalysis::Liveness, FunctionAnalysis::Escape, FunctionAnalysis::Guards, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::Identities))
        set.add({ FunctionAnalysis::Guards, FunctionAnalysis::FailedTests, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::Effects))
        set.add({ FunctionAnalysis::Escape, FunctionAnalysis::EscapeHistory, FunctionAnalysis::Guards, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::Escape))
        set.add({ FunctionAnalysis::EscapeHistory, FunctionAnalysis::Guards });
    if (set.containsAny({ FunctionAnalysis::EscapeHistory, FunctionAnalysis::Frequencies }))
        set.add(FunctionAnalysis::Guards);
    return set;
}

FunctionAnalyses::FunctionAnalyses(const AnalysisContext& context, const Function& function)
    : m_context(context)
    , m_function(function)
    , m_effectContext { context.program, context.effects ? *context.effects : m_unknownSummaries.emplace() }
    , m_effects(m_effectContext, function, context.build.options().verifiesAfterEveryPass)
    , m_countsBuilds(context.build.options().logging.logsPhaseTimes)
    , m_verifiesUpdates(context.build.options().verifiesAfterEveryPass)
{
}

FunctionAnalyses::FunctionAnalyses(const AnalysisContext& context, Function& function)
    : FunctionAnalyses(context, std::as_const(function))
{
    m_changes = function.changeLog();
    if (m_changes)
        return;
    constexpr size_t entriesForEachValue = 64;
    AOT_THRESHOLD(size_t, fewestEntries, 256);
    m_ownChanges.setMaxNumberOfEntries(fewestEntries + entriesForEachValue * function.values.size());
    m_changes = &m_ownChanges;
    m_functionWithOwnChanges = &function;
    function.attach(m_ownChanges);
}

ChangeLog& changeLogOf(FunctionAnalyses& analyses) { return analyses.changes(); }

FunctionAnalyses::~FunctionAnalyses()
{
    if (m_functionWithOwnChanges)
        m_functionWithOwnChanges->detach(m_ownChanges);
    Statistics& statistics = m_context.build.statistics();
    if (uint64_t numberOfComputedAnswers = m_effects.numberOfComputedAnswers())
        statistics.count(StatisticID::EffectsComputed, numberOfComputedAnswers);
    if (uint64_t numberOfKeptAnswers = m_effects.numberOfKeptAnswers())
        statistics.count(StatisticID::EffectsKept, numberOfKeptAnswers);
}

Subject FunctionAnalyses::subject(ValueID value) const
{
    if (!m_context.id)
        return { };

    FunctionID known = m_context.id;
    if (ProvisionalFunctions::contains(known))
        known = m_function.derivation.source;
    while (ProvisionalFunctions::contains(known))
        known = m_context.program.at(known).derivation.source;

    Subject result { .function = findFunctionAsRead(&m_context.program, known), .location = m_function.declarationLocation() };
    if (!value)
        return result;

    SourceLocation location = m_function.locationOf(value);
    if (hasModule(location))
        result.location = location;
    if (result.function == m_context.id)
        result.value = value;
    return result;
}

static constexpr unsigned indexOf(FunctionAnalysis analysis) { return std::countr_zero(static_cast<unsigned>(analysis)); }

static_assert(indexOf(FunctionAnalysis::Induction) + 1 == numberOfFunctionAnalyses);

using CountsByPass = std::array<std::atomic<uint64_t>, numberOfPasses>;

static constinit std::array<CountsByPass, numberOfFunctionAnalyses> buildsByAskingPass { };
static constinit std::array<CountsByPass, numberOfFunctionAnalyses> buildsByDroppingPass { };
static constinit std::array<std::atomic<uint64_t>, numberOfFunctionAnalyses> valuesOfAnalyzedFunctions { };
static constinit std::array<std::atomic<uint64_t>, numberOfFunctionAnalyses> blocksOfAnalyzedFunctions { };
static constinit std::atomic<uint64_t> numberOfKeptGraphs { 0 };
static constinit std::atomic<uint64_t> numberOfFormerGraphs { 0 };
static constinit std::atomic<uint64_t> numberOfKeptFrequencies { 0 };
static constinit std::atomic<uint64_t> numberOfFormerFrequencies { 0 };
static constinit CountsByPass useListUpdatesByChangingPass { };
static constinit std::atomic<uint64_t> valuesOfFunctionsWithUpdatedUseLists { 0 };
static constinit std::atomic<uint64_t> valuesReadByUseListUpdates { 0 };

void FunctionAnalyses::didBuild(FunctionAnalysis analysis)
{
    if (!m_countsBuilds) [[likely]]
        return;
    unsigned index = indexOf(analysis);
    buildsByAskingPass[index][static_cast<unsigned>(m_runningPass)].fetch_add(1, std::memory_order_relaxed);
    buildsByDroppingPass[index][static_cast<unsigned>(m_passesThatDropped[index])].fetch_add(1, std::memory_order_relaxed);
    valuesOfAnalyzedFunctions[index].fetch_add(m_function.values.size(), std::memory_order_relaxed);
    blocksOfAnalyzedFunctions[index].fetch_add(m_function.blocks.size(), std::memory_order_relaxed);
}

void FunctionAnalyses::didBringUseListsUpToDate(const UseLists::Update& update)
{
    if (update.hasMadeAnew) {
        didBuild(FunctionAnalysis::UseLists);
        return;
    }
    if (m_verifiesUpdates) [[unlikely]]
        verifyUseLists();
    if (!m_countsBuilds) [[likely]]
        return;
    useListUpdatesByChangingPass[static_cast<unsigned>(m_passesThatDropped[indexOf(FunctionAnalysis::UseLists)])].fetch_add(1, std::memory_order_relaxed);
    valuesOfFunctionsWithUpdatedUseLists.fetch_add(m_function.values.size(), std::memory_order_relaxed);
    valuesReadByUseListUpdates.fetch_add(update.numberOfValuesRead, std::memory_order_relaxed);
}

void FunctionAnalyses::willDrop(AnalysisSet set)
{
    if (!m_countsBuilds) [[likely]]
        return;
    for (FunctionAnalysis analysis : set & (computed() | heldControlFlowAnalyses()))
        m_passesThatDropped[indexOf(analysis)] = m_runningPass;
}

ASCIILiteral sectionNameOf(FunctionAnalysis analysis)
{
    switch (analysis) {
    case FunctionAnalysis::ControlFlow:
        return "control-flow"_s;
    case FunctionAnalysis::Dominators:
        return "dominators"_s;
    case FunctionAnalysis::NaturalLoops:
        return "natural-loops"_s;
    case FunctionAnalysis::Cycles:
        return "cycles"_s;
    case FunctionAnalysis::UseLists:
        return "use-lists"_s;
    case FunctionAnalysis::Identities:
        return "identities"_s;
    case FunctionAnalysis::Liveness:
        return "liveness"_s;
    case FunctionAnalysis::Effects:
        return "effects"_s;
    case FunctionAnalysis::Escape:
        return "escape"_s;
    case FunctionAnalysis::EscapeHistory:
        return "escape-history"_s;
    case FunctionAnalysis::Frequencies:
        return "frequencies"_s;
    case FunctionAnalysis::Guards:
        return "guards"_s;
    case FunctionAnalysis::FailedTests:
        return "failed-tests"_s;
    case FunctionAnalysis::Induction:
        return "induction"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t printAndResetCounts(PrintStream& out, CountsByPass& counts)
{
    uint64_t total = 0;
    for (unsigned pass = 0; pass < numberOfPasses; ++pass) {
        uint64_t count = counts[pass].exchange(0, std::memory_order_relaxed);
        if (!count)
            continue;
        total += count;
        out.print(" ", nameOf(static_cast<PassID>(pass)), " ", count);
    }
    return total;
}

void FunctionAnalyses::logBuilds()
{
    for (FunctionAnalysis analysis : allAnalyses) {
        StringPrintStream askingPasses;
        StringPrintStream droppingPasses;
        uint64_t total = printAndResetCounts(askingPasses, buildsByAskingPass[indexOf(analysis)]);
        printAndResetCounts(droppingPasses, buildsByDroppingPass[indexOf(analysis)]);
        uint64_t values = valuesOfAnalyzedFunctions[indexOf(analysis)].exchange(0, std::memory_order_relaxed);
        uint64_t blocks = blocksOfAnalyzedFunctions[indexOf(analysis)].exchange(0, std::memory_order_relaxed);
        if (total)
            dataLogLn("AOT analyses: ", sectionNameOf(analysis), " | built ", total, " | asked for by", askingPasses.toString(), " | dropped by", droppingPasses.toString(), " | over ", values, " values in ", blocks, " blocks");
    }
    uint64_t kept = numberOfKeptGraphs.exchange(0, std::memory_order_relaxed);
    uint64_t former = numberOfFormerGraphs.exchange(0, std::memory_order_relaxed);
    if (kept || former)
        dataLogLn("AOT analyses: graphs compared with the function | the same ", kept, " | changed ", former);
    uint64_t keptFrequencies = numberOfKeptFrequencies.exchange(0, std::memory_order_relaxed);
    uint64_t formerFrequencies = numberOfFormerFrequencies.exchange(0, std::memory_order_relaxed);
    if (keptFrequencies || formerFrequencies)
        dataLogLn("AOT analyses: frequencies compared with the function | the same ", keptFrequencies, " | changed ", formerFrequencies);
    StringPrintStream changingPasses;
    uint64_t updates = printAndResetCounts(changingPasses, useListUpdatesByChangingPass);
    uint64_t valuesHeld = valuesOfFunctionsWithUpdatedUseLists.exchange(0, std::memory_order_relaxed);
    uint64_t valuesRead = valuesReadByUseListUpdates.exchange(0, std::memory_order_relaxed);
    if (updates)
        dataLogLn("AOT analyses: use-lists | brought up to date ", updates, " | behind", changingPasses.toString(), " | in functions of ", valuesHeld, " values | ", valuesRead, " of them read again");
}

void FunctionAnalyses::dropResultsOfFormerGraph()
{
    if (!m_controlFlow.mayDescribeFormerGraph) [[likely]]
        return;
    m_controlFlow.mayDescribeFormerGraph = false;
    bool isSameGraph = m_controlFlow.cfg->describes(m_function);
    if (m_countsBuilds) [[unlikely]]
        (isSameGraph ? numberOfKeptGraphs : numberOfFormerGraphs).fetch_add(1, std::memory_order_relaxed);
    if (isSameGraph)
        return;
    dropFrequencies();
    m_controlFlow.naturalLoops.reset();
    m_controlFlow.dominators.reset();
    m_controlFlow.blockCycles.reset();
    m_controlFlow.cfg.reset();
}

void FunctionAnalyses::dropFrequencies()
{
    m_values.blockFrequencies.reset();
    m_values.frequenciesMayDescribeFormerFunction = false;
}

void FunctionAnalyses::dropFrequenciesOfFormerFunction()
{
    if (!m_values.frequenciesMayDescribeFormerFunction) [[likely]]
        return;
    dropResultsOfFormerGraph();
    if (!m_values.frequenciesMayDescribeFormerFunction)
        return;
    willComputeValueAnalysis();
    m_values.frequenciesMayDescribeFormerFunction = false;
    bool isSameEstimate = m_values.blockFrequencies->describes(m_context, m_function);
    if (m_countsBuilds) [[unlikely]]
        (isSameEstimate ? numberOfKeptFrequencies : numberOfFormerFrequencies).fetch_add(1, std::memory_order_relaxed);
    if (!isSameEstimate)
        m_values.blockFrequencies.reset();
    else if (m_verifiesUpdates) [[unlikely]]
        verifyFrequencies();
}

CFG& FunctionAnalyses::cfg()
{
    if (m_controlFlow.cfg && !m_controlFlow.mayDescribeFormerGraph) [[likely]] {
        checkBlockCount();
        return *m_controlFlow.cfg;
    }
    dropResultsOfFormerGraph();
    if (!m_controlFlow.cfg) {
        m_controlFlow.cfg.emplace(m_function);
        m_controlFlow.numberOfBlocks = m_function.blocks.size();
        didBuild(FunctionAnalysis::ControlFlow);
    }
    checkBlockCount();
    return *m_controlFlow.cfg;
}

Dominators& FunctionAnalyses::dominators()
{
    dropResultsOfFormerGraph();
    if (!m_controlFlow.dominators) {
        m_controlFlow.dominators.emplace(cfg(), false);
        didBuild(FunctionAnalysis::Dominators);
    }
    checkBlockCount();
    return *m_controlFlow.dominators;
}

NaturalLoops& FunctionAnalyses::naturalLoops()
{
    dropResultsOfFormerGraph();
    if (!m_controlFlow.naturalLoops) {
        m_controlFlow.naturalLoops.emplace(cfg(), dominators(), false);
        didBuild(FunctionAnalysis::NaturalLoops);
    }
    checkBlockCount();
    return *m_controlFlow.naturalLoops;
}

const BlockCycles& FunctionAnalyses::blockCycles()
{
    dropResultsOfFormerGraph();
    if (!m_controlFlow.blockCycles) {
        m_controlFlow.blockCycles.emplace(cfg());
        didBuild(FunctionAnalysis::Cycles);
    }
    checkBlockCount();
    return *m_controlFlow.blockCycles;
}

void FunctionAnalyses::willComputeValueAnalysis()
{
    if (!computed().containsAny(valueAnalyses))
        m_values.numberOfValues = m_function.values.size();
    m_values.mayHoldResults = true;
}

template<typename Result>
const Result& FunctionAnalyses::valueResult(std::optional<Result>& result, const Invocable<void()> auto& make)
{
    if (!result) {
        willComputeValueAnalysis();
        make();
    }
    checkBlockCount();
    checkValueCount();
    return *result;
}

const UseLists& FunctionAnalyses::useLists()
{
    if (m_values.useLists && m_values.useLists->isUpToDate() && !m_values.didDropBesideUseLists) [[likely]] {
        checkBlockCount();
        checkValueCount();
        return *m_values.useLists;
    }
    if (!m_values.useLists) {
        willComputeValueAnalysis();
        m_values.useLists.emplace(m_function, cfg());
        m_values.didDropBesideUseLists = false;
        didBuild(FunctionAnalysis::UseLists);
    } else if (!m_values.useLists->isUpToDate()) {
        willComputeValueAnalysis();
        UseLists::Update update = m_values.useLists->bringUpToDate(cfg());
        m_values.didDropBesideUseLists = false;
        didBringUseListsUpToDate(update);
    } else if (m_values.didDropBesideUseLists) [[unlikely]] {
        m_values.didDropBesideUseLists = false;
        if (m_verifiesUpdates)
            verifyUseLists();
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.useLists;
}

static constexpr bool reportsEveryChangeToUseListInputs(PassID pass) { return pass == PassID::EliminateDeadCode || pass == PassID::CompactBlocks; }

UseLists* FunctionAnalyses::updatingUseLists()
{
    if (!m_values.useLists || m_values.useLists->mustBeMadeAnew() || !(m_everyChangeIsReported || reportsEveryChangeToUseListInputs(m_runningPass)))
        return nullptr;
    if (m_countsBuilds) [[unlikely]]
        m_passesThatDropped[indexOf(FunctionAnalysis::UseLists)] = m_runningPass;
    return &*m_values.useLists;
}

static constexpr bool asksForIdentitiesOnlyBeforeEditing(PassID pass) { return pass == PassID::SimplifyControlFlow || pass == PassID::RemoveTrivialParameters; }

static constexpr EnumeratorBitSet<PassID, numberOfPasses> passesThatMayCompleteIdentitiesLate {
    PassID::FoldConstants, PassID::PropagateBranchConditions, PassID::HoistTestsIntoPredecessors, PassID::ThreadJumps, PassID::EliminateDeadCode,
    PassID::EliminateRedundantReads, PassID::RetypeValues, PassID::CompactBlocks,
};

void FunctionAnalyses::completeIdentitiesBeforePass()
{
    if (asksForIdentitiesOnlyBeforeEditing(m_runningPass) || passesThatMayCompleteIdentitiesLate.contains(m_runningPass))
        return;
    m_values.identities->setGraph(&cfg());
    m_values.identities->complete();
}

const ValueIdentities& FunctionAnalyses::identities()
{
    auto evaluation = asksForIdentitiesOnlyBeforeEditing(m_runningPass) ? ValueIdentities::Evaluation::OnDemand : ValueIdentities::Evaluation::Complete;
    if (!m_values.identities) {
        willComputeValueAnalysis();
        m_values.identities.emplace(m_function, cfg(), ValueIdentities::RepeatedWalks::StartAtFirstChangedBlock, &opcodesOfValues(), evaluation);
        m_values.didUpdateIdentities = false;
        didBuild(FunctionAnalysis::Identities);
        if (m_verifiesUpdates && m_values.identities->isComplete()) [[unlikely]]
            verifyIdentities(cfg());
    } else {
        if (!m_values.identities->isComplete()) {
            m_values.identities->setGraph(&cfg());
            if (evaluation == ValueIdentities::Evaluation::Complete)
                m_values.identities->complete();
        }
        if (std::exchange(m_values.didUpdateIdentities, false) && m_verifiesUpdates) [[unlikely]]
            verifyIdentities(cfg());
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.identities;
}

ValueID FunctionAnalyses::identityOf(ValueID id)
{
    if (!m_values.identities || !m_values.identities->isComplete() || m_values.didUpdateIdentities) [[unlikely]]
        return identities().identityOf(id);
    checkBlockCount();
    checkValueCount();
    return m_values.identities->identityOf(id);
}

const Liveness& FunctionAnalyses::liveness()
{
    return valueResult(m_values.liveness, [&] {
        LivenessOptions options;
        options.opcodesOfValues = &opcodesOfValues();
        m_values.liveness.emplace(m_function, cfg(), useLists(), options);
        didBuild(FunctionAnalysis::Liveness);
        if (m_verifiesUpdates) [[unlikely]]
            verifyLiveness();
    });
}

EffectsCache& FunctionAnalyses::effectsCache()
{
    if (!m_values.hasEffects) {
        willComputeValueAnalysis();
        m_effects.resizeToFunction();
        m_values.hasEffects = true;
    }
    RELEASE_ASSERT(m_effects.numberOfBlocks() == m_function.blocks.size());
    checkValueCount();
    return m_effects;
}

const EscapeAnalysis& FunctionAnalyses::escape()
{
    return valueResult(m_values.escape, [&] {
        cfg();
        m_values.escape.emplace(*this);
    });
}

EscapeAnalysisInputs FunctionAnalyses::inputsOfEscapeAnalysis()
{
    return { m_context, m_effectContext, effectsCache(), m_function, cfg(), useLists(), subject(), m_runningPass != PassID::None };
}

const EscapeAnalysis& FunctionAnalyses::escapeWithLiveInstances()
{
    escape();
    if (!m_values.escape->hasLiveInstances()) {
        m_values.escape->computeLiveInstances(cfg(), useLists(), [&]() -> const BlockCycles& {
            return blockCycles();
        });
    }
    return *m_values.escape;
}

const UseEffectHistory& FunctionAnalyses::escapeHistory()
{
    return valueResult(m_values.escapeHistory, [&] {
        escapeWithLiveInstances();
        m_values.escapeHistory.emplace(*this);
        didBuild(FunctionAnalysis::EscapeHistory);
    });
}

const BlockFrequencies& FunctionAnalyses::blockFrequencies()
{
    dropFrequenciesOfFormerFunction();
    return valueResult(m_values.blockFrequencies, [&] {
        m_values.blockFrequencies.emplace(m_context, m_function, cfg(), blockCycles());
        didBuild(FunctionAnalysis::Frequencies);
    });
}

const GuardAvailability& FunctionAnalyses::guards()
{
    return valueResult(m_values.guards, [&] {
        cfg();
        useLists();
        identities();
        m_values.guards.emplace(*this);
        didBuild(FunctionAnalysis::Guards);
    });
}

const OpcodeSet& FunctionAnalyses::opcodesOfValues()
{
    if (!m_values.opcodes || m_values.numberOfValuesWithCollectedOpcodes != m_function.values.size()) {
        m_values.numberOfValuesWithCollectedOpcodes = m_function.values.size();
        m_values.mayHoldResults = true;
        OpcodeSet& opcodes = m_values.opcodes.emplace();
        for (const Value& value : m_function.values)
            opcodes.add(value.opcode);
    }
    return *m_values.opcodes;
}

const FailedTests& FunctionAnalyses::failedTests()
{
    return valueResult(m_values.failedTests, [&] {
        m_values.failedTests.emplace(*this);
        didBuild(FunctionAnalysis::FailedTests);
    });
}

const InductionVariables& FunctionAnalyses::inductionVariables()
{
    return valueResult(m_values.inductionVariables, [&] {
        m_values.inductionVariables.emplace(*this);
        didBuild(FunctionAnalysis::Induction);
    });
}

GuardState FunctionAnalyses::guardStateAt(ValueID guard, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand: {
        const Value& user = m_function.at(use.user);
        if (user.operandKind(use.operandIndex) == OperandKind::Guard)
            return guards().stateAt(guard, user, useLists().pointOf(use));
        return guards().stateAt(guard, useLists().pointOf(use));
    }
    case Use::Kind::TerminatorOperand:
        return guards().stateAt(guard, useLists().pointOf(use));
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return GuardState::Available;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ExecutionCount FunctionAnalyses::executionCountOf(BlockID block)
{
    if (!m_context.flow || !m_context.id || !m_context.flow->runsAtMostOnce(m_context.program, m_context.id))
        return ExecutionCount::Unknown;
    if (!cfg().isReachable(block) || isInCycle(block))
        return ExecutionCount::Unknown;
    return ExecutionCount::AtMostOnce;
}

bool FunctionAnalyses::dominates(ValueID definition, ProgramPoint point)
{
    ValueLocation location = useLists().locationOf(definition);
    if (!cfg().isReachable(point.block))
        return false;

    switch (location.placement) {
    case ValuePlacement::Detached:
        return false;
    case ValuePlacement::Parameter:
        return dominators().dominates(location.block, point.block);
    case ValuePlacement::InBlock:
        if (location.block == point.block)
            return location.index < point.index;
        return dominators().terminatorDominates(location.block, point.block);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FunctionAnalyses::dominates(Edge edge, BlockID block)
{
    CFG& graph = cfg();
    if (!graph.isReachable(edge.from) || !graph.isReachable(block))
        return false;
    BlockID target = targetOf(m_function, edge).block;
    return graph.singleIncomingEdge(target) == edge && dominators().dominates(target, block);
}

void FunctionAnalyses::invalidate(AnalysisSet requested, AnalysisSet broughtUpToDate)
{
    drop(addDependentAnalyses(requested - broughtUpToDate) - broughtUpToDate);
}

static constexpr bool changesTypesAlone(PassID pass) { return pass == PassID::RetypeValues; }

void FunctionAnalyses::didRetypeValues()
{
    if (!changesTypesAlone(m_runningPass)) {
        didChangeTypes();
        return;
    }
    invalidate(valueAnalyses - analysesThatReadNoTypes, FunctionAnalysis::Escape);
    if (m_values.escape && !m_values.escape->didRetypeValues())
        invalidate(FunctionAnalysis::Escape);
}

template<typename... Arguments>
AnalysisSet FunctionAnalyses::tellUseLists(void (UseLists::*didChange)(Arguments...), std::type_identity_t<Arguments>... arguments)
{
    UseLists* useLists = updatingUseLists();
    if (!useLists)
        return { };
    (useLists->*didChange)(arguments...);
    return FunctionAnalysis::UseLists;
}

void FunctionAnalyses::didAddDetachedValue(ValueID)
{
    invalidate(valueAnalyses, tellUseLists(&UseLists::didAddValues));
    m_values.numberOfValues = m_function.values.size();
}

static constexpr bool reportsEveryChangeToIdentityInputs(PassID pass) { return pass == PassID::EliminateDeadCode; }

void FunctionAnalyses::didEraseValues(BlockID block, std::span<const ValueID> erased)
{
    AnalysisSet broughtUpToDate = tellUseLists(&UseLists::didChangeBlock, block);
    if (m_values.identities && reportsEveryChangeToIdentityInputs(m_runningPass)) {
        m_values.identities->didEraseValues(erased);
        m_values.didUpdateIdentities = true;
        broughtUpToDate.add(FunctionAnalysis::Identities);
    }
    invalidate(valueAnalyses, broughtUpToDate);
}

void FunctionAnalyses::didChangeBlock(BlockID block)
{
    invalidate(valueAnalyses, tellUseLists(&UseLists::didChangeBlock, block));
}

void FunctionAnalyses::didReplaceUses(ValueID from, ValueID to)
{
    invalidate(valueAnalyses, tellUseLists(&UseLists::didReplaceUses, from, to));
}

void FunctionAnalyses::didChangeValuesButNotUses()
{
    invalidate(valueAnalyses, updatingUseLists() ? AnalysisSet(FunctionAnalysis::UseLists) : AnalysisSet());
}

void FunctionAnalyses::didChangeControlFlow(std::span<const BlockID> changedBlocks)
{
    AnalysisSet broughtUpToDate;
    if (UseLists* useLists = updatingUseLists()) {
        for (BlockID block : changedBlocks)
            useLists->didChangeBlock(block);
        useLists->didChangeControlFlow();
        broughtUpToDate.add(FunctionAnalysis::UseLists);
    }
    invalidate(allAnalyses, broughtUpToDate);
}

void FunctionAnalyses::didRenumber(const Renumbering& renumbering)
{
    invalidate(allAnalyses, renumbering.blocks.isEmpty() ? controlFlowAnalyses | FunctionAnalysis::Frequencies : AnalysisSet());
    m_values.numberOfValues = m_function.values.size();
    m_controlFlow.numberOfBlocks = m_function.blocks.size();
}

void FunctionAnalyses::drop(AnalysisSet set)
{
    willDrop(set);
    m_values.didDropBesideUseLists = true;
    if (set.contains(FunctionAnalysis::ControlFlow)) {
        set.remove(controlFlowAnalyses);
        m_controlFlow.edgesFromUnreachableBlocks.reset();
        m_controlFlow.mayDescribeFormerGraph = m_controlFlow.cfg.has_value();
    }
    if (!m_values.mayHoldResults)
        set.remove(valueAnalyses);
    else if (set.containsAll(valueAnalyses))
        m_values.mayHoldResults = false;
    if (set.contains(FunctionAnalysis::Induction))
        m_values.inductionVariables.reset();
    if (set.contains(FunctionAnalysis::FailedTests))
        m_values.failedTests.reset();
    if (set.contains(FunctionAnalysis::Guards)) {
        m_values.guards.reset();
        m_values.opcodes.reset();
    }
    if (set.contains(FunctionAnalysis::Frequencies))
        m_values.frequenciesMayDescribeFormerFunction = m_values.blockFrequencies.has_value();
    if (set.contains(FunctionAnalysis::EscapeHistory))
        m_values.escapeHistory.reset();
    if (set.contains(FunctionAnalysis::Escape))
        m_values.escape.reset();
    if (set.contains(FunctionAnalysis::Effects)) {
        m_values.hasEffects = false;
        m_effects.didChangeFunction();
    }
    if (set.contains(FunctionAnalysis::Liveness))
        m_values.liveness.reset();
    if (set.contains(FunctionAnalysis::Identities))
        m_values.identities.reset();
    if (set.contains(FunctionAnalysis::UseLists) && m_values.useLists)
        m_values.useLists->didChangeEverything();
    if (set.contains(FunctionAnalysis::NaturalLoops))
        m_controlFlow.naturalLoops.reset();
    if (set.contains(FunctionAnalysis::Dominators))
        m_controlFlow.dominators.reset();
    if (set.contains(FunctionAnalysis::Cycles)) {
        dropFrequencies();
        m_controlFlow.blockCycles.reset();
    }
}

AnalysisSet FunctionAnalyses::heldControlFlowAnalyses() const
{
    AnalysisSet result;
    result.set(FunctionAnalysis::ControlFlow, m_controlFlow.cfg.has_value());
    result.set(FunctionAnalysis::Dominators, m_controlFlow.dominators.has_value());
    result.set(FunctionAnalysis::NaturalLoops, m_controlFlow.naturalLoops.has_value());
    result.set(FunctionAnalysis::Cycles, m_controlFlow.blockCycles.has_value());
    return result;
}

AnalysisSet FunctionAnalyses::computed() const
{
    AnalysisSet result;
    if (!m_controlFlow.mayDescribeFormerGraph)
        result = heldControlFlowAnalyses();
    result.set(FunctionAnalysis::UseLists, m_values.useLists && m_values.useLists->isUpToDate());
    result.set(FunctionAnalysis::Identities, m_values.identities.has_value());
    result.set(FunctionAnalysis::Liveness, m_values.liveness.has_value());
    result.set(FunctionAnalysis::Effects, m_values.hasEffects);
    result.set(FunctionAnalysis::Escape, m_values.escape.has_value());
    result.set(FunctionAnalysis::EscapeHistory, m_values.escapeHistory.has_value());
    result.set(FunctionAnalysis::Frequencies, m_values.blockFrequencies && !m_values.frequenciesMayDescribeFormerFunction);
    result.set(FunctionAnalysis::Guards, m_values.guards.has_value());
    result.set(FunctionAnalysis::FailedTests, m_values.failedTests.has_value());
    result.set(FunctionAnalysis::Induction, m_values.inductionVariables.has_value());
    return result;
}

std::span<const Edge> FunctionAnalyses::edgesFromUnreachableBlocks()
{
    if (!m_controlFlow.edgesFromUnreachableBlocks) {
        const CFG& graph = cfg();
        Vector<Edge>& edges = m_controlFlow.edgesFromUnreachableBlocks.emplace();
        for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex) {
            if (graph.isReachable(BlockID(blockIndex)))
                continue;
            forEachOutgoingEdge(m_function, BlockID(blockIndex), [&](Edge edge, const Target&) {
                edges.append(edge);
            });
        }
    }
    checkBlockCount();
    return m_controlFlow.edgesFromUnreachableBlocks->span();
}

bool FunctionAnalyses::isEnteredFromUnreachableBlock(BlockID block)
{
    return std::ranges::any_of(edgesFromUnreachableBlocks(), [&](Edge edge) {
        return targetOf(m_function, edge).block == block;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
