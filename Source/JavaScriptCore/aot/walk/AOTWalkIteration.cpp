/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// IteratorOpen, IteratorResume, IteratorClose and AppendSpread become the protocol as the language defines it, in closed operations, for every iterable alike.
// At a suspension: the state of an iteration is a cell of heap words, a value like any other. Each await of an async iteration is an await like any other.
// At a throw: a step leaves the state done while it runs, so the close in its handler does nothing. A close behind a throw drops what it throws itself.
// TypedOperations: a list that nobody can see being iterated is read by index: no iterator, no result object, no call, and nothing to close. Its position is a value like any other.
// By the same rows of the table a typed array is read by index, a Map and a Set are stepped through their tables, and a text by code point. Whether an instance is as it was made only the run shows: each step has both arms, and they rejoin.
// A pattern whose steps stand in a row, and a spread, are one operation of the run time each: DestructureIterable, AppendIterable.
// The protocol is never withheld. TypedOperations withheld: buildTypedIteration() is not asked, and every iteration goes by the protocol.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTIterationHelpers.h"
#include "AOTLinkTables.h"
#include "AOTSweep.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkKnown.h"
#include "AOTWalkTextBuilders.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkCoroutines.cpp
Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, bool takesExtraTurn, Origin);
bool canWaitForStepOfAsyncGenerator(Walker&);
// AOTWalkCalls.cpp
bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, Origin);

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
// AOTWalkNumbers.cpp
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

static constexpr OpcodeSet opcodesReplacedByIteration { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread };
static constexpr OpcodeSet appendsToListsInIteration { Opcode::AppendElement, Opcode::AppendHole };
static constexpr OpcodeSet callsOfListsInIteration { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
static constexpr OpcodeSet opcodesOfTypedIteration = opcodesReplacedByIteration | appendsToListsInIteration | callsOfListsInIteration | OpcodeSet { Opcode::ResultAt };

// ---- What the table says of the iteration of a list

static bool isConstantLocationForIteration(const Walker& walker, BuiltinID member)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (table.has(BuiltinMark::MayBeAbsent, member) || program.isUnlocked(member) || program.isUnlocked(table.canonicalID(member)))
        return false;
    if (table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member))
        return true;
    return walker.facts().builtinLocation(member) == BuiltinLocationState::Constant;
}

// What a read under the name finds from `start` upward in every run, as CallInWalkBuiltins::findMemberFrom() finds it. Null: nothing lies there, and nothing ever will. No answer: nobody knows.
static std::optional<BuiltinID> findMemberAboveForIteration(const Walker& walker, BuiltinID start, BuiltinName name)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    PropertyKeyID key = program.keyOf(name);
    if (!start || !key)
        return std::nullopt;
    MembersUnderKey members(program, key);
    BuiltinID holder = table.canonicalID(start);
    while (true) {
        if (BuiltinID member = members.findOwnMember(holder)) {
            bool liesOnHolder = !table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
            return liesOnHolder && isConstantLocationForIteration(walker, member) ? std::optional(member) : std::nullopt;
        }
        if (!listsEveryMemberUnder(program, holder, key) || !members.isClosedKey(holder) || !table.hasLockedPrototypeLink(holder))
            return std::nullopt;
        switch (table.at(holder).prototypeLink) {
        case BuiltinPrototypeLink::Unknown:
            return std::nullopt;
        case BuiltinPrototypeLink::Null:
            return BuiltinID();
        case BuiltinPrototypeLink::Builtin:
            holder = table.canonicalID(table.at(holder).inheritsFrom);
            break;
        }
    }
}

// An instance that has the names and the prototype of its creation finds one method under @@iterator. Its rows say that it makes an iterator of the engine over the receiver, and what the steps of that hand out.
// Nobody gets hold of that iterator, its `next` is the engine's for good, and no `return` lies on it or above it, or ever will: a close finds nothing to call.
// `isPrimitive`: a text has no name of its own but `length` and its indexes, and stands below the prototype of its class for good.
// `under`: the name that the method is looked for under, in the place of @@iterator: `map.values()`.
static std::optional<JSC::IterationKind> kindOfIterationThatNobodySees(const Walker& walker, BuiltinClassID builtinClass, bool isPrimitive = false, BuiltinName under = { })
{
    const BuiltinTable& table = walker.program().builtins();
    if (!isPrimitive && !table.mayHavePristineInstances(builtinClass))
        return std::nullopt;
    std::optional<BuiltinID> method = findMemberAboveForIteration(walker, table.at(builtinClass).prototype, under ? under : table.nameOf(WellKnownSymbol::Iterator));
    if (!method || !*method)
        return std::nullopt;

    BuiltinClassID iteratorClass;
    JSC::IterationKind kind = JSC::IterationKind::Values;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(*method))) {
        const BuiltinResult& result = overload.result;
        if (overload.mode != BuiltinCallMode::Call || !result.iteratedOperand || !result.iteratedOperand->isReceiver())
            return std::nullopt;
        const BuiltinType& made = table.at(result.type);
        if (made.parts.size() != 1 || (iteratorClass && (iteratorClass != made.parts[0].builtinClass || kind != result.iterationKind)))
            return std::nullopt;
        iteratorClass = made.parts[0].builtinClass;
        kind = result.iterationKind;
    }
    if (!iteratorClass)
        return std::nullopt;
    BuiltinID prototypeOfIterators = table.at(iteratorClass).prototype;
    std::optional<BuiltinID> next = findMemberAboveForIteration(walker, prototypeOfIterators, BuiltinNames::next);
    std::optional<BuiltinID> returnMethod = findMemberAboveForIteration(walker, prototypeOfIterators, BuiltinNames::returnKeyword);
    if (!next || !*next || !returnMethod || *returnMethod)
        return std::nullopt;
    return kind;
}

// From %GeneratorPrototype% upward `next`, `return`, `throw` and the method under @@iterator are the engine's for good: whoever iterates hands a generator to no code of the program. For AOTWalkCoroutines.cpp.
bool engineAnswersAboveGenerators(const Walker&);
bool engineAnswersAboveGenerators(const Walker& walker)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID prototype = table.at(BuiltinClasses::Generator).prototype;
    auto finds = [&](BuiltinName name, BuiltinID expected) {
        std::optional<BuiltinID> member = findMemberAboveForIteration(walker, prototype, name);
        return member && *member && table.canonicalID(*member) == table.canonicalID(expected);
    };
    if (!finds(BuiltinNames::next, Builtins::GeneratorPrototypeNext) || !finds(BuiltinNames::returnKeyword, Builtins::GeneratorPrototypeReturn) || !finds(BuiltinNames::throwKeyword, Builtins::GeneratorPrototypeThrow))
        return false;
    std::optional<BuiltinID> method = findMemberAboveForIteration(walker, prototype, table.nameOf(WellKnownSymbol::Iterator));
    return method && *method;
}

static bool listsAreIteratedByIndex(Walker& walker)
{
    std::optional<bool>& answer = walker.listsAreIteratedByIndex();
    if (!answer)
        answer = kindOfIterationThatNobodySees(walker, BuiltinClasses::Array) == JSC::IterationKind::Values;
    return *answer;
}

bool listsAreIteratedUnseen(Walker& walker)
{
    return listsAreIteratedByIndex(walker);
}

using ContentsStepped = Walker::IterationByIndex::Contents;

namespace {

struct ClassSteppedUnseen {
    BuiltinClassID builtinClass;
    ContentsStepped contents { ContentsStepped::List };
    JSC::IterationKind kind { JSC::IterationKind::Values };
};

} // namespace

// The type names one class of the engine, and whatever is of the type is an instance of it. An instance of a class below is none: it may have an @@iterator of its own.
static std::optional<ClassSteppedUnseen> findClassSteppedUnseen(Walker& walker, TypeID id)
{
    if (!id)
        return std::nullopt;
    Type type = walker.types().at(id);
    if (type.isAnyString()) {
        bool stepsHandOutValues = kindOfIterationThatNobodySees(walker, BuiltinClasses::String, true) == JSC::IterationKind::Values;
        return stepsHandOutValues ? std::optional(ClassSteppedUnseen { BuiltinClasses::String, ContentsStepped::Text }) : std::nullopt;
    }
    const ClassPart* onlyClass = type.isObject() ? singleBuiltinClassOf(walker.types(), type) : nullptr;
    if (!onlyClass)
        return std::nullopt;
    BuiltinClassID builtinClass = onlyClass->builtinClass;
    if (builtinClass == BuiltinClasses::Array)
        return listsAreIteratedByIndex(walker) ? std::optional(ClassSteppedUnseen { builtinClass }) : std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    BuiltinSlotID keys = table.findSlot(builtinClass, ContentsSlot::Key);
    BuiltinSlotID elements = table.findSlot(builtinClass, ContentsSlot::Element);
    ClassSteppedUnseen stepped { builtinClass };
    if (keys && table.at(keys).heap == AbstractHeap::CollectionEntries)
        stepped.contents = ContentsStepped::Map;
    else if (elements && table.at(elements).heap == AbstractHeap::CollectionEntries)
        stepped.contents = ContentsStepped::Set;
    else if (elements && table.at(elements).heap == AbstractHeap::TypedArrayElements && typedArrayTypeOf(BuiltinTypeContext { walker.program(), walker.types() }, id) != NotTypedArray)
        stepped.contents = ContentsStepped::View;
    else
        return std::nullopt;

    std::optional<JSC::IterationKind> kind = kindOfIterationThatNobodySees(walker, builtinClass);
    if (!kind || (stepped.contents == ContentsStepped::View && *kind != JSC::IterationKind::Values))
        return std::nullopt;
    stepped.kind = *kind;
    return stepped;
}

// openTypedIteration(): undefined and null are refused, and what is left is a text, which is stepped by index whatever else is known.
bool iterationGoesByIndexWithoutTest(Walker& walker, TypeID iterated)
{
    TypeID present = iterated ? walker.types().withoutNullish(iterated) : TypeID();
    std::optional<ClassSteppedUnseen> stepped = present && !isNeverType(present) ? findClassSteppedUnseen(walker, present) : std::nullopt;
    return stepped && stepped->contents == ContentsStepped::Text;
}

static std::optional<ClassSteppedUnseen> findClassSteppedUnseen(Walker& walker, Reference value)
{
    return value && value.isValue() ? findClassSteppedUnseen(walker, walker.typeOf(value)) : std::nullopt;
}

static TypeID typeOfInstancesStepped(Walker& walker, BuiltinClassID builtinClass, ContentsStepped contents)
{
    return contents == ContentsStepped::Text ? TypeID(TypeTable::string) : walker.types().builtinClass(builtinClass);
}

static bool isProvenListInIteration(Walker& walker, Reference value)
{
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, value);
    return stepped && stepped->contents == ContentsStepped::List;
}

// How many closed operations stand for an open, a step or a spread by the protocol at the least: what a guess turns into typed ones.
static constexpr uint32_t closedOperationsOfProtocol = 3;

// Whether an instance is still as it was made only the run shows. A value that is proven one is expected to be, as much as if the source said so. Of any other value the source or Link may expect the class.
// Where nobody says what is iterated over, or what is said names no one class and may be a list, a list is expected: most of what programs iterate over is one, and by the protocol each step of a list makes an object.
// The class to test for, where a test is to be put in. `stepsStandInLoop`: the test is asked once in front of a loop whose turns step the iteration.
static std::optional<ClassSteppedUnseen> findClassToTestFor(Walker& walker, ValueID input, Reference iterable, bool stepsStandInLoop = false)
{
    if (!iterable || !iterable.isValue())
        return std::nullopt;
    TypeArena& types = walker.types();
    bool isDeclared = true;
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, iterable);
    if (!stepped) {
        ExpectationOfValue expectation = findExpectation(walker, input);
        isDeclared = expectation.type && expectation.isDeclared;
        if (expectation.type)
            stepped = findClassSteppedUnseen(walker, types.withoutNullish(expectation.type));
        if (!stepped && listsAreIteratedByIndex(walker) && (!expectation.type || types.overlap(expectation.type, types.builtinClass(BuiltinClasses::Array)))) {
            stepped = ClassSteppedUnseen { BuiltinClasses::Array };
            isDeclared = false;
        }
    }
    if (!stepped)
        return std::nullopt;
    TypeID instances = typeOfInstancesStepped(walker, stepped->builtinClass, stepped->contents);
    if (!types.overlap(walker.typeOf(iterable), instances))
        return std::nullopt;
    if (stepsStandInLoop ? !walker.has(Mechanism::Guesses) : !shouldOpenGuess(walker, iterable, instances, isDeclared, closedOperationsOfProtocol))
        return std::nullopt;
    return stepped;
}

// `for (const [key, value] of map)`: what a step hands out where the iteration is the engine's. Null: the rows do not say.
TypeID typeOfPairsThatStepsHandOut(Walker& walker, TypeID iterable)
{
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, iterable);
    return stepped && stepped->kind == JSC::IterationKind::Entries ? walker.types().builtinClass(BuiltinClasses::Array) : TypeID();
}

static bool isFreshInIteration(Walker& walker, Reference name)
{
    if (!name || !name.isValue())
        return false;
    ValueID cell = rootOfCellInSweptCode(walker.code(), name.id());
    return !walker.code().isConstant(cell) && walker.state().isFresh(Reference::value(cell));
}

// ---- What the body says of an iteration

namespace {

// The position of an iteration by index is a value of Code, and no value of the input stands for it. So the walk has to know where it holds.
struct WayOfIteration {
    // Each step stands below the one in front of it, and at most one loop that the open stands in front of holds a step: one, in its head.
    bool canBeFollowed { false };
    BlockID headOfLoopThatSteps;
    // A step that throws comes to the close of its own iteration.
    bool stepsStandUnderOwnCleanup { false };
    // There is one step, and each close stands below it.
    bool closesStandBelowOnlyStep { false };
    // There is one step, at the head of a loop that the open stands in front of. So whatever names the iteration behind the loop closes it.
    bool onlyStepStandsAtHeadOfLoop { false };
    // And the head ends in a branch on whether the step was the last, which then leaves the loop, and each close stands behind the edge that is taken where it was not.
    // So no step follows the last one, and no close finds the iteration done.
    bool isOpenAtEveryStepAndClose { false };
};

} // anonymous namespace

static bool standsUnderCleanupOf(const BodyIndex& index, BodyBlock block, ValueID open)
{
    while (block.hasHandler()) {
        block = index.block(block.handler().block());
        if (block.subject() == open)
            return true;
    }
    return false;
}

