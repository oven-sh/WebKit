/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileQueue.h"

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTBodyIndex.h"
#include "AOTEntryEmitter.h"
#include "AOTFacts.h"
#include "AOTLinkTables.h"
#include "AOTMechanisms.h"
#include "AOTNumbering.h"
#include "AOTPlan.h"
#include "AOTRelocation.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Deque.h>
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT {

namespace {

struct SeedOfCompileStage {
    uint32_t sizeOfBody { 0 };
    CodeUnit unit;
};

struct OutputOfCompileThread {
    ListOfKeptCode code;
    BlocksOfUnits blocks;
    Vector<Typed::WalkNotes> notes;
    WTF::BitSet<numberOfEntries> entriesUsed;
    Typed::CompileWorker::InstructionsOfSteps instructionsOfSteps;
    Vector<UseOfCompileThreads::Unit> timesOfUnits;
};

class RetiredBodyCaches {
    WTF_MAKE_NONCOPYABLE(RetiredBodyCaches);
public:
    explicit RetiredBodyCaches(unsigned numberOfThreads)
        : m_threads(numberOfThreads, [](size_t) { return makeUniqueWithoutFastMallocCheck<OfThread>(); })
    {
    }

    void didBeginUnit(unsigned thread) { m_threads[thread]->startOfUnit.store(m_clock.load()); }

    void didEndUnit(unsigned thread, const Typed::BodyIndex* body)
    {
        OfThread& own = *m_threads[thread];
        own.startOfUnit.store(notInUnit);
        if (body) {
            std::unique_ptr<const Typed::KeptWithBodyIndex> kept = body->takeKeptByWalk();
            std::unique_ptr<const Typed::BodyLivenessData> liveness = body->takeLiveness();
            if (kept || liveness)
                own.retired.append(Retired { m_clock.fetch_add(1), WTF::move(kept), WTF::move(liveness) });
        }
        if (own.retired.isEmpty())
            return;
        uint64_t oldestStart = notInUnit;
        for (const std::unique_ptr<OfThread>& other : m_threads)
            oldestStart = std::min(oldestStart, other->startOfUnit.load());
        while (!own.retired.isEmpty() && own.retired.first().time < oldestStart)
            own.retired.removeFirst();
    }

private:
    static constexpr uint64_t notInUnit = UINT64_MAX;

    struct Retired {
        uint64_t time;
        std::unique_ptr<const Typed::KeptWithBodyIndex> kept;
        std::unique_ptr<const Typed::BodyLivenessData> liveness;
    };

    struct OfThread {
        std::atomic<uint64_t> startOfUnit { notInUnit };
        Deque<Retired> retired;
    };

    std::atomic<uint64_t> m_clock { 0 };
    Vector<std::unique_ptr<OfThread>> m_threads;
};

struct PlaceOfCompiledCode {
    uint32_t placeInCode { 0 };
    CompiledCode* code { nullptr };
};

} // anonymous namespace

void forEachUnitMentionedBy(const CompiledCode& code, const Typed::CompileInputs& inputs, NOESCAPE const ScopedLambda<void(Typed::CodeUnit)>& functor)
{
    forEachUnitMentionedBy(code, inputs, scopedLambda<void(Typed::CodeUnit, MentionKind)>([&](CodeUnit unit, MentionKind) {
        functor(unit);
    }));
}

void forEachUnitMentionedBy(const CompiledCode& code, const Typed::CompileInputs& inputs, NOESCAPE const ScopedLambda<void(Typed::CodeUnit, MentionKind)>& functor)
{
    const CodeBody& body = code.body;
    for (const Relocation& relocation : body.relocations) {
        if (relocation.space == SymbolSpace::Function)
            functor(body.callees[relocation.symbol], MentionKind::Call);
    }
    const Typed::WalkInputs& walk = inputs.walk;
    for (const Mention& mention : body.mentions)
        forEachUnitBehind(mention, body.mentionWords.span(), walk.program, walk.facts, walk.plan, functor);
}

void mentionUnitNamedOutsideCode(UnitQueue& queue, unsigned thread, CodeUnit unit)
{
    queue.mention(thread, unit, Typed::isGenericEntry(unit.role) ? MentionKind::ClosureWord : MentionKind::Call);
}

void mentionWhatCodeNames(UnitQueue& queue, unsigned thread, const CompiledCode& code, const Typed::CompileInputs& inputs)
{
    const CodeBody& body = code.body;
    for (const Relocation& relocation : body.relocations) {
        if (relocation.space == SymbolSpace::Function)
            queue.mention(thread, body.callees[relocation.symbol]);
    }
    const Typed::WalkInputs& walk = inputs.walk;
    struct ClassGoneThrough {
        LayoutID classLayout;
        Vector<CodeUnit> unitsThatAskWhoMentions;
    };
    Vector<ClassGoneThrough, 4> classesGoneThrough;
    bool goesThroughClass = false;
    auto mentionUnitsThatAskOrGoThrough = [&](LayoutID classOrNoneBehindLast) {
        goesThroughClass = false;
        if (!classOrNoneBehindLast)
            return false;
        if (const Vector<CodeUnit>* unitsThatAsk = queue.unitsThatAskAmongMentionedUnitsOfClass(classOrNoneBehindLast)) {
            for (CodeUnit unit : *unitsThatAsk)
                queue.mention(thread, unit, MentionKind::ClosureWord);
            return false;
        }
        classesGoneThrough.append({ classOrNoneBehindLast, { } });
        goesThroughClass = true;
        return true;
    };
    AsksForUnitsOfClass asksForUnitsOfClass = mentionUnitsThatAskOrGoThrough;
    for (const Mention& mention : body.mentions) {
        bool namesSelector = mention.space == NumberSpace::MethodSelector;
        if (namesSelector && queue.hasMentionedUnitsBehindSelector(keyOf(mention, body.mentionWords.span())[0]))
            continue;
        bool queueAsksWhoMentions = false;
        forEachUnitBehind(mention, body.mentionWords.span(), walk.program, walk.facts, walk.plan, scopedLambda<void(Typed::CodeUnit, MentionKind)>([&](CodeUnit unit, MentionKind kind) {
            queueAsksWhoMentions |= namesSelector && queue.asksWhoMentions(unit);
            if (goesThroughClass && queue.asksWhoMentions(unit))
                classesGoneThrough.last().unitsThatAskWhoMentions.append(unit);
            queue.mention(thread, unit, kind);
        }), &asksForUnitsOfClass);
        if (namesSelector && !queueAsksWhoMentions)
            queue.didMentionUnitsBehindSelector(keyOf(mention, body.mentionWords.span())[0]);
        for (ClassGoneThrough& gone : classesGoneThrough)
            queue.didMentionUnitsOfClass(gone.classLayout, WTF::move(gone.unitsThatAskWhoMentions));
        classesGoneThrough.shrink(0);
    }
}

static Vector<CodeUnit> seedsOfCompileStage(const Typed::WalkInputs& inputs, const ParallelLoops& loops)
{
    Vector<CodeUnit> units = Typed::unitsNamedOutsideCode(inputs.program, inputs.facts, inputs.plan);
    BitVector hasSeed(inputs.program.functions.size());
    Vector<SeedOfCompileStage> seeds(units.size(), [&](size_t index) {
        CodeUnit unit = units[index];
        const Typed::Summary* summary = hasSeed.set(unit.function.index()) ? nullptr : inputs.facts.summaryOf(unit.function);
        return SeedOfCompileStage { summary ? summary->sizeOfBody : 0, unit };
    });
    parallelSort(loops, seeds.mutableSpan(), [](const SeedOfCompileStage& a, const SeedOfCompileStage& b) {
        if (a.sizeOfBody != b.sizeOfBody)
            return a.sizeOfBody < b.sizeOfBody;
        return a.unit > b.unit;
    });
    return Vector<CodeUnit>(seeds.size(), [&](size_t index) {
        return seeds[index].unit;
    });
}

static bool prepareCodeOfCompileThread(CompiledCode& code, const Typed::CallingConventions& conventions, BuildContext& build)
{
    if (code.body.bytes.isEmpty()) {
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::FunctionWithoutCode, .function = code.unit.function });
        return false;
    }
    code.unitIndex = conventions.indexOf(code.unit);
    code.placeInCode = placeInCodeOf(code.unit, code.unitIndex);
    return true;
}

