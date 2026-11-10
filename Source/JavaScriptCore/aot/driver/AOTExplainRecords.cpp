/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStages.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTObjects.h"
#include "AOTThreads.h"
#include "AOTWalkNotes.h"
#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

static_assert(!static_cast<unsigned>(Wire::KnowledgeLevel::Unknown) && static_cast<unsigned>(Wire::KnowledgeLevel::Expected) == 1 && static_cast<unsigned>(Wire::KnowledgeLevel::Proven) == 2);
static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::accesses)> == numberOfEnumerators<Wire::KnowledgeLevel>);
static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::calls)> == numberOfEnumerators<Wire::KnowledgeLevel>);
static_assert(std::tuple_size_v<decltype(Wire::AccessCountRecord::accessCosts)> == numberOfEnumerators<Wire::AccessCost>);
static_assert(Wire::AccessCost::ByName < Wire::AccessCost::TestsReceiver && Wire::AccessCost::TestsReceiver < Wire::AccessCost::TestsSlot && Wire::AccessCost::TestsSlot < Wire::AccessCost::Free, "the dearer cost is the lesser enumerator");
static_assert(numberOfEnumerators<Wire::AccessKind> <= 256 && numberOfEnumerators<Wire::AllocationKind> <= 256);

namespace {

enum class NotesToExplain : uint8_t { Operations, Tests, Objects };

struct PlacedExplainNote {
    Wire::SourceLocation location;
    uint32_t function { 0 };
    uint32_t unit { 0 };
    uint32_t index { 0 };
    uint8_t kind { 0 };
    uint8_t rank { 0 };
};

struct ExplainedHeapCause {
    uint32_t reasons { 0 };
    Wire::AllocationHeapCause cause { Wire::AllocationHeapCause::None };
};

class NamesInExplainRecords {
public:
    explicit NamesInExplainRecords(const Typed::Program& program)
        : m_numberOfFunctions(program.functions.size())
        , m_numberOfLayouts(program.layouts.size())
        , m_functions(program.functions.size())
        , m_layouts(program.layouts.size())
    {
    }

    void addFunction(uint32_t function)
    {
        if (function < m_numberOfFunctions)
            m_functions.set(function);
    }

    void addLayout(uint32_t layout)
    {
        if (layout < m_numberOfLayouts)
            m_layouts.set(layout);
    }

    const BitVector& functions() const { return m_functions; }
    const BitVector& layouts() const { return m_layouts; }

private:
    size_t m_numberOfFunctions;
    size_t m_numberOfLayouts;
    BitVector m_functions;
    BitVector m_layouts;
};

} // anonymous namespace