// It reads the operations that name the open, which the index of the body lists.
static WayOfIteration findWayOfIteration(const BodyIndex& index, ValueID open)
{
    WayOfIteration way;
    BlockID blockOfOpen = index.blockOf(open);
    LoopID loopOfOpen = index.loopOf(blockOfOpen);

    BlockID blockOfLastStep = blockOfOpen;
    LoopID loopThatSteps;
    uint32_t numberOfSteps = 0;
    Vector<BlockID, 4> blocksOfCloses;
    ValueID lastUser;
    ValueID lastStep;
    for (const BodyMentionOfIteration& mention : index.mentionsOfIteration(open)) {
        if (std::exchange(lastUser, mention.user) == mention.user)
            continue;
        BodyValue operation = index.value(mention.user);
        if (!index.isReachable(mention.block) || operation.opcode() == Opcode::ReleaseHint)
            continue;
        if (operation.opcode() == Opcode::IteratorClose) {
            blocksOfCloses.append(mention.block);
            continue;
        }
        if (operation.opcode() != Opcode::IteratorResume)
            return way;

        auto step = operation.as<Opcode::IteratorResume>();
        if (step.resumeMode() != ResumeMode::Next || step.sentOperand() || step.readsValueWhenDone() || !index.dominates(blockOfLastStep, mention.block))
            return way;
        if (loopThatSteps && index.isInLoop(mention.block, loopThatSteps))
            return way;
        if (LoopID loop = index.loopOf(mention.block); loop != loopOfOpen) {
            if (loopThatSteps || index.loop(loop).parent != loopOfOpen || index.loop(loop).head != mention.block)
                return way;
            loopThatSteps = loop;
            way.headOfLoopThatSteps = mention.block;
        }
        blockOfLastStep = mention.block;
        lastStep = mention.user;
        ++numberOfSteps;
        way.stepsStandUnderOwnCleanup |= standsUnderCleanupOf(index, index.block(mention.block), open);
    }
    way.canBeFollowed = true;
    way.closesStandBelowOnlyStep = numberOfSteps == 1 && std::ranges::all_of(blocksOfCloses, [&](BlockID block) {
        return index.dominates(blockOfLastStep, block);
    });
    way.onlyStepStandsAtHeadOfLoop = numberOfSteps == 1 && way.headOfLoopThatSteps;
    if (!way.onlyStepStandsAtHeadOfLoop || way.stepsStandUnderOwnCleanup)
        return way;
    BodyTerminator end = index.terminator(way.headOfLoopThatSteps);
    ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
    if (!condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
        return way;
    auto asked = index.value(condition).as<Opcode::ResultAt>();
    if (asked.resultsOperand() != lastStep || asked.index() != static_cast<uint32_t>(IteratorResumeResult::Done))
        return way;
    BlockID turn = end.notTaken().block();
    if (index.isInLoop(end.taken().block(), loopThatSteps) || index.numberOfIncomingEdges(turn) != 1 || index.isEnteredByHandlerEdge(turn))
        return way;
    way.isOpenAtEveryStepAndClose = std::ranges::all_of(blocksOfCloses, [&](BlockID block) {
        return index.dominates(turn, block);
    });
    return way;
}

static bool haveOneHandler(BodyBlock first, BodyBlock second)
{
    if (!first.hasHandler() || !second.hasHandler())
        return !first.hasHandler() && !second.hasHandler();
    return first.handler().block() == second.handler().block() && std::ranges::equal(first.handler().arguments(), second.handler().arguments());
}

// What catches a throw of a step closes the iteration, which is done by then, and throws on to whoever would have caught a throw of the open.
static bool onlyClosesAndThrowsOn(const BodyIndex& index, BodyBlock cleanup, BodyBlock blockOfOpen, ValueID open)
{
    if (cleanup.subject() != open || cleanup.numberOfParameters() || cleanup.numberOfValues() != 2 || !haveOneHandler(cleanup, blockOfOpen))
        return false;
    auto cursor = cleanup.values().begin();
    BodyValue caught = *cursor;
    BodyValue close = *++cursor;
    BodyTerminator terminator = index.terminator(cleanup.id());
    return caught.opcode() == Opcode::CatchException && close.opcode() == Opcode::IteratorClose && close.operand(0) == open
        && terminator.kind() == TerminatorKind::Rethrow && terminator.operand(0) == caught.id();
}

// What may stand between two steps of a pattern: it writes nothing that is seen, reads nothing that other code may write, lets no other code run and throws by no rule of the language.
// So nobody can tell whether it ran between two steps or behind the last of them. A named built-in may be made where it is first read, which the order of keys shows.
static bool isUnseenBetweenStepsOfPattern(Opcode opcode)
{
    const Effects& bound = declaredBoundInTable(opcode);
    return opcode != Opcode::Builtin && !operandsMayRunCode(opcode) && bound.alwaysCompletes() && !bound.mayThrowByRule() && !bound.writes.containsAny(orderedHeaps) && !bound.reads.containsAny(orderedHeaps);
}

// `const [a, b] = pair`, `const [a = 0, b = ""] = pair`: between the open and the close behind the last step nothing is written but the steps, in their order, the reads of what they give, what nobody can see between them,
// jumps from one block to the next, and branches whose two sides hold only what nobody can see and meet again at once: the test for undefined in front of a default.
// So nothing runs in between: whoever takes all the steps and closes where the open stands does what is written in an order that nobody can tell from the order in which it is written. It reads what is written from the open to that close.
static bool stepsOfPatternStandInRow(const BodyIndex& index, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    if (!written.isDestructuring() || written.hasRest() || !written.elementCount())
        return false;
    uint32_t numberOfSteps = *written.elementCount();
    BodyBlock blockOfOpen = index.block(index.blockOf(open.id()));
    uint32_t numberOfCleanups = 0;
    BlockID lastCleanup;
    auto throwsWhereOpenThrows = [&](BodyBlock block) {
        if (block.id() == blockOfOpen.id() || haveOneHandler(block, blockOfOpen))
            return true;
        if (!block.hasHandler() || !block.handler().arguments().empty())
            return false;
        BodyBlock cleanup = index.block(block.handler().block());
        if (!onlyClosesAndThrowsOn(index, cleanup, blockOfOpen, open.id()))
            return false;
        numberOfCleanups += std::exchange(lastCleanup, cleanup.id()) != cleanup.id();
        return true;
    };
    // The block that the side of a branch out of `from` leads on to. Null: it holds more than what nobody can see.
    auto blockBehindSide = [&](BodyBlock from, BlockID side) -> BlockID {
        if (index.numberOfIncomingEdges(side) != 1 || index.isEnteredByHandlerEdge(side) || index.isLoopHead(side) || side.index() <= from.id().index())
            return { };
        BodyBlock block = index.block(side);
        if (!throwsWhereOpenThrows(block))
            return { };
        for (BodyValue operation : block.values()) {
            if (operation.opcode() == Opcode::ResultAt || !isUnseenBetweenStepsOfPattern(operation.opcode()))
                return { };
        }
        BodyTerminator terminator = block.terminator();
        return terminator.kind() == TerminatorKind::Jump && terminator.next().block().index() > side.index() ? terminator.next().block() : BlockID();
    };

    uint32_t stepsSeen = 0;
    ValueID lastStep;
    bool hasPassedOpen = false;
    for (BodyBlock block = blockOfOpen;;) {
        if (!throwsWhereOpenThrows(block))
            return false;
        for (BodyValue operation : block.values()) {
            if (!hasPassedOpen) {
                hasPassedOpen = operation.id() == open.id();
                continue;
            }
            switch (operation.opcode()) {
            case Opcode::IteratorResume: {
                auto step = operation.as<Opcode::IteratorResume>();
                if (step.iteratorOperand() != open.id() || step.stepIndex() != stepsSeen || step.resumeMode() != ResumeMode::Next || step.sentOperand() || step.readsValueWhenDone() || !step.heldOperands().empty())
                    return false;
                lastStep = operation.id();
                ++stepsSeen;
                break;
            }
            case Opcode::ResultAt:
                // Nobody asks whether a step was the last.
                if (operation.as<Opcode::ResultAt>().resultsOperand() != lastStep || (operation.as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value) && !index.isUnused(operation.id())))
                    return false;
                break;
            case Opcode::IteratorClose:
                // Nobody else names the open: each cleanup closes it once.
                return operation.operand(0) == open.id() && operation.as<Opcode::IteratorClose>().completion() == IteratorCompletion::Normal && stepsSeen == numberOfSteps
                    && index.mentionsOfIteration(open.id()).size() == numberOfSteps + 1 + numberOfCleanups;
            default:
                if (!isUnseenBetweenStepsOfPattern(operation.opcode()))
                    return false;
                break;
            }
        }
        BodyTerminator terminator = index.terminator(block.id());
        BlockID next;
        uint32_t numberOfWaysIn = 1;
        if (terminator.kind() == TerminatorKind::Jump && terminator.next().arguments().empty())
            next = terminator.next().block();
        else if (terminator.kind() == TerminatorKind::Branch) {
            next = blockBehindSide(block, terminator.taken().block());
            if (!next || blockBehindSide(block, terminator.notTaken().block()) != next)
                return false;
            numberOfWaysIn = 2;
        } else
            return false;
        if (index.numberOfIncomingEdges(next) != numberOfWaysIn || index.isEnteredByHandlerEdge(next) || index.isLoopHead(next) || next.index() <= block.id().index())
            return false;
        block = index.block(next);
    }
}

static bool isOpenThatMayGoByIndex(const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    return written.iteratorKind() == IteratorKind::Sync && !written.isDelegating();
}

// The literal that the open iterates over, where nothing else in the body names it: it is a list as a list is made, and stays what it is while the iteration lasts. Null: there is none.
static ValueID findLiteralIteratedAlone(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    ValueID literal = open.as<Opcode::IteratorOpen>().iterableOperand();
    if (!walker.has(Mechanism::TypedOperations) || !isOpenThatMayGoByIndex(open) || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewArray || index.onlyUsingValue(literal) != open.id())
        return { };
    if (!listsAreIteratedByIndex(walker))
        return { };
    // `const [before, k] = [line, i]; line += ..`: a step goes back to the element by its value of the body, which is bound to the builder again by then.
    if (walker.hasTextsInStringBuilders()) {
        for (ValueID element : index.value(literal).as<Opcode::NewArray>().elementsOperands()) {
            if (isNameOfTextInStringBuilder(walker, element))
                return { };
        }
    }
    // Only what collects the rest of a pattern asks for a position.
    if (!open.as<Opcode::IteratorOpen>().isDestructuring() || open.as<Opcode::IteratorOpen>().hasRest())
        return findWayOfIteration(index, open.id()).canBeFollowed ? literal : ValueID();
    return literal;
}

static uint32_t lengthOfLiteralInIteration(const BodyIndex& index, ValueID literal)
{
    auto made = index.value(literal).as<Opcode::NewArray>();
    return static_cast<uint32_t>(made.elementsOperands().size() + made.holes().size());
}

std::optional<StepOfListLiteral> stepOfListLiteral(Walker& walker, const BodyIndex& index, const BodyValue& value)
{
    auto step = value.as<Opcode::IteratorResume>();
    ValueID state = step.iteratorOperand();
    std::optional<uint32_t> stepIndex = step.stepIndex();
    if (!stepIndex || step.resumeMode() != ResumeMode::Next || step.sentOperand() || index.isParameter(state) || index.opcodeOf(state) != Opcode::IteratorOpen)
        return std::nullopt;
    BodyValue open = index.value(state);
    std::optional<uint32_t> elementCount = open.as<Opcode::IteratorOpen>().elementCount();
    if (!open.as<Opcode::IteratorOpen>().isDestructuring() || !elementCount || *stepIndex >= *elementCount)
        return std::nullopt;
    ValueID literal = findLiteralIteratedAlone(walker, index, open);
    if (!literal)
        return std::nullopt;

    auto made = index.value(literal).as<Opcode::NewArray>();
    std::span<const uint32_t> holes = made.holes();
    uint32_t position = 0;
    for (ValueID element : made.elementsOperands()) {
        for (; !holes.empty() && holes.front() == position; holes = holes.subspan(1)) {
            if (position++ == *stepIndex)
                return StepOfListLiteral { false, { } };
        }
        if (position++ == *stepIndex)
            return StepOfListLiteral { false, element };
    }
    return StepOfListLiteral { *stepIndex >= position + holes.size(), { } };
}

static constexpr OpcodeSet appendsToLiteralsInIteration = appendsToListsInIteration | OpcodeSet { Opcode::AppendSpread };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendHole::array && !OperandIndex::AppendSpread::array);

// `[x, ...one, y, ...other]`: the row of appends to a literal that begins with the append at `begin`. The place behind the last of them, among the operations of the block.
// Between two appends of a row stands only what writes nothing that is seen and lets no other code run, and nothing behind the next append reads what it gives:
// the appends may wait for the last of the row, and whoever builds the row twice lets nothing meet but the list. An append to another list ends the row, so no operation is read for two rows.
static uint32_t endOfRowOfAppendsInIteration(const BodyIndex& index, const BodyBlock& block, ValueID literal, uint32_t begin, uint32_t limit)
{
    uint32_t firstValue = block.firstValue().index();
    uint32_t end = begin + 1;
    // By operation from `end` on: the mentions of what it gives that are still to come.
    Vector<uint32_t, 8> mentionsToCome;
    for (uint32_t place = end; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        Opcode opcode = operation.opcode();
        uint32_t mentionsOfLiteral = 0;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            mentionsOfLiteral += operand == literal;
            if (operand.index() >= firstValue + end)
                --mentionsToCome[operand.index() - firstValue - end];
        });
        if (mentionsOfLiteral) {
            bool isAppend = appendsToLiteralsInIteration.contains(opcode) && mentionsOfLiteral == 1 && operation.operand(0) == literal;
            bool allIsTaken = std::ranges::all_of(mentionsToCome, [](uint32_t mentions) {
                return !mentions;
            });
            if (!isAppend || !allIsTaken)
                break;
            end = place + 1;
            mentionsToCome.shrink(0);
            continue;
        }
        const Effects& bound = declaredBoundInTable(opcode);
        uint32_t mentions = index.useCount(operation.id());
        if (mentions == BodyIndex::maxCountedUses || operandsMayRunCode(opcode) || !bound.alwaysCompletes() || !bound.writes.containsOnly(heapsWhoseWritesNobodySees))
            break;
        mentionsToCome.append(mentions);
    }
    return end;
}

bool isPartOfIterationOverListLiteral(Walker& walker, const BodyIndex& index, const BodyValue& value)
{
    ValueID open = value.opcode() == Opcode::IteratorOpen ? value.id() : value.operand(0);
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return false;
    return !!findLiteralIteratedAlone(walker, index, index.value(open));
}

namespace {

// The kind of site decides the wording of the error, and what the engine still reads and calls before it throws.
enum class WordingOfIteration : uint8_t { Source, Spread, Delegation };
enum class UseOfSyncIteration : uint8_t { Stepped, WrappedForAsyncIteration };
enum class ResultOfReturnInIteration : uint8_t { MustBeObject, IsIgnored };

// IteratorState is no value of JavaScript. A cell that NewHeapWords makes stands for it.
// IsDone: a step has answered done, or has thrown. A later step gives undefined and calls nothing, and a close does nothing.
// StepsInLibrary: a fourth word, which not every state has: laysVerdictOnNext(). `next` is the engine's own of a generator or of a Map's, a Set's or a list's iterator. The open asks once, for all the steps.
enum class WordOfIteration : uint32_t { Iterator, Next, IsDone, StepsInLibrary };
constexpr uint32_t numberOfWordsOfIteration = 3;

struct OpenedIteration {
    Reference iterator;
    Reference next;
};

struct JoinOfIteration {
    BlockID block;
    std::array<Reference, 2> results;
};

class BuilderOfIteration {
    WTF_MAKE_NONCOPYABLE(BuilderOfIteration);
public:
    // `held` outlives the builder.
    BuilderOfIteration(Walker& walker, Origin origin, IteratorKind kind, WordingOfIteration wording, std::span<const Reference> held = { })
        : m_walker(walker)
        , m_origin(origin)
        , m_kind(kind)
        , m_wording(wording)
        , m_held(held)
    {
        m_origin.isTailCall = false;
    }

    Reference requireObjectCoercible(Reference iterable)
    {
        // Neither undefined nor null is among what it may be: there is nothing to refuse.
        if (iterable && iterable.isValue() && !m_walker.isUnreachable() && !(m_walker.types().at(m_walker.typeOf(iterable)).primitives & Primitive::UndefinedOrNull))
            return iterable;
        Draft check = draft(Opcode::RequireObjectCoercible);
        check.setValueOperand(iterable);
        check.setNullishAccess(m_wording == WordingOfIteration::Spread ? NullishAccess::Spread : NullishAccess::Iterate);
        return build(WTF::move(check));
    }

    // Null, here and below: no way leads on from what was built.
    Reference openState(Reference iterable)
    {
        OpenedIteration opened = open(iterable);
        Draft allocation = draft(Opcode::NewHeapWords);
        bool laysVerdict = laysVerdictOnNext(iterable);
        allocation.setNumberOfWords(numberOfWordsOfIteration + laysVerdict);
        m_state = build(WTF::move(allocation));
        storeWord(WordOfIteration::Iterator, opened.iterator);
        storeWord(WordOfIteration::Next, opened.next);
        storeWord(WordOfIteration::IsDone, boolean(false));
        if (laysVerdict)
            storeWord(WordOfIteration::StepsInLibrary, testType(opened.next, methodsSteppedInLibrary()));
        return m_state;
    }

    // What IteratorResume gives: `done` and `value`.
    Reference resume(Reference state, ResumeMode mode, Reference sent, bool readsValueWhenDone)
    {
        takeState(state);
        JoinOfIteration join = openJoin();
        switch (mode) {
        case ResumeMode::Next:
            stepUnlessDone(sent, readsValueWhenDone, join);
            break;
        // `yield*` alone forwards a throw or a return, and the language has it ask nobody whether the iterator is done.
        case ResumeMode::Throw:
        case ResumeMode::Return:
            storeWord(WordOfIteration::IsDone, boolean(true));
            resumeByMethod(mode, sent ? sent : undefined(), readsValueWhenDone, join);
            break;
        }

        Draft results = draft(Opcode::Results);
        results.setValuesOperands(join.results);
        return build(WTF::move(results));
    }

    void close(Reference state, IteratorCompletion completion)
    {
        takeState(state);
        closeUnlessDone(completion);
    }

