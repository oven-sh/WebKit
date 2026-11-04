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

// Bun's src/aot/report/levels.rs finds the count of a level and of a cost at the number of the enumerator.
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
    // The AccessKind of an operation, the AllocationKind of an object, nothing of a test.
    uint8_t kind { 0 };
    // Lower is worse, so the worst note of a place sorts first.
    uint8_t rank { 0 };
};

struct ExplainedCause {
    Wire::UnprovenCause cause { Wire::UnprovenCause::Unknown };
    Wire::IllTypedCause illTypedCause { Wire::IllTypedCause::None };
    Wire::Witness witness { Typed::noWitness };
    uint32_t distance { 0 };
    bool hasOtherRoots { false };
    Wire::OtherOriginsRecord otherOrigins;
};

struct ExplainedAccess {
    Wire::AccessRecord record;
    Wire::OtherOriginsRecord otherOrigins;
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

static Wire::IllTypedCause illTypedCauseForExplainedCause(Wire::UnprovenCause cause)
{
    switch (cause) {
    case Wire::UnprovenCause::Unknown:
    case Wire::UnprovenCause::ResultDisabled:
    case Wire::UnprovenCause::Untracked:
        return Wire::IllTypedCause::None;
    case Wire::UnprovenCause::UnknownObject:
    case Wire::UnprovenCause::UncheckedConversionOfUntracked:
        return Wire::IllTypedCause::FromLooseType;
    case Wire::UnprovenCause::UndescribedNativeResult:
        return Wire::IllTypedCause::DeclarationWithoutBody;
    case Wire::UnprovenCause::DataFromOutside:
    case Wire::UnprovenCause::ThrownByRunTime:
        return Wire::IllTypedCause::OutsideData;
    case Wire::UnprovenCause::LayoutOutsideFamily:
        return Wire::IllTypedCause::LayoutOutsideFamily;
    case Wire::UnprovenCause::NonLayoutObject:
    case Wire::UnprovenCause::ObjectWithoutLayout:
    case Wire::UnprovenCause::BuiltinBase:
    case Wire::UnprovenCause::UnknownBase:
        return Wire::IllTypedCause::MemberWithoutSlot;
    case Wire::UnprovenCause::ClassEvaluatedAgain:
    case Wire::UnprovenCause::OwnPropertyHidesMember:
        return Wire::IllTypedCause::LookupMayChange;
    case Wire::UnprovenCause::ReachesNativeCode:
        return Wire::IllTypedCause::NativeWritesOperand;
    case Wire::UnprovenCause::ReachesUnknownJavaScript:
    case Wire::UnprovenCause::UnknownWriter:
    case Wire::UnprovenCause::UnseenCode:
    case Wire::UnprovenCause::ContentsOfEscapedObject:
        return Wire::IllTypedCause::UnseenWriter;
    case Wire::UnprovenCause::ParameterOfEscapedFunction:
        return Wire::IllTypedCause::UnseenCaller;
    case Wire::UnprovenCause::UnknownCallee:
    case Wire::UnprovenCause::ResultOfUnknownCallee:
        return Wire::IllTypedCause::UnprovenCallee;
    case Wire::UnprovenCause::UnprovenContents:
        return Wire::IllTypedCause::ContainerWriterUnseen;
    case Wire::UnprovenCause::FieldDeleted:
    case Wire::UnprovenCause::UnknownKeyDelete:
    case Wire::UnprovenCause::PropertyRedefined:
    case Wire::UnprovenCause::PropertyAdded:
    case Wire::UnprovenCause::IntegrityChanged:
    case Wire::UnprovenCause::PrototypeChanged:
        return Wire::IllTypedCause::StateChanged;
    case Wire::UnprovenCause::UnknownKeyStore:
        return Wire::IllTypedCause::ComputedKeyStore;
    case Wire::UnprovenCause::StoreOfOtherType:
        return Wire::IllTypedCause::StoreOutsideFieldType;
    case Wire::UnprovenCause::OtherNewTarget:
        return Wire::IllTypedCause::OtherThis;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Typed::ObjectMutation> mutationForExplainedCause(Wire::UnprovenCause cause)
{
    switch (cause) {
    case Wire::UnprovenCause::ReachesNativeCode:
        return Typed::ObjectMutation::ReachNativeCode;
    case Wire::UnprovenCause::ReachesUnknownJavaScript:
        return Typed::ObjectMutation::ReachUnknownJavaScript;
    case Wire::UnprovenCause::FieldDeleted:
        return Typed::ObjectMutation::DeleteField;
    case Wire::UnprovenCause::PropertyRedefined:
        return Typed::ObjectMutation::RedefineProperty;
    case Wire::UnprovenCause::PropertyAdded:
        return Typed::ObjectMutation::AddProperty;
    case Wire::UnprovenCause::IntegrityChanged:
        return Typed::ObjectMutation::ChangeIntegrity;
    case Wire::UnprovenCause::PrototypeChanged:
        return Typed::ObjectMutation::ChangePrototype;
    case Wire::UnprovenCause::StoreOfOtherType:
        return Typed::ObjectMutation::StoreOfOtherType;
    case Wire::UnprovenCause::Unknown:
    case Wire::UnprovenCause::ResultDisabled:
    case Wire::UnprovenCause::UnknownObject:
    case Wire::UnprovenCause::NonLayoutObject:
    case Wire::UnprovenCause::BuiltinBase:
    case Wire::UnprovenCause::UnknownBase:
    case Wire::UnprovenCause::ClassEvaluatedAgain:
    case Wire::UnprovenCause::UnknownWriter:
    case Wire::UnprovenCause::UnknownCallee:
    case Wire::UnprovenCause::UnprovenContents:
    case Wire::UnprovenCause::UnknownKeyDelete:
    case Wire::UnprovenCause::UnknownKeyStore:
    case Wire::UnprovenCause::OwnPropertyHidesMember:
    case Wire::UnprovenCause::OtherNewTarget:
    case Wire::UnprovenCause::UndescribedNativeResult:
    case Wire::UnprovenCause::UnseenCode:
    case Wire::UnprovenCause::UncheckedConversionOfUntracked:
    case Wire::UnprovenCause::DataFromOutside:
    case Wire::UnprovenCause::ThrownByRunTime:
    case Wire::UnprovenCause::ParameterOfEscapedFunction:
    case Wire::UnprovenCause::ContentsOfEscapedObject:
    case Wire::UnprovenCause::ResultOfUnknownCallee:
    case Wire::UnprovenCause::ObjectWithoutLayout:
    case Wire::UnprovenCause::Untracked:
    case Wire::UnprovenCause::LayoutOutsideFamily:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Typed::ObjectID objectNamedByExplainNote(const Typed::Program& program, const Typed::WalkNote& note)
{
    if (!note.hasSubject || note.subject.kind() != Typed::FactSubject::Kind::Object)
        return { };
    Typed::ObjectID object(note.subject.first());
    return object && object.index() < program.types.objects().size() ? object : Typed::ObjectID();
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

static Wire::AllocationKind allocationKindOfExplainedClass(BuiltinClassID builtinClass)
{
    if (builtinClass == BuiltinClasses::Object)
        return Wire::AllocationKind::Object;
    if (builtinClass == BuiltinClasses::Array)
        return Wire::AllocationKind::Array;
    if (builtinClass == BuiltinClasses::Arguments)
        return Wire::AllocationKind::Arguments;
    if (builtinClass == BuiltinClasses::Generator || builtinClass == BuiltinClasses::AsyncGenerator)
        return Wire::AllocationKind::Generator;
    if (builtinClass == BuiltinClasses::Promise)
        return Wire::AllocationKind::Promise;
    if (builtinClass == BuiltinClasses::BoundFunction)
        return Wire::AllocationKind::BoundFunction;
    if (builtinClass == BuiltinClasses::Function || builtinClass == BuiltinClasses::AsyncFunction || builtinClass == BuiltinClasses::GeneratorFunction || builtinClass == BuiltinClasses::AsyncGeneratorFunction)
        return Wire::AllocationKind::Closure;
    if (builtinClass == BuiltinClasses::String)
        return Wire::AllocationKind::StringRange;
    if (builtinClass == BuiltinClasses::Map || builtinClass == BuiltinClasses::Set || builtinClass == BuiltinClasses::WeakMap || builtinClass == BuiltinClasses::WeakSet)
        return Wire::AllocationKind::Collection;
    return Wire::AllocationKind::Other;
}

static Wire::AllocationKind allocationKindOfExplainNote(const Typed::Program& program, const Typed::WalkNote& note)
{
    Typed::LayoutID layout = note.layout;
    BuiltinClassID builtinClass;
    if (Typed::ObjectID object = objectNamedByExplainNote(program, note)) {
        const Typed::ObjectRecord& record = program.types.objects().at(object);
        switch (record.kind) {
        case Typed::ObjectKind::Closure:
            return Wire::AllocationKind::Closure;
        case Typed::ObjectKind::RestArray:
            return Wire::AllocationKind::Rest;
        case Typed::ObjectKind::Unknown:
        case Typed::ObjectKind::Builtin:
        case Typed::ObjectKind::ResultOfBuiltin:
        case Typed::ObjectKind::HostObject:
        case Typed::ObjectKind::EscapedFunctionsOfHolder:
        case Typed::ObjectKind::EscapedOfClass:
        case Typed::ObjectKind::EscapedWithoutShape:
        case Typed::ObjectKind::CoroutineResult:
        case Typed::ObjectKind::FunctionPrototype:
        case Typed::ObjectKind::ConstructedObject:
        case Typed::ObjectKind::Namespace:
        case Typed::ObjectKind::ModuleBuiltin:
        case Typed::ObjectKind::Instance:
        case Typed::ObjectKind::Prototype:
        case Typed::ObjectKind::EscapedOfLayout:
        case Typed::ObjectKind::Site:
            break;
        }
        if (record.layout)
            layout = record.layout;
        builtinClass = record.builtinClass;
    }
    if (layout) {
        Wire::AllocationKind kind = allocationKindOfExplainedLayout(program.at(layout).kind);
        if (kind != Wire::AllocationKind::Other)
            return kind;
    }
    return builtinClass ? allocationKindOfExplainedClass(builtinClass) : Wire::AllocationKind::Other;
}

// The walk declines a frame and never adds one, so an object whose site has none lies in the heap.
static Wire::AllocationOutcome outcomeOfExplainNote(const Typed::Program& program, const Typed::Facts& facts, const Typed::WalkNote& note)
{
    if (note.kind == Typed::WalkNoteKind::ObjectStayedVirtual)
        return Wire::AllocationOutcome::Removed;
    if (note.kind == Typed::WalkNoteKind::ObjectNotInFrame)
        return Wire::AllocationOutcome::Heap;
    if (Typed::ObjectID object = objectNamedByExplainNote(program, note))
        return facts.object(object).frame == Typed::FrameVerdict::MayLiveInFrame ? Wire::AllocationOutcome::Frame : Wire::AllocationOutcome::Heap;
    if (note.layout && !facts.has(note.layout, Typed::LayoutFact::MayLiveInFrame))
        return Wire::AllocationOutcome::Heap;
    return Wire::AllocationOutcome::Unknown;
}

static uint8_t rankOfExplainedOutcome(Wire::AllocationOutcome outcome)
{
    switch (outcome) {
    case Wire::AllocationOutcome::Heap:
        return 0;
    case Wire::AllocationOutcome::Slim:
        return 1;
    case Wire::AllocationOutcome::Deferred:
        return 2;
    case Wire::AllocationOutcome::FrameUntilMaterialized:
        return 3;
    case Wire::AllocationOutcome::Frame:
        return 4;
    case Wire::AllocationOutcome::Removed:
        return 5;
    case Wire::AllocationOutcome::Unknown:
        return 6;
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
        return { 0, Wire::AllocationHeapCause::HeldAcrossIterations };
    case Typed::ObjectBuiltCause::Suspension:
        return { 0, Wire::AllocationHeapCause::LiveAcrossSuspension };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ExplainedHeapCause heapCauseOfExplainNote(const Typed::Program& program, const Typed::Facts& facts, const Typed::WalkNote& note)
{
    Wire::AllocationOutcome outcome = outcomeOfExplainNote(program, facts, note);
    if (outcome == Wire::AllocationOutcome::Frame || outcome == Wire::AllocationOutcome::Removed)
        return { };
    ExplainedHeapCause why = explainedHeapCauseFor(note.objectBuilt);
    if (why.cause != Wire::AllocationHeapCause::None)
        return why;
    if (Typed::ObjectID object = objectNamedByExplainNote(program, note)) {
        Typed::ObjectFacts objectFacts = facts.object(object);
        if (objectFacts.frame == Typed::FrameVerdict::Heap)
            why.cause = objectFacts.heapCause;
    }
    return why;
}

static std::optional<PlacedExplainNote> classifyExplainNote(const Typed::Program& program, const Typed::Facts& facts, const Typed::WalkNote& note, NotesToExplain wanted)
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
        placed.rank = rankOfExplainedOutcome(outcomeOfExplainNote(program, facts, note));
        return placed;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Ascending by module, position, kind and function, which is a place, then by rank, unit and index: no two notes compare equal.
static Vector<PlacedExplainNote> placeExplainNotes(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, NotesToExplain wanted, const ParallelLoops& loops)
{
    Vector<PlacedExplainNote> placedNotes = collectInParallel<PlacedExplainNote>(loops, static_cast<unsigned>(units.size()), [&](unsigned unit, Vector<PlacedExplainNote>& found) {
        const Typed::WalkNotes& notes = units[unit];
        for (uint32_t index = 0; index < notes.notes.size(); ++index) {
            const Typed::WalkNote& note = notes.notes[index];
            if (!note.writtenIn)
                continue;
            std::optional<PlacedExplainNote> placed = classifyExplainNote(program, facts, note, wanted);
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

// Where the notes of each place begin, and behind the last place the number of notes.
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

// One inline frame of one unit is one copy of a place. The walk may take several notes there: a test of each operand, of each parameter, of the shape and of the slot.
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

// The notes of a place that have the outcome reported, which is the worst: they come first.
static std::span<const PlacedExplainNote> explainNotesWithWorstOutcome(std::span<const PlacedExplainNote> notesOfPlace)
{
    return notesOfPlace.first(static_cast<size_t>(std::ranges::count(notesOfPlace, notesOfPlace.front().rank, &PlacedExplainNote::rank)));
}

// Each ObjectBuilt is an allocation in the code, and each ObjectStayedVirtual is one that is gone from it.
// Of an object that it builds and declines a frame the walk also takes an ObjectNotInFrame. The ObjectBuilt beside it reads Frame while its site may live in a frame, so it is not among these notes.
// Where it is, the two are one allocation: an ObjectNotInFrame counts only where no ObjectBuilt of its inline frame stands for the same object.
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

static ExplainedCause explainCauseOfNote(const Typed::Program& program, const Typed::Facts& facts, const Typed::WalkNote& note)
{
    ExplainedCause explained;
    if (note.hasSubject && facts.tracesUnknownValues()) {
        Typed::UnknownOrigin origin = facts.originOfUnknownAt(note.subject);
        if (origin.cause != Wire::UnprovenCause::Unknown) {
            explained.cause = origin.cause;
            explained.witness = origin.witness;
            explained.distance = origin.distance;
            explained.hasOtherRoots = origin.hasOtherRoots;
            explained.otherOrigins.locations = origin.otherOrigins;
        }
    }
    if (explained.cause == Wire::UnprovenCause::Unknown) {
        explained.cause = note.cause;
        // No seed stands behind a mutation or an escape of a named object. Facts keeps the lowest place that does it.
        if (std::optional<Typed::ObjectMutation> mutation = mutationForExplainedCause(explained.cause)) {
            if (Typed::ObjectID object = objectNamedByExplainNote(program, note))
                explained.witness = facts.witnessOf(object, *mutation);
        }
    }
    explained.illTypedCause = note.illTypedCause != Wire::IllTypedCause::None ? note.illTypedCause : illTypedCauseForExplainedCause(explained.cause);
    if (Typed::hasFunction(explained.witness) && !hasModule(explained.witness.location))
        explained.witness.location = program.at(Typed::FunctionID(explained.witness.function)).declarationLocation();
    return explained;
}

static bool isExplainedOperator(Wire::AccessKind kind)
{
    switch (kind) {
    case Wire::AccessKind::Truthiness:
    case Wire::AccessKind::Equality:
    case Wire::AccessKind::Arithmetic:
    case Wire::AccessKind::Comparison:
    case Wire::AccessKind::Conversion:
    case Wire::AccessKind::TypeOf:
    case Wire::AccessKind::InstanceOf:
    case Wire::AccessKind::Iteration:
    case Wire::AccessKind::Spread:
        return true;
    case Wire::AccessKind::Unknown:
    case Wire::AccessKind::Read:
    case Wire::AccessKind::Store:
    case Wire::AccessKind::Define:
    case Wire::AccessKind::Has:
    case Wire::AccessKind::Delete:
    case Wire::AccessKind::Call:
    case Wire::AccessKind::Construct:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ExplainedAccess makeExplainedAccess(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, std::span<const PlacedExplainNote> notesOfPlace, bool asksForCause)
{
    const PlacedExplainNote& place = notesOfPlace.front();
    const Typed::WalkNote& worst = explainedWalkNote(units, place);

    ExplainedAccess access;
    Wire::AccessRecord& record = access.record;
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
    if (!asksForCause || (record.level == Wire::KnowledgeLevel::Proven && record.cost == Wire::AccessCost::Free))
        return access;

    ExplainedCause explained = explainCauseOfNote(program, facts, worst);
    // `x + 1` with `x: number | string` has no typed form though no `unknown` reaches it. Of an access, Unknown reads as a fault of the compiler.
    if (explained.cause == Wire::UnprovenCause::Unknown && isExplainedOperator(record.kind))
        explained.cause = Wire::UnprovenCause::Untracked;
    // Bun's src/aot/report/causes.rs shows a cause at its witness. Where nothing leads further back, that is the line that pays.
    if (!Typed::hasFunction(explained.witness) || !hasModule(explained.witness.location))
        explained.witness = { place.function, Wire::none, place.location };
    record.cause = explained.cause;
    record.illTypedCause = explained.illTypedCause;
    record.witness = explained.witness;
    record.distance = explained.distance;
    record.hasOtherRoots = explained.hasOtherRoots;
    access.otherOrigins = explained.otherOrigins;
    return access;
}

static Vector<ExplainedAccess> explainAccesses(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, bool asksForCauses, const ParallelLoops& loops)
{
    Vector<PlacedExplainNote> notes = placeExplainNotes(program, facts, units, NotesToExplain::Operations, loops);
    Vector<uint32_t> firstNoteOfPlace = findExplainedPlaces(notes.span());
    return collectInParallel<ExplainedAccess>(loops, static_cast<unsigned>(firstNoteOfPlace.size() - 1), [&](unsigned place, Vector<ExplainedAccess>& found) {
        found.append(makeExplainedAccess(program, facts, units, notes.span().subspan(firstNoteOfPlace[place], firstNoteOfPlace[place + 1] - firstNoteOfPlace[place]), asksForCauses));
    });
}

static Wire::TypeTestRecord makeExplainedTypeTest(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, std::span<const PlacedExplainNote> notesOfPlace, bool asksForCause)
{
    const PlacedExplainNote& place = notesOfPlace.front();
    const Typed::WalkNote& worst = explainedWalkNote(units, place);

    Wire::TypeTestRecord record;
    record.location = place.location;
    record.function = place.function;
    record.failure = *failureOfExplainedTest(worst.kind);
    record.copies = countExplainedCopies(units, notesOfPlace);
    record.layout = worst.layout.index();
    if (!asksForCause)
        return record;

    ExplainedCause explained = explainCauseOfNote(program, facts, worst);
    record.cause = explained.cause;
    record.illTypedCause = explained.illTypedCause;
    record.witness = explained.witness;
    return record;
}

static Vector<Wire::TypeTestRecord> explainTypeTests(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, bool asksForCauses, const ParallelLoops& loops)
{
    Vector<PlacedExplainNote> notes = placeExplainNotes(program, facts, units, NotesToExplain::Tests, loops);
    Vector<uint32_t> firstNoteOfPlace = findExplainedPlaces(notes.span());
    return collectInParallel<Wire::TypeTestRecord>(loops, static_cast<unsigned>(firstNoteOfPlace.size() - 1), [&](unsigned place, Vector<Wire::TypeTestRecord>& found) {
        found.append(makeExplainedTypeTest(program, facts, units, notes.span().subspan(firstNoteOfPlace[place], firstNoteOfPlace[place + 1] - firstNoteOfPlace[place]), asksForCauses));
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

static Vector<Wire::AccessCountRecord> countExplainedAccesses(const Typed::Program& program, std::span<const ExplainedAccess> accesses, std::span<const Wire::TypeTestRecord> typeTests)
{
    Vector<Wire::AccessCountRecord> counts(program.modules.size(), [](size_t module) {
        Wire::AccessCountRecord record;
        record.module = static_cast<uint32_t>(module);
        return record;
    });
    for (const ExplainedAccess& access : accesses) {
        Wire::AccessCountRecord& record = counts[access.record.location.module];
        auto* countsByLevel = countersOfExplainedAccess(record, access.record.kind);
        if (!countsByLevel)
            continue;
        ++(*countsByLevel)[static_cast<size_t>(access.record.level)];
        if (countsByLevel == &record.accesses)
            ++record.accessCosts[static_cast<size_t>(access.record.cost)];
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

static std::array<uint32_t, 6> keyOfExplainedOrigins(const Wire::OtherOriginsRecord& origins)
{
    const auto& locations = origins.locations;
    return { locations[0].module, locations[0].position, locations[1].module, locations[1].position, locations[2].module, locations[2].position };
}

static bool hasExplainedOrigin(const Wire::OtherOriginsRecord& origins)
{
    return std::ranges::any_of(origins.locations, [](Wire::SourceLocation location) {
        return hasModule(location);
    });
}

// The records ascend by content, so neither a thread nor the order of the accesses decides a number.
static Vector<Wire::OtherOriginsRecord> shareExplainedOrigins(Vector<ExplainedAccess>& accesses, const ParallelLoops& loops)
{
    Vector<Wire::OtherOriginsRecord> shared;
    for (const ExplainedAccess& access : accesses) {
        if (hasExplainedOrigin(access.otherOrigins))
            shared.append(access.otherOrigins);
    }
    sortAndRemoveDuplicates(loops, shared, [](const Wire::OtherOriginsRecord& a, const Wire::OtherOriginsRecord& b) {
        return keyOfExplainedOrigins(a) < keyOfExplainedOrigins(b);
    }, [](const Wire::OtherOriginsRecord& a, const Wire::OtherOriginsRecord& b) {
        return a == b;
    });

    std::span<const Wire::OtherOriginsRecord> records = shared.span();
    for (ExplainedAccess& access : accesses) {
        if (!hasExplainedOrigin(access.otherOrigins))
            continue;
        auto found = std::ranges::lower_bound(records, keyOfExplainedOrigins(access.otherOrigins), { }, keyOfExplainedOrigins);
        RELEASE_ASSERT(found != records.end() && *found == access.otherOrigins);
        access.record.otherOrigins = static_cast<uint32_t>(found - records.begin()) + 1;
    }
    return shared;
}

static Vector<Wire::AllocationRemarkRecord> explainAllocations(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, const ParallelLoops& loops, CompileOutput& output)
{
    Vector<PlacedExplainNote> notes = placeExplainNotes(program, facts, units, NotesToExplain::Objects, loops);
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
        remark.outcome = outcomeOfExplainNote(program, facts, worst);
        remark.copies = countExplainedAllocations(units, notesWithOutcome);
        remark.function = first.function;
        remark.kind = static_cast<Wire::AllocationKind>(first.kind);
        remark.cause = heapCauseOfExplainNote(program, facts, worst).cause;
        uint32_t reasons = 0;
        for (const PlacedExplainNote& placed : notesWithOutcome)
            reasons |= heapCauseOfExplainNote(program, facts, explainedWalkNote(units, placed)).reasons;
        remark.reasons = static_cast<Wire::AllocationEscapeReason>(reasons);
        // Bun's src/aot/report/allocations.rs prints it as the place where the object escapes. A cell of the frame is needed there and does not escape.
        if (worst.neededIn && remark.outcome != Wire::AllocationOutcome::Frame)
            remark.forcedBy = program.at(worst.neededIn).locationAt(worst.neededAt.position);
        // The default of WalkNote::opcode, and no operation that needs a cell.
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

void writeExplainSections(const Typed::Program& program, const Typed::Facts& facts, std::span<const Typed::WalkNotes> units, BuildContext& build, CompileOutput& output)
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
        Vector<ExplainedAccess> accesses = explainAccesses(program, facts, units, explainsFlow, loops);
        Vector<Wire::TypeTestRecord> typeTests = explainTypeTests(program, facts, units, explainsFlow, loops);
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
            output.setRecords(Wire::ResultSection::OtherOrigins, shareExplainedOrigins(accesses, loops).span());
            Vector<Wire::AccessRecord> records(accesses.size(), [&](size_t index) {
                return accesses[index].record;
            });
            for (const Wire::AccessRecord& record : records) {
                names.addFunction(record.function);
                names.addFunction(record.witness.function);
                names.addLayout(record.layout);
            }
            for (const Wire::TypeTestRecord& record : typeTests) {
                names.addFunction(record.function);
                names.addFunction(record.witness.function);
                names.addLayout(record.layout);
            }
            output.setRecords(Wire::ResultSection::Accesses, records.span());
            output.setRecords(Wire::ResultSection::TypeTests, typeTests.span());
        }
    }

    if (explainsAllocations) {
        Vector<Wire::AllocationRemarkRecord> remarks = explainAllocations(program, facts, units, loops, output);
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