static void keepCodeOfCompileThread(OutputOfCompileThread& output, CompiledCode&& code)
{
    output.code.append(output.blocks.keep(WTF::move(code)));
}

static Vector<FusedCallEntry> describeFusedCallEntries(std::span<const CodeUnit> entries, const Typed::CompileInputs& inputs, const ParallelLoops& loops)
{
    const Typed::CallingConventions& conventions = inputs.walk.plan.conventions();
    Vector<FusedCallEntry> described(entries.size());
    forEachRangeOfIndicesInParallel(loops, entries.size(), [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            CodeUnit entry = entries[index];
            CodeUnit target = conventions.genericEntry(entry).target;
            uint32_t unitIndex = conventions.indexOf(entry);
            described[index] = FusedCallEntry { entry, unitIndex, placeInCodeOf(entry, unitIndex), placeInCodeOf(target, conventions.indexOf(target)), Typed::describeEntry(inputs.generation, entry) };
        }
    });
    return described;
}

static std::optional<LinkedFunctions> linkCodeOfCompileThreads(std::span<OutputOfCompileThread> outputs, std::span<const CodeUnit> fusedCallEntries, const Typed::CompileInputs& inputs, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfOutputs = static_cast<unsigned>(outputs.size());
    Vector<size_t> firstPlaceOf;
    firstPlaceOf.reserveInitialCapacity(numberOfOutputs);
    size_t numberOfUnits = 0;
    for (const OutputOfCompileThread& output : outputs) {
        firstPlaceOf.append(numberOfUnits);
        numberOfUnits += output.code.size();
    }
    if (!WTF::isValidCapacityForVector<PlaceOfCompiledCode>(numberOfUnits)) {
        build.reportLimitExceeded(LimitID::VectorBytes, numberOfUnits * sizeof(PlaceOfCompiledCode));
        return std::nullopt;
    }

    Vector<PlaceOfCompiledCode> places(numberOfUnits);
    forEachIndexInParallel(loops, numberOfOutputs, [&](unsigned index) {
        size_t place = firstPlaceOf[index];
        OutputOfCompileThread& output = outputs[index];
        for (size_t number = 0; number < output.code.size(); ++number)
            places[place++] = { output.code[number].placeInCode, &output.code[number] };
    });
    parallelSort(loops, places.mutableSpan(), [](const PlaceOfCompiledCode& a, const PlaceOfCompiledCode& b) {
        return a.placeInCode < b.placeInCode;
    });

    Vector<CompiledCode*> functions(numberOfUnits);
    CodeInBlocks linked;
    linked.placesInCode.grow(numberOfUnits);
    forEachRangeOfIndicesInParallel(loops, numberOfUnits, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            const PlaceOfCompiledCode& place = places[index];
            RELEASE_ASSERT(!index || places[index - 1].placeInCode < place.placeInCode);
            functions[index] = place.code;
            linked.placesInCode[index] = place.placeInCode;
        }
    });
    linked.code = ListOfKeptCode(WTF::move(functions));
    linked.blocks = Vector<BlocksOfUnits>(numberOfOutputs, [&](size_t index) {
        outputs[index].code.clear();
        return WTF::move(outputs[index].blocks);
    });

    Vector<FusedCallEntry> described = describeFusedCallEntries(fusedCallEntries, inputs, loops);
    if (AOT::hasFlag(build.options().injectedFaults, Wire::FaultInjection::ReverseFusedCallEntries))
        described.reverse();
    if (!std::ranges::is_sorted(described, { }, &FusedCallEntry::unitIndex) || !std::ranges::is_sorted(described, { }, &FusedCallEntry::place)) {
        build.diagnostics().add(InternalError { .kind = InternalErrorKind::ProgramTableCheckFailed, .detail = "TableNotSorted: fusedCallEntries"_s });
        return std::nullopt;
    }
    return LinkedFunctions(WTF::move(linked), WTF::move(described));
}