    void closeUnlessDone(IteratorCompletion completion)
    {
        Reference wasDone = readWord(WordOfIteration::IsDone);
        // In front of whatever may throw: a handler that still sees the state closes nothing a second time.
        storeWord(WordOfIteration::IsDone, boolean(true));

        BlockID finished = addBlock();
        BlockID closing = addBlock();
        BlockID after = addBlock();
        branch(wasDone, finished, closing);
        continueIn(finished);
        jumpTo(after);

        continueIn(closing);
        Reference iterator = readWord(WordOfIteration::Iterator);
        switch (completion) {
        case IteratorCompletion::Normal:
            callReturn(iterator, ResultOfReturnInIteration::MustBeObject);
            break;
        case IteratorCompletion::Throw:
            callReturnAndDropWhatItThrows(iterator);
            break;
        }
        jumpTo(after);
    }

    // Nothing closes the iterator of a spread, and nothing but this loop steps it. So it has no state.
    void appendAll(Reference array, Reference iterable)
    {
        OpenedIteration opened = open(iterable);
        if (m_walker.isUnreachable())
            return;

        // What the loop may write is dropped in front of its head. No effect ends what is known of a fresh object, so what the loop names leaves first.
        for (Reference named : { array, opened.iterator, opened.next })
            m_walker.letLeave(named);
        m_walker.state().invalidateHeap();
        m_isInLoopOfSpread = true;
        BlockID head = addBlock();
        m_walker.code().setLoopMarks(head, true, true);
        jumpTo(head);
        build(draft(Opcode::LoopHint));

        JoinOfIteration join = openJoin();
        step(opened.iterator, opened.next, { }, false, join);

        BlockID appending = addBlock();
        m_isInLoopOfSpread = false;
        BlockID exit = addBlock();
        branch(join.results[0], exit, appending);

        continueIn(appending);
        Draft append = draft(Opcode::AppendElement);
        append.setArrayOperand(array);
        append.setValueOperand(join.results[1]);
        build(WTF::move(append));
        jumpBackTo(head);

        continueIn(exit);
    }

    // ---- By index

    // `iterable` may be a list that is as it was made, which only the run shows. Where it is none the protocol opens the iteration.
    // `keepsStateInMemory`: a step that throws comes to the close of the iteration, which has to find it done. The arm of the protocol makes a list of its three words, and the arm that reads by index makes nothing.
    Walker::IterationByIndex openByIndexOrProtocol(Reference iterable, const ClassSteppedUnseen& stepped, bool keepsStateInMemory, bool isOpenAtEveryStepAndClose = false)
    {
        Walker::IterationByIndex iteration;
        iteration.isOpenAtEveryStepAndClose = !keepsStateInMemory && isOpenAtEveryStepAndClose;
        iteration.builtinClass = stepped.builtinClass;
        iteration.contents = stepped.contents;
        iteration.kind = stepped.kind;
        iteration.mayGoByProtocol = true;
        iteration.position = integer(0);

        BlockID byProtocol = addBlock();
        Reference instance = goOnWhereInstanceIsAsMade(iterable, stepped, byProtocol);
        Reference table = beginByIndex(instance, stepped.contents);
        BlockID after = addBlock();
        Vector<Reference, 4> byIndex { instance };
        iteration.list = m_walker.addParameter(after, { }, Representation::JSValue);
        if (keepsStateInMemory) {
            iteration.state = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.append(undefined());
        } else {
            iteration.iterator = m_walker.addParameter(after, { }, Representation::JSValue);
            iteration.next = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.appendList({ undefined(), undefined() });
        }
        if (stepsThroughTable(stepped.contents)) {
            iteration.table = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.append(table);
        }
        jumpTo(after, byIndex.span());

        continueIn(byProtocol);
        if (m_givenByCallThatMakesIterator) {
            m_walker.walkStretch(m_blockOfCallThatMakesIterator, m_placeOfReadOfMethod, m_placeBehindCallThatMakesIterator);
            if (m_walker.isUnreachable()) {
                continueIn(after);
                return iteration;
            }
            iterable = m_walker.referenceOf(m_givenByCallThatMakesIterator);
        }
        OpenedIteration opened = open(iterable);
        Vector<Reference, 4> openedByProtocol { undefined() };
        if (keepsStateInMemory) {
            static_assert(!static_cast<uint32_t>(WordOfIteration::Iterator) && static_cast<uint32_t>(WordOfIteration::Next) == 1 && static_cast<uint32_t>(WordOfIteration::IsDone) == 2);
            Draft words = draft(Opcode::NewArray);
            words.setContents(TypeTable::anything);
            iteration.stateHoldsVerdictOnNext = laysVerdictOnNext(iterable);
            if (iteration.stateHoldsVerdictOnNext)
                words.setElementsOperands(std::array { opened.iterator, opened.next, boolean(false), testType(opened.next, methodsSteppedInLibrary()) });
            else
                words.setElementsOperands(std::array { opened.iterator, opened.next, boolean(false) });
            openedByProtocol.append(build(WTF::move(words)));
        } else
            openedByProtocol.appendList({ opened.iterator, opened.next });
        if (iteration.table)
            openedByProtocol.append(undefined());
        jumpTo(after, openedByProtocol.span());
        return iteration;
    }

    // Of an instance that is surely as it was made. Null: of a list or a text.
    Reference tableThatFirstStepBeginsIn(Reference instance, ContentsStepped contents) { return beginByIndex(instance, contents); }

    // stepsOfPatternStandInRow(): every step is taken here, and the iterator is closed. A step that throws leaves the iteration done, so there is nothing to keep for whoever catches it.
    // One operation of the run time opens, steps and closes, for every pattern of the program. It reads a list that nobody can see being iterated by index, in a loop in line: no test for a list stands in front of it.
    void takeStepsOutOfLine(Reference iterable, uint32_t numberOfSteps, Vector<Reference, 4>& taken)
    {
        Draft check = draft(Opcode::RequireObjectCoercible);
        check.setValueOperand(iterable);
        check.setNullishAccess(NullishAccess::Iterate);
        Draft steps = draft(Opcode::DestructureIterable);
        steps.setIterableOperand(build(WTF::move(check)));
        steps.setCount(numberOfSteps);
        steps.setReadsListsByIndex(listsAreIteratedByIndex(m_walker));
        Reference results = build(WTF::move(steps));
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
        for (uint32_t step = 0; step < numberOfSteps; ++step) {
            Draft read = draft(Opcode::ResultAt);
            read.setResultsOperand(results);
            read.setIndex(step);
            taken.append(build(WTF::move(read)));
        }
    }

    Walker::IterationByIndex takeStepsAtOpen(Reference iterable, uint32_t numberOfSteps)
    {
        Walker::IterationByIndex iteration;
        iteration.isTakenAtOpen = true;
        takeStepsOutOfLine(iterable, numberOfSteps, iteration.takenAtOpen);
        return iteration;
    }

    // What IteratorResume gives.
    Reference resumeByIndex(Walker::IterationByIndex& iteration, std::optional<uint32_t> stepIndex)
    {
        if (iteration.isTakenAtOpen) {
            RELEASE_ASSERT(stepIndex && *stepIndex < iteration.takenAtOpen.size());
            return resultsOf(boolean(false), iteration.takenAtOpen[*stepIndex]);
        }
        if (iteration.holdsFlatEntries)
            return resumeThroughFlatEntries(iteration);
        bool runsNoCode = readsOfListRunNoCode(iteration);
        if (!iteration.mayGoByProtocol) {
            std::optional<int64_t> position = integerIn(iteration.position);
            std::optional<uint32_t> length = lengthKnownOf(iteration);
            bool isBehindEnd = position && (*position == Walker::positionBehindEveryEnd || (length && *position >= *length));
            if (isBehindEnd) {
                iteration.position = integer(Walker::positionBehindEveryEnd);
                return resultsOf(boolean(true), undefined());
            }
            if (position && length) {
                Reference value = elementAt(iteration.list, iteration.position, runsNoCode);
                iteration.position = integer(*position + 1);
                return resultsOf(boolean(false), value);
            }
        }

        BlockID after = addBlock();
        Reference done = m_walker.addParameter(after, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
        Reference value = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference position = m_walker.addParameter(after, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::positionBehindEveryEnd }), { }));
        Reference table = iteration.table ? m_walker.addParameter(after, { }, Representation::JSValue) : Reference();

        Reference list = iteration.list;
        if (iteration.mayGoByProtocol) {
            BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
            list = goOnWhereParameterHoldsInstance(iteration, byProtocol);
            BlockID byIndex = m_walker.isUnreachable() ? BlockID() : m_walker.currentBlock();
            continueIn(byProtocol);
            stepByProtocolBesideIndex(iteration, after);
            if (byIndex)
                continueIn(byIndex);
        }
        if (iteration.table)
            stepThroughTable(iteration, list, after);
        else {
            std::optional<uint32_t> length = lengthKnownOf(iteration);
            stepByIndex(iteration, list, length ? integer(*length) : lengthOf(iteration, list), runsNoCode, after);
        }
        iteration.position = position;
        iteration.table = table;
        return resultsOf(done, value);
    }

    // Nobody but the loop has the list, which is dense and as long as it was made.
    Reference resumeThroughFlatEntries(Walker::IterationByIndex& iteration)
    {
        BlockID after = addBlock();
        Reference done = m_walker.addParameter(after, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
        Reference key = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference value = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference position = m_walker.addParameter(after, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::positionBehindEveryEnd }), { }));

        Reference isInside = operatorOnNumbers(Opcode::Less, iteration.position, lengthOf(iteration, iteration.list));
        BlockID reading = addBlock();
        BlockID ending = addBlock();
        branch(isInside, reading, ending);
        continueIn(ending);
        jumpTo(after, { boolean(true), undefined(), undefined(), integer(Walker::positionBehindEveryEnd) });

        continueIn(reading);
        Reference atKey = narrowed(iteration.position, isInside, OperandIndex::Less::left);
        Reference atValue = operatorOnNumbers(Opcode::Increment, atKey);
        Reference keyRead = elementAt(iteration.list, atKey, true);
        Reference valueRead = elementAt(iteration.list, atValue, true);
        jumpTo(after, { boolean(false), keyRead, valueRead, operatorOnNumbers(Opcode::Increment, atValue) });
        iteration.position = position;
        iteration.valueBesideKey = value;
        return resultsOf(done, key);
    }

    // No `return` lies on the iterator of a list or above it, and nobody can put one there: there is nothing to call.
    void closeByIndex(Walker::IterationByIndex& iteration, IteratorCompletion completion)
    {
        if (!iteration.mayGoByProtocol)
            return;
        // What has caught a throw runs seldom. The function of the library does nothing for undefined, which the parameter holds where the open went by index: so no test and no arm stands in the handler.
        if (completion == IteratorCompletion::Throw && iteration.isOpenAtEveryStepAndClose && findHelper(IterationHelper::CloseIteratorAfterThrow)) {
            callReturnAndDropWhatItThrows(iteration.iterator);
            return;
        }
        BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        goOnWhereParameterHoldsInstance(iteration, byProtocol);
        jumpTo(after);

        continueIn(byProtocol);
        if (iteration.state) {
            takeStateInList(iteration.state);
            closeUnlessDone(completion);
        } else {
            if (!iteration.isOpenAtEveryStepAndClose) {
                BlockID closing = addBlock();
                BlockID finished = addBlock();
                branch(operatorOnNumbers(Opcode::Less, iteration.position, integer(Walker::positionBehindEveryEnd)), closing, finished);
                continueIn(finished);
                jumpTo(after);
                continueIn(closing);
            }
            switch (completion) {
            case IteratorCompletion::Normal:
                callReturn(iteration.iterator, ResultOfReturnInIteration::MustBeObject);
                break;
            case IteratorCompletion::Throw:
                callReturnAndDropWhatItThrows(iteration.iterator);
                break;
            }
        }
        jumpTo(after);
    }

    // `list` is a list that is as it was made: nothing runs while its elements are read, so they are read in one piece.
    void appendElementsOf(Reference array, Reference list)
    {
        if (std::optional<uint32_t> length = lengthOfFreshList(m_walker, list)) {
            for (uint32_t position = 0; position < *length; ++position)
                appendElement(array, elementAt(list, integer(position), true));
            return;
        }
        Draft append = draft(Opcode::AppendElements);
        append.setArrayOperand(array);
        append.setSourceOperand(list);
        build(WTF::move(append));
    }

    // `[x, ...set]`, `[...set, x]`, `[...one, ...other]`, `f(x, ...set)`: `spread` begins a row of appends to a literal that nothing but this code has. Nobody can tell when a list that nobody has was made:
    // it is made behind the tests of the sets, with room for what it is written with, for the members and for what else the row appends, so the appends find room.
    // Where a test fails nothing of the row has happened yet: ONE other arm makes the list as it is written and does the row out of line, and the two meet behind the row. False: nothing was built.
    bool appendRowToListMadeWithRoom(const BodyValue& spread, Reference array, Reference iterable, const ClassSteppedUnseen& stepped)
    {
        RELEASE_ASSERT(isListedByAppendMembers(stepped));
        const BodyIndex& index = m_walker.currentBodyIndex();
        ValueID literal = spread.as<Opcode::AppendSpread>().arrayOperand();
        BodyBlock block = index.block(index.blockOf(spread.id()));
        if (!m_walker.opensGuesses() || !m_walker.has(Mechanism::UnboxedNumbers))
            return false;
        // Behind the row the literal goes under another name. What another block has made is read on ways that pass the row by, and whoever walks a stretch as an arm lets only what the stretch defines meet.
        if (m_walker.walksStretchInsideAnother() || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewArray || !block.holds(literal))
            return false;
        std::optional<Vector<Reference, 8>> elements = isFreshListAsMade(m_walker, array) ? elementsOfFreshList(m_walker, array) : std::nullopt;
        if (!elements || elements->size() > maxComputedElementsOfArrayLiteral)
            return false;
        uint32_t firstValue = block.firstValue().index();
        uint32_t begin = spread.id().index() - firstValue;
        uint32_t end = endOfRowOfAppendsInIteration(index, block, literal, begin, std::min(block.numberOfValues(), m_walker.endOfStretchBeingWalked()));
        // `[...set]` and nothing more: the list takes its room from its site.
        if (elements->isEmpty() && end == begin + 1)
            return false;
        m_walker.checkElementsRememberedOfList(array, elements->span(), m_origin);

        // `members`: the set, of a spread that has passed its tests. Null: the append goes through its chain.
        struct AppendOfRow {
            uint32_t place { 0 };
            Reference members;
        };
        struct ArmOfRow {
            BlockID last;
            Reference list;
        };
        Vector<AppendOfRow, 4> appends;
        Vector<Reference, 4> sets;
        Vector<ArmOfRow, 2> arms;
        BodyValue made = index.value(literal);
        Reference boundBefore = m_walker.referenceBoundTo(literal);
        int64_t numberOfElements = static_cast<int64_t>(elements->size());

        // Neither a test nor what stands between two of them writes anything that is seen.
        BlockID otherArm = addBlock(BlockFrequency::Unlikely);
        appends.append({ begin, goOnWhereInstanceIsAsMade(iterable, stepped, otherArm) });
        sets.append(appends.last().members);
        for (uint32_t place = begin + 1; place < end && !m_walker.isUnreachable(); ++place) {
            BodyValue operation = index.value(ValueID(firstValue + place));
            if (!appendsToLiteralsInIteration.contains(operation.opcode()) || operation.operand(0) != literal) {
                m_walker.walkStretch(block.id(), place, place + 1);
                continue;
            }
            appends.append({ place, { } });
            if (operation.opcode() != Opcode::AppendSpread) {
                ++numberOfElements;
                continue;
            }
            ValueID written = operation.as<Opcode::AppendSpread>().iterableOperand();
            Reference next = m_walker.referenceOf(written);
            std::optional<ClassSteppedUnseen> tested = findClassToTestFor(m_walker, written, next);
            // A spread of anything else is the last of the row.
            if (!tested || !isListedByAppendMembers(*tested)) {
                end = place + 1;
                break;
            }
            appends.last().members = BuilderOfIteration(m_walker, m_walker.originOf(operation), IteratorKind::Sync, WordingOfIteration::Spread).goOnWhereInstanceIsAsMade(next, *tested, otherArm);
            sets.append(appends.last().members);
            // It goes through no chain, so Walker::walkOperation() does not say this of it.
            if (FunctionID callee = m_walker.facts().linked().calleeAt(m_walker.currentFunction(), operation.id()))
                m_walker.arrivals().leaveAnything(callee);
        }

        if (Reference list = makeListAgain(made, elements->span(), roomFor(numberOfElements, sets.span()))) {
            m_walker.bind(literal, list);
            for (const AppendOfRow& append : appends) {
                if (!append.members) {
                    m_walker.walkStretch(block.id(), append.place, append.place + 1);
                    continue;
                }
                Draft appended = draft(Opcode::AppendMembers);
                appended.setArrayOperand(list);
                appended.setSourceOperand(append.members);
                build(WTF::move(appended));
            }
            if (!m_walker.isUnreachable())
                arms.append({ m_walker.currentBlock(), list });
        }
        if (m_walker.hasFailed())
            return true;

        continueIn(otherArm);
        {
            Walker::ClosedArm closedArm(m_walker);
            if (Reference list = makeListAgain(made, elements->span(), { })) {
                m_walker.bind(literal, list);
                for (uint32_t place = begin; place < end && !m_walker.isUnreachable(); ++place) {
                    bool isSpreadOfSet = appends.containsIf([&](const AppendOfRow& append) {
                        return append.place == place && append.members;
                    });
                    if (!isSpreadOfSet) {
                        m_walker.walkStretch(block.id(), place, place + 1);
                        continue;
                    }
                    BodyValue operation = index.value(ValueID(firstValue + place));
                    Reference taken = m_walker.referenceOf(operation.as<Opcode::AppendSpread>().iterableOperand());
                    BuilderOfIteration(m_walker, m_walker.originOf(operation), IteratorKind::Sync, WordingOfIteration::Spread).appendAllOutOfLine(list, taken);
                }
                if (!m_walker.isUnreachable())
                    arms.append({ m_walker.currentBlock(), list });
            }
        }
        if (m_walker.hasFailed())
            return true;

        BlockID after = addBlock();
        Reference met = arms.isEmpty() ? Reference() : m_walker.addParameter(after, { }, m_walker.formOfParameter(m_walker.code().at(arms[0].list.id()).type, { }));
        for (const ArmOfRow& arm : arms) {
            continueIn(arm.last);
            jumpTo(after, { arm.list });
        }
        continueIn(after);
        m_walker.bind(literal, met ? met : boundBefore);
        // Whoever walks the block goes on behind the row.
        m_walker.walkStretch(block.id(), end, end);
        return true;
    }

    void appendMembersOrAllOutOfLine(Reference array, Reference iterable, const ClassSteppedUnseen& stepped)
    {
        RELEASE_ASSERT(isListedByAppendMembers(stepped));
        BlockID outOfLine = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        Draft append = draft(Opcode::AppendMembers);
        append.setArrayOperand(array);
        append.setSourceOperand(goOnWhereInstanceIsAsMade(iterable, stepped, outOfLine));
        build(WTF::move(append));
        jumpTo(after);
        continueIn(outOfLine);
        appendAllOutOfLine(array, iterable);
        jumpTo(after);
    }

    // One operation of the run time opens and steps, for every spread of the program. It reads a list that nobody can see being iterated by index:
    // its stub asks what a test in line would ask and goes where AppendElements goes, so no test for a list stands in front of it.
    void appendAllOutOfLine(Reference array, Reference iterable)
    {
        Draft check = draft(Opcode::RequireObjectCoercible);
        check.setValueOperand(iterable);
        check.setNullishAccess(NullishAccess::Spread);
        Draft append = draft(Opcode::AppendIterable);
        append.setArrayOperand(array);
        append.setIterableOperand(build(WTF::move(check)));
        append.setReadsListsByIndex(listsAreIteratedByIndex(m_walker));
        build(WTF::move(append));
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
    }

    // The operation of the run time steps from native code, and each step of a generator would enter compiled code anew. The function of the library stays in compiled code from the first step to the last.
    // It is entered with a generator that is as it was made, so that nothing in it can fail but the body: whatever a spread has to say of an iterable that is none, the operation says.
    // False: nothing is built.
    bool tryAppendGeneratorInLibrary(Reference array, Reference generator)
    {
        FunctionID appends = m_walker.isUnreachable() ? FunctionID() : functionOfIterationHelper(m_walker.program(), IterationHelper::AppendGenerator);
        if (!canCallCompilerHelper(m_walker, appends) || !engineAnswersForGenerators(m_walker))
            return false;
        Draft test = draft(Opcode::IsUnmodifiedGenerator);
        test.setValueOperand(generator);
        BlockID isAsMade = addBlock();
        BlockID isChanged = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        branch(build(WTF::move(test)), isAsMade, isChanged);
        continueIn(isAsMade);
        callHelper(appends, { array, generator });
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
        jumpTo(after);
        continueIn(isChanged);
        appendAllOutOfLine(array, generator);
        jumpTo(after);
        return true;
    }

    // The run time lists no object of the program's own in one piece: it would call `next` from native code and read `done` and `value` as native code reads them. The function of the library goes by the protocol as a spread does, in a spread's words.
    // False: nothing is built.
    bool tryAppendObjectOfProgramInLibrary(Reference array, Reference iterable)
    {
        FunctionID appends = m_walker.isUnreachable() ? FunctionID() : functionOfIterationHelper(m_walker.program(), IterationHelper::AppendIterable);
        if (!canCallCompilerHelper(m_walker, appends))
            return false;
        Draft check = draft(Opcode::RequireObjectCoercible);
        check.setValueOperand(iterable);
        check.setNullishAccess(NullishAccess::Spread);
        callHelper(appends, { array, build(WTF::move(check)) });
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
        return true;
    }

    static bool isListedByAppendMembers(const ClassSteppedUnseen& stepped)
    {
        return stepped.builtinClass == BuiltinClasses::Set && stepped.kind == JSC::IterationKind::Values;
    }

    // Behind the last element of a list that the state knows there is room for one more: nothing but a lack of memory stops the append. So the sweep drops it with a list that nothing comes to need.
    void appendToList(Draft&& append)
    {
        Reference array = append.operand(0);
        if (!lengthOfFreshList(m_walker, array) || !isListAsMade(m_walker, array)) {
            build(WTF::move(append));
            return;
        }
        Effects found = declaredBoundOf(append.opcode);
        found.mayThrow = ThrowEffect::onlyForLackOfMemory();
        if (m_walker.isUnreachable())
            return;
        m_walker.chooseConservativeFields(append);
        m_walker.build(WTF::move(append), &found);
    }

    void appendElement(Reference array, Reference value)
    {
        Draft append = draft(Opcode::AppendElement);
        append.setArrayOperand(array);
        append.setValueOperand(value);
        appendToList(WTF::move(append));
    }

    Reference resultsOf(Reference done, Reference value)
    {
        Draft results = draft(Opcode::Results);
        results.setValuesOperands(std::array { done, value });
        return build(WTF::move(results));
    }

    Reference integer(int64_t value)
    {
        Draft written = draft(Opcode::Integer);
        written.setInteger(value);
        return m_walker.constant(WTF::move(written));
    }

    Reference undefinedValue() { return undefined(); }
    void expectNoGenerator() { m_mayStepGenerator = false; }
    void expectNoStepByProtocol() { m_protocolIsForTheOddOne = true; }
    // Link's word: the instance, whose class is proven, has the names, the prototype and the storage of its making in every run. goOnWhereInstanceIsAsMade() asks nothing of it.
    void takeInstanceForAsMade(Reference instance) { m_instanceAsMadeForGood = instance; }
    // `for (.. of map.values())`: what is tested is the receiver. Where a test fails the read of the method, the call and what is written of its type are walked as they stand, [begin, end) of the block, and what they give is opened.
    void openWhatCallGivesByProtocol(BlockID inputBlock, uint32_t begin, uint32_t end, ValueID given)
    {
        m_blockOfCallThatMakesIterator = inputBlock;
        m_placeOfReadOfMethod = begin;
        m_placeBehindCallThatMakesIterator = end;
        m_givenByCallThatMakesIterator = given;
    }
    Reference booleanValue(bool value) { return boolean(value); }