static std::optional<uint8_t> rankOfExplainedOperation(Typed::WalkNoteKind kind)
{
    switch (kind) {
    case Typed::WalkNoteKind::GenericOperation:
        return 0;
    case Typed::WalkNoteKind::TestThatRejoins:
        return 1;
    case Typed::WalkNoteKind::TestThatLeaves:
        return 2;
    case Typed::WalkNoteKind::TestThatThrows:
        return 3;
    case Typed::WalkNoteKind::ResolvedOperation:
        return 4;
    case Typed::WalkNoteKind::CallNotInlined:
    case Typed::WalkNoteKind::ObjectBuilt:
    case Typed::WalkNoteKind::ObjectNotInFrame:
    case Typed::WalkNoteKind::ObjectStayedVirtual:
    case Typed::WalkNoteKind::LoopWalkedAgain:
    case Typed::WalkNoteKind::WayEnds:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wire::KnowledgeLevel levelOfExplainedOperation(Typed::WalkNoteKind kind)
{
    switch (kind) {
    case Typed::WalkNoteKind::GenericOperation:
        return Wire::KnowledgeLevel::Unknown;
    case Typed::WalkNoteKind::TestThatRejoins:
    case Typed::WalkNoteKind::TestThatLeaves:
    case Typed::WalkNoteKind::TestThatThrows:
        return Wire::KnowledgeLevel::Expected;
    case Typed::WalkNoteKind::ResolvedOperation:
        return Wire::KnowledgeLevel::Proven;
    case Typed::WalkNoteKind::CallNotInlined:
    case Typed::WalkNoteKind::ObjectBuilt:
    case Typed::WalkNoteKind::ObjectNotInFrame:
    case Typed::WalkNoteKind::ObjectStayedVirtual:
    case Typed::WalkNoteKind::LoopWalkedAgain:
    case Typed::WalkNoteKind::WayEnds:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Wire::TypeTestFailure> failureOfExplainedTest(Typed::WalkNoteKind kind)
{
    switch (kind) {
    case Typed::WalkNoteKind::TestThatRejoins:
        return Wire::TypeTestFailure::TakesGenericPath;
    case Typed::WalkNoteKind::TestThatLeaves:
        return Wire::TypeTestFailure::ExitsToSlowVersion;
    case Typed::WalkNoteKind::TestThatThrows:
        return Wire::TypeTestFailure::Throws;
    case Typed::WalkNoteKind::ResolvedOperation:
    case Typed::WalkNoteKind::GenericOperation:
    case Typed::WalkNoteKind::CallNotInlined:
    case Typed::WalkNoteKind::ObjectBuilt:
    case Typed::WalkNoteKind::ObjectNotInFrame:
    case Typed::WalkNoteKind::ObjectStayedVirtual:
    case Typed::WalkNoteKind::LoopWalkedAgain:
    case Typed::WalkNoteKind::WayEnds:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wire::AllocationKind allocationKindOfExplainedLayout(Typed::LayoutKind kind)
{
    switch (kind) {
    case Typed::LayoutKind::Record:
    case Typed::LayoutKind::Instance:
    case Typed::LayoutKind::ModuleNamespace:
        return Wire::AllocationKind::Object;
    case Typed::LayoutKind::Environment:
        return Wire::AllocationKind::Environment;
    case Typed::LayoutKind::Closure:
        return Wire::AllocationKind::Closure;
    case Typed::LayoutKind::CoroutineFrame:
        return Wire::AllocationKind::Generator;
    case Typed::LayoutKind::Container:
    case Typed::LayoutKind::BuiltinResult:
        return Wire::AllocationKind::Other;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Wire::AllocationKind allocationKindOfExplainNote(const Typed::Program& program, const Typed::WalkNote& note)
{
    return note.layout ? allocationKindOfExplainedLayout(program.at(note.layout).kind) : Wire::AllocationKind::Other;
}

static Wire::AllocationOutcome outcomeOfExplainNote(const Typed::WalkNote& note)
{
    if (note.kind == Typed::WalkNoteKind::ObjectStayedVirtual)
        return Wire::AllocationOutcome::Removed;
    if (note.kind == Typed::WalkNoteKind::ObjectNotInFrame)
        return Wire::AllocationOutcome::Heap;
    if (note.layout)
        return Wire::AllocationOutcome::Heap;
    return Wire::AllocationOutcome::Unknown;
}

static uint8_t rankOfExplainedOutcome(Wire::AllocationOutcome outcome)
{
    switch (outcome) {
    case Wire::AllocationOutcome::Heap:
        return 0;
    case Wire::AllocationOutcome::Removed:
        return 1;
    case Wire::AllocationOutcome::Unknown:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ExplainedHeapCause explainedHeapCauseFor(Typed::ObjectBuiltCause cause)
{
    switch (cause) {
    case Typed::ObjectBuiltCause::None:
        return { };
    case Typed::ObjectBuiltCause::Retained:
        return { 0, Wire::AllocationHeapCause::Escapes };
    case Typed::ObjectBuiltCause::Returned:
        return { static_cast<uint32_t>(Wire::AllocationEscapeReason::Returned), Wire::AllocationHeapCause::Escapes };
    case Typed::ObjectBuiltCause::Thrown:
        return { static_cast<uint32_t>(Wire::AllocationEscapeReason::Thrown), Wire::AllocationHeapCause::Escapes };
    case Typed::ObjectBuiltCause::DiffersAtJoin:
    case Typed::ObjectBuiltCause::MayThrowUnderHandler:
    case Typed::ObjectBuiltCause::OperationNeedsCell:
        return { 0, Wire::AllocationHeapCause::NotTracked };
    case Typed::ObjectBuiltCause::RetainedInLoop:
        return { 0, Wire::AllocationHeapCause::RetainedAcrossIterations };
    case Typed::ObjectBuiltCause::Suspension:
        return { 0, Wire::AllocationHeapCause::LiveAcrossSuspension };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ExplainedHeapCause heapCauseOfExplainNote(const Typed::WalkNote& note)
{
    if (outcomeOfExplainNote(note) == Wire::AllocationOutcome::Removed)
        return { };
    return explainedHeapCauseFor(note.objectBuilt);
}

static std::optional<PlacedExplainNote> classifyExplainNote(const Typed::Program& program, const Typed::WalkNote& note, NotesToExplain wanted)
{
    PlacedExplainNote placed;
    switch (wanted) {
    case NotesToExplain::Operations: {
        std::optional<uint8_t> rank = rankOfExplainedOperation(note.kind);
        if (!rank || note.access == Wire::AccessKind::Unknown)
            return std::nullopt;
        placed.kind = static_cast<uint8_t>(note.access);
        placed.rank = *rank;
        return placed;
    }
    case NotesToExplain::Tests:
        if (!failureOfExplainedTest(note.kind))
            return std::nullopt;
        placed.rank = *rankOfExplainedOperation(note.kind);
        return placed;
    case NotesToExplain::Objects:
        if (note.kind != Typed::WalkNoteKind::ObjectBuilt && note.kind != Typed::WalkNoteKind::ObjectNotInFrame && note.kind != Typed::WalkNoteKind::ObjectStayedVirtual)
            return std::nullopt;
        placed.kind = static_cast<uint8_t>(allocationKindOfExplainNote(program, note));
        placed.rank = rankOfExplainedOutcome(outcomeOfExplainNote(note));
        return placed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<PlacedExplainNote> placeExplainNotes(const Typed::Program& program, std::span<const Typed::WalkNotes> units, NotesToExplain wanted, const ParallelLoops& loops)
{
    Vector<PlacedExplainNote> placedNotes = collectInParallel<PlacedExplainNote>(loops, static_cast<unsigned>(units.size()), [&](unsigned unit, Vector<PlacedExplainNote>& found) {
        const Typed::WalkNotes& notes = units[unit];
        for (uint32_t index = 0; index < notes.notes.size(); ++index) {
            const Typed::WalkNote& note = notes.notes[index];
            if (!note.writtenIn)
                continue;
            std::optional<PlacedExplainNote> placed = classifyExplainNote(program, note, wanted);
            if (!placed)
                continue;
            placed->location = Typed::locationOf(program, notes, note);
            if (!hasModule(placed->location))
                continue;
            placed->function = note.writtenIn.index();
            placed->unit = unit;
            placed->index = index;
            found.append(*placed);
        }
    });
    auto keyOf = [](const PlacedExplainNote& note) {
        return std::tuple { note.location.module, note.location.position, note.kind, note.function, note.rank, note.unit, note.index };
    };
    parallelSort(loops, placedNotes.mutableSpan(), [&](const PlacedExplainNote& a, const PlacedExplainNote& b) {
        return keyOf(a) < keyOf(b);
    });
    return placedNotes;
}

static Vector<uint32_t> findExplainedPlaces(std::span<const PlacedExplainNote> sortedNotes)
{
    Vector<uint32_t> firstNoteOfPlace;
    for (uint32_t index = 0; index < sortedNotes.size(); ++index) {
        if (index) {
            const PlacedExplainNote& previous = sortedNotes[index - 1];
            const PlacedExplainNote& note = sortedNotes[index];
            if (previous.location == note.location && previous.kind == note.kind && previous.function == note.function)
                continue;
        }
        firstNoteOfPlace.append(index);
    }
    firstNoteOfPlace.append(static_cast<uint32_t>(sortedNotes.size()));
    return firstNoteOfPlace;
}

static const Typed::WalkNote& explainedWalkNote(std::span<const Typed::WalkNotes> units, const PlacedExplainNote& placed)
{
    return units[placed.unit].notes[placed.index];
}

static uint32_t countExplainedCopies(std::span<const Typed::WalkNotes> units, std::span<const PlacedExplainNote> notesOfPlace)
{
    if (notesOfPlace.size() == 1)
        return 1;
    Vector<std::pair<uint32_t, uint32_t>, 8> frames;
    for (const PlacedExplainNote& placed : notesOfPlace)
        frames.append({ placed.unit, explainedWalkNote(units, placed).origin.inlineFrame });
    AOT::sort(frames);
    return static_cast<uint32_t>(removeRepeatedElements(frames));
}

static std::span<const PlacedExplainNote> explainNotesWithWorstOutcome(std::span<const PlacedExplainNote> notesOfPlace)
{
    return notesOfPlace.first(static_cast<size_t>(std::ranges::count(notesOfPlace, notesOfPlace.front().rank, &PlacedExplainNote::rank)));
}

static uint32_t countExplainedAllocations(std::span<const Typed::WalkNotes> units, std::span<const PlacedExplainNote> notesWithOutcome)
{
    if (notesWithOutcome.size() == 1)
        return 1;
    Vector<std::pair<uint32_t, uint32_t>, 8> framesThatBuilt;
    uint32_t numberOfAllocations = 0;
    for (const PlacedExplainNote& placed : notesWithOutcome) {
        const Typed::WalkNote& note = explainedWalkNote(units, placed);
        if (note.kind == Typed::WalkNoteKind::ObjectNotInFrame)
            continue;
        ++numberOfAllocations;
        if (note.kind == Typed::WalkNoteKind::ObjectBuilt)
            framesThatBuilt.append({ placed.unit, note.origin.inlineFrame });
    }
    AOT::sort(framesThatBuilt);
    for (const PlacedExplainNote& placed : notesWithOutcome) {
        const Typed::WalkNote& note = explainedWalkNote(units, placed);
        if (note.kind == Typed::WalkNoteKind::ObjectNotInFrame && !std::ranges::binary_search(framesThatBuilt, std::pair { placed.unit, note.origin.inlineFrame }))
            ++numberOfAllocations;
    }
    return numberOfAllocations;
}

static Wire::AccessRecord makeExplainedAccess(std::span<const Typed::WalkNotes> units, std::span<const PlacedExplainNote> notesOfPlace, bool asksForCause)
{
    const PlacedExplainNote& place = notesOfPlace.front();
    const Typed::WalkNote& worst = explainedWalkNote(units, place);

    Wire::AccessRecord record;
    record.location = place.location;
    record.kind = worst.access;
    record.level = levelOfExplainedOperation(worst.kind);
    record.layout = worst.layout.index();
    record.copies = countExplainedCopies(units, notesOfPlace);
    record.function = place.function;
    record.cost = worst.cost;
    for (const PlacedExplainNote& placed : notesOfPlace) {
        const Typed::WalkNote& note = explainedWalkNote(units, placed);
        record.cost = std::min(record.cost, note.cost);
        record.loopDepth = std::max<uint32_t>(record.loopDepth, note.loopDepth);
    }
    if (asksForCause)
        record.cause = worst.cause;
    return record;
}

static Vector<Wire::AccessRecord> explainAccesses(const Typed::Program& program, std::span<const Typed::WalkNotes> units, bool asksForCauses, const ParallelLoops& loops)
{
    Vector<PlacedExplainNote> notes = placeExplainNotes(program, units, NotesToExplain::Operations, loops);
    Vector<uint32_t> firstNoteOfPlace = findExplainedPlaces(notes.span());
    return collectInParallel<Wire::AccessRecord>(loops, static_cast<unsigned>(firstNoteOfPlace.size() - 1), [&](unsigned place, Vector<Wire::AccessRecord>& found) {
        found.append(makeExplainedAccess(units, notes.span().subspan(firstNoteOfPlace[place], firstNoteOfPlace[place + 1] - firstNoteOfPlace[place]), asksForCauses));
    });
}

static Wire::TypeTestRecord makeExplainedTypeTest(std::span<const Typed::WalkNotes> units, std::span<const PlacedExplainNote> notesOfPlace)
{
    const PlacedExplainNote& place = notesOfPlace.front();
    const Typed::WalkNote& worst = explainedWalkNote(units, place);

    Wire::TypeTestRecord record;
    record.location = place.location;
    record.function = place.function;
    record.failure = *failureOfExplainedTest(worst.kind);
    record.copies = countExplainedCopies(units, notesOfPlace);
    record.layout = worst.layout.index();
    return record;
}

static Vector<Wire::TypeTestRecord> explainTypeTests(const Typed::Program& program, std::span<const Typed::WalkNotes> units, const ParallelLoops& loops)
{
    Vector<PlacedExplainNote> notes = placeExplainNotes(program, units, NotesToExplain::Tests, loops);
    Vector<uint32_t> firstNoteOfPlace = findExplainedPlaces(notes.span());
    return collectInParallel<Wire::TypeTestRecord>(loops, static_cast<unsigned>(firstNoteOfPlace.size() - 1), [&](unsigned place, Vector<Wire::TypeTestRecord>& found) {
        found.append(makeExplainedTypeTest(units, notes.span().subspan(firstNoteOfPlace[place], firstNoteOfPlace[place + 1] - firstNoteOfPlace[place])));
    });
}

static decltype(Wire::AccessCountRecord::accesses)* countersOfExplainedAccess(Wire::AccessCountRecord& record, Wire::AccessKind kind)
{
    switch (kind) {
    case Wire::AccessKind::Unknown:
    case Wire::AccessKind::Truthiness:
    case Wire::AccessKind::Equality:
    case Wire::AccessKind::Arithmetic:
    case Wire::AccessKind::Comparison:
    case Wire::AccessKind::Conversion:
    case Wire::AccessKind::TypeOf:
    case Wire::AccessKind::InstanceOf:
    case Wire::AccessKind::Iteration:
    case Wire::AccessKind::Spread:
        return nullptr;
    case Wire::AccessKind::Read:
    case Wire::AccessKind::Store:
    case Wire::AccessKind::Define:
    case Wire::AccessKind::Has:
    case Wire::AccessKind::Delete:
        return &record.accesses;
    case Wire::AccessKind::Call:
    case Wire::AccessKind::Construct:
        return &record.calls;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<Wire::AccessCountRecord> countExplainedAccesses(const Typed::Program& program, std::span<const Wire::AccessRecord> accesses, std::span<const Wire::TypeTestRecord> typeTests)
{
    Vector<Wire::AccessCountRecord> counts(program.modules.size(), [](size_t module) {
        Wire::AccessCountRecord record;
        record.module = static_cast<uint32_t>(module);
        return record;
    });
    for (const Wire::AccessRecord& access : accesses) {
        Wire::AccessCountRecord& record = counts[access.location.module];
        auto* countsByLevel = countersOfExplainedAccess(record, access.kind);
        if (!countsByLevel)
            continue;
        ++(*countsByLevel)[static_cast<size_t>(access.level)];
        if (countsByLevel == &record.accesses)
            ++record.accessCosts[static_cast<size_t>(access.cost)];
    }
    for (const Wire::TypeTestRecord& typeTest : typeTests)
        ++counts[typeTest.location.module].typeTests;

    counts.removeAllMatching([](const Wire::AccessCountRecord& record) {
        Wire::AccessCountRecord empty;
        empty.module = record.module;
        return record == empty;
    });
    return counts;
}

static Vector<Wire::AllocationRemarkRecord> explainAllocations(const Typed::Program& program, std::span<const Typed::WalkNotes> units, const ParallelLoops& loops, CompileOutput& output)
{
    Vector<PlacedExplainNote> notes = placeExplainNotes(program, units, NotesToExplain::Objects, loops);
    Vector<uint32_t> firstNoteOfPlace = findExplainedPlaces(notes.span());
    std::array<Wire::TextRange, numberOfEnumerators<Typed::Opcode>> namesOfOpcodes { };

    Vector<Wire::AllocationRemarkRecord> remarks;
    remarks.reserveInitialCapacity(firstNoteOfPlace.size() - 1);
    for (size_t place = 0; place + 1 < firstNoteOfPlace.size(); ++place) {
        std::span<const PlacedExplainNote> notesOfPlace = notes.span().subspan(firstNoteOfPlace[place], firstNoteOfPlace[place + 1] - firstNoteOfPlace[place]);
        const PlacedExplainNote& first = notesOfPlace.front();
        const Typed::WalkNote& worst = explainedWalkNote(units, first);
        std::span<const PlacedExplainNote> notesWithOutcome = explainNotesWithWorstOutcome(notesOfPlace);

        Wire::AllocationRemarkRecord remark;
        remark.location = first.location;
        remark.outcome = outcomeOfExplainNote(worst);
        remark.copies = countExplainedAllocations(units, notesWithOutcome);
        remark.function = first.function;
        remark.kind = static_cast<Wire::AllocationKind>(first.kind);
        remark.cause = heapCauseOfExplainNote(worst).cause;
        uint32_t reasons = 0;
        for (const PlacedExplainNote& placed : notesWithOutcome)
            reasons |= heapCauseOfExplainNote(explainedWalkNote(units, placed)).reasons;
        remark.reasons = static_cast<Wire::AllocationEscapeReason>(reasons);
        if (worst.neededIn)
            remark.forcedBy = program.at(worst.neededIn).locationAt(worst.neededAt.position);
        if (worst.opcode != Typed::Opcode::Undefined) {
            Wire::TextRange& name = namesOfOpcodes[static_cast<size_t>(worst.opcode)];
            if (!name.length)
                name = output.appendText(String { nameOf(worst.opcode) });
            remark.forcingOperation = name;
        }
        remarks.append(remark);
    }
    return remarks;
}

static void writeExplainedFunctionNames(const Typed::Program& program, const BitVector& functions, CompileOutput& output)
{
    Vector<Wire::FunctionNameRecord> records;
    for (size_t index : functions) {
        const Typed::Function& function = program.functions[index];
        Wire::FunctionNameRecord record;
        record.function = static_cast<uint32_t>(index);
        record.module = function.module.index();
        if (function.name)
            record.name = output.appendText(program.string(function.name));
        records.append(record);
    }
    output.setRecords(Wire::ResultSection::FunctionNames, records.span());
}

static void writeExplainedLayoutNames(const Typed::Program& program, const BitVector& layouts, CompileOutput& output)
{
    Vector<Wire::LayoutNameRecord> records;
    for (size_t index : layouts) {
        const Typed::Layout& layout = program.layouts[index];
        Wire::LayoutNameRecord record;
        record.layout = static_cast<uint32_t>(index);
        if (layout.name)
            record.name = output.appendText(program.string(layout.name));
        records.append(record);
    }
    output.setRecords(Wire::ResultSection::LayoutNames, records.span());
}

void writeExplainSections(const Typed::Program& program, std::span<const Typed::WalkNotes> units, BuildContext& build, CompileOutput& output)
{
    Wire::ReportSet reports = build.options().reports;
    bool explainsFlow = hasFlag(reports, Wire::ReportSet::Flow);
    bool countsAccesses = hasFlag(reports, Wire::ReportSet::AccessCounts);
    bool explainsAllocations = hasFlag(reports, Wire::ReportSet::Allocations);
    if (!explainsFlow && !countsAccesses && !explainsAllocations)
        return;

    const ParallelLoops& loops = build.parallelLoops();
    NamesInExplainRecords names(program);

    if (explainsFlow || countsAccesses) {
        Vector<Wire::AccessRecord> accesses = explainAccesses(program, units, explainsFlow, loops);
        Vector<Wire::TypeTestRecord> typeTests = explainTypeTests(program, units, loops);
        if (countsAccesses) {
            Vector<Wire::AccessCountRecord> counts = countExplainedAccesses(program, accesses.span(), typeTests.span());
            for (const Wire::AccessCountRecord& record : counts) {
                build.statistics().count(StatisticID::AccessesUnknown, record.accesses[static_cast<size_t>(Wire::KnowledgeLevel::Unknown)]);
                build.statistics().count(StatisticID::AccessesGuarded, record.accesses[static_cast<size_t>(Wire::KnowledgeLevel::Expected)]);
                build.statistics().count(StatisticID::AccessesProven, record.accesses[static_cast<size_t>(Wire::KnowledgeLevel::Proven)]);
                build.statistics().count(StatisticID::IndirectCalls, record.calls[static_cast<size_t>(Wire::KnowledgeLevel::Unknown)]);
                build.statistics().count(StatisticID::DirectCalls, record.calls[static_cast<size_t>(Wire::KnowledgeLevel::Expected)] + record.calls[static_cast<size_t>(Wire::KnowledgeLevel::Proven)]);
            }
            output.setRecords(Wire::ResultSection::AccessCounts, counts.span());
        }
        if (explainsFlow) {
            for (const Wire::AccessRecord& record : accesses) {
                names.addFunction(record.function);
                names.addLayout(record.layout);
            }
            for (const Wire::TypeTestRecord& record : typeTests) {
                names.addFunction(record.function);
                names.addLayout(record.layout);
            }
            output.setRecords(Wire::ResultSection::Accesses, accesses.span());
            output.setRecords(Wire::ResultSection::TypeTests, typeTests.span());
        }
    }

    if (explainsAllocations) {
        Vector<Wire::AllocationRemarkRecord> remarks = explainAllocations(program, units, loops, output);
        for (const Wire::AllocationRemarkRecord& remark : remarks)
            names.addFunction(remark.function);
        output.setRecords(Wire::ResultSection::AllocationRemarks, remarks.span());
    }

    if (explainsFlow || explainsAllocations)
        writeExplainedFunctionNames(program, names.functions(), output);
    if (explainsFlow)
        writeExplainedLayoutNames(program, names.layouts(), output);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