static Vector<Typed::WalkNotes> takeNotesOfCompileThreads(std::span<OutputOfCompileThread> outputs, const ParallelLoops& loops)
{
    unsigned numberOfOutputs = static_cast<unsigned>(outputs.size());
    Vector<size_t> firstNoteOf;
    firstNoteOf.reserveInitialCapacity(numberOfOutputs);
    size_t numberOfNotes = 0;
    for (const OutputOfCompileThread& output : outputs) {
        firstNoteOf.append(numberOfNotes);
        numberOfNotes += output.notes.size();
    }

    Vector<Typed::WalkNotes*> inOrder(numberOfNotes);
    forEachIndexInParallel(loops, numberOfOutputs, [&](unsigned index) {
        size_t place = firstNoteOf[index];
        for (Typed::WalkNotes& notes : outputs[index].notes)
            inOrder[place++] = &notes;
    });
    parallelSort(loops, inOrder.mutableSpan(), [](const Typed::WalkNotes* a, const Typed::WalkNotes* b) {
        return a->unit < b->unit;
    });

    Vector<Typed::WalkNotes> result(numberOfNotes);
    forEachRangeOfIndicesInParallel(loops, numberOfNotes, [&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index)
            result[index] = WTF::move(*inOrder[index]);
    });
    return result;
}