private:
    // ---- What everything below is made of
    // An arm may end early: an operation gives what is of the type never, or the walk fails. From there on the Walker is unreachable, nothing is built and null stands for every value,
    // until continueIn() or jumpTo() enters a block that something has arrived at. So every arm is built to its end, whatever became of the one in front of it.

    Draft draft(Opcode opcode) const { return Draft(opcode, m_origin); }

    Reference build(Draft&& operation)
    {
        if (m_walker.isUnreachable())
            return { };
        m_walker.chooseConservativeFields(operation);
        std::optional<Effects> ofAppend = effectsOfAppendToListAsMade(m_walker, operation);
        return m_walker.build(WTF::move(operation), ofAppend ? &*ofAppend : nullptr);
    }

    // It goes to parameters of joins and into heap words, which hold boxed values.
    Reference constant(Draft&& written)
    {
        written.representation = Representation::JSValue;
        return m_walker.constant(WTF::move(written));
    }

    Reference undefined() { return constant(draft(Opcode::Undefined)); }

    Reference boolean(bool value)
    {
        Draft written = draft(Opcode::Boolean);
        written.setBoolean(value);
        return constant(WTF::move(written));
    }

    Reference read(Reference object, PropertyKeyID key)
    {
        Draft access = draft(Opcode::PropGet);
        access.setObjectOperand(object);
        access.setKey(key);
        return build(WTF::move(access));
    }

    Reference read(Reference object, BuiltinName name) { return read(object, m_walker.program().keyOf(name)); }
    Reference read(Reference object, WellKnownSymbol symbol) { return read(object, m_walker.types().wellKnownSymbolKey(symbol)); }

    Reference call(Reference method, Reference receiver, std::span<const Reference> arguments = { })
    {
        Draft invocation = draft(Opcode::Call);
        invocation.setCalleeOperand(method);
        invocation.setThisValueOperand(receiver);
        invocation.setArgumentsOperands(arguments);
        return build(WTF::move(invocation));
    }

    // THE PROTOCOL IS WRITTEN OUT ONCE, IN THE LIBRARY. A loop or a pattern over what nobody knows calls the functions that do its parts, each in the order of the language and with nothing else that a getter or a Proxy would see.
    // Null: the protocol is written out here: the iteration is asynchronous, the site words its errors otherwise or reads and calls more before it throws, or the program does not hold the function.
    FunctionID findHelper(IterationHelper helper)
    {
        if (m_kind != IteratorKind::Sync || m_wording != WordingOfIteration::Source || m_walker.isUnreachable())
            return { };
        FunctionID function = functionOfIterationHelper(m_walker.program(), helper);
        return canCallCompilerHelper(m_walker, function) ? function : FunctionID();
    }

    Reference callHelper(FunctionID helper, std::initializer_list<Reference> arguments)
    {
        return m_walker.isUnreachable() ? Reference() : callCompilerHelper(m_walker, helper, std::span<const Reference>(arguments.begin(), arguments.size()), m_origin);
    }

    Reference testType(Reference value, TypeID tested)
    {
        Draft test = draft(Opcode::TypeTest);
        test.setValueOperand(value);
        test.setTestedType(tested);
        return build(WTF::move(test));
    }

    Draft draftOfObjectCheck(Reference value, RuntimeErrorKind error) const
    {
        Draft check = draft(Opcode::RequireObject);
        check.setValueOperand(value);
        check.setRuntimeError(error);
        return check;
    }

    Reference requireObject(Reference value, RuntimeErrorKind error) { return build(draftOfObjectCheck(value, error)); }

    Reference await(Reference value)
    {
        switch (m_kind) {
        case IteratorKind::Sync:
            return value;
        case IteratorKind::Async:
            break;
        }
        if (m_walker.isUnreachable())
            return { };
        return buildAwaitBySavingFrame(m_walker, value, m_held, false, m_origin);
    }

    // ---- Control

    BlockID addBlock(BlockFrequency frequency = BlockFrequency::Normal)
    {
        BlockID block = m_walker.addBlock(frequency);
        if (m_isInLoopOfSpread)
            m_walker.code().setLoopMarks(block, true, false);
        return block;
    }

    // The Walker leads an edge to the handler of the block being walked in front of the first operation that may throw, and only out of a block that has no handler edge.
    // So a block that is entered while m_handler is set gets its edge here, in front of its first operation.
    void standUnderHandler()
    {
        if (!m_handler || m_walker.isUnreachable())
            return;
        Code& code = m_walker.code();
        BlockID block = m_walker.currentBlock();
        if (!code.at(block).handler)
            code.closeEdge(code.addHandlerEdge(block), m_handler, { });
    }

    void branch(Reference condition, BlockID taken, BlockID notTaken) { m_walker.branch(condition, taken, notTaken); }

    void continueIn(BlockID block)
    {
        m_walker.continueIn(block);
        standUnderHandler();
    }

    void jumpTo(BlockID block)
    {
        m_walker.jumpTo(block);
        standUnderHandler();
    }

    // The walk is left in a block that has ended: whoever goes on calls continueIn().
    void jumpBackTo(BlockID head)
    {
        if (m_walker.isUnreachable())
            return;
        Code& code = m_walker.code();
        BlockID block = m_walker.currentBlock();
        code.end(block, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
        code.closeEdge(code.at(block).terminator.next(), head, { });
    }

    // What it gives is of the type never, and nobody asks for it. Whoever goes on calls continueIn().
    void buildThrowingOperation(Draft&& throwing)
    {
        build(WTF::move(throwing));
        if (!m_walker.isUnreachable())
            m_walker.code().end(m_walker.currentBlock(), TerminatorDraft(TerminatorKind::Unreachable, m_origin), 0);
    }

    // RequireObject of what is no object is the closed form of a throw of its error.
    void throwError(RuntimeErrorKind error) { buildThrowingOperation(draftOfObjectCheck(undefined(), error)); }

    // Walker::jumpTo() gives each parameter the join of what arrives.
    JoinOfIteration openJoin()
    {
        JoinOfIteration join;
        join.block = addBlock();
        for (Reference& result : join.results)
            result = m_walker.addParameter(join.block, TypeTable::anything, Representation::JSValue);
        return join;
    }

    void jumpTo(const JoinOfIteration& join, Reference first, Reference second)
    {
        std::array<Reference, 2> arguments { first, second };
        m_walker.jumpTo(join.block, arguments);
        standUnderHandler();
    }

    void jumpTo(BlockID block, std::span<const Reference> arguments)
    {
        m_walker.jumpTo(block, arguments);
        standUnderHandler();
    }

    void jumpTo(BlockID block, std::initializer_list<Reference> arguments) { jumpTo(block, std::span<const Reference>(arguments.begin(), arguments.size())); }

    // ---- What the forms by index are made of

    std::optional<int64_t> integerIn(Reference reference) const
    {
        if (!reference || !reference.isConstant() || m_walker.code().at(reference.id()).opcode != Opcode::Integer)
            return std::nullopt;
        return m_walker.code().at(reference.id()).integer();
    }

    // An operation whose fields are chosen.
    Reference buildAsItIs(Draft&& operation)
    {
        return m_walker.isUnreachable() ? Reference() : m_walker.build(WTF::move(operation));
    }

    Reference operatorOnNumbers(Opcode opcode, Reference left, Reference right = { })
    {
        if (m_walker.isUnreachable())
            return { };
        Draft operation = draft(opcode);
        m_walker.chooseConservativeFields(operation);
        operation.setOperand(0, left);
        if (right)
            operation.setOperand(1, right);
        operation.setForm(formAllowedForTypedOperator(m_walker, operation));
        return boxedUnlessNumbersAreUnboxed(m_walker, m_walker.build(WTF::move(operation)), m_origin);
    }

    // The walk stands in a block that the taken edge of a branch on the test alone enters.
    Reference narrowed(Reference value, Reference test, uint32_t operandIndex)
    {
        Draft proof = draft(Opcode::Narrowed);
        proof.setValueOperand(value);
        proof.setTestOperand(test);
        proof.setOperandIndex(operandIndex);
        proof.setIsOnTakenEdge(true);
        return buildAsItIs(WTF::move(proof));
    }

    TypeID typeOfLists() { return m_walker.types().builtinClass(BuiltinClasses::Array); }

    // As makeListAgainInKnownLeaving(): where nothing needs the first allocation any more the sweep drops it. `room`: null for none.
    Reference makeListAgain(const BodyValue& made, std::span<const Reference> elements, Reference room)
    {
        if (m_walker.isUnreachable())
            return { };
        Draft again = m_walker.draftOf(made);
        again.standsForNoValueOfInput = true;
        again.inputValueGiven = { };
        again.setElementsOperands(elements);
        again.setCapacityOperand(room);
        m_walker.chooseConservativeFields(again);
        Reference built = m_walker.build(WTF::move(again));
        if (built)
            noteFreshList(m_walker, built);
        return built;
    }

    // What the getter of `size` gives, by its code in line, whatever the name finds by now. Null: the table has no such row.
    Reference sizeOfSet(Reference set)
    {
        const BuiltinTable& table = m_walker.program().builtins();
        BuiltinOverloadID getter;
        for (BuiltinOverloadID overload : table.overloadIDsOf(table.canonicalID(Builtins::SetPrototypeSize))) {
            if (table.at(overload).mode == BuiltinCallMode::Get && table.at(overload).code.implementation.inlineBuiltin == InlineBuiltin::SetSize)
                getter = overload;
        }
        if (!getter || !set || m_walker.isUnreachable())
            return { };
        Draft read = draft(Opcode::CallInline);
        read.setInlineBuiltin(InlineBuiltin::SetSize);
        read.setOverload(getter);
        read.setReceiverOperand(set);
        Effects readsTable = Effects::none();
        readsTable.reads.add(AbstractHeap::CollectionStorage);
        Reference size = m_walker.build(WTF::move(read), &readsTable);
        return size && m_walker.types().at(m_walker.typeOf(size)).isInteger() ? size : Reference();
    }

    // Both are proven integers. Null where integer registers may not hold the sum: no hint is worth a test.
    Reference sumInIntegerRegisters(Reference left, Reference right)
    {
        if (integerIn(left) == std::optional<int64_t>(0))
            return right;
        if (m_walker.isUnreachable())
            return { };
        Draft sum = draft(Opcode::Add);
        m_walker.chooseConservativeFields(sum);
        sum.setLeftOperand(left);
        sum.setRightOperand(right);
        if (formAllowedForTypedOperator(m_walker, sum) != OperatorForm::Integers)
            return { };
        sum.setForm(OperatorForm::Integers);
        Reference result = m_walker.build(WTF::move(sum));
        return result && m_walker.types().at(m_walker.typeOf(result)).isInteger() ? result : Reference();
    }

    // Room is a hint that no program sees. Null: none is asked for.
    Reference roomFor(int64_t numberOfElements, std::span<const Reference> sets)
    {
        Reference room = integer(numberOfElements);
        for (Reference set : sets) {
            Reference size = sizeOfSet(set);
            room = size ? sumInIntegerRegisters(room, size) : Reference();
            if (!room)
                return { };
        }
        return room;
    }

    // The walk goes on where the test passes, with what it leaves of the value.
    Reference goOnWhereTypeTestPasses(Reference value, TypeID tested, BlockID otherwise)
    {
        Reference test = testType(value, tested);
        BlockID passed = addBlock();
        branch(test, passed, otherwise);
        continueIn(passed);
        return narrowed(value, test, OperandIndex::TypeTest::value);
    }

    Reference goOnWhereParameterHoldsInstance(const Walker::IterationByIndex& iteration, BlockID otherwise)
    {
        return goOnWhereTypeTestPasses(iteration.list, typeOfInstancesStepped(m_walker, iteration.builtinClass, iteration.contents), otherwise);
    }

    // One bit of the cell says that an instance has no name of its own but those of its creation and stands below the prototypes that it was made below. Another says of a list that it has no accessor among its elements.
    Reference goOnWhereInstanceIsAsMade(Reference iterable, const ClassSteppedUnseen& stepped, BlockID otherwise)
    {
        TypeID instances = typeOfInstancesStepped(m_walker, stepped.builtinClass, stepped.contents);
        Reference instance = m_walker.types().isSubset(m_walker.typeOf(iterable), instances) ? iterable : goOnWhereTypeTestPasses(iterable, instances, otherwise);
        if (stepped.contents == ContentsStepped::Text || (instance == iterable && iterable == m_instanceAsMadeForGood))
            return instance;
        bool isList = stepped.contents == ContentsStepped::List;
        Draft test = draft(Opcode::IsPristine);
        test.setValueOperand(instance);
        test.setBuiltinClass(stepped.builtinClass);
        test.setAspects(isList ? PristineAspectSet { PristineAspects::Storage, PristineAspects::Names } : PristineAspectSet { PristineAspects::Names });
        BlockID passed = addBlock();
        branch(build(WTF::move(test)), passed, otherwise);
        continueIn(passed);
        if (isList && !m_walker.isUnreachable())
            noteListAsMade(m_walker, instance);
        return instance;
    }

    static bool stepsThroughTable(ContentsStepped contents) { return contents == ContentsStepped::Map || contents == ContentsStepped::Set; }

    Reference tableOf(Reference collection)
    {
        Draft table = draft(Opcode::CollectionTable);
        table.setCollectionOperand(collection);
        return build(WTF::move(table));
    }

    void requireAttached(Reference view)
    {
        Draft check = draft(Opcode::RequireAttachedTypedArray);
        check.setViewOperand(view);
        build(WTF::move(check));
    }

    // What the method under @@iterator does beside making the iterator: it throws for a view that has lost its memory. The table that the first step begins in, of a Map or a Set.
    Reference beginByIndex(Reference instance, ContentsStepped contents)
    {
        if (contents == ContentsStepped::View)
            requireAttached(instance);
        return stepsThroughTable(contents) ? tableOf(instance) : Reference();
    }

    bool readsOfListRunNoCode(const Walker::IterationByIndex& iteration)
    {
        if (iteration.mayGoByProtocol || iteration.contents != ContentsStepped::List)
            return false;
        if (iteration.isAsMadeForGood && indexesAreAbsentAboveLists(m_walker))
            return true;
        return iteration.literal || (isFreshInIteration(m_walker, iteration.list) && isListAsMade(m_walker, iteration.list));
    }

    std::optional<uint32_t> lengthKnownOf(const Walker::IterationByIndex& iteration)
    {
        if (iteration.mayGoByProtocol || iteration.contents != ContentsStepped::List)
            return std::nullopt;
        if (iteration.literal)
            return lengthOfLiteralInIteration(m_walker.currentBodyIndex(), iteration.literal);
        return lengthOfListInSight(m_walker, iteration.list);
    }

    TypedArrayType typedArrayTypeOfView(Reference view) { return typedArrayTypeOf(BuiltinTypeContext { m_walker.program(), m_walker.types() }, m_walker.typeOf(view)); }

    // It is read anew at each step, as the protocol reads it. A view that has lost its memory, or that reaches beyond it, has the length 0.
    Reference lengthOf(const Walker::IterationByIndex& iteration, Reference list)
    {
        if (m_walker.isUnreachable())
            return { };
        if (iteration.contents == ContentsStepped::View) {
            Draft length = draft(Opcode::GetTypedArrayLength);
            length.setViewOperand(list);
            length.setTypedArrayType(typedArrayTypeOfView(list));
            return buildAsItIs(WTF::move(length));
        }
        if (iteration.contents == ContentsStepped::Text) {
            Draft length = draft(Opcode::GetStringLength);
            length.setStringOperand(list);
            return buildAsItIs(WTF::move(length));
        }
        Draft length = draft(Opcode::GetArrayLength);
        length.setArrayOperand(list);
        return buildAsItIs(WTF::move(length));
    }

    // A view answers for an index without running code.
    Reference elementOfView(Reference view, Reference position)
    {
        if (m_walker.isUnreachable())
            return { };
        Draft read = draft(Opcode::GetTypedArrayElement);
        read.setViewOperand(view);
        read.setPositionOperand(position);
        read.setTypedArrayType(typedArrayTypeOfView(view));
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        return buildAsItIs(WTF::move(read));
    }

    // `position` is an index. `runsNoCode`: nothing but this code has the list, and it is as it was made, so a hole is undefined.
    // Otherwise the read finds an element of the list's own by itself and does the closed operation for whatever else it meets, as a step of the protocol does.
    // `isInsideLength`: the position was found below the length that the list has where the read stands.
    Reference elementAt(Reference list, Reference position, bool runsNoCode, bool isInsideLength = false)
    {
        if (m_walker.isUnreachable())
            return { };
        if (std::optional<int64_t> known = integerIn(position)) {
            Reference held = elementOfListInSight(m_walker, list, static_cast<uint32_t>(*known));
            if (held && !(m_walker.types().at(m_walker.typeOf(held)).primitives & Primitive::Absent))
                return held;
        }

        KnowledgeLevel level = runsNoCode ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
        ArrayStorage storage = runsNoCode ? ArrayStorage::Other : arrayStorageDeclaredBy(m_walker.program(), m_walker.types(), m_walker.typeOf(list)).predicted();
        if (storage == ArrayStorage::Int32)
            storage = ArrayStorage::Double;
        Draft read = draft(Opcode::GetArrayElement);
        read.setArrayOperand(list);
        read.setPositionOperand(position);
        read.setArrayStorage(storage);
        read.setAdmitsInt32Storage(storage == ArrayStorage::Double);
        read.setLevel(level);
        read.setKeyIsIntegral(true);
        read.setIsInBounds(isInsideLength && level == KnowledgeLevel::Expected);
        if (!runsNoCode)
            m_walker.letLeave(list);
        return m_walker.build(WTF::move(read));
    }

    Reference resultAt(Reference results, MapNextResult index, TypeID provenByLink = { })
    {
        Draft read = draft(Opcode::ResultAt);
        read.setResultsOperand(results);
        read.setIndex(static_cast<uint32_t>(index));
        TypeID before = m_walker.exchangeContentsBeingTyped(provenByLink);
        Reference built = build(WTF::move(read));
        m_walker.exchangeContentsBeingTyped(before);
        return built;
    }

    // As %MapIteratorPrototype%.next and %SetIteratorPrototype%.next step. Every way ends in `after`, with `done`, `value`, the position of the next step and the table that it counts in.
    void stepThroughTable(const Walker::IterationByIndex& iteration, Reference collection, BlockID after)
    {
        static_assert(static_cast<uint32_t>(MapNextResult::HasEntry) == static_cast<uint32_t>(SetNextResult::HasEntry) && static_cast<uint32_t>(MapNextResult::Entry) == static_cast<uint32_t>(SetNextResult::Entry) && static_cast<uint32_t>(MapNextResult::Key) == static_cast<uint32_t>(SetNextResult::Element));
        // An iteration that has answered done stays done, and looks at nothing any more. Where no step follows the last one nobody has to ask.
        if (!iteration.isOpenAtEveryStepAndClose) {
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(iteration.table);
            BlockID finished = addBlock();
            BlockID stepping = addBlock();
            branch(build(WTF::move(isClosed)), finished, stepping);
            continueIn(finished);
            jumpTo(after, { boolean(true), undefined(), iteration.position, iteration.table });
            continueIn(stepping);
        }
        bool isMap = iteration.contents == ContentsStepped::Map;
        Draft next = draft(isMap ? Opcode::MapNext : Opcode::SetNext);
        next.setOperand(0, collection);
        next.setTableOperand(iteration.table);
        next.setEntryOperand(iteration.position);
        Reference found = build(WTF::move(next));
        BlockID giving = addBlock();
        BlockID ending = addBlock();
        branch(resultAt(found, MapNextResult::HasEntry), giving, ending);
        continueIn(ending);
        jumpTo(after, { boolean(true), undefined(), iteration.position, iteration.isOpenAtEveryStepAndClose ? undefined() : build(draft(Opcode::CollectionIteratorClosedMark)) });

        continueIn(giving);
        Reference key = resultAt(found, MapNextResult::Key, isMap ? iteration.keysProven : iteration.valuesProven);
        Reference value = isMap ? resultAt(found, MapNextResult::Value, iteration.valuesProven) : key;
        Reference given = iteration.kind == JSC::IterationKind::Keys ? key : value;
        if (iteration.kind == JSC::IterationKind::Entries) {
            Draft pair = draft(Opcode::NewArray);
            pair.setContents(TypeTable::anything);
            pair.setElementsOperands(std::array { key, value });
            given = build(WTF::move(pair));
        }
        Reference behind = operatorOnNumbers(Opcode::Increment, resultAt(found, MapNextResult::Entry));
        // The entry counts in the table that the collection has now: the step follows a table that was replaced to its successor.
        jumpTo(after, { boolean(false), given, behind, tableOf(collection) });
    }

    Reference codeUnitAt(Reference text, Reference position)
    {
        Draft read = draft(Opcode::GetStringCodeUnit);
        read.setStringOperand(text);
        read.setPositionOperand(position);
        return build(WTF::move(read));
    }

    Reference isSurrogate(Reference codeUnit, int64_t firstOfKind)
    {
        return operatorOnNumbers(Opcode::StrictEq, operatorOnNumbers(Opcode::BitAnd, codeUnit, integer(0xFC00)), integer(firstOfKind));
    }

    // A step of %StringIteratorPrototype%.next hands out one code point: two code units where a trail surrogate stands behind a lead surrogate, else one. `position` lies inside the text.
    Reference endOfCodePointAt(Reference text, Reference length, Reference position)
    {
        BlockID found = addBlock();
        Reference end = m_walker.addParameter(found, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::positionBehindEveryEnd }), { }));
        Reference behindFirst = operatorOnNumbers(Opcode::Increment, position);

        BlockID single = addBlock();
        BlockID afterLead = addBlock(BlockFrequency::Unlikely);
        branch(isSurrogate(codeUnitAt(text, position), 0xD800), afterLead, single);
        continueIn(afterLead);
        Reference hasSecond = operatorOnNumbers(Opcode::Less, behindFirst, length);
        BlockID second = addBlock();
        branch(hasSecond, second, single);
        continueIn(second);
        Reference inside = narrowed(behindFirst, hasSecond, OperandIndex::Less::left);
        BlockID pair = addBlock();
        branch(isSurrogate(codeUnitAt(text, inside), 0xDC00), pair, single);
        continueIn(pair);
        jumpTo(found, { operatorOnNumbers(Opcode::Increment, inside) });
        continueIn(single);
        jumpTo(found, { behindFirst });
        return end;
    }

    // A step throws for a view that has lost its memory. Such a view has no element, so only the step that finds none asks. An iteration that has answered done stays done, and looks at nothing any more.
    void requireAttachedUnlessDone(Reference view, Reference position)
    {
        if (std::optional<int64_t> known = integerIn(position)) {
            if (*known < Walker::positionBehindEveryEnd)
                requireAttached(view);
            return;
        }
        BlockID asking = addBlock();
        BlockID asked = addBlock();
        branch(operatorOnNumbers(Opcode::Less, position, integer(Walker::positionBehindEveryEnd)), asking, asked);
        continueIn(asking);
        requireAttached(view);
        jumpTo(asked);
    }

    // Every way ends in `after`, with `done`, `value` and the position of the next step.
    void stepByIndex(const Walker::IterationByIndex& iteration, Reference list, Reference length, bool runsNoCode, BlockID after)
    {
        Reference position = iteration.position;
        Reference isInside = operatorOnNumbers(Opcode::Less, position, length);
        BlockID reading = addBlock();
        BlockID ending = addBlock();
        branch(isInside, reading, ending);
        continueIn(ending);
        if (iteration.contents == ContentsStepped::View)
            requireAttachedUnlessDone(list, position);
        jumpTo(after, { boolean(true), undefined(), integer(Walker::positionBehindEveryEnd) });

        continueIn(reading);
        std::optional<int64_t> known = integerIn(position);
        Reference inside = known ? position : narrowed(position, isInside, OperandIndex::Less::left);
        if (iteration.contents == ContentsStepped::Text) {
            Reference end = endOfCodePointAt(list, length, inside);
            Draft piece = draft(Opcode::Substring);
            piece.setStringOperand(list);
            piece.setStartOperand(inside);
            piece.setEndOperand(end);
            jumpTo(after, { boolean(false), build(WTF::move(piece)), end });
            return;
        }
        Reference value = iteration.contents == ContentsStepped::View ? elementOfView(list, inside) : elementAt(list, inside, runsNoCode, true);
        Reference next = known ? integer(*known + 1) : operatorOnNumbers(Opcode::Increment, inside);
        jumpTo(after, { boolean(false), value, next });
    }

    // The arm of a step beside stepByIndex(). It ends in `after` too, and leaves the walk there or nowhere.
    void stepByProtocolBesideIndex(const Walker::IterationByIndex& iteration, BlockID after)
    {
        JoinOfIteration stepped = openJoin();
        if (iteration.state) {
            takeStateInList(iteration.state);
            m_listOfStateHoldsVerdictOnNext = iteration.stateHoldsVerdictOnNext;
            stepUnlessDone({ }, false, stepped);
            endStepByProtocol(iteration, after, stepped.results[0], stepped.results[1], iteration.position);
            return;
        }

        // No step follows the last one, and no close asks: the position is that of the arm that reads by index alone.
        if (iteration.isOpenAtEveryStepAndClose) {
            Reference isLast = stepped.results[0];
            Reference handedOut = stepped.results[1];
            // The arm is there for the odd one: one call, and the mark of a closed iterator says whether the step was the last. Nothing meets in between.
            if (FunctionID steps = m_protocolIsForTheOddOne ? findHelper(IterationHelper::StepIterator) : FunctionID()) {
                handedOut = callHelper(steps, { iteration.iterator, iteration.next });
                Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
                isClosed.setTableOperand(handedOut);
                isLast = build(WTF::move(isClosed));
            } else
                step(iteration.iterator, iteration.next, { }, false, stepped);
            // Each way says whether it is the last in so many words: the branch behind the join goes by that, and the arm that reads by index pays nothing for this one.
            BlockID last = addBlock();
            BlockID further = addBlock();
            branch(isLast, last, further);
            continueIn(last);
            endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::positionBehindEveryEnd));
            continueIn(further);
            // Nobody reads it. A constant holds no register round the arm that reads by index.
            endStepByProtocol(iteration, after, boolean(false), handedOut, integer(0));
            return;
        }

        // Nothing in memory: the position says whether the iterator is done.
        BlockID stepping = addBlock();
        BlockID finished = addBlock();
        branch(operatorOnNumbers(Opcode::Less, iteration.position, integer(Walker::positionBehindEveryEnd)), stepping, finished);
        continueIn(finished);
        jumpTo(stepped, boolean(true), undefined());
        continueIn(stepping);
        step(iteration.iterator, iteration.next, { }, false, stepped);

        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(stepped.results[0], last, further);
        continueIn(last);
        endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::positionBehindEveryEnd));
        continueIn(further);
        endStepByProtocol(iteration, after, boolean(false), stepped.results[1], integer(0));
    }

    void endStepByProtocol(const Walker::IterationByIndex& iteration, BlockID after, Reference done, Reference value, Reference position)
    {
        if (iteration.table)
            jumpTo(after, { done, value, position, iteration.table });
        else
            jumpTo(after, { done, value, position });
    }

    // ---- The state

    // verifyTypesOfCode() follows a word by the record of its cell, so the state is the cell itself and no parameter that brings it.
    void takeState(Reference state)
    {
        if (!state || !state.isValue() || m_walker.code().at(state.id()).opcode != Opcode::NewHeapWords) {
            m_walker.fail("an iteration that the protocol has not opened is stepped or closed by the protocol"_s);
            return;
        }
        m_state = state;
    }

    // Beside an iteration by index a parameter brings the state to the steps, and it brings no cell. So the state is a list, which this code alone has. The walk stands where the protocol has opened the iteration.
    void takeStateInList(Reference parameter, bool holdsVerdictOnNext = false)
    {
        Reference test = testType(parameter, typeOfLists());
        BlockID holdsList = addBlock();
        BlockID never = addBlock(BlockFrequency::Rare);
        branch(test, holdsList, never);
        continueIn(never);
        if (!m_walker.isUnreachable())
            m_walker.code().end(never, TerminatorDraft(TerminatorKind::Unreachable, m_origin), 0);
        continueIn(holdsList);
        m_state = narrowed(parameter, test, OperandIndex::TypeTest::value);
        m_stateIsList = true;
        m_listOfStateHoldsVerdictOnNext = holdsVerdictOnNext;
    }

    Reference readWord(WordOfIteration word)
    {
        if (m_stateIsList) {
            // The list holds an object, so its storage holds any value, and nobody has taken one of its three elements away.
            Draft read = draft(Opcode::GetArrayElement);
            read.setArrayOperand(m_state);
            read.setPositionOperand(integer(static_cast<uint32_t>(word)));
            read.setArrayStorage(ArrayStorage::Contiguous);
            read.setLevel(KnowledgeLevel::Proven);
            read.setKeyIsIntegral(true);
            read.setIsInBounds(true);
            read.setIsPresent(true);
            return buildAsItIs(WTF::move(read));
        }
        Draft load = draft(Opcode::GetHeapFrameWord);
        load.setFrameOperand(m_state);
        load.setIndex(static_cast<uint32_t>(word));
        load.setValueType(word == WordOfIteration::IsDone || word == WordOfIteration::StepsInLibrary ? TypeTable::boolean : TypeTable::anything);
        // The word holds a boxed value, whatever form the type has elsewhere.
        load.representation = Representation::JSValue;
        return build(WTF::move(load));
    }

    void storeWord(WordOfIteration word, Reference value)
    {
        if (m_stateIsList) {
            Draft put = draft(Opcode::PutArrayElement);
            put.setArrayOperand(m_state);
            put.setPositionOperand(integer(static_cast<uint32_t>(word)));
            put.setValueOperand(value);
            put.setArrayStorage(ArrayStorage::Contiguous);
            put.setMode(ECMAMode::Strict);
            put.setLevel(KnowledgeLevel::Proven);
            put.setKeyIsIntegral(true);
            put.setIsInBounds(true);
            put.setStorageIsWritable(true);
            buildAsItIs(WTF::move(put));
            return;
        }
        Draft store = draft(Opcode::PutHeapFrameWord);
        store.setFrameOperand(m_state);
        store.setIndex(static_cast<uint32_t>(word));
        store.setValueOperand(value);
        store.setRemembersFrame(word == WordOfIteration::Iterator || word == WordOfIteration::Next);
        build(WTF::move(store));
    }

    // ---- Open

    OpenedIteration open(Reference iterable)
    {
        iterable = requireObjectCoercible(iterable);

        switch (m_wording) {
        case WordingOfIteration::Spread: {
            // The engine lists a text, and a view whose iteration nobody has replaced, in one piece, and throws in front of the first element where no list holds them all.
            Draft fits = draft(Opcode::RequireLengthFitsList);
            fits.setSourceOperand(iterable);
            build(WTF::move(fits));
            break;
        }
        case WordingOfIteration::Source:
        case WordingOfIteration::Delegation:
            break;
        }

        switch (m_kind) {
        case IteratorKind::Sync:
            return openSync(iterable, UseOfSyncIteration::Stepped);
        case IteratorKind::Async:
            return openAsync(iterable);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    OpenedIteration withNextMethod(Reference iterator) { return { iterator, read(iterator, BuiltinNames::next) }; }

    // `method` is what was read under @@iterator, and is no function.
    void throwForMissingMethod(Reference method, Reference iterable, UseOfSyncIteration use)
    {
        switch (use) {
        case UseOfSyncIteration::WrappedForAsyncIteration:
            throwError(RuntimeErrorKind::NoSyncIteratorForAsyncIteration);
            return;
        case UseOfSyncIteration::Stepped:
            break;
        }

        switch (m_wording) {
        case WordingOfIteration::Delegation:
            // `yield*` calls whatever it has read, so the error is the one that the call throws.
            call(method, iterable);
            throwError(RuntimeErrorKind::IteratorIsNotAnObject);
            return;
        case WordingOfIteration::Source:
        case WordingOfIteration::Spread:
            break;
        }
        Draft error = draft(Opcode::ThrowNotIterable);
        error.setIterableOperand(iterable);
        error.setIsSpread(m_wording == WordingOfIteration::Spread);
        buildThrowingOperation(WTF::move(error));
    }

    // `iterable` is neither undefined nor null.
    OpenedIteration openSync(Reference iterable, UseOfSyncIteration use)
    {
        if (FunctionID opens = use == UseOfSyncIteration::Stepped ? findHelper(IterationHelper::OpenIterator) : FunctionID()) {
            // Undefined: there is no method to call.
            Reference iterator = callHelper(opens, { iterable });
            BlockID opened = addBlock();
            BlockID lacksMethod = addBlock(BlockFrequency::Rare);
            branch(testType(iterator, TypeTable::undefined), lacksMethod, opened);
            continueIn(lacksMethod);
            throwForMissingMethod(iterator, iterable, use);
            continueIn(opened);
            return withNextMethod(iterator);
        }

        Reference method = read(iterable, WellKnownSymbol::Iterator);
        BlockID callable = addBlock();
        BlockID missing = addBlock(BlockFrequency::Rare);
        branch(testType(method, TypeTable::function), callable, missing);
        continueIn(missing);
        throwForMissingMethod(method, iterable, use);

        continueIn(callable);
        Reference iterator = call(method, iterable);
        switch (use) {
        case UseOfSyncIteration::WrappedForAsyncIteration:
            return withNextMethod(requireObject(iterator, RuntimeErrorKind::SyncIteratorForAsyncIterationIsNotAnObject));
        case UseOfSyncIteration::Stepped:
            break;
        }
        switch (m_wording) {
        case WordingOfIteration::Source:
            return withNextMethod(requireObject(iterator, RuntimeErrorKind::IteratorIsNotAnObject));
        case WordingOfIteration::Spread:
        case WordingOfIteration::Delegation:
            break;
        }

        // The engine's spread and `yield*` read `next` off whatever the method returns, and call it, before they throw because it is not an object.
        BlockID isObject = addBlock();
        BlockID isNoObject = addBlock(BlockFrequency::Rare);
        branch(testType(iterator, TypeTable::object), isObject, isNoObject);
        continueIn(isNoObject);
        call(read(iterator, BuiltinNames::next), iterator);
        throwError(RuntimeErrorKind::IteratorIsNotAnObject);

        continueIn(isObject);
        return withNextMethod(iterator);
    }

    // `iterable` is neither undefined nor null.
    OpenedIteration openAsync(Reference iterable)
    {
        Reference method = read(iterable, WellKnownSymbol::AsyncIterator);
        BlockID absent = addBlock();
        BlockID present = addBlock();
        JoinOfIteration join = openJoin();
        branch(testType(method, TypeTable::nullish), absent, present);

        continueIn(absent);
        OpenedIteration sync = openSync(iterable, UseOfSyncIteration::WrappedForAsyncIteration);
        Draft wrapper = draft(Opcode::CreateAsyncFromSyncIterator);
        wrapper.setIteratorOperand(sync.iterator);
        wrapper.setNextOperand(sync.next);
        OpenedIteration wrapped = withNextMethod(build(WTF::move(wrapper)));
        jumpTo(join, wrapped.iterator, wrapped.next);

        continueIn(present);
        OpenedIteration opened = withNextMethod(requireObject(call(method, iterable), RuntimeErrorKind::IteratorIsNotAnObject));
        jumpTo(join, opened.iterator, opened.next);
        return { join.results[0], join.results[1] };
    }

    // ---- Step. Every way that goes on ends in `join`, with `done` and `value`. A jump there is what is built last, so the walk goes on in `join` if any way does.

    // `result` is what the method returned. `value` is read only where the step is not the last, unless the step as written reads the last value too.
    void readStepResult(Reference result, bool readsValueWhenDone, const JoinOfIteration& join) { readAwaitedStepResult(await(result), readsValueWhenDone, join); }

    void readAwaitedStepResult(Reference result, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        Reference object = requireObject(result, m_wording == WordingOfIteration::Spread ? RuntimeErrorKind::SpreadResultNotObject : RuntimeErrorKind::IteratorResultNotObject);
        Draft conversion = draft(Opcode::ToBoolean);
        conversion.setValueOperand(read(object, BuiltinNames::done));
        Reference done = build(WTF::move(conversion));

        if (readsValueWhenDone) {
            Reference value = read(object, BuiltinNames::value);
            if (m_state)
                storeWord(WordOfIteration::IsDone, done);
            jumpTo(join, done, value);
            return;
        }

        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(done, last, further);
        continueIn(last);
        jumpTo(join, boolean(true), undefined());

        // A read of `value` that throws leaves the state done too.
        continueIn(further);
        Reference value = read(object, BuiltinNames::value);
        if (m_state)
            storeWord(WordOfIteration::IsDone, boolean(false));
        jumpTo(join, boolean(false), value);
    }

    // Where nothing is sent `next` is called without an argument, which it can tell apart from undefined.
    void step(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (!stepIteratorOfEngineInLibrary(iterator, next, sent, readsValueWhenDone, join))
            stepGeneratorWhereNextIsOfEngine(iterator, next, sent, readsValueWhenDone, join);
        if (FunctionID steps = m_protocolIsForTheOddOne && !sent && !readsValueWhenDone ? findHelper(IterationHelper::StepIterator) : FunctionID()) {
            // What the iterator hands out, or the mark of a closed iterator, which no program ever holds. No object is made for a step of an iterator of the engine.
            Reference handedOut = callHelper(steps, { iterator, next });
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(handedOut);
            endStepWithoutResultObject(build(WTF::move(isClosed)), handedOut, false, join);
            return;
        }
        std::span<const Reference> arguments;
        if (sent)
            arguments = singleElementSpan(sent);
        readStepResult(call(next, iterator, arguments), readsValueWhenDone, join);
    }

    TypeID methodsSteppedInLibrary()
    {
        TypeArena& types = m_walker.types();
        const BuiltinTable& table = m_walker.program().builtins();
        return types.unionOf(std::array {
            types.forBuiltin(table.canonicalID(Builtins::GeneratorPrototypeNext)),
            types.forBuiltin(table.canonicalID(Builtins::MapIteratorPrototypeNext)),
            types.forBuiltin(table.canonicalID(Builtins::SetIteratorPrototypeNext)),
            types.forBuiltin(table.canonicalID(Builtins::ArrayIteratorPrototypeNext)),
            types.forBuiltin(table.canonicalID(Builtins::CompilerHelpersNextOfStepsOverText)),
        });
    }

    bool mayStepInLibrary()
    {
        return !m_protocolIsForTheOddOne && m_kind == IteratorKind::Sync && m_wording == WordingOfIteration::Source && m_walker.has(Mechanism::Guesses) && m_walker.has(Mechanism::BuiltinCalls)
            && !m_walker.isUnreachable() && findHelper(IterationHelper::StepIterator);
    }

    // For whoever lays the state of an iteration in memory: there a step reads `next` back, and a test of it would be asked anew in each turn of a loop.
    // Of an object of the program's own the steps ask what they have always asked, and nothing is laid.
    bool laysVerdictOnNext(Reference iterable)
    {
        if (!mayStepInLibrary())
            return false;
        TypeID proven = iterable && iterable.isValue() ? m_walker.typeOf(iterable) : TypeID();
        return !proven || !m_walker.types().at(proven).hasOnlyLayouts();
    }

    bool stateHoldsVerdictOnNext()
    {
        if (m_stateIsList)
            return m_listOfStateHoldsVerdictOnNext;
        return m_walker.code().at(m_state.id()).numberOfWords() > static_cast<uint32_t>(WordOfIteration::StepsInLibrary);
    }

    // `next` is what the open has read off the iterator. Where it is the engine's own of a generator or of a Map's, a Set's or a list's iterator, the call would make an object of what it hands out,
    // and this code alone would get the object. The function of the library steps such an iterator without either. The test asks the function itself, so nothing is asked of the place that it was read from.
    // The walk goes on where the step has to be written out: a call of the library for each step would cost a loop over an iterator of the program's own.
    // False: nothing is built.
    bool stepIteratorOfEngineInLibrary(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (sent || readsValueWhenDone || !mayStepInLibrary())
            return false;
        FunctionID steps = findHelper(IterationHelper::StepIterator);
        Reference method = next;
        BlockID byCall;
        if (m_state) {
            if (!stateHoldsVerdictOnNext())
                return false;
            byCall = addBlock();
            BlockID inLibrary = addBlock();
            branch(readWord(WordOfIteration::StepsInLibrary), inLibrary, byCall);
            continueIn(inLibrary);
        } else {
            TypeID methods = methodsSteppedInLibrary();
            if (!next.isValue() || !m_walker.types().overlap(m_walker.typeOf(next), methods))
                return false;
            byCall = addBlock();
            method = goOnWhereTypeTestPasses(next, methods, byCall);
        }
        m_walker.noteGuessOpened();
        m_walker.count(StatisticID::IteratorsSteppedWithoutCall);
        Reference handedOut = callHelper(steps, { iterator, method });
        Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
        isClosed.setTableOperand(handedOut);
        endStepWithoutResultObject(build(WTF::move(isClosed)), handedOut, false, join);
        continueIn(byCall);
        return true;
    }

    // What readAwaitedStepResult() does behind its reads.
    void endStepWithoutResultObject(Reference done, Reference value, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (readsValueWhenDone) {
            if (m_state)
                storeWord(WordOfIteration::IsDone, done);
            jumpTo(join, done, value);
            return;
        }

        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(done, last, further);
        continueIn(last);
        jumpTo(join, boolean(true), undefined());
        continueIn(further);
        if (m_state)
            storeWord(WordOfIteration::IsDone, boolean(false));
        jumpTo(join, boolean(false), value);
    }

    Reference resultOfGeneratorStep(Reference step, StepGeneratorResult result)
    {
        Draft read = draft(Opcode::ResultAt);
        read.setResultsOperand(step);
        read.setIndex(static_cast<uint32_t>(result));
        return build(WTF::move(read));
    }

    template<typename Status>
    Reference hasStatus(Reference status, Status asked) { return operatorOnNumbers(Opcode::StrictEq, status, integer(static_cast<int64_t>(asked))); }

    // `next` is what the open has read off the iterator. Where it is the engine's own, on a generator, the call would resume the body and make an object of what the body hands out, with `value` and `done` as its own data properties,
    // and this code alone would get the object. So the body is resumed here, and nothing is made. The test asks the function itself, so nothing is asked of the place that it was read from.
    // The walk goes on where the call has to be made: `next` is another function, the iterator is no generator, or it is one that is running or has ended, which the method answers.
    void stepGeneratorWhereNextIsOfEngine(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (!m_mayStepGenerator || !m_walker.has(Mechanism::Guesses) || !m_walker.has(Mechanism::BuiltinCalls) || m_walker.isUnreachable())
            return;
        bool isAsync = m_kind == IteratorKind::Async;
        if (isAsync && !canWaitForStepOfAsyncGenerator(m_walker))
            return;

        TypeArena& types = m_walker.types();
        BuiltinID method = m_walker.program().builtins().canonicalID(isAsync ? Builtins::AsyncGeneratorPrototypeNext : Builtins::GeneratorPrototypeNext);
        BlockID byCall = addBlock();
        goOnWhereTypeTestPasses(next, types.forBuiltin(method), byCall);
        m_walker.noteGuessOpened();

        if (isAsync) {
            Reference generator = goOnWhereTypeTestPasses(iterator, types.builtinClass(BuiltinClasses::AsyncGenerator), byCall);
            Reference stepped = m_walker.isUnreachable() ? Reference() : buildStepOfAsyncGenerator(m_walker, generator, sent ? sent : undefined(), m_held, m_origin);
            Reference value = resultOfGeneratorStep(stepped, StepGeneratorResult::Value);
            Reference status = resultOfGeneratorStep(stepped, StepGeneratorResult::Status);
            // The run time has gone the whole way of the call and the await, and this is what the await gave.
            BlockID isAnsweredByObject = addBlock(BlockFrequency::Rare);
            BlockID isAnswered = addBlock();
            branch(hasStatus(status, AsyncGeneratorStepStatus::Result), isAnsweredByObject, isAnswered);
            continueIn(isAnsweredByObject);
            readAwaitedStepResult(value, readsValueWhenDone, join);
            continueIn(isAnswered);
            endStepWithoutResultObject(hasStatus(status, AsyncGeneratorStepStatus::Completed), value, readsValueWhenDone, join);
        } else {
            Draft step = draft(Opcode::StepGenerator);
            step.setGeneratorOperand(iterator);
            step.setResumeMode(ResumeMode::Next);
            step.setValueOperand(sent ? sent : undefined());
            step.setCheck(GeneratorStepCheck::IsGenerator);
            Reference stepped = build(WTF::move(step));
            Reference value = resultOfGeneratorStep(stepped, StepGeneratorResult::Value);
            Reference status = resultOfGeneratorStep(stepped, StepGeneratorResult::Status);
            BlockID isStepped = addBlock();
            BlockID isNotStepped = addBlock(BlockFrequency::Rare);
            branch(hasStatus(status, GeneratorStepStatus::NotStepped), isNotStepped, isStepped);
            continueIn(isNotStepped);
            jumpTo(byCall);
            continueIn(isStepped);
            endStepWithoutResultObject(hasStatus(status, GeneratorStepStatus::Completed), value, readsValueWhenDone, join);
        }
        continueIn(byCall);
    }

    void stepUnlessDone(Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        BlockID finished = addBlock();
        BlockID stepping = addBlock();
        branch(readWord(WordOfIteration::IsDone), finished, stepping);
        continueIn(finished);
        jumpTo(join, boolean(true), undefined());

        continueIn(stepping);
        // A step that throws leaves the state done, and this is what its handler sees.
        storeWord(WordOfIteration::IsDone, boolean(true));
        Reference iterator = readWord(WordOfIteration::Iterator);
        Reference next = readWord(WordOfIteration::Next);
        step(iterator, next, sent, readsValueWhenDone, join);
    }

    // `yield*` forwards a throw or a return to the iterator, if the iterator has the method.
    void resumeByMethod(ResumeMode mode, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        bool isThrow = mode == ResumeMode::Throw;
        Reference iterator = readWord(WordOfIteration::Iterator);
        Reference method = read(iterator, isThrow ? BuiltinNames::throwKeyword : BuiltinNames::returnKeyword);
        BlockID absent = addBlock(isThrow ? BlockFrequency::Rare : BlockFrequency::Normal);
        BlockID present = addBlock();
        branch(testType(method, TypeTable::nullish), absent, present);

        continueIn(absent);
        if (isThrow) {
            callReturn(iterator, ResultOfReturnInIteration::MustBeObject);
            throwError(RuntimeErrorKind::IteratorWithoutThrowMethod);
        } else {
            Reference returned = await(sent);
            jumpTo(join, boolean(true), returned);
        }

        continueIn(present);
        readStepResult(call(method, iterator, singleElementSpan(sent)), readsValueWhenDone, join);
    }

    // ---- Close

    // `return` is looked up each time: it may have been added since the open.
    void callReturn(Reference iterator, ResultOfReturnInIteration expectation)
    {
        if (FunctionID closes = expectation == ResultOfReturnInIteration::MustBeObject ? findHelper(IterationHelper::CloseIterator) : FunctionID()) {
            callHelper(closes, { iterator });
            return;
        }

        Reference method = read(iterator, BuiltinNames::returnKeyword);
        BlockID absent = addBlock();
        BlockID present = addBlock();
        BlockID after = addBlock();
        branch(testType(method, TypeTable::nullish), absent, present);
        continueIn(absent);
        jumpTo(after);

        continueIn(present);
        Reference result = await(call(method, iterator));
        switch (expectation) {
        case ResultOfReturnInIteration::MustBeObject:
            requireObject(result, RuntimeErrorKind::IteratorResultNotObject);
            break;
        case ResultOfReturnInIteration::IsIgnored:
            break;
        }
        jumpTo(after);
    }

    // The error that is already being thrown wins: whatever reading `return`, calling it or awaiting its result throws is dropped.
    void callReturnAndDropWhatItThrows(Reference iterator)
    {
        if (m_walker.isUnreachable())
            return;
        if (FunctionID closes = findHelper(IterationHelper::CloseIteratorAfterThrow)) {
            callHelper(closes, { iterator });
            return;
        }

        BlockID after = addBlock();
        Code& code = m_walker.code();
        m_handler = code.addBlock(BlockRole::CatchHandler, BlockFrequency::Rare);
        code.setLoopMarks(m_handler, m_walker.isInLoop(), false);
        jumpTo(addBlock());
        callReturn(iterator, ResultOfReturnInIteration::IsIgnored);
        BlockID handler = std::exchange(m_handler, BlockID());
        jumpTo(after);

        // No edge of the Walker's leads here, so it brings no state.
        continueIn(handler);
        m_walker.state().clear();
        build(draft(Opcode::CatchValue));
        jumpTo(after);
    }

    Walker& m_walker;
    Origin m_origin;
    IteratorKind m_kind;
    WordingOfIteration m_wording;
    std::span<const Reference> m_held;
    // Null in a spread.
    Reference m_state;
    bool m_stateIsList { false };
    bool m_listOfStateHoldsVerdictOnNext { false };
    // Blocks that are entered stand under this handler block. Null: under the handler of the block being walked.
    BlockID m_handler;
    bool m_isInLoopOfSpread { false };
    bool m_mayStepGenerator { true };
    // A call for each step costs the loop that takes them. The open and the closes are taken once.
    bool m_protocolIsForTheOddOne { false };
    Reference m_instanceAsMadeForGood;
    BlockID m_blockOfCallThatMakesIterator;
    uint32_t m_placeOfReadOfMethod { 0 };
    uint32_t m_placeBehindCallThatMakesIterator { 0 };
    ValueID m_givenByCallThatMakesIterator;
};

} // anonymous namespace

// A list, a text, a Set is proven or expected of what the loop iterates over: the arm of the protocol is there for the odd one that is not as it was made.
static bool isExpectedToBeSteppedUnseen(Walker& walker, ValueID state)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isParameter(state) || index.opcodeOf(state) != Opcode::IteratorOpen)
        return false;
    ValueID iterable = index.value(state).as<Opcode::IteratorOpen>().iterableOperand();
    if (findClassSteppedUnseen(walker, walker.referenceOf(iterable)))
        return true;
    ExpectationOfValue expectation = findExpectation(walker, iterable);
    return expectation.type && findClassSteppedUnseen(walker, walker.types().withoutNullish(expectation.type));
}

// The arm that steps a generator costs its bytes at every loop. It stands where a generator is what is proven or expected of what the loop iterates over, and at a `yield*`.
static bool isExpectedToStepGenerator(Walker& walker, ValueID state)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isParameter(state) || index.opcodeOf(state) != Opcode::IteratorOpen)
        return false;
    auto open = index.value(state).as<Opcode::IteratorOpen>();
    if (open.isDelegating())
        return true;
    TypeArena& types = walker.types();
    auto namesGenerators = [&](TypeID type) {
        const ClassPart* onlyClass = type && types.at(type).isObject() ? singleBuiltinClassOf(types, types.at(type)) : nullptr;
        return onlyClass && onlyClass->builtinClass == BuiltinClasses::Generator;
    };
    Reference iterable = walker.referenceOf(open.iterableOperand());
    if (iterable && iterable.isValue() && namesGenerators(walker.typeOf(iterable)))
        return true;
    ExpectationOfValue expectation = findExpectation(walker, open.iterableOperand());
    return expectation.type && namesGenerators(types.withoutNullish(expectation.type));
}