static UseOfCompileThreads describeUseOfCompileThreads(std::span<const OutputOfCompileThread> outputs)
{
    using Unit = UseOfCompileThreads::Unit;
    UseOfCompileThreads use;
    use.numberOfThreads = static_cast<unsigned>(outputs.size());

    struct Change {
        Seconds at;
        int threads;
    };
    Vector<Change> changes;
    for (const OutputOfCompileThread& output : outputs) {
        for (const Unit& unit : output.timesOfUnits) {
            use.wall = std::max(use.wall, unit.begin + unit.duration);
            use.busy += unit.duration;
            changes.append({ unit.begin, 1 });
            changes.append({ unit.begin + unit.duration, -1 });
            use.longestUnits.append(unit);
        }
    }
    if (changes.isEmpty())
        return use;

    size_t numberKept = std::min(use.longestUnits.size(), UseOfCompileThreads::numberOfLongestUnits);
    std::partial_sort(use.longestUnits.begin(), use.longestUnits.begin() + numberKept, use.longestUnits.end(), [](const Unit& a, const Unit& b) {
        return a.duration > b.duration;
    });
    use.longestUnits.shrink(numberKept);

    std::ranges::sort(changes, [](const Change& a, const Change& b) {
        return a.at != b.at ? a.at < b.at : a.threads < b.threads;
    });
    Seconds lengthOfPart = use.wall / static_cast<double>(UseOfCompileThreads::numberOfParts);
    int threadsBusy = 0;
    Seconds reached;
    size_t part = 0;
    auto countUpTo = [&](Seconds end) {
        while (reached < end) {
            bool isLastPart = part + 1 == UseOfCompileThreads::numberOfParts;
            Seconds endOfPart = lengthOfPart * static_cast<double>(part + 1);
            if (!isLastPart && endOfPart <= reached) {
                ++part;
                continue;
            }
            Seconds endInPart = isLastPart ? end : std::min(end, endOfPart);
            use.threadsBusyInPart[part] += threadsBusy * ((endInPart - reached) / lengthOfPart);
            reached = endInPart;
        }
    };
    for (const Change& change : changes) {
        countUpTo(change.at);
        threadsBusy += change.threads;
        use.mostThreadsBusyAtOnce = std::max(use.mostThreadsBusyAtOnce, static_cast<unsigned>(threadsBusy));
    }
    return use;
}

static void makeQueueOfCompileStage(std::optional<UnitQueue>& queue, const Typed::WalkInputs& inputs, unsigned numberOfThreads)
{
    const Typed::CallingConventions& conventions = inputs.plan.conventions();
    if (inputs.options.mechanisms.contains(Typed::Mechanism::Arrivals))
        queue.emplace(inputs.program, conventions, inputs.facts.linked(), inputs.arrivals, numberOfThreads);
    else
        queue.emplace(inputs.program, conventions, numberOfThreads);
}