// ---- TypedOperations

static ValueID onlyTakerBehindCasts(const BodyIndex& index, ValueID value)
{
    ValueID taker = index.onlyUsingValue(value);
    while (taker && inputOperationsThatHandOn.contains(index.opcodeOf(taker)))
        taker = index.onlyUsingValue(taker);
    return taker;
}

bool isListOfEntriesTakenApartAtOnce(Walker& walker, const BodyValue& call)
{
    // `const { entries } = Object`, held and not proven: the call stands behind a test of what is called, and the other side of the test brings a list of pairs to the same loop.
    Reference called = walker.referenceOf(call.operand(OperandIndex::Call::callee));
    while (called && called.isValue() && walker.code().at(called.id()).opcode == Opcode::Narrowed) {
        ValueID tested = walker.code().at(called.id()).valueOperand();
        called = walker.code().isConstant(tested) ? Reference::constant(tested) : Reference::value(tested);
    }
    if (!called || !called.isRecord() || !walker.types().at(walker.code().at(called.id()).type).isObject() || !singleBuiltinOf(walker.types(), walker.types().at(walker.code().at(called.id()).type)))
        return false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis) || !listsAreIteratedByIndex(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID open = onlyTakerBehindCasts(index, call.id());
    if (!open || index.opcodeOf(open) != Opcode::IteratorOpen || !isOpenThatMayGoByIndex(index.value(open)) || index.value(open).as<Opcode::IteratorOpen>().isDestructuring())
        return false;
    WayOfIteration way = findWayOfIteration(index, open);
    if (!way.canBeFollowed || !way.headOfLoopThatSteps)
        return false;

    ValueID step;
    for (const BodyMentionOfIteration& mention : index.mentionsOfIteration(open)) {
        if (index.opcodeOf(mention.user) != Opcode::IteratorResume || mention.user == step)
            continue;
        if (step)
            return false;
        step = mention.user;
    }
    if (!step)
        return false;
    // The results of an operation stand behind it.
    ValueID pair;
    BodyBlock block = index.block(index.blockOf(step));
    for (uint32_t number = step.index() + 1; number < block.endOfValues().index() && index.opcodeOf(ValueID(number)) == Opcode::ResultAt && index.value(ValueID(number)).operand(0) == step; ++number) {
        if (index.value(ValueID(number)).as<Opcode::ResultAt>().index() == 1)
            pair = ValueID(number);
    }
    ValueID pattern = pair ? onlyTakerBehindCasts(index, pair) : ValueID();
    if (!pattern || index.opcodeOf(pattern) != Opcode::IteratorOpen || !isOpenThatMayGoByIndex(index.value(pattern)))
        return false;
    return index.value(pattern).as<Opcode::IteratorOpen>().elementCount() == 2 && stepsOfPatternStandInRow(index, index.value(pattern));
}

// The iteration over flat entries whose step has handed out what `open` takes apart.
static Walker::IterationByIndex* findFlatEntriesSteppedInto(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    ValueID pair = index.valueBehindCasts(open.as<Opcode::IteratorOpen>().iterableOperand());
    if (!pair || index.isParameter(pair) || index.opcodeOf(pair) != Opcode::ResultAt)
        return nullptr;
    ValueID step = index.value(pair).operand(0);
    if (index.isParameter(step) || index.opcodeOf(step) != Opcode::IteratorResume)
        return nullptr;
    Reference state = walker.referenceOf(index.value(step).operand(0));
    if (!state || !state.isVirtual() || !walker.iterationByIndex(state).holdsFlatEntries)
        return nullptr;
    return &walker.iterationByIndex(state);
}

static bool openTypedIteration(Walker& walker, const BodyValue& open)
{
    if (!isOpenThatMayGoByIndex(open))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (Walker::IterationByIndex* entries = findFlatEntriesSteppedInto(walker, index, open)) {
        Walker::IterationByIndex pair;
        pair.builtinClass = BuiltinClasses::Array;
        pair.isTakenAtOpen = true;
        pair.takenAtOpen.append(walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand()));
        pair.takenAtOpen.append(entries->valueBesideKey);
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(pair)));
        return true;
    }
    if (ValueID list = index.valueBehindCasts(open.as<Opcode::IteratorOpen>().iterableOperand()); list && !index.isParameter(list) && walker.isListOfFlatEntries(list)) {
        Walker::IterationByIndex entries;
        entries.list = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand());
        entries.builtinClass = BuiltinClasses::Array;
        entries.holdsFlatEntries = true;
        entries.position = BuilderOfIteration(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source).integer(0);
        entries.inputHead = findWayOfIteration(index, open.id()).headOfLoopThatSteps;
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(entries)));
        return true;
    }
    Reference iterable = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand());
    // `arguments` that nothing else has finds the method of lists on itself, which walks what the caller handed over. Each step reads the object anew: the list stands for it to the last step only where nothing can come between the steps, or nothing but the open names the object.
    ValueID iterated = open.as<Opcode::IteratorOpen>().iterableOperand();
    bool isNamedByOpenAlone = !index.isParameter(iterated) && opcodesThatMakeArguments.contains(index.opcodeOf(iterated)) && index.onlyUsingValue(iterated) == open.id();
    bool staysAsItIsWhileIterated = listsAreIteratedByIndex(walker) && (isNamedByOpenAlone || stepsOfPatternStandInRow(index, open));
    if (Reference list = staysAsItIsWhileIterated ? listStandingForFreshArguments(walker, iterable) : Reference())
        iterable = list;
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);

    if (ValueID literal = findLiteralIteratedAlone(walker, index, open)) {
        Walker::IterationByIndex iteration;
        iteration.list = iterable;
        iteration.builtinClass = BuiltinClasses::Array;
        iteration.literal = literal;
        // The steps of a pattern go by their numbers: stepOfListLiteral(). What collects the rest begins behind them.
        iteration.position = builder.integer(open.as<Opcode::IteratorOpen>().elementCount().value_or(0));
        iteration.inputHead = findWayOfIteration(index, open.id()).headOfLoopThatSteps;
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
        return true;
    }

    // `for (const unit of text)`, text: undefined | null | string. The protocol begins by refusing the two, so they are refused first, and what is left needs no protocol.
    if (iterable && iterable.isValue()) {
        TypeID present = walker.types().withoutNullish(walker.typeOf(iterable));
        if (present != walker.typeOf(iterable) && !isNeverType(present) && findClassSteppedUnseen(walker, present)) {
            iterable = builder.requireObjectCoercible(iterable);
            if (walker.isUnreachable())
                return true;
        }
    }
    std::optional<ClassSteppedUnseen> proven = iterable ? findClassSteppedUnseen(walker, walker.typeOf(iterable)) : std::nullopt;
    // Link has followed all that arrives to be iterated over: it has the names, the prototype and the storage of its making in every run, whatever has run since.
    bool isAsMadeForGood = proven && proven->contents != ContentsStepped::Text && classOfIterableThatStaysAsMadeByLink(walker, open, iterated) == proven->builtinClass;
    bool isSurelyAsMade = proven && (proven->contents == ContentsStepped::Text || isAsMadeForGood || (proven->contents == ContentsStepped::List && isListAsMade(walker, iterable)));
    if (iterable && !isSurelyAsMade && stepsOfPatternStandInRow(index, open)) {
        Walker::IterationByIndex iteration = builder.takeStepsAtOpen(iterable, *open.as<Opcode::IteratorOpen>().elementCount());
        if (!walker.isUnreachable())
            walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
        return true;
    }

    WayOfIteration way = findWayOfIteration(index, open.id());
    if (!way.canBeFollowed)
        return false;
    std::optional<ClassSteppedUnseen> tested = isSurelyAsMade ? std::nullopt : findClassToTestFor(walker, open.as<Opcode::IteratorOpen>().iterableOperand(), iterable, !!way.headOfLoopThatSteps);
    if (!isSurelyAsMade && !tested)
        return false;

    Walker::IterationByIndex iteration;
    if (isSurelyAsMade) {
        iteration.list = iterable;
        iteration.builtinClass = proven->builtinClass;
        iteration.contents = proven->contents;
        iteration.kind = proven->kind;
        iteration.position = builder.integer(0);
        iteration.table = builder.tableThatFirstStepBeginsIn(iterable, proven->contents);
        iteration.isAsMadeForGood = isAsMadeForGood;
        iteration.isOpenAtEveryStepAndClose = way.isOpenAtEveryStepAndClose;
    } else {
        if (isExpectedToBeSteppedUnseen(walker, open.id()))
            builder.expectNoStepByProtocol();
        iteration = builder.openByIndexOrProtocol(iterable, *tested, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.isOpenAtEveryStepAndClose);
        walker.noteGuessOpened();
    }
    iteration.inputHead = way.headOfLoopThatSteps;
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
    return true;
}