CompiledProgram compileQueuedUnits(const Typed::CompileInputs& inputs, Typed::TypeTable& types, BuildContext& build)
{
    const Typed::CallingConventions& conventions = inputs.walk.plan.conventions();
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfThreads = loops.width();

    Vector<CodeUnit> seeds;
    std::optional<UnitQueue> queueOfStage;
    runLanes(loops, {
        [&] { seeds = seedsOfCompileStage(inputs.walk, loops); },
        [&] { makeQueueOfCompileStage(queueOfStage, inputs.walk, numberOfThreads); },
    });
    UnitQueue& queue = *queueOfStage;
    queue.addSeeds(seeds.span(), loops);
    std::atomic<bool> didFail { false };
    Vector<OutputOfCompileThread> outputs(numberOfThreads);
    RetiredBodyCaches retiredCaches(numberOfThreads);
    bool timesUnits = build.options().logging.logsPhaseTimes;
    MonotonicTime startOfUnits = MonotonicTime::now();

    forEachIndexInParallel(loops, numberOfThreads, [&](unsigned index, unsigned thread) {
        std::optional<CodeUnit> unit = queue.take(thread);
        if (!unit)
            return;

        OutputOfCompileThread& output = outputs[index];
        Typed::CompileWorker worker(inputs, build, types);
        for (; unit; unit = queue.take(thread)) {
            MonotonicTime startOfUnit = timesUnits ? MonotonicTime::now() : MonotonicTime();
            Typed::CompileWorker::InstructionsOfSteps instructionsInFront = worker.instructionsOfSteps();
            retiredCaches.didBeginUnit(index);
            std::optional<CompiledCode> code = worker.compile(*unit);
            bool walksBody = unit->role == Typed::CodeRole::Body || unit->role == Typed::CodeRole::CoroutineBody || unit->role == Typed::CodeRole::SlowBody;
            retiredCaches.didEndUnit(index, walksBody ? inputs.walk.facts.bodies().find(unit->function) : nullptr);
            if (timesUnits) [[unlikely]] {
                const Typed::CompileWorker::InstructionsOfSteps& instructions = worker.instructionsOfSteps();
                output.timesOfUnits.append({ *unit, startOfUnit - startOfUnits, MonotonicTime::now() - startOfUnit, instructions.walkAndSweep - instructionsInFront.walkAndSweep, instructions.emitToEncode - instructionsInFront.emitToEncode });
            }
            if (code && prepareCodeOfCompileThread(*code, conventions, build)) {
                for (FunctionID entered : worker.functionsThatUnitMayEnterByValue())
                    queue.noteEntryByValue(thread, entered, inputs.walk.options.verifiesCode);
                mentionWhatCodeNames(queue, thread, *code, inputs);
                keepCodeOfCompileThread(output, WTF::move(*code));
            } else {
                didFail.store(true, std::memory_order_relaxed);
            }
            worker.functionsThatUnitMayEnterByValue().shrink(0);
            queue.didFinish(thread);
        }
        output.notes = WTF::move(worker.notes());
        output.entriesUsed = worker.totals().entriesUsed;
        output.instructionsOfSteps = worker.instructionsOfSteps();
    });


    CompiledProgram program;
    program.succeeded = !didFail.load(std::memory_order_relaxed);
    for (const OutputOfCompileThread& output : outputs) {
        program.entriesUsed.merge(output.entriesUsed);
        program.instructionsOfWalkAndSweep += output.instructionsOfSteps.walkAndSweep;
        program.instructionsOfEmitToEncode += output.instructionsOfSteps.emitToEncode;
    }
    if (timesUnits)
        program.useOfThreads = describeUseOfCompileThreads(outputs.span());
    program.notes = takeNotesOfCompileThreads(outputs.mutableSpan(), loops);
    Vector<CodeUnit> fusedCallEntries = queue.mentionedFusedCallEntries();
    if (std::optional<LinkedFunctions> functions = linkCodeOfCompileThreads(outputs.mutableSpan(), fusedCallEntries.span(), inputs, build))
        program.functions = WTF::move(*functions);
    else
        program.succeeded = false;
    return program;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