// `for (const value of map.values())`: a read of the method off the receiver, a call of it on that receiver without an argument, and the open of what the call gives, in a row, each named by the next alone (what is written of a type may stand between).
// Nobody gets hold of the method or of the iterator. An instance that is as it was made finds the engine's own method under the name, for good, and its rows say that it makes an iterator of the engine over the receiver
// and what the steps of that hand out: so the loop walks the table of the receiver, as `for (.. of map)` does. It is a link of the chain at the read, and walks all three.
bool openLoopOverIteratorThatEngineMakesOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || walker.walksStretchInsideAnother() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
    ValueID callID(read.id().index() + 1);
    if (callID.index() >= block.endOfValues().index() || index.opcodeOf(callID) != Opcode::Call || index.onlyUsingValue(read.id()) != callID)
        return false;
    // Between the call and the open stands nothing but what is written of the type of what the call gives.
    ValueID openID = onlyTakerBehindCasts(index, callID);
    if (!openID || index.isParameter(openID) || index.blockOf(openID) != block.id() || index.opcodeOf(openID) != Opcode::IteratorOpen)
        return false;
    for (uint32_t number = callID.index() + 1; number < openID.index(); ++number) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(ValueID(number))))
            return false;
    }
    BodyValue call = index.value(callID);
    BodyValue open = index.value(openID);
    ValueID receiverID = read.objectOperand();
    if (call.calleeOperand() != read.id() || call.thisValueOperand() != receiverID || !call.as<Opcode::Call>().argumentsOperands().empty())
        return false;
    if (index.valueBehindCasts(open.as<Opcode::IteratorOpen>().iterableOperand()) != callID || !isOpenThatMayGoByIndex(open) || open.as<Opcode::IteratorOpen>().isDestructuring())
        return false;
    BuiltinName name = walker.program().builtinNameOf(read.key());
    WayOfIteration way = name ? findWayOfIteration(index, openID) : WayOfIteration();
    if (!way.canBeFollowed)
        return false;

    Reference receiver = walker.referenceOf(receiverID);
    // Link has followed all that arrives at the receiver of the call: it has the names and the prototype of its making in every run, so nothing is asked of it.
    std::optional<ClassSteppedUnseen> proven = findClassSteppedUnseen(walker, receiver);
    bool isAsMadeForGood = proven && receiverStaysAsMadeByLink(walker, call);
    std::optional<ClassSteppedUnseen> tested = isAsMadeForGood ? proven : findClassToTestFor(walker, receiverID, receiver, !!way.headOfLoopThatSteps);
    if (!tested || (tested->contents != ContentsStepped::Map && tested->contents != ContentsStepped::Set))
        return false;
    std::optional<JSC::IterationKind> kind = kindOfIterationThatNobodySees(walker, tested->builtinClass, false, name);
    if (!kind)
        return false;
    tested->kind = *kind;

    uint32_t place = read.id().index() - block.firstValue().index();
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    uint32_t placeOfOpen = openID.index() - block.firstValue().index();
    Walker::IterationByIndex iteration;
    if (isAsMadeForGood) {
        iteration.list = receiver;
        iteration.builtinClass = tested->builtinClass;
        iteration.contents = tested->contents;
        iteration.kind = tested->kind;
        iteration.position = builder.integer(0);
        iteration.table = builder.tableThatFirstStepBeginsIn(receiver, tested->contents);
        iteration.isAsMadeForGood = true;
        iteration.isOpenAtEveryStepAndClose = way.isOpenAtEveryStepAndClose;
    } else {
        builder.openWhatCallGivesByProtocol(block.id(), place, placeOfOpen, open.as<Opcode::IteratorOpen>().iterableOperand());
        iteration = builder.openByIndexOrProtocol(receiver, *tested, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.isOpenAtEveryStepAndClose);
        walker.noteGuessOpened();
    }
    iteration.inputHead = way.headOfLoopThatSteps;
    if (const LinkedContentsAtOperation* contents = iteration.table ? walker.facts().linked().contentsAtOperation(walker.currentFunction(), callID) : nullptr) {
        iteration.keysProven = contents->key;
        iteration.valuesProven = contents->value;
    }
    if (!walker.isUnreachable())
        walker.bind(openID, walker.addIterationByIndex(WTF::move(iteration)));
    // The block's own walk goes on behind the open.
    walker.walkStretch(block.id(), placeOfOpen + 1, placeOfOpen + 1);
    return true;
}

static bool appendSpreadOfList(Walker& walker, const BodyValue& spread)
{
    Reference array = walker.referenceOf(spread.as<Opcode::AppendSpread>().arrayOperand());
    Reference iterable = walker.referenceOf(spread.as<Opcode::AppendSpread>().iterableOperand());
    // As in openTypedIteration().
    if (array && listsAreIteratedByIndex(walker)) {
        if (Reference list = listStandingForFreshArguments(walker, iterable))
            iterable = list;
        else if (std::optional<Vector<Reference, 8>> elements = elementsOfFreshArguments(walker, iterable)) {
            BuilderOfIteration builder(walker, walker.originOf(spread), IteratorKind::Sync, WordingOfIteration::Spread);
            for (Reference element : *elements)
                builder.appendElement(array, element);
            return true;
        }
    }
    // Link has followed all that arrives to be spread. AppendElements reads a hole as undefined.
    BuiltinClassID classAsMadeForGood = classOfIterableThatStaysAsMadeByLink(walker, spread, spread.as<Opcode::AppendSpread>().iterableOperand());
    bool isListAsMadeForGood = classAsMadeForGood == BuiltinClasses::Array && indexesAreAbsentAboveLists(walker);
    bool isSurelyAsMade = isProvenListInIteration(walker, iterable) && (isListAsMadeForGood || isListAsMade(walker, iterable));
    std::optional<ClassSteppedUnseen> tested = !array || isSurelyAsMade ? std::nullopt : findClassToTestFor(walker, spread.as<Opcode::AppendSpread>().iterableOperand(), iterable);
    if (!array || !iterable)
        return false;
    // No test is put in, so nothing has to pay for one.
    if (std::optional<ClassSteppedUnseen> proven = !tested && !isSurelyAsMade && classAsMadeForGood ? findClassSteppedUnseen(walker, iterable) : std::nullopt; proven && proven->builtinClass == classAsMadeForGood)
        tested = proven;
    BuilderOfIteration builder(walker, walker.originOf(spread), IteratorKind::Sync, WordingOfIteration::Spread);
    if (isSurelyAsMade)
        builder.appendElementsOf(array, iterable);
    else if (tested && BuilderOfIteration::isListedByAppendMembers(*tested)) {
        if (classAsMadeForGood == tested->builtinClass)
            builder.takeInstanceForAsMade(iterable);
        if (!builder.appendRowToListMadeWithRoom(spread, array, iterable, *tested))
            builder.appendMembersOrAllOutOfLine(array, iterable, *tested);
        walker.noteGuessOpened();
    } else
    {
        TypeArena& types = walker.types();
        TypeID proven = iterable.isValue() ? walker.typeOf(iterable) : TypeID();
        bool isGenerator = proven && types.isSubset(proven, types.builtinClass(BuiltinClasses::Generator));
        bool isOfProgram = proven && types.at(proven).hasOnlyLayouts();
        bool isInLibrary = (isGenerator && builder.tryAppendGeneratorInLibrary(array, iterable)) || (isOfProgram && builder.tryAppendObjectOfProgramInLibrary(array, iterable));
        if (!isInLibrary)
            builder.appendAllOutOfLine(array, iterable);
    }
    return true;
}

// `f(...[x, y])`: the call lays the elements of its list out as arguments and hands the list to nobody. Where the state knows them the call writes them out, and nothing needs the list.
std::optional<Draft> draftOfCallWithListWrittenOut(Walker& walker, const BodyValue& call)
{
    Draft written = walker.draftOf(call);
    std::optional<Vector<Reference, 8>> arguments = elementsOfFreshList(walker, written.listOperand());
    if (!arguments)
        return std::nullopt;

    Draft plain = written;
    switch (call.opcode()) {
    case Opcode::CallList:
        plain.become(Opcode::Call);
        plain.setCalleeOperand(written.calleeOperand());
        plain.setThisValueOperand(written.thisValueOperand());
        plain.setCalleeKey(written.calleeKey());
        plain.setCalleeName(written.calleeName());
        plain.setExpectedResult(written.expectedResult());
        plain.setCalledBuiltin(written.calledBuiltin());
        break;
    case Opcode::ConstructList:
        plain.become(Opcode::Construct);
        plain.setCalleeOperand(written.calleeOperand());
        plain.setNewTargetOperand(written.newTargetOperand());
        plain.setCalleeName(written.calleeName());
        break;
    case Opcode::SuperConstructList:
        plain.become(Opcode::SuperConstruct);
        plain.setLayout(written.layout());
        plain.setParentOperand(written.parentOperand());
        plain.setNewTargetOperand(written.newTargetOperand());
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    plain.setArgumentsOperands(arguments->span());
    plain.expected = written.expected;
    plain.resultIsReturnedAtOnce = written.resultIsReturnedAtOnce;
    plain.inputValueGiven = written.inputValueGiven;
    walker.chooseConservativeFields(plain);
    return plain;
}

static bool writeOutArgumentsOfList(Walker& walker, const BodyValue& call)
{
    std::optional<Draft> plain = draftOfCallWithListWrittenOut(walker, call);
    if (!plain)
        return false;
    if (Reference result = walker.build(WTF::move(*plain)))
        walker.bind(call.id(), result);
    return true;
}

// What a Results lists is what its ResultAt gives: a constant stays one, and what the state knows of a value it knows under one name.
static bool handOnResultOfStep(Walker& walker, const BodyValue& read)
{
    Reference results = walker.referenceOf(read.as<Opcode::ResultAt>().resultsOperand());
    if (!results || !results.isValue())
        return false;
    const Code& code = walker.code();
    const Operation& listed = code.at(results.id());
    uint32_t index = read.as<Opcode::ResultAt>().index();
    if (listed.opcode != Opcode::Results || index >= listed.valuesOperands().size())
        return false;
    ValueID result = listed.valuesOperands()[index];
    walker.bind(read.id(), code.isConstant(result) ? Reference::constant(result) : Reference::value(result));
    return true;
}

// `for (const unit of text)` over what the types of a look prove a text, beside undefined and null at the most. openTypedIteration() steps such a value by index whatever else it finds, so each step hands out a text, and undefined at the end.
TypeID typeOfStepOverTextAhead(Walker& walker, const BodyIndex& index, const BodyValue& step, const TypesOfInputs& types)
{
    ValueID open = step.operand(0);
    if (!walker.has(Mechanism::TypedOperations) || &index != &walker.currentBodyIndex() || !open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return { };
    BodyValue opened = index.value(open);
    if (!isOpenThatMayGoByIndex(opened))
        return { };
    TypeID iterated = types(opened.as<Opcode::IteratorOpen>().iterableOperand());
    TypeID present = iterated ? walker.types().withoutNullish(iterated) : TypeID();
    if (!present || isNeverType(present) || !walker.types().at(present).isAnyString() || !findClassSteppedUnseen(walker, present))
        return { };
    if (!findWayOfIteration(index, open).canBeFollowed)
        return { };
    return walker.types().unionOf(TypeTable::undefined, TypeTable::string);
}

constinit const OpcodeSet opcodesThatBuildTypedIterationTakes = opcodesOfTypedIteration;

bool buildTypedIterationOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfTypedIteration.contains(opcode))
        return false;
    if (buildLoopOverGenerator(walker, value))
        return true;
    if (opcode == Opcode::ResultAt)
        return handOnResultOfStep(walker, value);
    if (opcode == Opcode::IteratorOpen)
        return openTypedIteration(walker, value);
    if (opcode == Opcode::AppendSpread)
        return appendSpreadOfList(walker, value);
    if (callsOfListsInIteration.contains(opcode))
        return writeOutArgumentsOfList(walker, value);
    if (appendsToListsInIteration.contains(opcode)) {
        if (!lengthOfFreshList(walker, walker.referenceOf(value.operand(0))))
            return false;
        BuilderOfIteration(walker, walker.originOf(value), IteratorKind::Sync, WordingOfIteration::Spread).appendToList(walker.draftOf(value));
        return true;
    }

    Reference state = walker.referenceOf(value.operand(0));
    if (!state || !state.isVirtual())
        return false;
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), value.id()))
        walker.arrivals().leaveAnything(listed);
    BuilderOfIteration builder(walker, walker.originOf(value), IteratorKind::Sync, WordingOfIteration::Source);
    if (opcode == Opcode::IteratorClose) {
        builder.closeByIndex(walker.iterationByIndex(state), value.as<Opcode::IteratorClose>().completion());
        return true;
    }

    // `for (const [name, price] of prices)`: the receiver of the open is the Map itself.
    if (Walker::IterationByIndex& stepped = walker.iterationByIndex(state); stepped.table && !stepped.keysProven && !stepped.valuesProven) {
        if (const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), value.operand(0))) {
            stepped.keysProven = contents->key;
            stepped.valuesProven = contents->value;
        }
    }
    if (!isExpectedToStepGenerator(walker, value.operand(0)))
        builder.expectNoGenerator();
    if (isExpectedToBeSteppedUnseen(walker, value.operand(0)))
        builder.expectNoStepByProtocol();
    Reference results;
    if (std::optional<StepOfListLiteral> step = stepOfListLiteral(walker, walker.currentBodyIndex(), value))
        results = builder.resultsOf(builder.booleanValue(step->isDone), step->element ? walker.referenceOf(step->element) : builder.undefinedValue());
    else
        results = builder.resumeByIndex(walker.iterationByIndex(state), value.as<Opcode::IteratorResume>().stepIndex());
    if (results)
        walker.bind(value.id(), results);
    return true;
}

bool replaceIteration(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!opcodesReplacedByIteration.contains(opcode))
        return false;

    Draft written = walker.draftOf(value);
    if (opcode == Opcode::IteratorOpen) {
        BuilderOfIteration builder(walker, written.origin(), written.iteratorKind(), written.isDelegating() ? WordingOfIteration::Delegation : WordingOfIteration::Source);
        if (Reference state = builder.openState(written.iterableOperand()))
            walker.bind(value.id(), state);
    } else if (opcode == Opcode::IteratorResume) {
        BuilderOfIteration builder(walker, written.origin(), written.iteratorKind(), WordingOfIteration::Source, written.heldOperands());
        if (written.iteratorKind() == IteratorKind::Sync && !isExpectedToStepGenerator(walker, value.operand(0)))
            builder.expectNoGenerator();
        if (Reference results = builder.resume(written.iteratorOperand(), written.resumeMode(), written.sentOperand(), written.readsValueWhenDone()))
            walker.bind(value.id(), results);
    } else if (opcode == Opcode::IteratorClose) {
        BuilderOfIteration builder(walker, written.origin(), written.iteratorKind(), WordingOfIteration::Source, written.heldOperands());
        builder.close(written.iteratorOperand(), written.completion());
    } else {
        RELEASE_ASSERT(opcode == Opcode::AppendSpread);
        BuilderOfIteration builder(walker, written.origin(), IteratorKind::Sync, WordingOfIteration::Spread);
        builder.appendAll(written.arrayOperand(), written.iterableOperand());
    }

    // A helper of the library counts as the call of the operation, and enters by value whomever Link lists here.
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), value.id()))
        walker.arrivals().leaveAnything(listed);

    // Whichever arm was walked last, code of the program has run.
    walker.state().invalidateHeap();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
