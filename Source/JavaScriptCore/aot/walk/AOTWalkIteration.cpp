/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
#include "AOTBuiltinPristine.h"
#include "AOTSweep.h"
#include "AOTTypeRelations.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkNumberGuesses.h"
#include "AOTWalkResultsInParts.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, Origin);
Reference buildAwaitOfElement(Walker&, Reference element, std::span<const Reference> held, Origin);
bool canWaitForStepOfAsyncGenerator(Walker&);
std::optional<Reference> buildAwaitOfProtocolCall(Walker&, const BodyValue& site, FunctionID callee, Reference thisValue, Reference closure, std::span<const Reference> held, Origin);
bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, BuiltinID awaitedCallee, Reference goesByDriver, Origin);

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet opcodesReplacedByIteration { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread };
static constexpr OpcodeSet appendsToListsInIteration { Opcode::AppendElement, Opcode::AppendHole };
static constexpr OpcodeSet callsOfListsInIteration { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
static constexpr OpcodeSet opcodesOfTypedIteration = opcodesReplacedByIteration | appendsToListsInIteration | callsOfListsInIteration | OpcodeSet { Opcode::ResultAt };

static bool nobodySeesStepsOfIterators(const Walker& walker, BuiltinClassID iteratorClass, bool areOpenedAgain)
{
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID prototypeOfIterators = table.at(iteratorClass).prototype;
    std::optional<BuiltinID> next = findMemberAboveForIteration(walker, prototypeOfIterators, BuiltinNames::next);
    std::optional<BuiltinID> returnMethod = findMemberAboveForIteration(walker, prototypeOfIterators, BuiltinNames::returnKeyword);
    if (!next || !*next || !returnMethod || *returnMethod)
        return false;
    if (!areOpenedAgain)
        return true;
    std::optional<BuiltinID> opensAgain = findMemberAboveForIteration(walker, prototypeOfIterators, table.nameOf(WellKnownSymbol::Iterator));
    if (!opensAgain || !*opensAgain)
        return false;
    bool returnsReceiver = false;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(*opensAgain))) {
        if (overload.mode != BuiltinCallMode::Call || overload.code.implementation.inlineBuiltin != InlineBuiltin::ReturnThis)
            return false;
        returnsReceiver = true;
    }
    return returnsReceiver;
}

static std::optional<JSC::IterationKind> kindOfIterationThatNobodySees(const Walker& walker, BuiltinClassID builtinClass, bool isPrimitive = false, BuiltinName under = { }, MembersBehindWatch aboveInstances = MembersBehindWatch::AreOpen, BuiltinID* methodThatOpens = nullptr)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!isPrimitive && !table.mayHavePristineInstances(builtinClass) && pristineTestOf(table, builtinClass) != PristineTest::InstanceStructure)
        return std::nullopt;
    std::optional<BuiltinID> method = findMemberAboveForIteration(walker, table.at(builtinClass).prototype, under ? under : table.nameOf(WellKnownSymbol::Iterator), aboveInstances);
    if (!method || !*method)
        return std::nullopt;
    if (methodThatOpens)
        *methodThatOpens = *method;

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
    if (!iteratorClass || !nobodySeesStepsOfIterators(walker, iteratorClass, !!under))
        return std::nullopt;
    return kind;
}

static bool nobodyThrowsIntoIterators(const Walker& walker)
{
    return std::ranges::all_of(std::array { BuiltinClasses::ArrayIterator, BuiltinClasses::MapIterator, BuiltinClasses::SetIterator, BuiltinClasses::StringIterator }, [&](BuiltinClassID iterators) {
        std::optional<BuiltinID> member = findMemberAboveForIteration(walker, walker.program().builtins().at(iterators).prototype, BuiltinNames::throwKeyword);
        return member && !*member;
    });
}

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

bool isArrayIteratorProtocolIntact(Walker& walker)
{
    std::optional<bool>& answer = walker.isArrayIteratorProtocolIntact();
    if (!answer)
        answer = kindOfIterationThatNobodySees(walker, BuiltinClasses::Array) == JSC::IterationKind::Values;
    return *answer;
}

using ContentsStepped = Walker::IndexedIteration::Contents;

namespace {

struct ClassSteppedUnseen {
    BuiltinClassID builtinClass;
    ContentsStepped contents { ContentsStepped::List };
    JSC::IterationKind kind { JSC::IterationKind::Values };
    bool restsOnWatchAboveInstances { false };
    BuiltinID methodAskedByItsByte;
    bool isListForWantOfWord { false };
    bool isGuessedMap { false };
    bool textMayComeInstead { false };
    bool mapMayComeInstead { false };
    LayoutID subclass;
    PropertyKeyID opener;
};

} // namespace

static void restOnWatchAboveInstances(const Walker& walker, ClassSteppedUnseen& stepped, BuiltinID methodThatOpens, BuiltinName under)
{
    stepped.restsOnWatchAboveInstances = true;
    PropertyKeyID key = walker.program().keyOf(under ? under : walker.program().builtins().nameOf(WellKnownSymbol::Iterator));
    bool hasByte = methodThatOpens && beliefInMemberFoundOnObjectAsMade(walker, methodThatOpens, walker.types().builtinClass(stepped.builtinClass), key) == BeliefInMemberAbove::IsAskedByItsByte;
    stepped.methodAskedByItsByte = hasByte ? methodThatOpens : BuiltinID();
}

static std::optional<ClassSteppedUnseen> findClassSteppedUnseen(Walker& walker, TypeID id, MembersBehindWatch aboveInstances = MembersBehindWatch::AreOpen)
{
    if (!id)
        return std::nullopt;
    Type type = walker.types().at(id);
    if (type.isAnyString()) {
        bool iteratesValues = kindOfIterationThatNobodySees(walker, BuiltinClasses::String, true) == JSC::IterationKind::Values;
        return iteratesValues ? std::optional(ClassSteppedUnseen { BuiltinClasses::String, ContentsStepped::Text }) : std::nullopt;
    }
    const ClassPart* onlyClass = type.isObject() ? singleBuiltinClassOf(walker.types(), type) : nullptr;
    if (!onlyClass)
        return std::nullopt;
    BuiltinClassID builtinClass = onlyClass->builtinClass;
    if (builtinClass == BuiltinClasses::Array)
        return isArrayIteratorProtocolIntact(walker) ? std::optional(ClassSteppedUnseen { builtinClass }) : std::nullopt;

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
    if (!kind && aboveInstances == MembersBehindWatch::AreAsBuilt && stepped.contents != ContentsStepped::View) {
        BuiltinID methodThatOpens;
        kind = kindOfIterationThatNobodySees(walker, builtinClass, false, { }, aboveInstances, &methodThatOpens);
        restOnWatchAboveInstances(walker, stepped, methodThatOpens, { });
    }
    if (!kind || (stepped.contents == ContentsStepped::View && *kind != JSC::IterationKind::Values))
        return std::nullopt;
    stepped.kind = *kind;
    return stepped;
}

bool iterationGoesByIndexWithoutTest(Walker& walker, TypeID iterated)
{
    TypeID present = iterated ? walker.types().withoutNullish(iterated) : TypeID();
    std::optional<ClassSteppedUnseen> stepped = present && !isNeverType(present) ? findClassSteppedUnseen(walker, present) : std::nullopt;
    return stepped && stepped->contents == ContentsStepped::Text;
}

static std::optional<ClassSteppedUnseen> findClassSteppedUnseen(Walker& walker, Reference value, MembersBehindWatch aboveInstances = MembersBehindWatch::AreOpen)
{
    return value && value.isValue() ? findClassSteppedUnseen(walker, walker.typeOf(value), aboveInstances) : std::nullopt;
}

static PropertyKeyID keyOfIterator(const Walker& walker)
{
    return walker.program().keyOf(walker.program().builtins().nameOf(WellKnownSymbol::Iterator));
}

static std::optional<ClassSteppedUnseen> findSubclassStepped(Walker& walker, TypeID iterated, PropertyKeyID opener)
{
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    TypeID present = iterated && !program.admitsReplacedBuiltins ? types.withoutNullish(iterated) : TypeID();
    LayoutID subclass = present && !isNeverType(present) ? layoutKnownByStructureOfClass(walker, present) : LayoutID();
    BuiltinClassID above = subclass ? program.at(subclass).writtenBuiltinBase : BuiltinClassID();
    if (!above || (above == BuiltinClasses::Array && !arraySubclassWithElements(walker, present)))
        return std::nullopt;
    if (program.at(subclass).findField(opener) || walker.facts().hasMemberUnder(subclass, ClassSide::Prototype, opener))
        return std::nullopt;
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, types.builtinClass(above));
    if (stepped) {
        stepped->subclass = subclass;
        stepped->opener = opener;
    }
    return stepped;
}

static TypeID typeOfInstancesStepped(Walker& walker, BuiltinClassID builtinClass, ContentsStepped contents)
{
    return contents == ContentsStepped::Text ? TypeID(TypeTable::string) : walker.types().builtinClass(builtinClass);
}

static bool isProvenArrayIterable(Walker& walker, Reference value)
{
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, value);
    return stepped && stepped->contents == ContentsStepped::List;
}

static constexpr uint32_t closedOperationsOfProtocol = 3;

static std::optional<ClassSteppedUnseen> findClassToTestFor(Walker& walker, ValueID input, Reference iterable, bool stepsStandInLoop = false, PropertyKeyID openerOfSubclasses = { })
{
    if (!iterable || !iterable.isValue())
        return std::nullopt;
    TypeArena& types = walker.types();
    bool isDeclared = true;
    MembersBehindWatch aboveInstances = walker.program().admitsReplacedBuiltins ? MembersBehindWatch::AreAsBuilt : MembersBehindWatch::AreOpen;
    std::optional<ClassSteppedUnseen> stepped;
    if (openerOfSubclasses && walker.program().someClassExtendsIterable) [[unlikely]] {
        ExpectationOfValue expectation = findExpectation(walker, input);
        stepped = findSubclassStepped(walker, walker.typeOf(iterable), openerOfSubclasses);
        if (!stepped)
            stepped = findSubclassStepped(walker, expectation.type, openerOfSubclasses);
        if (stepped)
            isDeclared = expectation.type && expectation.isDeclared;
    }
    if (!stepped)
        stepped = findClassSteppedUnseen(walker, iterable, aboveInstances);
    if (!stepped) {
        ExpectationOfValue expectation = findExpectation(walker, input);
        isDeclared = expectation.type && expectation.isDeclared;
        if (expectation.type)
            stepped = findClassSteppedUnseen(walker, types.withoutNullish(expectation.type), aboveInstances);
        TypeID expectedToBePresent = expectation.type ? types.withoutNullish(expectation.type) : TypeID();
        bool hasNoIterableExpectation = !expectedToBePresent || isNeverType(expectedToBePresent) || types.at(expectedToBePresent).hasOnlyClosures();
        if (!stepped && isArrayIteratorProtocolIntact(walker) && (hasNoIterableExpectation || types.overlap(expectation.type, types.builtinClass(BuiltinClasses::Array)))) {
            stepped = ClassSteppedUnseen { BuiltinClasses::Array };
            stepped->isListForWantOfWord = hasNoIterableExpectation;
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

TypeID entryPairType(Walker& walker, TypeID iterable)
{
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, iterable);
    return stepped && stepped->kind == JSC::IterationKind::Entries ? walker.types().builtinClass(BuiltinClasses::Array) : TypeID();
}

namespace {

struct WayOfIteration {
    bool canBeFollowed { false };
    BlockID headOfLoopThatSteps;
    bool stepsStandUnderOwnCleanup { false };
    bool closesStandBelowOnlyStep { false };
    bool hasSingleStepAtHeader { false };
    bool canSkipDoneChecks { false };
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

static WayOfIteration findWayOfIteration(const BodyIndex& index, ValueID open)
{
    WayOfIteration way;
    BlockID blockOfOpen = index.blockOf(open);
    LoopID loopOfOpen = index.loopOf(blockOfOpen);
    const bool delegates = index.value(open).as<Opcode::IteratorOpen>().isDelegating();

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
        if (!index.isReachable(mention.block))
            continue;
        if (operation.opcode() == Opcode::IteratorClose) {
            blocksOfCloses.append(mention.block);
            continue;
        }
        if (operation.opcode() != Opcode::IteratorResume)
            return way;

        auto step = operation.as<Opcode::IteratorResume>();
        if (delegates && step.resumeMode() != ResumeMode::Next)
            continue;
        if (step.resumeMode() != ResumeMode::Next || (!delegates && (step.sentOperand() || step.readsValueWhenDone())) || !index.dominates(blockOfLastStep, mention.block))
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
    way.hasSingleStepAtHeader = numberOfSteps == 1 && way.headOfLoopThatSteps;
    if (!way.hasSingleStepAtHeader || way.stepsStandUnderOwnCleanup)
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
    way.canSkipDoneChecks = std::ranges::all_of(blocksOfCloses, [&](BlockID block) {
        return index.dominates(turn, block);
    });
    return way;
}

ValueID findSoleLoopHeaderStep(const BodyIndex& index, ValueID open)
{
    WayOfIteration way = findWayOfIteration(index, open);
    if (!way.canBeFollowed || !way.hasSingleStepAtHeader || !way.canSkipDoneChecks)
        return { };
    for (const BodyMentionOfIteration& mention : index.mentionsOfIteration(open)) {
        if (index.isReachable(mention.block) && index.opcodeOf(mention.user) == Opcode::IteratorResume)
            return mention.user;
    }
    return { };
}

static bool haveOneHandler(BodyBlock first, BodyBlock second)
{
    if (!first.hasHandler() || !second.hasHandler())
        return !first.hasHandler() && !second.hasHandler();
    return first.handler().block() == second.handler().block() && std::ranges::equal(first.handler().arguments(), second.handler().arguments());
}

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

static bool isUnseenBetweenStepsOfPattern(Opcode opcode)
{
    const Effects& bound = declaredBoundInTable(opcode);
    return opcode != Opcode::Builtin && !operandsMayRunCode(opcode) && bound.alwaysCompletes() && !bound.mayThrowByRule() && !bound.writes.containsAny(orderedHeaps) && !bound.reads.containsAny(orderedHeaps);
}

bool stepsOfPatternStandInRow(const BodyIndex& index, const BodyValue& open, RestOfPattern* rest)
{
    auto written = open.as<Opcode::IteratorOpen>();
    if (!written.isDestructuring() || (written.hasRest() && !rest) || !written.elementCount())
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

    auto findLoopOfRest = [&](BlockID head) -> ValueID {
        BodyBlock block = index.block(head);
        if (!index.isLoopHead(head) || index.isEnteredByHandlerEdge(head) || index.numberOfIncomingEdges(head) != 2 || block.numberOfParameters() || block.terminator().kind() != TerminatorKind::Branch || !throwsWhereOpenThrows(block))
            return { };
        ValueID step;
        ValueID done;
        ValueID value;
        for (BodyValue operation : block.values()) {
            if (operation.opcode() == Opcode::IteratorResume && !step) {
                auto resume = operation.as<Opcode::IteratorResume>();
                if (resume.iteratorOperand() != open.id() || resume.stepIndex() != numberOfSteps || resume.resumeMode() != ResumeMode::Next || resume.sentOperand() || resume.readsValueWhenDone() || !resume.heldOperands().empty())
                    return { };
                step = operation.id();
            } else if (operation.opcode() == Opcode::ResultAt && step && operation.as<Opcode::ResultAt>().resultsOperand() == step)
                (operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Done) ? done : value) = operation.id();
            else if (operation.opcode() != Opcode::LoopHint)
                return { };
        }
        BodyTerminator branch = block.terminator();
        LoopID loop = index.loopOf(head);
        if (!step || !done || !value || branch.operand(0) != done || index.isInLoop(branch.taken().block(), loop))
            return { };
        BodyBlock appending = index.block(branch.notTaken().block());
        if (!index.isInLoop(appending.id(), loop) || appending.numberOfParameters() || appending.numberOfValues() != 1 || !throwsWhereOpenThrows(appending) || appending.terminator().kind() != TerminatorKind::Jump || appending.terminator().next().block() != head)
            return { };
        BodyValue append = index.value(appending.firstValue());
        bool appendsToRest = append.opcode() == Opcode::AppendElement && append.operand(0) == rest->array && append.operand(1) == value;
        return appendsToRest && index.hasOneUse(value) && index.hasOneUse(done) ? step : ValueID();
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
                if (operation.as<Opcode::ResultAt>().resultsOperand() != lastStep || (operation.as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value) && !index.isUnused(operation.id())))
                    return false;
                break;
            case Opcode::NewArray: {
                BodyTerminator end = block.terminator();
                if (!written.hasRest() || stepsSeen != numberOfSteps || operation.numberOfVariadicOperands() || operation.id().index() + 1 != block.endOfValues().index() || end.kind() != TerminatorKind::Jump)
                    return false;
                rest->array = operation.id();
                rest->step = findLoopOfRest(end.next().block());
                return rest->step && index.mentionsOfIteration(open.id()).size() == numberOfSteps + 2 + numberOfCleanups;
            }
            case Opcode::IteratorClose:
                return !written.hasRest() && operation.operand(0) == open.id() && operation.as<Opcode::IteratorClose>().completion() == IteratorCompletion::Normal && stepsSeen == numberOfSteps
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

std::optional<ForOfByText> findForOfSteppedAtHead(const BodyIndex& index, LoopID loop)
{
    BodyBlock head = index.block(index.loop(loop).head);
    for (BodyValue operation : head.values()) {
        if (operation.opcode() == Opcode::LoopHint)
            continue;
        ValueID open = operation.opcode() == Opcode::IteratorResume ? operation.operand(0) : ValueID();
        if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen || !isOpenThatMayGoByIndex(index.value(open)) || index.value(open).as<Opcode::IteratorOpen>().isDestructuring())
            return std::nullopt;
        WayOfIteration way = findWayOfIteration(index, open);
        if (!way.canBeFollowed || way.headOfLoopThatSteps != head.id() || !way.canSkipDoneChecks)
            return std::nullopt;
        return ForOfByText { open, operation.id() };
    }
    return std::nullopt;
}

bool openOverListAsMadeGoesByIndex(Walker& walker, const BodyIndex& index, ValueID open)
{
    return walker.has(Mechanism::TypedOperations) && isArrayIteratorProtocolIntact(walker) && isOpenThatMayGoByIndex(index.value(open)) && findWayOfIteration(index, open).canBeFollowed;
}

static ValueID findLiteralIteratedAlone(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    ValueID literal = open.as<Opcode::IteratorOpen>().iterableOperand();
    if (!walker.has(Mechanism::TypedOperations) || !isOpenThatMayGoByIndex(open) || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewArray || index.onlyUsingValue(literal) != open.id())
        return { };
    if (index.blockOf(literal) != index.blockOf(open.id()) || !isArrayIteratorProtocolIntact(walker))
        return { };
    if (walker.hasStringsInBuilders()) {
        for (ValueID element : index.value(literal).as<Opcode::NewArray>().elementsOperands()) {
            if (isBoundToStringInBuilder(walker, element))
                return { };
        }
    }
    if (!open.as<Opcode::IteratorOpen>().isDestructuring() || open.as<Opcode::IteratorOpen>().hasRest())
        return findWayOfIteration(index, open.id()).canBeFollowed ? literal : ValueID();
    return literal;
}

static uint32_t lengthOfLiteralInIteration(const BodyIndex& index, ValueID literal)
{
    auto made = index.value(literal).as<Opcode::NewArray>();
    return static_cast<uint32_t>(made.elementsOperands().size() + made.holes().size());
}

static std::optional<uint32_t> placeOfStepInPattern(const BodyIndex& index, const BodyValue& value)
{
    auto step = value.as<Opcode::IteratorResume>();
    ValueID state = step.iteratorOperand();
    std::optional<uint32_t> stepIndex = step.stepIndex();
    if (!stepIndex || step.resumeMode() != ResumeMode::Next || step.sentOperand() || index.isParameter(state) || index.opcodeOf(state) != Opcode::IteratorOpen)
        return std::nullopt;
    auto open = index.value(state).as<Opcode::IteratorOpen>();
    std::optional<uint32_t> elementCount = open.elementCount();
    if (!open.isDestructuring() || !elementCount || *stepIndex >= *elementCount)
        return std::nullopt;
    return stepIndex;
}

static ValueID elementWrittenAt(std::span<const ValueID> elements, std::span<const uint32_t> holes, uint32_t position)
{
    auto holeBehind = std::ranges::lower_bound(holes, position);
    size_t place = position - (holeBehind - holes.begin());
    return (holeBehind != holes.end() && *holeBehind == position) || place >= elements.size() ? ValueID() : elements[place];
}

static StepOfListLiteral stepAtPlaceOfLiteral(const BodyIndex& index, ValueID literal, uint32_t position)
{
    auto made = index.value(literal).as<Opcode::NewArray>();
    return { position >= lengthOfLiteralInIteration(index, literal), elementWrittenAt(made.elementsOperands(), made.holes(), position) };
}

static Reference elementInRecordOfLiteral(Walker& walker, Reference list, uint32_t position)
{
    if (!list || !list.isValue())
        return { };
    const Code& code = walker.code();
    const Operation& made = code.at(underlyingCellOf(code, list.id()));
    ValueID element = made.opcode == Opcode::NewArray ? elementWrittenAt(made.elementsOperands(), made.holes(), position) : ValueID();
    if (!element)
        return { };
    while (conversionsOfForm.contains(code.at(element).opcode))
        element = code.at(element).operand(0);
    return code.isConstant(element) ? Reference::constant(element) : Reference::value(element);
}

static constexpr OpcodeSet appendsToLiteralsInIteration = appendsToListsInIteration | OpcodeSet { Opcode::AppendSpread };
static_assert(!OperandIndex::AppendElement::array && !OperandIndex::AppendHole::array && !OperandIndex::AppendSpread::array);

static uint32_t endOfRowOfAppendsInIteration(const BodyIndex& index, const BodyBlock& block, ValueID literal, uint32_t begin, uint32_t limit)
{
    uint32_t firstValue = block.firstValue().index();
    uint32_t end = begin + 1;
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
        if (mentions == BodyIndex::maxCountedUses || operandsMayRunCode(opcode) || !bound.alwaysCompletes() || !bound.writes.containsOnly(unobservableWriteHeaps))
            break;
        mentionsToCome.append(mentions);
    }
    return end;
}

static FunctionID functionWrittenUnderNextOfIteratorOf(Walker&, ValueID iterated);

static bool returnsFreshIterator(const Walker& walker, FunctionID function)
{
    const BodyIndex* body = walker.facts().bodies().find(function);
    if (!body || walker.program().at(function).isCoroutine())
        return false;
    ValueID returned;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyTerminator end = body->terminator(BlockID(number));
        if (!body->isReachable(BlockID(number)) || (end.kind() != TerminatorKind::Return && end.kind() != TerminatorKind::ReturnCall))
            continue;
        if (returned || !end.operand(0))
            return false;
        returned = end.operand(0);
        while (!body->isParameter(returned) && inputPassThroughOpcodes.contains(body->opcodeOf(returned)) && body->useCount(returned) == 1)
            returned = body->value(returned).operand(0);
    }
    return returned && !body->isParameter(returned) && body->opcodeOf(returned) == Opcode::Call && body->useCount(returned) == 1 && body->value(returned).as<Opcode::Call>().argumentsOperands().empty();
}

static std::optional<ClassSteppedUnseen> findCollectionOfIterator(Walker& walker, Reference iterator, Reference& collection)
{
    const Code& code = walker.code();
    if (!iterator || !iterator.isValue())
        return std::nullopt;
    const Operation& made = code.at(underlyingCellOf(code, iterator.id()));
    if (made.opcode != Opcode::CallInline || !made.receiverOperand() || code.isConstant(made.receiverOperand()))
        return std::nullopt;
    ContentsStepped contents;
    switch (made.inlineBuiltin()) {
    case InlineBuiltin::ArrayIterator:
        contents = ContentsStepped::List;
        break;
    case InlineBuiltin::MapIterator:
        contents = ContentsStepped::Map;
        break;
    case InlineBuiltin::SetIterator:
        contents = ContentsStepped::Set;
        break;
    default:
        return std::nullopt;
    }
    const BuiltinResult& result = walker.program().builtins().at(made.overload()).result;
    collection = Reference::value(made.receiverOperand());
    const ClassPart* classOfCollection = singleBuiltinClassOf(walker.types(), walker.types().at(walker.typeOf(collection)));
    const ClassPart* classOfIterator = singleBuiltinClassOf(walker.types(), walker.types().at(walker.typeOf(iterator)));
    if (!classOfCollection || !classOfIterator || !result.iteratedOperand || !result.iteratedOperand->isReceiver() || !nobodySeesStepsOfIterators(walker, classOfIterator->builtinClass, false))
        return std::nullopt;
    if (contents == ContentsStepped::List && classOfCollection->builtinClass != BuiltinClasses::Array)
        return std::nullopt;
    return ClassSteppedUnseen { classOfCollection->builtinClass, contents, result.iterationKind };
}

namespace {

enum class WordingOfIteration : uint8_t { Source, Spread, Delegation };

static bool isResultOfDelegatedCall(const Walker& walker, Reference iterable)
{
    if (!iterable || !iterable.isValue() || walker.code().isConstant(iterable.id()))
        return false;
    const Operation& read = walker.code().at(iterable.id());
    return read.opcode == Opcode::ResultAt && walker.code().at(read.resultsOperand()).opcode == Opcode::CallDelegate;
}

static Reference completionOfDelegatedCall(Walker& walker, Reference iterable, Origin origin)
{
    if (walker.isUnreachable() || !isResultOfDelegatedCall(walker, iterable))
        return { };
    Draft read(Opcode::ResultAt, origin);
    read.setResultsOperand(Reference::value(walker.code().at(iterable.id()).resultsOperand()));
    read.setIndex(static_cast<uint32_t>(DelegatedCallResult::HasCompleted));
    return walker.build(WTF::move(read));
}
enum class UseOfSyncIteration : uint8_t { Stepped, WrappedForAsyncIteration };
enum class ResultOfReturnInIteration : uint8_t { MustBeObject, IsIgnored };

enum class WordOfIteration : uint32_t { Iterator, Next, IsDone, StepsInLibrary, ListSteppedByIndex, PositionInList };
constexpr uint32_t numberOfWordsOfIteration = 3;
constexpr uint32_t numberOfWordsWhereListIsSteppedByIndex = 6;

struct OpenedIteration {
    Reference iterator;
    Reference next;
    Reference goesByDriver { };
};

struct JoinOfIteration {
    BlockID block;
    std::array<Reference, 2> results;
};

struct CopyByArrayFrom {
    ValueID given;
    ValueID test;
    ValueID spread;
};

std::optional<CopyByArrayFrom> findCopyByArrayFrom(const BodyIndex& index, ValueID call)
{
    BodyBlock block = index.block(index.blockOf(call));
    ValueID given = call;
    ValueID next(call.index() + 1);
    while (next.index() < block.endOfValues().index() && opcodesThatStateTypes.contains(index.opcodeOf(next)) && index.onlyUsingValue(given) == next) {
        given = next;
        next = ValueID(next.index() + 1);
    }
    BodyTerminator end = block.terminator();
    if (next.index() + 1 != block.endOfValues().index() || index.opcodeOf(next) != Opcode::IsPristineArray || index.value(next).operand(0) != given || end.kind() != TerminatorKind::Branch || end.operand(0) != next || !index.hasOneUse(next))
        return std::nullopt;
    BodyBlock spreading = index.block(end.taken().block());
    BlockID otherwise = end.notTaken().block();
    auto isEnteredHereAlone = [&](BlockID target) {
        return index.numberOfIncomingEdges(target) == 1 && !index.block(target).numberOfParameters() && !index.isEnteredByHandlerEdge(target);
    };
    static constexpr uint32_t valuesOfSpread = 2;
    if (!isEnteredHereAlone(spreading.id()) || !isEnteredHereAlone(otherwise) || spreading.numberOfValues() < valuesOfSpread)
        return std::nullopt;
    BodyValue array = index.value(spreading.firstValue());
    ValueID spread(spreading.firstValue().index() + 1);
    if (array.opcode() != Opcode::NewArray || array.numberOfVariadicOperands() || index.opcodeOf(spread) != Opcode::AppendSpread)
        return std::nullopt;
    auto append = index.value(spread).as<Opcode::AppendSpread>();
    if (append.arrayOperand() != array.id() || append.iterableOperand() != given)
        return std::nullopt;

    uint32_t knownUses = 2;
    auto countUse = [&](ValueID operand, uint32_t) {
        knownUses += operand == given;
    };
    for (uint32_t number = otherwise.index(); number < index.numberOfBlocks(); ++number) {
        BlockID behind(number);
        if (!index.isReachable(behind) || !index.dominates(otherwise, behind))
            continue;
        for (BodyValue operation : index.block(behind).values())
            operation.forEachOperand(countUse);
        BodyTerminator terminator = index.terminator(behind);
        terminator.forEachOperand(countUse);
        ValueID condition = terminator.kind() == TerminatorKind::Branch ? terminator.operand(0) : ValueID();
        bool isTested = condition && !index.isParameter(condition) && index.opcodeOf(condition) == Opcode::TypeTest && index.value(condition).operand(0) == given;
        for (BodyTarget target : terminator.targets()) {
            for (ValueID argument : target.arguments())
                knownUses += argument == given;
            knownUses += isTested ? target.proofs().size() : 0;
        }
    }
    if (index.useCount(given) != knownUses)
        return std::nullopt;
    return CopyByArrayFrom { given, next, spread };
}

class BuilderOfIteration {
    WTF_MAKE_NONCOPYABLE(BuilderOfIteration);
public:
    BuilderOfIteration(Walker& walker, Origin origin, IteratorKind kind, WordingOfIteration wording, std::span<const Reference> held = { })
        : m_walker(walker)
        , m_origin(origin)
        , m_kind(kind)
        , m_wording(wording)
        , m_held(held)
    {
        m_origin.isTailCall = false;
    }

    static bool mayTakePairApartWhenDone(const Walker::IndexedIteration& iteration) { return iteration.isEntryPairElided && !iteration.inputHead; }

    void standFor(const BodyValue& site) { m_site = &site; }
    void expectUnderNext(FunctionID function) { m_expectedUnderNext = function; }
    void expectWhatIsWrittenUnderNextOfIteratorOf(ValueID iterated) { m_iteratedWhoseNextIsAsked = iterated; }
    void testForListSteppedByIndex(const ClassSteppedUnseen& lists) { m_listsSteppedByIndexBehindWrapper = lists; }

    Reference requireObjectCoercible(Reference iterable)
    {
        if (iterable && iterable.isValue() && !m_walker.isUnreachable() && !(m_walker.types().at(m_walker.typeOf(iterable)).primitives & Primitive::UndefinedOrNull))
            return iterable;
        Draft check = draft(Opcode::RequireObjectCoercible);
        check.setValueOperand(iterable);
        check.setNullishAccess(m_wording == WordingOfIteration::Spread ? NullishAccess::Spread : NullishAccess::Iterate);
        return build(WTF::move(check));
    }

    Reference openState(Reference iterable)
    {
        if (m_listsSteppedByIndexBehindWrapper)
            return openStateOverListOrByProtocol(iterable, *m_listsSteppedByIndexBehindWrapper);
        OpenedIteration opened = open(iterable);
        Draft allocation = draft(Opcode::NewHeapWords);
        bool delegates = m_wording == WordingOfIteration::Delegation && m_kind == IteratorKind::Sync && m_walker.has(Mechanism::Guesses) && m_walker.has(Mechanism::BuiltinCalls);
        bool laysVerdict = m_kind == IteratorKind::Async || delegates || laysVerdictOnNext(iterable);
        allocation.setNumberOfWords(numberOfWordsOfIteration + laysVerdict);
        m_state = build(WTF::move(allocation));
        storeWord(WordOfIteration::Iterator, opened.iterator);
        storeWord(WordOfIteration::Next, opened.next);
        storeWord(WordOfIteration::IsDone, boolean(false));
        if (laysVerdict)
            storeWord(WordOfIteration::StepsInLibrary, m_kind == IteratorKind::Async ? opened.goesByDriver : delegates ? testForEngineGenerator(opened) : testType(opened.next, methodsSteppedInLibrary()));
        return m_state;
    }

    Reference testForEngineGenerator(const OpenedIteration& opened)
    {
        TypeArena& types = m_walker.types();
        BlockID otherwise = addBlock();
        BlockID after = addBlock();
        Reference isGenerator = m_walker.addParameter(after, TypeTable::boolean, Representation::Boolean);
        continueIfTypeMatches(opened.next, types.forBuiltin(m_walker.program().builtins().canonicalID(Builtins::GeneratorPrototypeNext)), otherwise);
        jumpTo(after, { testType(opened.iterator, types.builtinClass(BuiltinClasses::Generator)) });
        continueIn(otherwise);
        jumpTo(after, { boolean(false) });
        continueIn(after);
        return isGenerator;
    }

    void stepDelegate() { m_stepsDelegate = true; }
    bool keepsDoneWord() { return m_state && !(m_stepsDelegate && stateHoldsVerdictOnNext()); }

    Reference openStateOverListOrByProtocol(Reference iterable, const ClassSteppedUnseen& lists)
    {
        Draft allocation = draft(Opcode::NewHeapWords);
        allocation.setNumberOfWords(numberOfWordsWhereListIsSteppedByIndex);
        m_state = build(WTF::move(allocation));
        storeWord(WordOfIteration::IsDone, boolean(false));
        storeWord(WordOfIteration::PositionInList, integer(0));

        BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        Reference list = continueIfPristine(iterable, lists, byProtocol);
        storeWord(WordOfIteration::Iterator, undefined());
        storeWord(WordOfIteration::Next, undefined());
        storeWord(WordOfIteration::StepsInLibrary, boolean(false));
        storeWord(WordOfIteration::ListSteppedByIndex, list);
        m_walker.count(StatisticID::AsyncArrayIterationsIndexed);
        jumpTo(after);

        continueIn(byProtocol);
        OpenedIteration opened = open(iterable);
        storeWord(WordOfIteration::Iterator, opened.iterator);
        storeWord(WordOfIteration::Next, opened.next);
        storeWord(WordOfIteration::StepsInLibrary, opened.goesByDriver);
        storeWord(WordOfIteration::ListSteppedByIndex, undefined());
        jumpTo(after);
        return m_state;
    }

    bool stateMayHoldListSteppedByIndex() { return m_state && !m_stateIsList && m_walker.code().at(m_state.id()).numberOfWords() == numberOfWordsWhereListIsSteppedByIndex; }

    Reference continueIfIndexedState(BlockID otherwise) { return continueIfTypeMatches(readWord(WordOfIteration::ListSteppedByIndex), typeOfLists(), otherwise); }

    void stepListBehindWrapper(const JoinOfIteration& join)
    {
        RELEASE_ASSERT(!m_handler);
        BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
        Walker::IndexedIteration iteration;
        iteration.builtinClass = BuiltinClasses::Array;
        iteration.list = continueIfIndexedState(byProtocol);

        Code& code = m_walker.code();
        m_handler = code.addBlock(BlockRole::CatchHandler, BlockFrequency::Rare);
        code.setLoopMarks(m_handler, m_walker.isInLoop(), false);
        jumpTo(addBlock());
        iteration.position = readWord(WordOfIteration::PositionInList);
        BlockID stepped = addBlock();
        Reference done = m_walker.addParameter(stepped, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
        Reference value = m_walker.addParameter(stepped, { }, Representation::JSValue);
        Reference position = m_walker.addParameter(stepped, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::doneIterationPosition }), { }));
        stepByIndex(iteration, iteration.list, lengthOf(iteration, iteration.list), false, stepped);
        continueIn(stepped);
        storeWord(WordOfIteration::PositionInList, position);
        Reference settled = m_walker.isUnreachable() ? Reference() : buildAwaitOfElement(m_walker, value, m_held, m_origin);
        BlockID handler = std::exchange(m_handler, BlockID());
        jumpTo(addBlock());
        await(undefined());
        storeWord(WordOfIteration::IsDone, done);
        jumpTo(join, done, settled);

        continueIn(handler);
        m_walker.state().clear();
        Reference thrown = build(draft(Opcode::CatchValue));
        await(undefined());
        if (!m_walker.isUnreachable())
            m_walker.endWithThrow(thrown, m_origin);
        continueIn(byProtocol);
    }

    void makeWrapperWhereListWasSteppedByIndex()
    {
        const BuiltinTable& table = m_walker.program().builtins();
        BlockID after = addBlock();
        BlockID isOpenedByProtocol = addBlock();
        Reference list = continueIfIndexedState(isOpenedByProtocol);
        auto functionOfEngine = [&](BuiltinID builtin) {
            Draft function = draft(Opcode::Builtin);
            function.setBuiltin(table.canonicalID(builtin));
            function.setIsNotReadFromHolder(true);
            return build(WTF::move(function));
        };
        std::optional<BuiltinID> opens = findMemberAboveForIteration(m_walker, table.at(BuiltinClasses::Array).prototype, table.nameOf(WellKnownSymbol::Iterator));
        RELEASE_ASSERT(opens && *opens);
        Draft wrapper = draft(Opcode::CreateAsyncFromSyncIterator);
        wrapper.setIteratorOperand(requireObject(call(functionOfEngine(*opens), list), RuntimeErrorKind::AsyncFromSyncIteratorNotObject));
        wrapper.setNextOperand(functionOfEngine(Builtins::ArrayIteratorPrototypeNext));
        storeWord(WordOfIteration::Iterator, build(WTF::move(wrapper)));
        storeWord(WordOfIteration::ListSteppedByIndex, undefined());
        jumpTo(after);
        continueIn(isOpenedByProtocol);
        jumpTo(after);
    }

    void sendToDelegate(Reference sent) { m_sentToDelegate = sent; }

    Reference leaveDelegate(const Walker::IndexedIteration& iteration, ResumeMode mode, Reference sent, bool readsValueWhenDone)
    {
        if (!sent)
            sent = undefined();
        BlockID byProtocol;
        JoinOfIteration join;
        if (iteration.hasProtocolFallback || mode == ResumeMode::Return)
            join = openJoin();
        if (iteration.isProtocolOnly) {
            resumeByMethod(mode, iteration.iterator, sent, readsValueWhenDone, join);
            return resultsOf(join.results[0], join.results[1]);
        }
        if (iteration.hasProtocolFallback) {
            byProtocol = addBlock(BlockFrequency::Unlikely);
            continueIfIndexedIteration(iteration, byProtocol);
        }
        if (m_walker.isUnreachable()) { }
        else if (mode == ResumeMode::Throw)
            throwError(RuntimeErrorKind::IteratorWithoutThrowMethod);
        else
            jumpTo(join, boolean(true), sent);
        if (byProtocol) {
            continueIn(byProtocol);
            resumeByMethod(mode, iteration.iterator, sent, readsValueWhenDone, join);
        }
        return m_walker.isUnreachable() ? Reference() : resultsOf(join.results[0], join.results[1]);
    }

    Reference resume(Reference state, ResumeMode mode, Reference sent, bool readsValueWhenDone)
    {
        takeState(state);
        JoinOfIteration join = openJoin();
        switch (mode) {
        case ResumeMode::Next:
            stepUnlessDone(sent, readsValueWhenDone, join);
            break;
        case ResumeMode::Throw:
        case ResumeMode::Return:
            if (stateMayHoldListSteppedByIndex())
                makeWrapperWhereListWasSteppedByIndex();
            if (keepsDoneWord())
                storeWord(WordOfIteration::IsDone, boolean(true));
            resumeByMethod(mode, readWord(WordOfIteration::Iterator), sent ? sent : undefined(), readsValueWhenDone, join);
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
        storeWord(WordOfIteration::IsDone, boolean(true));

        BlockID finished = addBlock();
        BlockID closing = addBlock();
        BlockID after = addBlock();
        branch(wasDone, finished, closing);
        continueIn(finished);
        jumpTo(after);

        continueIn(closing);
        if (stateMayHoldListSteppedByIndex()) {
            BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
            continueIfIndexedState(byProtocol);
            await(undefined());
            jumpTo(after);
            continueIn(byProtocol);
        }
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

    void appendAll(Reference array, Reference iterable)
    {
        OpenedIteration opened = open(iterable);
        if (m_walker.isUnreachable())
            return;

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

    void openUnmodifiedGenerator(Reference iterable, NOESCAPE const Invocable<void(Reference, Reference)> auto& leaveOpened)
    {
        TypeArena& types = m_walker.types();
        const BuiltinTable& table = m_walker.program().builtins();
        if (m_walker.isUnreachable() || !m_walker.has(Mechanism::Guesses) || !m_walker.has(Mechanism::BuiltinCalls) || !m_walker.opensGuesses() || !engineAnswersForGenerators(m_walker) || !hasInstanceTest(table, BuiltinClasses::Generator) || !types.overlap(m_walker.typeOf(iterable), types.builtinClass(BuiltinClasses::Generator)))
            return;
        BlockID isOther = addBlock();
        Reference generator = continueIfTypeMatches(iterable, types.builtinClass(BuiltinClasses::Generator), isOther);
        Draft test = draft(Opcode::IsUnmodifiedGenerator);
        test.setValueOperand(generator);
        BlockID isAsMade = addBlock();
        branch(build(WTF::move(test)), isAsMade, isOther);
        continueIn(isAsMade);
        buildFirstReadsThatOpenOfGeneratorLeavesOut(m_walker, m_origin);
        Draft next = draft(Opcode::Builtin);
        next.setBuiltin(table.canonicalID(Builtins::GeneratorPrototypeNext));
        next.setIsNotReadFromHolder(true);
        leaveOpened(generator, build(WTF::move(next)));
        continueIn(isOther);
    }

    Walker::IndexedIteration openDelegation(Reference iterable)
    {
        Walker::IndexedIteration iteration;
        iteration.canSkipDoneChecks = true;
        iteration.hasProtocolFallback = true;
        iteration.isProtocolOnly = true;
        iteration.isDelegating = true;
        iteration.position = integer(0);
        BlockID after = addBlock();
        iteration.iterator = m_walker.addParameter(after, { }, Representation::JSValue);
        iteration.next = m_walker.addParameter(after, { }, Representation::JSValue);
        iteration.stepsGenerator = m_walker.addParameter(after, TypeTable::boolean, Representation::Boolean);
        openUnmodifiedGenerator(iterable, [&](Reference generator, Reference next) {
            jumpTo(after, { generator, next, boolean(true) });
        });
        OpenedIteration opened = open(iterable);
        jumpTo(after, { opened.iterator, opened.next, m_walker.isUnreachable() ? Reference() : testForEngineGenerator(opened) });
        continueIn(after);
        return iteration;
    }

    bool stepsMatchesOfOpen()
    {
        if (!m_site || m_site->opcode() != Opcode::IteratorOpen || m_site->as<Opcode::IteratorOpen>().isDestructuring())
            return false;
        const BodyIndex& index = m_walker.currentBodyIndex();
        const ValueID written = index.stripCasts(m_site->as<Opcode::IteratorOpen>().iterableOperand());
        return !index.isParameter(written) && isWrittenMatchAll(m_walker.program(), index.value(written)) && findHelper(IterationHelper::WayToStepMatches) && findHelper(IterationHelper::StepMatchesInWay);
    }

    Reference askWayToStep(const OpenedIteration& opened, bool stepsMatches)
    {
        Reference way = callHelper(findHelper(stepsMatches ? IterationHelper::WayToStepMatches : IterationHelper::WayToStepIterator), { opened.iterator, opened.next });
        if (!m_walker.fileHoldsGenerators() || m_walker.isUnreachable())
            return way;
        BlockID told = addBlock();
        Reference result = m_walker.addParameter(told, { }, Representation::JSValue);
        BlockID hasNextOfGenerators = addBlock();
        BlockID hasOtherNext = addBlock();
        BlockID isGenerator = addBlock();
        BlockID isNoGenerator = addBlock(BlockFrequency::Rare);
        branch(testType(way, TypeArena::fromPrimitives(Primitive::False)), hasNextOfGenerators, hasOtherNext);
        continueIn(hasOtherNext);
        jumpTo(told, { way });
        continueIn(hasNextOfGenerators);
        branch(testType(opened.iterator, m_walker.types().builtinClass(BuiltinClasses::Generator)), isGenerator, isNoGenerator);
        continueIn(isNoGenerator);
        jumpTo(told, { undefined() });
        continueIn(isGenerator);
        jumpTo(told, { boolean(false) });
        continueIn(told);
        return result;
    }

    Walker::IndexedIteration openByIndexOrProtocol(Reference iterable, const ClassSteppedUnseen& stepped, bool keepsStateInMemory, bool canSkipDoneChecks = false, bool opensGenerators = false)
    {
        Walker::IndexedIteration iteration;
        iteration.canSkipDoneChecks = !keepsStateInMemory && canSkipDoneChecks;
        iteration.builtinClass = stepped.builtinClass;
        iteration.contents = stepped.contents;
        iteration.kind = stepped.kind;
        iteration.hasProtocolFallback = true;
        iteration.position = integer(0);

        iteration.subclass = stepped.contents == ContentsStepped::List ? stepped.subclass : LayoutID();
        iteration.isStringOrArray = stepped.textMayComeInstead;
        iteration.isArrayOrMap = stepped.mapMayComeInstead;

        BlockID byProtocol = addBlock();
        Reference instance = continueIfPristine(iterable, stepped, byProtocol);
        Reference table = beginByIndex(instance, stepped.contents);
        BlockID after = addBlock();
        Vector<Reference, 4> byIndex { instance };
        iteration.list = m_walker.addParameter(after, { }, Representation::JSValue);
        iteration.arrayBeforeJoin = instance;
        if (keepsStateInMemory) {
            iteration.state = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.append(undefined());
        } else {
            iteration.iterator = m_walker.addParameter(after, { }, Representation::JSValue);
            iteration.next = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.appendList({ undefined(), undefined() });
            if (iteration.canSkipDoneChecks && laysVerdictOnNext(iterable) && findHelper(IterationHelper::WayToStepIterator) && findHelper(IterationHelper::StepIteratorInWay)) {
                iteration.wayToStep = m_walker.addParameter(after, { }, Representation::JSValue);
                iteration.stepsMatches = stepsMatchesOfOpen();
                byIndex.append(undefined());
            }
        }
        if (stepsThroughTable(stepped.contents) || stepped.mapMayComeInstead) {
            iteration.table = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.append(stepped.mapMayComeInstead ? undefined() : table);
        }
        jumpTo(after, byIndex.span());

        continueIn(byProtocol);
        if (iteration.isStringOrArray) {
            BlockID neitherTextNorList = addBlock();
            byIndex[0] = continueIfPristine(iterable, ClassSteppedUnseen { BuiltinClasses::String, ContentsStepped::Text }, neitherTextNorList);
            jumpTo(after, byIndex.span());
            continueIn(neitherTextNorList);
        }
        if (iteration.isArrayOrMap) {
            BlockID neitherListNorMap = addBlock();
            byIndex[0] = continueIfPristine(iterable, ClassSteppedUnseen { BuiltinClasses::Map, ContentsStepped::Map, stepped.kind }, neitherListNorMap);
            byIndex.last() = tableOf(byIndex[0]);
            jumpTo(after, byIndex.span());
            continueIn(neitherListNorMap);
        }
        if (m_iteratorCallResult) {
            if (m_placeWalkedBeforeCall)
                m_walker.walkStretch(m_iteratorCallBlock, m_iteratorCallBegin, *m_placeWalkedBeforeCall);
            if (!m_walker.isUnreachable())
                m_walker.walkStretch(m_iteratorCallBlock, m_placeWalkedBeforeCall ? *m_placeWalkedBeforeCall + 1 : m_iteratorCallBegin, m_iteratorCallEnd);
            if (m_walker.isUnreachable()) {
                continueIn(after);
                return iteration;
            }
            iterable = m_walker.referenceOf(m_iteratorCallResult);
        }
        if (opensGenerators && !keepsStateInMemory && !iteration.wayToStep && !iteration.table)
            openUnmodifiedGenerator(iterable, [&](Reference generator, Reference next) {
                jumpTo(after, { undefined(), generator, next });
            });
        OpenedIteration opened = m_iteratorReturned ? openReturned(makeIteratorAgain(m_iteratorReturned)) : open(iterable);
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
            iteration.listMadeForState = build(WTF::move(words));
            openedByProtocol.append(iteration.listMadeForState);
        } else {
            openedByProtocol.appendList({ opened.iterator, opened.next });
            if (iteration.wayToStep)
                openedByProtocol.append(askWayToStep(opened, iteration.stepsMatches));
        }
        if (iteration.table)
            openedByProtocol.append(undefined());
        jumpTo(after, openedByProtocol.span());
        return iteration;
    }

    Walker::IndexedIteration openByProtocolAlone(Reference iterable, Reference iteratorReturned = { })
    {
        Walker::IndexedIteration iteration;
        iteration.canSkipDoneChecks = true;
        iteration.hasProtocolFallback = true;
        iteration.isProtocolOnly = true;
        iteration.isAsync = m_kind == IteratorKind::Async;
        iteration.position = integer(0);
        iteration.isSteppedByCallsAlone = !laysVerdictOnNext(iterable);
        bool asksWay = !iteration.isSteppedByCallsAlone && findHelper(IterationHelper::WayToStepIterator) && findHelper(IterationHelper::StepIteratorInWay);
        OpenedIteration opened = iteratorReturned ? openReturned(iteratorReturned) : open(iterable);
        iteration.iterator = opened.iterator;
        iteration.next = opened.next;
        iteration.goesByDriver = opened.goesByDriver;
        if (asksWay) {
            iteration.stepsMatches = stepsMatchesOfOpen();
            iteration.wayToStep = askWayToStep(opened, iteration.stepsMatches);
        }
        return iteration;
    }

    std::optional<Walker::IndexedIteration> tryOpenReturnedIterator(Reference iterable, bool keepsStateInMemory, bool canSkipDoneChecks)
    {
        if (!goesByMethodsInSight() || !iterable || !iterable.isValue())
            return std::nullopt;
        PropertyKeyID key = m_walker.types().wellKnownSymbolKey(WellKnownSymbol::Iterator);
        FunctionID function = findMethodInSightOfProvenType(m_walker, m_walker.types().withoutNullish(m_walker.typeOf(iterable)), key);
        if (!function || !returnsFreshIterator(m_walker, function))
            return std::nullopt;
        iterable = requireObjectCoercible(iterable);
        Reference method = m_walker.isUnreachable() ? Reference() : readMethodInSightOfProvenObject(m_walker, iterable, key, m_origin);
        if (!method)
            return m_walker.isUnreachable() ? std::optional(Walker::IndexedIteration()) : std::nullopt;

        m_walker.count(StatisticID::IteratorMethodsCalledDirectly);
        Reference iterator = call(method, iterable);
        if (m_walker.isUnreachable())
            return Walker::IndexedIteration();
        Reference collection;
        std::optional<ClassSteppedUnseen> stepped = findCollectionOfIterator(m_walker, iterator, collection);
        if (!stepped)
            return openByProtocolAlone(iterable, iterator);
        m_walker.count(StatisticID::IteratorAllocationsElidedInInlinees);
        if (stepped->contents == ContentsStepped::List) {
            m_iteratorReturned = iterator;
            return openByIndexOrProtocol(collection, *stepped, keepsStateInMemory, canSkipDoneChecks);
        }
        Walker::IndexedIteration iteration;
        iteration.list = collection;
        iteration.builtinClass = stepped->builtinClass;
        iteration.contents = stepped->contents;
        iteration.kind = stepped->kind;
        iteration.position = integer(0);
        iteration.table = beginByIndex(collection, stepped->contents);
        iteration.canSkipDoneChecks = canSkipDoneChecks;
        return iteration;
    }

    Reference tableThatFirstStepBeginsIn(Reference instance, ContentsStepped contents) { return beginByIndex(instance, contents); }

    void takeStepsOutOfLine(Reference iterable, uint32_t numberOfSteps, Vector<Reference, 4>& taken)
    {
        if (m_walker.isInLoop()) {
            Draft check = draft(Opcode::RequireObjectCoercible);
            check.setValueOperand(iterable);
            check.setNullishAccess(NullishAccess::Iterate);
            iterable = build(WTF::move(check));
        }
        Draft steps = draft(Opcode::DestructureIterable);
        steps.setIterableOperand(iterable);
        steps.setCount(numberOfSteps);
        steps.setReadsListsByIndex(isArrayIteratorProtocolIntact(m_walker));
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

    Walker::IndexedIteration takeStepsAtOpen(Reference iterable, uint32_t numberOfSteps)
    {
        Walker::IndexedIteration iteration;
        iteration.hasPrecomputedSteps = true;
        takeStepsOutOfLine(iterable, numberOfSteps, iteration.precomputedSteps);
        return iteration;
    }

    std::optional<Walker::IndexedIteration> tryOpenWithIterationGuard(const BodyValue& open, Reference iterable, const ClassSteppedUnseen& stepped)
    {
        if (stepped.contents != ContentsStepped::List || stepped.kind != JSC::IterationKind::Values || stepped.restsOnWatchAboveInstances || stepped.methodAskedByItsByte)
            return std::nullopt;
        if (((!m_walker.canBuildIterationGuard() || !m_walker.canRestartIteration()) && !m_walker.isInIterationAssumedRestartable()) || !isArrayPrototypeChainSane(m_walker) || !isDestructuredArrayLikelyNumeric(m_walker, open, iterable))
            return std::nullopt;
        Walker::IndexedIteration iteration;
        BlockID otherwise = addBlock(BlockFrequency::Rare);
        if (m_walker.shouldAssumePristineDestructuring()) [[unlikely]] {
            TypeID instances = typeOfInstancesStepped(m_walker, stepped.builtinClass, stepped.contents);
            iteration.list = m_walker.types().isSubset(m_walker.typeOf(iterable), instances) ? iterable : continueIfTypeMatches(iterable, instances, otherwise);
            if (!m_walker.isUnreachable())
                markObjectPristine(m_walker, iteration.list);
        } else
            iteration.list = continueIfPristine(iterable, stepped, otherwise);
        BlockID held = m_walker.isUnreachable() ? BlockID() : m_walker.currentBlock();
        continueIn(otherwise);
        if (!m_walker.isUnreachable())
            m_walker.restartIteration();
        if (held)
            continueIn(held);
        iteration.builtinClass = stepped.builtinClass;
        iteration.contents = stepped.contents;
        iteration.kind = stepped.kind;
        iteration.position = integer(0);
        iteration.stepsOfPatternStandInRow = true;
        m_walker.recordSpeculation();
        m_walker.count(StatisticID::ArrayPatternsGuardedInIterations);
        return iteration;
    }

    Walker::IndexedIteration takePartsOfPairOrStepsAtOpen(const Walker::IndexedIteration& stepping, Reference pair)
    {
        Walker::IndexedIteration pattern;
        pattern.builtinClass = BuiltinClasses::Array;
        pattern.hasPrecomputedSteps = true;
        if (!stepping.hasProtocolFallback && !stepping.pairIsInParts) {
            pattern.precomputedSteps.append(stepping.keyOfPair);
            pattern.precomputedSteps.append(stepping.valueOfPair);
            m_walker.count(StatisticID::MapEntryAllocationsElided);
            return pattern;
        }
        BlockID after = addBlock();
        pattern.precomputedSteps.append(m_walker.addParameter(after, { }, Representation::JSValue));
        pattern.precomputedSteps.append(m_walker.addParameter(after, { }, Representation::JSValue));
        BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
        if (!m_walker.shouldSkipPairGuard()) [[likely]] {
            if (stepping.pairIsInParts) {
                BlockID isInParts = addBlock();
                branch(stepping.pairIsInParts, isInParts, byProtocol);
                continueIn(isInParts);
            } else
                continueIfIndexedIteration(stepping, byProtocol);
        }
        if (m_walker.checksProofs() && !m_walker.isUnreachable()) {
            Draft again = draft(Opcode::TypeTest);
            again.setValueOperand(stepping.list);
            again.setTestedType(typeOfInstancesStepped(m_walker, stepping.builtinClass, stepping.contents));
            m_walker.verifyRememberedTest(WTF::move(again));
        }
        if (!m_walker.isUnreachable())
            jumpTo(after, { stepping.keyOfPair, stepping.valueOfPair });
        continueIn(byProtocol);
        Vector<Reference, 4> taken;
        takeStepsOutOfLine(pair, 2, taken);
        if (!m_walker.isUnreachable())
            jumpTo(after, taken.span());
        continueIn(after);
        m_walker.count(StatisticID::MapEntryAllocationsElided);
        return pattern;
    }

    Walker::IndexedIteration takeStepsByProtocolAtOpen(Reference iterable, uint32_t numberOfSteps)
    {
        Walker::IndexedIteration iteration;
        iteration.hasPrecomputedSteps = true;
        OpenedIteration opened = open(iterable);
        if (m_walker.isUnreachable())
            return iteration;
        BlockID after = addBlock();
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            iteration.precomputedSteps.append(m_walker.addParameter(after, { }, Representation::JSValue));
        auto takeSteps = [&](Reference next) {
            Vector<Reference, 4> taken;
            for (uint32_t step = 0; step < numberOfSteps && !m_walker.isUnreachable(); ++step) {
                Reference answer = requireObject(call(next, opened.iterator), RuntimeErrorKind::IteratorResultNotObject);
                Reference done = truthOf(readOffAnswer(answer, BuiltinNames::done));
                if (m_walker.isUnreachable())
                    break;
                BlockID last = addBlock();
                BlockID further = addBlock();
                branch(done, last, further);
                continueIn(last);
                Vector<Reference, 4> takenWhereItEnds = taken;
                while (takenWhereItEnds.size() < numberOfSteps)
                    takenWhereItEnds.append(undefined());
                jumpTo(after, takenWhereItEnds.span());
                continueIn(further);
                taken.append(readOffAnswer(answer, BuiltinNames::value));
            }
            if (m_walker.isUnreachable())
                return;
            callReturn(opened.iterator, ResultOfReturnInIteration::MustBeObject);
            jumpTo(after, taken.span());
        };
        if (TypeID closures = closuresExpectedUnderNext(opened.next)) {
            BlockID byValue = addBlock(BlockFrequency::Unlikely);
            Reference known = continueIfTypeMatches(opened.next, closures, byValue);
            m_walker.recordSpeculation();
            m_walker.count(StatisticID::SpeculatedNextMethodsGuarded);
            takeSteps(known);
            continueIn(byValue);
        }
        if (TypeID closures = closuresWrittenUnderNext(opened.next)) {
            BlockID byValue = addBlock(BlockFrequency::Unlikely);
            Reference known = continueIfTypeMatches(opened.next, closures, byValue);
            m_walker.recordSpeculation();
            m_walker.count(StatisticID::DeclaredNextMethodsGuarded);
            takeSteps(known);
            continueIn(byValue);
        }
        takeSteps(opened.next);
        continueIn(after);
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
        return iteration;
    }

    Walker::IndexedIteration takeStepsThroughTableAtOpen(Reference iterable, uint32_t numberOfSteps, const ClassSteppedUnseen& stepped)
    {
        BlockID outOfLine = addBlock(BlockFrequency::Unlikely);
        Walker::IndexedIteration inLine;
        inLine.list = continueIfPristine(iterable, stepped, outOfLine);
        inLine.builtinClass = stepped.builtinClass;
        inLine.contents = stepped.contents;
        inLine.kind = stepped.kind;
        inLine.position = integer(0);
        inLine.table = tableOf(inLine.list);
        Vector<Reference, 4> taken;
        for (uint32_t step = 0; step < numberOfSteps; ++step) {
            Draft read = draft(Opcode::ResultAt);
            read.setResultsOperand(resumeByIndex(inLine, std::nullopt));
            read.setIndex(static_cast<uint32_t>(IteratorResumeResult::Value));
            taken.append(build(WTF::move(read)));
        }

        Walker::IndexedIteration iteration;
        iteration.hasPrecomputedSteps = true;
        BlockID after = addBlock();
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            iteration.precomputedSteps.append(m_walker.addParameter(after, { }, Representation::JSValue));
        jumpTo(after, taken.span());
        continueIn(outOfLine);
        taken.shrink(0);
        takeStepsOutOfLine(iterable, numberOfSteps, taken);
        jumpTo(after, taken.span());
        return iteration;
    }

    Walker::IndexedIteration takeSubclassElementsAtOpen(Reference iterable, uint32_t numberOfSteps, const ClassSteppedUnseen& stepped)
    {
        BlockID outOfLine = addBlock(BlockFrequency::Unlikely);
        Reference list = continueIfPristine(iterable, stepped, outOfLine);
        Vector<Reference, 4> taken;
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            taken.append(elementAt(list, integer(step), false));
        Walker::IndexedIteration iteration;
        iteration.hasPrecomputedSteps = true;
        BlockID after = addBlock();
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            iteration.precomputedSteps.append(m_walker.addParameter(after, { }, Representation::JSValue));
        jumpTo(after, taken.span());
        continueIn(outOfLine);
        taken.shrink(0);
        takeStepsOutOfLine(iterable, numberOfSteps, taken);
        jumpTo(after, taken.span());
        return iteration;
    }

    template<typename TakeAsWritten>
    Vector<Reference, 4> takeStepsOffCollection(Reference receiver, uint32_t numberOfSteps, const ClassSteppedUnseen& stepped, const LinkedContentsAtOperation* contents, const TakeAsWritten& takeAsWritten)
    {
        BlockID asWritten = addBlock(BlockFrequency::Unlikely);
        Walker::IndexedIteration inLine;
        inLine.list = continueIfPristine(receiver, stepped, asWritten);
        inLine.builtinClass = stepped.builtinClass;
        inLine.contents = stepped.contents;
        inLine.kind = stepped.kind;
        inLine.position = integer(0);
        inLine.table = beginByIndex(inLine.list, stepped.contents);
        inLine.isAsMadeForGood = receiver == m_instanceAsMadeForGood;
        if (contents && inLine.table) {
            inLine.keysProven = contents->key;
            inLine.valuesProven = contents->value;
        } else if (contents && contents->value)
            inLine.elementsProven = m_walker.types().withPrimitives(contents->value, Primitive::Undefined);
        Vector<Reference, 4> taken;
        for (uint32_t step = 0; step < numberOfSteps; ++step) {
            Draft read = draft(Opcode::ResultAt);
            read.setResultsOperand(resumeByIndex(inLine, std::nullopt));
            read.setIndex(static_cast<uint32_t>(IteratorResumeResult::Value));
            taken.append(build(WTF::move(read)));
        }

        BlockID after = addBlock();
        Vector<Reference, 4> met;
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            met.append(m_walker.addParameter(after, { }, Representation::JSValue));
        if (!m_walker.isUnreachable())
            jumpTo(after, taken.span());
        continueIn(asWritten);
        {
            Walker::ClosedArm closedArm(m_walker, true);
            taken.shrink(0);
            if (!m_walker.isUnreachable())
                takeAsWritten(taken);
            if (!m_walker.isUnreachable() && !m_walker.hasFailed())
                jumpTo(after, taken.span());
        }
        continueIn(after);
        return met;
    }

    Reference resumeByIndex(Walker::IndexedIteration& iteration, std::optional<uint32_t> stepIndex)
    {
        if (iteration.hasPrecomputedSteps) {
            if (iteration.restList && stepIndex == iteration.precomputedSteps.size()) {
                m_walker.bind(iteration.restArray, iteration.restList);
                return resultsOf(boolean(true), undefined());
            }
            RELEASE_ASSERT(stepIndex && *stepIndex < iteration.precomputedSteps.size());
            return resultsOf(boolean(false), iteration.precomputedSteps[*stepIndex]);
        }
        if (std::optional<int64_t> turn = iteration.precomputedSteps.isEmpty() ? std::nullopt : integerIn(iteration.position); turn && static_cast<uint64_t>(*turn) <= iteration.precomputedSteps.size()) {
            bool isDone = static_cast<uint64_t>(*turn) == iteration.precomputedSteps.size();
            Reference name = isDone ? undefined() : iteration.precomputedSteps[static_cast<size_t>(*turn)];
            iteration.position = integer(isDone ? Walker::doneIterationPosition : *turn + 1);
            return resultsOf(boolean(isDone), name);
        }
        if (iteration.isOverFlatEntries)
            return resumeThroughFlatEntries(iteration);
        if (iteration.isProtocolOnly) {
            BlockID stepped = addBlock();
            Reference isLast = m_walker.addParameter(stepped, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
            Reference stepValue = m_walker.addParameter(stepped, { }, Representation::JSValue);
            stepByProtocolBesideIndex(iteration, stepped);
            return resultsOf(isLast, stepValue);
        }
        bool isEffectFree = areElementReadsEffectFree(iteration);
        if (!iteration.hasProtocolFallback) {
            std::optional<int64_t> position = integerIn(iteration.position);
            std::optional<uint32_t> length = lengthKnownOf(iteration);
            if (position && length && !iteration.literal)
                m_walker.checkWhatIsInSightOfList(iteration.list, m_origin);
            bool isBehindEnd = position && (*position == Walker::doneIterationPosition || (length && *position >= *length));
            if (isBehindEnd) {
                iteration.position = integer(Walker::doneIterationPosition);
                return resultsOf(boolean(true), undefined());
            }
            if (position && length) {
                Reference value = stepValueAt(iteration, iteration.list, iteration.position, isEffectFree, std::nullopt);
                iteration.position = integer(*position + 1);
                return resultsOf(boolean(false), value);
            }
            if (position && iteration.stepsOfPatternStandInRow && isEffectFree && !iteration.table && iteration.kind == JSC::IterationKind::Values) {
                SetForScope takesApart(m_takesListApart, true);
                if (!*position)
                    m_walker.count(StatisticID::ArrayPatternsReadByIndex);
                Reference value = stepValueAt(iteration, iteration.list, iteration.position, true, std::nullopt);
                iteration.position = integer(*position + 1);
                return resultsOf(boolean(false), value);
            }
        }

        BlockID after = addBlock();
        Reference done = m_walker.addParameter(after, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
        Reference value = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference position = m_walker.addParameter(after, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::doneIterationPosition }), { }));
        Reference table = iteration.table ? m_walker.addParameter(after, { }, Representation::JSValue) : Reference();
        Reference keyOfPair = !iteration.isEntryPairElided ? Reference() : iteration.table ? m_walker.addParameter(after, { }, Representation::JSValue) : m_walker.addParameter(after, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::doneIterationPosition }), { }));
        Reference valueOfPair = iteration.isEntryPairElided ? m_walker.addParameter(after, { }, Representation::JSValue) : Reference();
        Reference pairIsInParts = mayTakePairApartWhenDone(iteration) ? m_walker.addParameter(after, TypeTable::boolean, m_walker.formOfParameter(TypeTable::boolean, { })) : Reference();

        if (iteration.hasProtocolFallback)
            iteration.elementsProven = { };
        stepWithProtocolFallback(iteration, isEffectFree, after);
        iteration.position = position;
        iteration.table = table;
        iteration.keyOfPair = keyOfPair;
        iteration.valueOfPair = valueOfPair;
        iteration.pairIsInParts = pairIsInParts;
        return resultsOf(done, value);
    }

    void stepWithProtocolFallback(const Walker::IndexedIteration& iteration, bool isEffectFree, BlockID after)
    {
        Reference list = iteration.list;
        if (iteration.hasProtocolFallback) {
            const bool passesGeneratorsOn = iteration.wayToStep && m_walker.fileHoldsGenerators() && m_walker.program().at(m_walker.unit().function).isCoroutine();
            const BlockFrequency frequencyOfProtocol = passesGeneratorsOn ? BlockFrequency::Normal : BlockFrequency::Unlikely;
            BlockID byProtocol = addBlock(iteration.isStringOrArray && iteration.contents == ContentsStepped::Text ? BlockFrequency::Normal : frequencyOfProtocol);
            if (iteration.isStringOrArray && !m_walker.opensGuesses()) {
                m_whereNeitherTextNorListIsStepped = addBlock();
                BlockID holdsOneOfThem = addBlock(BlockFrequency::Unlikely);
                branch(testType(iteration.list, TypeTable::undefined), m_whereNeitherTextNorListIsStepped, holdsOneOfThem);
                continueIn(holdsOneOfThem);
            }
            list = continueIfIndexedIteration(iteration, byProtocol);
            BlockID byIndex = m_walker.isUnreachable() ? BlockID() : m_walker.currentBlock();
            continueIn(byProtocol);
            {
                SetForScope frequency(m_frequencyOfBlocksAdded, frequencyOfProtocol);
                stepByProtocolBesideIndex(iteration, after);
            }
            if (byIndex)
                continueIn(byIndex);
        }
        if (iteration.otherForm == Walker::IndexedIteration::OtherForm::PiecesOfSplit) {
            stepOverPiecesOfString(m_walker, list, iteration.subject, iteration.position, m_origin, scopedLambda<void()>([&] {
                endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::doneIterationPosition));
            }), scopedLambda<void(Reference, Reference)>([&](Reference piece, Reference next) {
                endStepByProtocol(iteration, after, boolean(false), piece, next);
            }));
        } else if (iteration.otherForm == Walker::IndexedIteration::OtherForm::MatchesOfRegExp) {
            stepOverMatchesOfString(m_walker, list, iteration.subject, m_origin, scopedLambda<void()>([&] {
                endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::doneIterationPosition));
            }), scopedLambda<void(Reference)>([&](Reference match) {
                endStepByProtocol(iteration, after, boolean(false), match, integer(0));
            }));
        } else if (stepsThroughTable(iteration.contents))
            stepThroughTable(iteration, list, after);
        else {
            std::optional<uint32_t> length = lengthKnownOf(iteration);
            stepByIndex(iteration, list, length ? integer(*length) : lengthOf(iteration, list), isEffectFree, after);
        }
    }

    Reference resumeThroughFlatEntries(Walker::IndexedIteration& iteration)
    {
        BlockID after = addBlock();
        Reference done = m_walker.addParameter(after, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
        Reference key = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference value = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference position = m_walker.addParameter(after, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::doneIterationPosition }), { }));

        Reference isInside = operatorOnNumbers(Opcode::Less, iteration.position, lengthOf(iteration, iteration.list));
        BlockID reading = addBlock();
        BlockID ending = addBlock();
        branch(isInside, reading, ending);
        continueIn(ending);
        jumpTo(after, { boolean(true), undefined(), undefined(), integer(Walker::doneIterationPosition) });

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

    Reference stepValueAt(const Walker::IndexedIteration& iteration, Reference list, Reference position, bool isEffectFree, const std::optional<PassedTest>& insideLength, bool shouldElideEntryPair = false)
    {
        if (iteration.kind == JSC::IterationKind::Keys)
            return position;
        bool restsOnLinksWord = isEffectFree && !iteration.literal && !(isFreshAllocation(m_walker, iteration.list) && isKnownPristine(m_walker, iteration.list));
        Reference element = elementAt(list, position, isEffectFree, insideLength, iteration.elementsProven, restsOnLinksWord, iteration.listHasNoHoles);
        if (iteration.kind == JSC::IterationKind::Values && !m_walker.isUnreachable() && !m_walker.hasFailed())
            prefetchIteratedElementsAhead(m_walker, list, position);
        if (iteration.kind != JSC::IterationKind::Entries || shouldElideEntryPair || m_walker.isUnreachable())
            return element;
        Draft pair = draft(Opcode::NewArray);
        pair.setContents(TypeTable::anything);
        pair.setElementsOperands(std::array { position, element });
        return build(WTF::move(pair));
    }

    ValueID findStepValue(const BodyValue& resume)
    {
        const BodyIndex& index = m_walker.currentBodyIndex();
        ValueID stepValue;
        for (BodyValue operation : index.block(index.blockOf(resume.id())).values()) {
            if (operation.opcode() == Opcode::ResultAt && operation.as<Opcode::ResultAt>().resultsOperand() == resume.id() && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
                stepValue = operation.id();
        }
        return index.useCount(resume.id()) == 2 ? stepValue : ValueID();
    }

    bool isIndexAddend(ValueID stepValue)
    {
        const BodyIndex& index = m_walker.currentBodyIndex();
        ValueID name = stepValue;
        ValueID user = index.onlyUsingValue(name);
        while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
            name = user;
            user = index.onlyUsingValue(user);
        }
        if (!user || (index.opcodeOf(user) != Opcode::Add && index.opcodeOf(user) != Opcode::Sub))
            return false;
        BodyValue sum = index.value(user);
        ValueID other = sum.operand(0) == name ? sum.operand(1) : sum.operand(0);
        Reference built = other.index() < stepValue.index() ? m_walker.referenceOf(other) : Reference();
        Type type = built ? m_walker.types().at(m_walker.typeOf(built)) : Type();
        return built && (type.primitives & Primitive::Integer) && type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && usesSpeakForInt32(m_walker, user);
    }

    bool isUsedAsIndex(ValueID stepValue)
    {
        return stepValue && m_walker.canBuildIterationGuard() && (turnTakesElementForKeyInListReadsInLoops(m_walker, stepValue) || isIndexAddend(stepValue));
    }

    bool isFollowedByDoneBranch(const BodyValue& resume)
    {
        const BodyIndex& index = m_walker.currentBodyIndex();
        BodyTerminator end = index.terminator(index.blockOf(resume.id()));
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        if (!m_walker.has(Mechanism::Folding) || !condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
            return false;
        auto asked = index.value(condition).as<Opcode::ResultAt>();
        return asked.resultsOperand() == resume.id() && asked.index() == static_cast<uint32_t>(IteratorResumeResult::Done);
    }

    bool tryThreadIndexedStep(Reference state, const BodyValue& resume)
    {
        const Walker::IndexedIteration& record = m_walker.indexedIteration(state);
        if ((!record.isIndexedArrayIteration() && !record.isIndexedTypedArrayIteration() && !record.isIndexedStringIteration()) || !record.head || !findWayOfIteration(m_walker.currentBodyIndex(), resume.operand(0)).hasSingleStepAtHeader)
            return false;
        Walker::IndexedIteration entered = record;
        const BodyIndex& index = m_walker.currentBodyIndex();
        if (!isFollowedByDoneBranch(resume) || integerIn(entered.position) || !m_walker.canThreadBranchAfter(resume))
            return false;
        bool isEffectFree = areElementReadsEffectFree(entered);
        Reference length = lengthOf(entered, entered.list);
        Reference isInside = operatorOnNumbers(Opcode::Less, entered.position, length);
        if (m_walker.isUnreachable())
            return true;
        BlockID giving = addBlock();
        BlockID finished = addBlock();
        m_walker.branchOnTest(isInside, giving, finished);
        if (index.block(index.blockOf(resume.id())).hasHandler())
            m_walker.count(StatisticID::IteratorStepsThreadedUnderHandlers);

        continueIn(finished);
        if (entered.contents == ContentsStepped::View && !m_walker.isUnreachable()) {
            if (entered.canSkipDoneChecks)
                requireAttached(entered.list);
            else
                requireAttachedUnlessDone(entered.list, entered.position);
            if (m_walker.hasFailed())
                return true;
        }
        m_walker.indexedIteration(state).position = integer(Walker::doneIterationPosition);
        m_walker.bind(resume.id(), resultsOf(boolean(true), undefined()));
        if (!m_walker.threadBranchAfter(resume)) {
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
            return true;
        }

        continueIn(giving);
        Reference inside = narrowed(entered.position, isInside, OperandIndex::Less::left);
        if (entered.contents == ContentsStepped::Text) {
            CharacterOfText stepped = characterAt(entered.list, length, inside);
            if (m_walker.isUnreachable() || m_walker.hasFailed())
                return true;
            m_walker.indexedIteration(state).position = stepped.behind;
            m_walker.bind(resume.id(), resultsOf(boolean(false), stepped.character));
            if (!m_walker.threadBranchAfter(resume))
                m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
            return true;
        }
        ValueID stepValue = entered.kind == JSC::IterationKind::Values ? findStepValue(resume) : ValueID();
        m_isStepUsedAsIndex = isUsedAsIndex(stepValue);
        m_valueThatStepHandsToArithmeticAlone = stepValue && !m_isStepUsedAsIndex && onlyUserTakesUndefinedForNaNInTypedElements(m_walker, index.value(stepValue)) ? stepValue : ValueID();
        std::optional<PassedTest> insideLength = m_walker.boundsProofAfterComparison(entered.list, inside, length, isInside);
        Reference given = entered.contents == ContentsStepped::View ? elementOfView(entered.list, inside, insideLength) : stepValueAt(entered, entered.list, inside, isEffectFree, insideLength, entered.isEntryPairElided);
        m_valueThatStepHandsToArithmeticAlone = { };
        m_isStepUsedAsIndex = false;
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return true;
        m_walker.indexedIteration(state).position = operatorOnNumbers(Opcode::Increment, inside);
        if (m_walker.isUnreachable())
            return true;
        if (entered.isEntryPairElided) {
            m_walker.indexedIteration(state).keyOfPair = inside;
            m_walker.indexedIteration(state).valueOfPair = given;
            given = undefined();
        }
        m_walker.bind(resume.id(), resultsOf(boolean(false), given));
        if (entered.kind == JSC::IterationKind::Entries && !entered.isEntryPairElided)
            noteFreshList(m_walker, given);
        if (!m_walker.threadBranchAfter(resume))
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
        return true;
    }

    bool tryThreadStepWithFallback(Reference state, const BodyValue& resume)
    {
        const Walker::IndexedIteration& record = m_walker.indexedIteration(state);
        if (!record.hasProtocolFallback || record.isProtocolOnly || record.state || record.hasPrecomputedSteps || record.isOverFlatEntries || record.literal || (record.isEntryPairElided && !record.table) || !record.head || !findWayOfIteration(m_walker.currentBodyIndex(), resume.operand(0)).hasSingleStepAtHeader)
            return false;
        Walker::IndexedIteration entered = record;
        if (!isFollowedByDoneBranch(resume) || integerIn(entered.position) || !m_walker.canThreadBranchAfter(resume))
            return false;
        entered.elementsProven = { };
        BlockID finished = addBlock();
        BlockID giving = addBlock();
        Reference given = m_walker.addParameter(giving, { }, Representation::JSValue);
        Reference behind = m_walker.addParameter(giving, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, Walker::doneIterationPosition }), { }));
        Reference table = entered.table ? m_walker.addParameter(giving, { }, Representation::JSValue) : Reference();
        Reference keyOfPair = entered.isEntryPairElided ? m_walker.addParameter(giving, { }, Representation::JSValue) : Reference();
        Reference valueOfPair = entered.isEntryPairElided ? m_walker.addParameter(giving, { }, Representation::JSValue) : Reference();
        {
            SetForScope doneBlockScope(m_doneBlock, finished);
            stepWithProtocolFallback(entered, areElementReadsEffectFree(entered), giving);
        }
        if (m_walker.hasFailed())
            return true;

        continueIn(finished);
        if (!m_walker.isUnreachable()) {
            m_walker.indexedIteration(state).position = integer(Walker::doneIterationPosition);
            if (table)
                m_walker.indexedIteration(state).table = entered.canSkipDoneChecks ? undefined() : build(draft(Opcode::CollectionIteratorClosedMark));
            m_walker.bind(resume.id(), resultsOf(boolean(true), undefined()));
            if (!m_walker.threadBranchAfter(resume)) {
                m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
                return true;
            }
        }

        continueIn(giving);
        if (m_walker.isUnreachable())
            return true;
        m_walker.indexedIteration(state).position = behind;
        if (table)
            m_walker.indexedIteration(state).table = table;
        if (keyOfPair) {
            m_walker.indexedIteration(state).keyOfPair = keyOfPair;
            m_walker.indexedIteration(state).valueOfPair = valueOfPair;
        }
        m_walker.bind(resume.id(), resultsOf(boolean(false), given));
        if (!m_walker.threadBranchAfter(resume))
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
        return true;
    }

    bool tryThreadKnownNextStep(Reference state, const BodyValue& resume)
    {
        const Walker::IndexedIteration& entered = m_walker.indexedIteration(state);
        if (!entered.isProtocolOnly || !entered.canSkipDoneChecks || entered.wayToStep || !goesByMethodsInSight())
            return false;
        Reference next = entered.next;
        Reference iterator = entered.iterator;
        if (!findOnlyCallee(m_walker, resume, next))
            return false;
        if (!isFollowedByDoneBranch(resume) || !m_walker.canThreadBranchAfter(resume))
            return false;
        Reference answer = requireObject(call(next, iterator), RuntimeErrorKind::IteratorResultNotObject);
        Reference done = truthOf(readOffAnswer(answer, BuiltinNames::done));
        if (m_walker.isUnreachable())
            return true;
        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(done, last, further);

        continueIn(last);
        m_walker.bind(resume.id(), resultsOf(boolean(true), undefined()));
        if (!m_walker.threadBranchAfter(resume)) {
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
            return true;
        }

        continueIn(further);
        Reference value = readOffAnswer(answer, BuiltinNames::value);
        if (m_walker.isUnreachable())
            return true;
        m_walker.bind(resume.id(), resultsOf(boolean(false), value));
        if (!m_walker.threadBranchAfter(resume))
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
        return true;
    }

    bool tryThreadTableStep(Reference state, const BodyValue& resume)
    {
        const Walker::IndexedIteration& record = m_walker.indexedIteration(state);
        if (record.hasProtocolFallback || record.hasPrecomputedSteps || record.isOverFlatEntries || record.literal || !record.table || !stepsThroughTable(record.contents) || !record.head || !findWayOfIteration(m_walker.currentBodyIndex(), resume.operand(0)).hasSingleStepAtHeader)
            return false;
        Walker::IndexedIteration entered = record;
        if (!isFollowedByDoneBranch(resume) || entered.otherForm != Walker::IndexedIteration::OtherForm::None || !m_walker.canThreadBranchAfter(resume))
            return false;
        BlockID finished = addBlock();
        if (!entered.canSkipDoneChecks) {
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(entered.table);
            Reference wasClosed = build(WTF::move(isClosed));
            if (m_walker.isUnreachable())
                return true;
            BlockID stepping = addBlock();
            branch(wasClosed, finished, stepping);
            continueIn(stepping);
        }
        bool isMap = entered.contents == ContentsStepped::Map;
        Draft next = draft(isMap ? Opcode::MapNext : Opcode::SetNext);
        next.setOperand(0, entered.list);
        next.setTableOperand(entered.table);
        next.setEntryOperand(entered.position);
        Reference found = build(WTF::move(next));
        BlockID giving = addBlock();
        branch(resultAt(found, MapNextResult::HasEntry), giving, finished);

        continueIn(finished);
        m_walker.indexedIteration(state).table = entered.canSkipDoneChecks ? undefined() : build(draft(Opcode::CollectionIteratorClosedMark));
        m_walker.bind(resume.id(), resultsOf(boolean(true), undefined()));
        if (!m_walker.threadBranchAfter(resume)) {
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
            return true;
        }

        continueIn(giving);
        Reference key = !isMap || entered.kind != JSC::IterationKind::Values ? resultAt(found, MapNextResult::Key, isMap ? entered.keysProven : entered.valuesProven) : Reference();
        Reference value = !isMap ? key : entered.kind != JSC::IterationKind::Keys ? resultAt(found, MapNextResult::Value, entered.valuesProven) : Reference();
        Reference given = entered.kind == JSC::IterationKind::Keys ? key : value;
        if (entered.kind == JSC::IterationKind::Entries) {
            Draft pair = draft(Opcode::NewArray);
            pair.setContents(TypeTable::anything);
            pair.setElementsOperands(std::array { key, value });
            given = build(WTF::move(pair));
        }
        m_walker.indexedIteration(state).position = operatorOnNumbers(Opcode::Increment, resultAt(found, MapNextResult::Entry));
        m_walker.indexedIteration(state).table = tableOf(entered.list);
        if (m_walker.isUnreachable())
            return true;
        m_walker.bind(resume.id(), resultsOf(boolean(false), given));
        if (entered.kind == JSC::IterationKind::Entries)
            noteFreshList(m_walker, given);
        if (!m_walker.threadBranchAfter(resume))
            m_walker.fail("a path of a threaded step did not visit the end of its block"_s);
        return true;
    }

    void closeByIndex(Walker::IndexedIteration& iteration, IteratorCompletion completion)
    {
        if (!iteration.hasProtocolFallback)
            return;
        if (completion == IteratorCompletion::Throw && iteration.canSkipDoneChecks && findHelper(IterationHelper::CloseIteratorAfterThrow)) {
            callReturnAndDropWhatItThrows(iteration.iterator);
            return;
        }
        if (iteration.isProtocolOnly) {
            if (completion == IteratorCompletion::Normal)
                callReturn(iteration.iterator, ResultOfReturnInIteration::MustBeObject);
            else
                callReturnAndDropWhatItThrows(iteration.iterator);
            return;
        }
        BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        continueIfIndexedIteration(iteration, byProtocol);
        jumpTo(after);

        continueIn(byProtocol);
        if (iteration.isStringOrArray) {
            BlockID neitherTextNorList = addBlock(BlockFrequency::Unlikely);
            continueIfIndexedIteration(overOtherOfTextAndList(iteration), neitherTextNorList);
            jumpTo(after);
            continueIn(neitherTextNorList);
        }
        if (iteration.isArrayOrMap) {
            BlockID neitherListNorMap = addBlock(BlockFrequency::Unlikely);
            continueIfIndexedIteration(overMapInsteadOfList(iteration), neitherListNorMap);
            jumpTo(after);
            continueIn(neitherListNorMap);
        }
        if (iteration.state) {
            takeStateInList(iteration.state, iteration.listMadeForState);
            closeUnlessDone(completion);
        } else {
            if (!iteration.canSkipDoneChecks) {
                BlockID closing = addBlock();
                BlockID finished = addBlock();
                branch(operatorOnNumbers(Opcode::Less, iteration.position, integer(Walker::doneIterationPosition)), closing, finished);
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

    template<typename Functor>
    void appendElementsOf(Reference array, Reference list, const Functor& isTakenByCallAlone)
    {
        const Code& code = m_walker.code();
        std::optional<uint64_t> lengthAsMade = list && list.isValue() ? lengthThatListIsMadeWith(code, code.at(underlyingCellOf(code, list.id()))) : std::nullopt;
        if (std::optional<Vector<Reference, 8>> elements = lengthAsMade ? elementsOfFreshList(m_walker, list) : std::nullopt; elements && (elements->size() == *lengthAsMade || isTakenByCallAlone())) {
            m_walker.checkWhatIsInSightOfList(list, m_origin);
            for (uint32_t position = 0; position < elements->size(); ++position)
                appendElement(array, elementAt(list, integer(position), true));
            return;
        }
        Draft append = draft(Opcode::AppendElements);
        append.setArrayOperand(array);
        append.setSourceOperand(list);
        build(WTF::move(append));
    }

    void appendMembersOrAllOutOfLine(Reference array, Reference iterable, const ClassSteppedUnseen& stepped)
    {
        RELEASE_ASSERT(isListedByAppendMembers(stepped));
        BlockID outOfLine = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        Draft append = draft(Opcode::AppendMembers);
        append.setArrayOperand(array);
        append.setSourceOperand(continueIfPristine(iterable, stepped, outOfLine));
        build(WTF::move(append));
        jumpTo(after);
        continueIn(outOfLine);
        appendAllOutOfLine(array, iterable);
        jumpTo(after);
    }

    void appendElementsOfSubclass(Reference array, Reference iterable, const ClassSteppedUnseen& stepped, NOESCAPE const Invocable<bool()> auto& isTakenByCallsAlone)
    {
        BlockID outOfLine = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        Reference list = continueIfPristine(iterable, stepped, outOfLine);
        if (list && !m_walker.isUnreachable()) {
            appendElementsOf(array, list, isTakenByCallsAlone);
            jumpTo(after);
        }
        continueIn(outOfLine);
        appendAllOutOfLine(array, iterable);
        jumpTo(after);
        continueIn(after);
    }

    bool walkCallApartFromWhatIsNoList(const BodyValue& spread, Reference iterable, const ClassSteppedUnseen& stepped)
    {
        const BodyIndex& index = m_walker.currentBodyIndex();
        BodyBlock block = index.block(index.blockOf(spread.id()));
        uint32_t place = spread.id().index() - block.firstValue().index();
        ValueID call(spread.id().index() + 1);
        if (!m_walker.isInLoop() || !m_walker.opensGuesses() || m_walker.someFrameWalksStretchInsideAnother() || m_walker.hasStringsInBuilders())
            return false;
        ValueID copy = spread.as<Opcode::AppendSpread>().arrayOperand();
        if (place + 2 > m_walker.endOfStretchBeingWalked() || index.opcodeOf(call) != Opcode::CallList || index.value(call).as<Opcode::CallList>().listOperand() != copy)
            return false;
        if (index.isParameter(copy) || !block.holds(copy) || index.opcodeOf(copy) != Opcode::NewArray || !index.value(copy).as<Opcode::NewArray>().elementsOperands().empty() || index.useCount(copy) != 2)
            return false;
        m_walker.count(StatisticID::SpreadCallsSpecializedForArrays);
        uint32_t placeOfCopy = copy.index() - block.firstValue().index();
        BlockID isNoList = addBlock(BlockFrequency::Unlikely);
        Reference list = continueIfPristine(iterable, stepped, isNoList);
        BlockID whereListIsAsMade = m_walker.currentBlock();
        bool listIsReached = !m_walker.isUnreachable();
        BlockID behind = addBlock();
        bool isRead = !index.isUnused(call);
        Reference answer = isRead ? m_walker.addParameter(behind, { }, Representation::JSValue) : Reference();
        auto walkUpToCallFrom = [&](uint32_t begin) {
            m_walker.walkStretch(block.id(), begin, place + 2);
            if (m_walker.isUnreachable() || m_walker.hasFailed())
                return;
            if (!isRead) {
                m_walker.jumpTo(behind, { });
                return;
            }
            Reference given = m_walker.referenceOf(call);
            m_walker.jumpTo(behind, singleElementSpan(given));
        };
        continueIn(isNoList);
        {
            Walker::ClosedArm closedArm(m_walker, true);
            if (!m_walker.isUnreachable())
                m_walker.walkStretch(block.id(), placeOfCopy, placeOfCopy + 1);
            if (!m_walker.isUnreachable() && !m_walker.hasFailed())
                walkUpToCallFrom(place);
        }
        if (m_walker.hasFailed())
            return true;
        if (listIsReached) {
            continueIn(whereListIsAsMade);
            ValueID input = spread.as<Opcode::AppendSpread>().iterableOperand();
            Reference spreadInFront = m_walker.referenceBoundTo(input);
            Reference copyInFront = m_walker.referenceBoundTo(copy);
            m_walker.walkStretch(block.id(), placeOfCopy, placeOfCopy + 1);
            m_walker.bind(input, list);
            if (!m_walker.isUnreachable() && !m_walker.hasFailed())
                walkUpToCallFrom(place);
            m_walker.bind(input, spreadInFront);
            m_walker.bind(copy, copyInFront);
            if (m_walker.hasFailed())
                return true;
        }
        continueIn(behind);
        if (isRead && !m_walker.isUnreachable())
            m_walker.bind(call, answer);
        m_walker.walkStretch(block.id(), place + 2, place + 2);
        return true;
    }

    void appendMembersOf(Reference array, Reference collection, const ClassSteppedUnseen& stepped)
    {
        Draft append = draft(stepped.contents == ContentsStepped::Map && stepped.kind == JSC::IterationKind::Values ? Opcode::AppendValuesOfMap : Opcode::AppendMembers);
        append.setArrayOperand(array);
        append.setSourceOperand(collection);
        build(WTF::move(append));
    }

    void appendMembersOrWalkAsWritten(Reference array, Reference collection, std::span<const ClassSteppedUnseen> tables, BlockID inputBlock, uint32_t begin, uint32_t end)
    {
        BlockID after = addBlock();
        for (const ClassSteppedUnseen& stepped : tables) {
            BlockID asWritten = addBlock(BlockFrequency::Unlikely);
            appendMembersOf(array, continueIfPristine(collection, stepped, asWritten), stepped);
            jumpTo(after);
            continueIn(asWritten);
            if (m_walker.isUnreachable())
                break;
        }
        {
            Walker::ClosedArm closedArm(m_walker, true);
            if (!m_walker.isUnreachable())
                m_walker.walkStretch(inputBlock, begin, end);
        }
        jumpTo(after);
    }

    void takeCollectionOrWalkGeneric(Reference collection, std::span<const ClassSteppedUnseen> tables, const CopyByArrayFrom& copy, BlockID inputBlock, uint32_t begin, uint32_t end)
    {
        BlockID after = addBlock();
        Reference given = m_walker.addParameter(after, { }, Representation::JSValue);
        Reference isCopied = m_walker.addParameter(after, TypeTable::boolean, Representation::Boolean);
        for (const ClassSteppedUnseen& stepped : tables) {
            BlockID asWritten = addBlock(BlockFrequency::Unlikely);
            Reference taken = continueIfPristine(collection, stepped, asWritten);
            jumpTo(after, { taken, boolean(true) });
            continueIn(asWritten);
            if (m_walker.isUnreachable())
                break;
        }
        {
            Walker::ClosedArm closedArm(m_walker, true);
            if (!m_walker.isUnreachable())
                m_walker.walkStretch(inputBlock, begin, end);
            if (!m_walker.isUnreachable() && !m_walker.hasFailed())
                jumpTo(after, { m_walker.referenceOf(copy.given), m_walker.referenceOf(copy.test) });
        }
        continueIn(after);
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return;
        m_walker.bind(copy.given, given);
        m_walker.bind(copy.test, isCopied);
    }

    void takePermanentlyUnmodifiedCollection(Reference collection, const CopyByArrayFrom& copy)
    {
        m_walker.bind(copy.given, collection);
        m_walker.bind(copy.test, boolean(true));
    }

    void testForUnmodifiedSet(const BodyValue& test, Reference copied, const ClassSteppedUnseen& sets)
    {
        BlockID isOther = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        Reference isCopied = m_walker.addParameter(after, TypeTable::boolean, Representation::Boolean);
        continueIfPristine(copied, sets, isOther);
        jumpTo(after, { boolean(true) });
        continueIn(isOther);
        if (!m_walker.isUnreachable()) {
            Draft asks = draft(Opcode::IsPristineArray);
            asks.setValueOperand(copied);
            Reference isArray = build(WTF::move(asks));
            jumpTo(after, { isArray });
        }
        continueIn(after);
        if (!m_walker.isUnreachable())
            m_walker.bind(test.id(), isCopied);
    }

    void appendCopiedCollection(Reference array, Reference given, std::span<const ClassSteppedUnseen> collections)
    {
        BlockID after = addBlock();
        for (const ClassSteppedUnseen& stepped : collections) {
            TypeID instances = typeOfInstancesStepped(m_walker, stepped.builtinClass, stepped.contents);
            if (m_walker.types().isSubset(m_walker.typeOf(given), instances)) {
                appendMembersOf(array, given, stepped);
                jumpTo(after);
                return;
            }
            if (!m_walker.types().overlap(m_walker.typeOf(given), instances))
                continue;
            BlockID isOther = addBlock(BlockFrequency::Unlikely);
            appendMembersOf(array, continueIfTypeMatches(given, instances, isOther), stepped);
            jumpTo(after);
            continueIn(isOther);
        }
        appendAllOutOfLine(array, given);
        jumpTo(after);
    }

    void appendAllOutOfLine(Reference array, Reference iterable)
    {
        Draft append = draft(Opcode::AppendIterable);
        append.setArrayOperand(array);
        append.setIterableOperand(iterable);
        append.setReadsListsByIndex(isArrayIteratorProtocolIntact(m_walker));
        build(WTF::move(append));
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
    }

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

    void appendToList(Draft&& append)
    {
        Reference array = append.operand(0);
        if (!lengthOfFreshList(m_walker, array) || !isKnownPristine(m_walker, array)) {
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

    Reference elementOfLiteral(const Walker::IndexedIteration& iteration, uint32_t position)
    {
        if (Reference written = elementInRecordOfLiteral(m_walker, iteration.list, position))
            return written;
        return elementAt(iteration.list, integer(position), true);
    }

    Reference resultsOf(Reference done, Reference value)
    {
        if (m_walker.isUnreachable())
            return { };
        if (!done || !value) {
            m_walker.fail("a step of an iteration hands out a value that nothing was built for"_s);
            return { };
        }
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
    void takeInstanceForAsMade(Reference instance) { m_instanceAsMadeForGood = instance; }
    void setIteratorCall(BlockID inputBlock, uint32_t begin, uint32_t end, ValueID given, std::optional<uint32_t> placeWalkedBefore = std::nullopt)
    {
        m_placeWalkedBeforeCall = placeWalkedBefore;
        m_iteratorCallBlock = inputBlock;
        m_iteratorCallBegin = begin;
        m_iteratorCallEnd = end;
        m_iteratorCallResult = given;
    }
    Reference booleanValue(bool value) { return boolean(value); }

private:
    Draft draft(Opcode opcode) const { return Draft(opcode, m_origin); }

    Reference build(Draft&& operation)
    {
        if (m_walker.isUnreachable())
            return { };
        m_walker.chooseConservativeFields(operation);
        std::optional<Effects> ofAppend = effectsOfAppendToListAsMade(m_walker, operation);
        return m_walker.build(WTF::move(operation), ofAppend ? &*ofAppend : nullptr);
    }

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

    bool canSeeMethods() const { return m_site && m_wording == WordingOfIteration::Source && !m_handler && m_walker.has(Mechanism::DirectCalls) && !m_walker.isUnreachable(); }
    bool goesByMethodsInSight() const { return m_kind == IteratorKind::Sync && canSeeMethods(); }

    Reference readMethod(Reference object, PropertyKeyID key)
    {
        Reference method = canSeeMethods() ? readMethodForCallOnly(m_walker, object, key, m_origin) : Reference();
        return method ? method : read(object, key);
    }

    Reference readOffAnswer(Reference answer, BuiltinName name)
    {
        Reference held = canSeeMethods() ? readFilledFieldOfProvenObject(m_walker, answer, m_walker.program().keyOf(name), m_origin) : Reference();
        return held ? held : read(answer, name);
    }

    Reference truthOf(Reference value)
    {
        if (value && value.isRecord() && m_walker.types().isSubset(m_walker.typeOf(value), TypeTable::boolean))
            return value;
        Draft conversion = draft(Opcode::ToBoolean);
        conversion.setValueOperand(value);
        return build(WTF::move(conversion));
    }

    bool isProvenObject(Reference value) const { return value && value.isValue() && m_walker.types().isSubset(m_walker.typeOf(value), TypeTable::object); }

    Reference call(Reference method, Reference receiver, std::span<const Reference> arguments = { })
    {
        if (FunctionID callee = arguments.empty() && canSeeMethods() ? findOnlyCallee(m_walker, *m_site, method) : FunctionID()) {
            if (callMethodOfProtocol(m_walker, *m_site, callee, receiver, method))
                return m_walker.isUnreachable() ? Reference() : m_walker.referenceBoundTo(m_site->id());
        }
        Draft invocation = draft(Opcode::Call);
        invocation.setCalleeOperand(method);
        invocation.setThisValueOperand(receiver);
        invocation.setArgumentsOperands(arguments);
        return build(WTF::move(invocation));
    }

    std::optional<Reference> awaitMethodCall(Reference method, Reference receiver)
    {
        FunctionID callee = m_kind == IteratorKind::Async && canSeeMethods() ? findOnlyCallee(m_walker, *m_site, method) : FunctionID();
        return callee ? buildAwaitOfProtocolCall(m_walker, *m_site, callee, receiver, method, m_held, m_origin) : std::nullopt;
    }

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

    Reference requireObject(Reference value, RuntimeErrorKind error) { return isProvenObject(value) ? value : build(draftOfObjectCheck(value, error)); }

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
        return buildAwaitBySavingFrame(m_walker, value, m_held, m_origin);
    }

    BlockID addBlock(BlockFrequency frequency = BlockFrequency::Normal)
    {
        BlockID block = m_walker.addBlock(std::max(frequency, m_frequencyOfBlocksAdded));
        if (m_isInLoopOfSpread)
            m_walker.code().setLoopMarks(block, true, false);
        return block;
    }

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

    void jumpBackTo(BlockID head)
    {
        if (m_walker.isUnreachable())
            return;
        Code& code = m_walker.code();
        BlockID block = m_walker.currentBlock();
        code.end(block, TerminatorDraft(TerminatorKind::Jump, m_origin), 1);
        code.closeEdge(code.at(block).terminator.next(), head, { });
    }

    void buildThrowingOperation(Draft&& throwing)
    {
        build(WTF::move(throwing));
        if (!m_walker.isUnreachable())
            m_walker.code().end(m_walker.currentBlock(), TerminatorDraft(TerminatorKind::Unreachable, m_origin), 0);
    }

    void throwError(RuntimeErrorKind error) { buildThrowingOperation(draftOfObjectCheck(undefined(), error)); }

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

    std::optional<int64_t> integerIn(Reference reference) const
    {
        if (!reference || !reference.isConstant() || m_walker.code().at(reference.id()).opcode != Opcode::Integer)
            return std::nullopt;
        return m_walker.code().at(reference.id()).integer();
    }

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
        operation.setForm(operatorFormFor(m_walker, operation));
        return boxNumberIfNeeded(m_walker, m_walker.build(WTF::move(operation)), m_origin);
    }

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
        if (operatorFormFor(m_walker, sum) != OperatorForm::Integers)
            return { };
        sum.setForm(OperatorForm::Integers);
        Reference result = m_walker.build(WTF::move(sum));
        return result && m_walker.types().at(m_walker.typeOf(result)).isInteger() ? result : Reference();
    }

    Reference continueIfTypeMatches(Reference value, TypeID tested, BlockID otherwise)
    {
        Reference test = testType(value, tested);
        BlockID passed = addBlock();
        branch(test, passed, otherwise);
        continueIn(passed);
        return narrowed(value, test, OperandIndex::TypeTest::value);
    }

    Reference continueIfIndexedIteration(const Walker::IndexedIteration& iteration, BlockID otherwise)
    {
        return continueIfTypeMatches(iteration.list, typeOfInstancesStepped(m_walker, iteration.builtinClass, iteration.contents), otherwise);
    }

    bool instancesBelowAreSteppedAlike(const ClassSteppedUnseen& stepped)
    {
        const BuiltinTable& table = m_walker.program().builtins();
        BuiltinClassID below = stepped.contents == ContentsStepped::View ? classBelowWithTestedStructure(table, stepped.builtinClass) : BuiltinClassID();
        if (!below)
            return false;
        BuiltinName name = table.nameOf(WellKnownSymbol::Iterator);
        std::optional<BuiltinID> own = findMemberAboveForIteration(m_walker, table.at(stepped.builtinClass).prototype, name);
        std::optional<BuiltinID> ofBelow = findMemberAboveForIteration(m_walker, table.at(below).prototype, name);
        return own && *own && ofBelow && *ofBelow && table.canonicalID(*own) == table.canonicalID(*ofBelow);
    }

    Reference continueIfInstanceOfSubclass(Reference iterable, const ClassSteppedUnseen& stepped, BlockID otherwise)
    {
        Draft look(Opcode::HasStructureOfClass, m_origin);
        if (stepped.builtinClass == BuiltinClasses::Array)
            look = draftSubclassElementsTest(iterable, stepped.subclass, m_origin);
        else {
            look.setValueOperand(iterable);
            look.setLayout(stepped.subclass);
        }
        BlockID isInstance = addBlock();
        branch(build(WTF::move(look)), isInstance, otherwise);
        continueIn(isInstance);
        MemberConditionRecord opensAsBuilt { MemberConditionKind::MemberUnchanged, stepped.subclass, ClassSide::Prototype, stepped.opener };
        Draft test = draft(Opcode::AreMembersUnchanged);
        test.setConditions(singleElementSpan(opensAsBuilt));
        BlockID passed = addBlock();
        branch(build(WTF::move(test)), passed, otherwise);
        continueIn(passed);
        m_walker.count(StatisticID::BuiltinSubclassesCheckedByStructure);
        if (m_walker.isUnreachable() || m_walker.hasFailed())
            return { };
        TypeID instances = m_walker.types().builtinClass(stepped.builtinClass);
        if (layoutKnownByStructureOfClass(m_walker, m_walker.typeOf(iterable))) {
            Reference name = stepped.builtinClass == BuiltinClasses::Array ? arrayNameOfInstance(m_walker, iterable, m_origin) : iterable;
            if (name)
                return name;
        }
        return continueIfTypeMatches(iterable, instances, otherwise);
    }

    Reference continueIfPristine(Reference iterable, const ClassSteppedUnseen& stepped, BlockID otherwise)
    {
        if (stepped.subclass)
            return continueIfInstanceOfSubclass(iterable, stepped, otherwise);
        TypeID instances = typeOfInstancesStepped(m_walker, stepped.builtinClass, stepped.contents);
        Reference instance = m_walker.types().isSubset(m_walker.typeOf(iterable), instances) ? iterable : continueIfTypeMatches(iterable, instances, otherwise);
        if (stepped.contents == ContentsStepped::Text || (instance == iterable && iterable == m_instanceAsMadeForGood && !stepped.restsOnWatchAboveInstances))
            return instance;
        bool isList = stepped.contents == ContentsStepped::List;
        if (isList && m_iteratorCallResult && m_walker.shouldAssumePristineReceivers()) [[unlikely]] {
            markObjectPristine(m_walker, instance);
            m_walker.verifyPristineObject(instance);
            return instance;
        }
        Draft test = draft(Opcode::IsPristine);
        test.setValueOperand(instance);
        test.setBuiltinClass(stepped.builtinClass);
        PristineAspectSet aspects = isList ? PristineAspectSet { PristineAspects::Storage, PristineAspects::Names } : instancesBelowAreSteppedAlike(stepped) ? PristineAspectSet { PristineAspects::InheritedNamesKept } : PristineAspectSet { PristineAspects::Names };
        if (m_walker.program().admitsReplacedBuiltins && (!stepped.restsOnWatchAboveInstances || stepped.methodAskedByItsByte))
            aspects.add(PristineAspects::CellAlone);
        test.setAspects(aspects);
        BlockID passed = addBlock();
        branch(build(WTF::move(test)), passed, otherwise);
        continueIn(passed);
        if (stepped.methodAskedByItsByte) {
            Draft isAsBuilt = draft(Opcode::IsMemberAsBuilt);
            isAsBuilt.setBuiltin(stepped.methodAskedByItsByte);
            BlockID opensAsBuilt = addBlock();
            branch(build(WTF::move(isAsBuilt)), opensAsBuilt, otherwise);
            continueIn(opensAsBuilt);
        }
        if (isList && !m_walker.isUnreachable()) {
            markObjectPristine(m_walker, instance);
            if (m_iteratorCallResult)
                m_walker.verifyPristineObject(instance);
        }
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

    Reference beginByIndex(Reference instance, ContentsStepped contents)
    {
        if (contents == ContentsStepped::View)
            requireAttached(instance);
        return stepsThroughTable(contents) ? tableOf(instance) : Reference();
    }

    bool areElementReadsEffectFree(const Walker::IndexedIteration& iteration)
    {
        if (iteration.hasProtocolFallback || iteration.contents != ContentsStepped::List)
            return false;
        if (iteration.isAsMadeForGood && isArrayPrototypeChainSane(m_walker))
            return true;
        if (iteration.stepsOfPatternStandInRow && isKnownPristine(m_walker, iteration.list) && isArrayPrototypeChainSane(m_walker))
            return true;
        return iteration.literal || (isFreshAllocation(m_walker, iteration.list) && isKnownPristine(m_walker, iteration.list));
    }

    std::optional<uint32_t> lengthKnownOf(const Walker::IndexedIteration& iteration)
    {
        if (iteration.hasProtocolFallback || iteration.contents != ContentsStepped::List)
            return std::nullopt;
        if (iteration.literal)
            return lengthOfLiteralInIteration(m_walker.currentBodyIndex(), iteration.literal);
        if (std::optional<uint32_t> length = lengthOfListInSight(m_walker, iteration.list))
            return length;
        std::optional<uint32_t> noted = lengthOfListMadeInSight(m_walker, iteration.list);
        if (noted)
            checkLengthOfListMadeInSight(m_walker, iteration.list, *noted, m_origin);
        return noted;
    }

    TypedArrayType typedArrayTypeOfView(Reference view) { return typedArrayTypeOf(BuiltinTypeContext { m_walker.program(), m_walker.types() }, m_walker.typeOf(view)); }

    Reference lengthOf(const Walker::IndexedIteration& iteration, Reference list)
    {
        if (m_walker.isUnreachable())
            return { };
        auto heldAgainstMemory = [&](auto&& read) -> Reference {
            uint32_t numberOfRecordsInFront = m_walker.code().numberOfValues();
            Reference believed = buildAsItIs(read());
            if (m_walker.checksBounds() && believed && !m_walker.wasBuiltInPlaceSince(numberOfRecordsInFront, believed))
                m_walker.checkThatMemoryAnswersAsBelieved(believed, read());
            return believed;
        };
        if (iteration.contents == ContentsStepped::View) {
            return heldAgainstMemory([&] {
                Draft length = draft(Opcode::GetTypedArrayLength);
                length.setViewOperand(list);
                length.setTypedArrayType(typedArrayTypeOfView(list));
                return length;
            });
        }
        if (iteration.contents == ContentsStepped::Text) {
            Draft length = draft(Opcode::GetStringLength);
            length.setStringOperand(list);
            return buildAsItIs(WTF::move(length));
        }
        return heldAgainstMemory([&] {
            Draft length = draft(Opcode::GetArrayLength);
            length.setArrayOperand(list);
            return length;
        });
    }

    Reference elementOfView(Reference view, Reference position, const std::optional<PassedTest>& insideLength)
    {
        if (m_walker.isUnreachable())
            return { };
        Draft read = draft(Opcode::GetTypedArrayElement);
        read.setViewOperand(view);
        read.setPositionOperand(position);
        read.setTypedArrayType(typedArrayTypeOfView(view));
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        if (insideLength && hasInlineElementAccess(typedArrayTypeOfView(view))) {
            read.setIsInsideView(*insideLength);
            Draft vector = draft(Opcode::GetTypedArrayVector);
            m_walker.chooseConservativeFields(vector);
            vector.setViewOperand(view);
            if (Reference built = buildAsItIs(WTF::move(vector)))
                read.setVectorOperand(built);
        }
        return buildAsItIs(WTF::move(read));
    }

    Reference elementAt(Reference list, Reference position, bool isEffectFree, const std::optional<PassedTest>& insideLength = std::nullopt, TypeID provenByLink = { }, bool restsOnLinksWord = false, bool listHasNoHoles = false)
    {
        if (m_walker.isUnreachable())
            return { };
        if (std::optional<int64_t> known = integerIn(position)) {
            Reference held = elementOfListInSight(m_walker, list, static_cast<uint32_t>(*known));
            if (held && !(m_walker.types().at(m_walker.typeOf(held)).primitives & Primitive::Absent))
                return held;
            if (Reference written = isEffectFree ? elementWrittenInListMadeInSight(m_walker, list, static_cast<uint32_t>(*known)) : Reference()) {
                if (m_walker.checksProofs())
                    m_walker.checkThatMemoryAnswersAsBelieved(written, readOfElementAt(list, position, true, std::nullopt));
                m_walker.count(StatisticID::ConstantArrayElementsFolded);
                return written;
            }
        }

        if (!isEffectFree)
            m_walker.letLeave(list);
        TypeID before = m_walker.exchangeResultOfReadBeingTyped(provenByLink);
        Reference built;
        std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(m_walker, list);
        if (!tested && m_takesListApart && m_site)
            tested = tryHoistDestructuredArrayGuard(m_walker, m_walker.currentBodyIndex().value(m_site->operand(0)), list);
        if (!tested && m_takesListApart && m_site)
            tested = tryGuardDestructuredArrayInIteration(m_walker, m_walker.currentBodyIndex().value(m_site->operand(0)), list);
        if (!tested && m_takesListApart)
            m_walker.noteListFromInFrontTakenApart(list);
        if (tested) {
            Draft read = readOfListTestedInFrontOfLoop(list, position, *tested, insideLength);
            read.setListHasNoHoles(listHasNoHoles);
            if (m_valueThatStepHandsToArithmeticAlone && read.isInBounds() && isNumericStorage(read.arrayStorage())) {
                read.setReadsHoleAsNaN(true);
                read.inputValueGiven = m_valueThatStepHandsToArithmeticAlone;
                m_walker.count(StatisticID::IteratorStepHoleChecksRemoved);
            }
            takeStorageAndLengthFromInFrontOfLoop(m_walker, read, *tested);
            bool innerLoopsTakeInteger = !read.readsHoleAsNaN() && read.arrayStorage() == ArrayStorage::Int32 && m_walker.turnHoldsInnerLoops() && m_walker.canBuildIterationGuard();
            if ((m_isStepUsedAsIndex || innerLoopsTakeInteger) && read.isInBounds() && isNumericStorage(read.arrayStorage())) {
                if (!guardElementPresenceInIteration(m_walker, read, list, position, *tested))
                    return { };
            }
            built = m_walker.build(WTF::move(read));
        } else {
            Draft read = readOfElementAt(list, position, isEffectFree, insideLength);
            read.setAsksCellWhereNothingIsFound(restsOnLinksWord);
            read.setListHasNoHoles(listHasNoHoles);
            if (isEffectFree && list.isValue() && nameStorageOfListMadeInSight(m_walker, read)) {
                if (read.isPresent())
                    read.setAsksCellWhereNothingIsFound(false);
                else if (restsOnLinksWord)
                    read.setArrayStorage(ArrayStorage::Other);
            }
            built = m_walker.build(WTF::move(read));
        }
        m_walker.exchangeResultOfReadBeingTyped(before);
        return built;
    }

    Draft readOfListTestedInFrontOfLoop(Reference list, Reference position, const HoistedArrayGuard& tested, const std::optional<PassedTest>& insideLength)
    {
        Draft read = draft(Opcode::GetArrayElement);
        read.setArrayOperand(list);
        read.setPositionOperand(position);
        nameKindOfListTestedInFrontOfLoop(read, tested);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        if (insideLength)
            read.setIsInBounds(*insideLength);
        return read;
    }

    Draft readOfElementAt(Reference list, Reference position, bool isEffectFree, const std::optional<PassedTest>& insideLength)
    {
        KnowledgeLevel level = isEffectFree ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
        ArrayStorage storage = isEffectFree ? ArrayStorage::Other : arrayStorageDeclaredBy(m_walker.program(), m_walker.types(), m_walker.typeOf(list)).predicted();
        if (storage == ArrayStorage::Int32)
            storage = ArrayStorage::Double;
        Draft read = draft(Opcode::GetArrayElement);
        read.setArrayOperand(list);
        read.setPositionOperand(position);
        read.setArrayStorage(storage);
        read.setAdmitsInt32Storage(storage == ArrayStorage::Double);
        read.setLevel(level);
        read.setKeyIsIntegral(true);
        if (insideLength && level == KnowledgeLevel::Expected)
            read.setIsInBounds(*insideLength);
        return read;
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

    void stepThroughTable(const Walker::IndexedIteration& iteration, Reference collection, BlockID after)
    {
        static_assert(static_cast<uint32_t>(MapNextResult::HasEntry) == static_cast<uint32_t>(SetNextResult::HasEntry) && static_cast<uint32_t>(MapNextResult::Entry) == static_cast<uint32_t>(SetNextResult::Entry) && static_cast<uint32_t>(MapNextResult::Key) == static_cast<uint32_t>(SetNextResult::Element));
        if (!iteration.canSkipDoneChecks) {
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(iteration.table);
            BlockID finished = addBlock();
            BlockID stepping = addBlock();
            branch(build(WTF::move(isClosed)), finished, stepping);
            continueIn(finished);
            Reference isLast = boolean(true);
            endStepThroughTable(iteration, after, isLast, undefined(), iteration.position, iteration.table);
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
        Reference isLast = boolean(true);
        Reference nothing = undefined();
        endStepThroughTable(iteration, after, isLast, nothing, iteration.position, iteration.canSkipDoneChecks ? nothing : build(draft(Opcode::CollectionIteratorClosedMark)));

        continueIn(giving);
        Reference key = !isMap || iteration.kind != JSC::IterationKind::Values ? resultAt(found, MapNextResult::Key, isMap ? iteration.keysProven : iteration.valuesProven) : Reference();
        Reference value = !isMap ? key : iteration.kind != JSC::IterationKind::Keys ? resultAt(found, MapNextResult::Value, iteration.valuesProven) : Reference();
        Reference given = iteration.kind == JSC::IterationKind::Keys ? key : value;
        if (iteration.isEntryPairElided)
            given = undefined();
        else if (iteration.kind == JSC::IterationKind::Entries) {
            Draft pair = draft(Opcode::NewArray);
            pair.setContents(TypeTable::anything);
            pair.setElementsOperands(std::array { key, value });
            given = build(WTF::move(pair));
        }
        Reference behind = operatorOnNumbers(Opcode::Increment, resultAt(found, MapNextResult::Entry));
        Reference goesOn = boolean(false);
        endStepThroughTable(iteration, after, goesOn, given, behind, tableOf(collection), key, value);
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

    Reference pieceOfText(Reference text, Reference start, Reference end)
    {
        Draft piece = draft(Opcode::Substring);
        piece.setStringOperand(text);
        piece.setStartOperand(start);
        piece.setEndOperand(end);
        return build(WTF::move(piece));
    }

    Reference textOfCodeUnit(Reference codeUnit)
    {
        const BuiltinTable& table = m_walker.program().builtins();
        for (BuiltinOverloadID id : table.overloadIDsOf(table.canonicalID(Builtins::StringFromCharCode))) {
            const BuiltinOverload& overload = table.at(id);
            if (overload.mode != BuiltinCallMode::Call || overload.code.implementation.inlineBuiltin != InlineBuiltin::StringFromCharCode)
                continue;
            Draft call = draft(Opcode::CallInline);
            call.setInlineBuiltin(InlineBuiltin::StringFromCharCode);
            call.setOverload(id);
            call.setArgumentsOperands(std::array { codeUnit });
            if (m_walker.isUnreachable())
                return { };
            m_walker.chooseConservativeFields(call);
            Effects ofPiece = declaredBoundInTable(Opcode::Substring);
            return m_walker.build(WTF::move(call), &ofPiece);
        }
        return { };
    }

    struct CharacterOfText {
        Reference character;
        Reference behind;
    };

    Reference letterOfStep(NOESCAPE const Invocable<Reference()> auto& make)
    {
        bool before = m_walker.exchangeMakesLetterOfStep(true);
        Reference made = make();
        m_walker.exchangeMakesLetterOfStep(before);
        return made;
    }

    bool hasOnlySingleCodeUnits(Reference text)
    {
        StringID literal = textKnownForStrings(m_walker, text);
        if (!literal)
            return false;
        Vector<char16_t> buffer;
        StringView characters = m_walker.types().viewOf(literal, buffer);
        return characters.is8Bit() || std::ranges::none_of(characters.span16(), [](char16_t codeUnit) { return codeUnit >= 0xD800 && codeUnit <= 0xDFFF; });
    }

    CharacterOfText characterAt(Reference text, Reference length, Reference position)
    {
        Reference flat = !m_walker.isInLoop() ? Reference() : text.isConstant() ? flatLiteralBeforeLoop(m_walker, text, m_origin) : flatStringForReaderOfCharacters(m_walker, text, m_origin);
        Reference lookedInto = flat ? flat : text;
        if (hasOnlySingleCodeUnits(text)) {
            CharacterOfText met;
            met.behind = operatorOnNumbers(Opcode::Increment, position);
            Reference codeUnit = codeUnitAt(lookedInto, position);
            met.character = letterOfStep([&] {
                Reference ofOneUnit = textOfCodeUnit(codeUnit);
                return ofOneUnit ? ofOneUnit : pieceOfText(text, position, met.behind);
            });
            noteCodePointBesideCharacter(m_walker, met.character, codeUnit);
            return met;
        }
        BlockID single = addBlock();
        BlockID afterLead = addBlock(BlockFrequency::Unlikely);
        BlockID second = addBlock(BlockFrequency::Unlikely);
        BlockID pair = addBlock(BlockFrequency::Unlikely);
        BlockID meeting = addBlock();
        CharacterOfText met;
        met.character = m_walker.addParameter(meeting, { }, Representation::JSValue);
        Reference codePoint = m_walker.addParameter(meeting, { }, m_walker.formOfParameter(m_walker.types().integer(Range { 0, 0x10FFFF }), { }));
        met.behind = m_walker.addParameter(meeting, { }, m_walker.formOfParameter(TypeTable::int32, { }));

        Reference behindFirst = operatorOnNumbers(Opcode::Increment, position);
        Reference first = codeUnitAt(lookedInto, position);
        branch(isSurrogate(first, 0xD800), afterLead, single);
        continueIn(afterLead);
        Reference hasSecond = operatorOnNumbers(Opcode::Less, behindFirst, length);
        branch(hasSecond, second, single);
        continueIn(second);
        Reference inside = narrowed(behindFirst, hasSecond, OperandIndex::Less::left);
        Reference trail = codeUnitAt(lookedInto, inside);
        branch(isSurrogate(trail, 0xDC00), pair, single);
        continueIn(pair);
        Reference behindPair = operatorOnNumbers(Opcode::Increment, inside);
        Reference high = operatorOnNumbers(Opcode::LShift, operatorOnNumbers(Opcode::BitAnd, first, integer(0x3FF)), integer(10));
        Reference both = operatorOnNumbers(Opcode::Add, high, operatorOnNumbers(Opcode::BitAnd, trail, integer(0x3FF)));
        Reference codePointOfPair = operatorOnNumbers(Opcode::Add, both, integer(0x10000));
        jumpTo(meeting, { letterOfStep([&] { return pieceOfText(text, position, behindPair); }), codePointOfPair, behindPair });

        continueIn(single);
        Reference character = letterOfStep([&] {
            Reference ofOneUnit = textOfCodeUnit(first);
            return ofOneUnit ? ofOneUnit : pieceOfText(text, position, behindFirst);
        });
        jumpTo(meeting, { character, first, behindFirst });
        continueIn(meeting);
        noteCodePointBesideCharacter(m_walker, met.character, codePoint);
        return met;
    }

    void stepOverCodePointAt(const Walker::IndexedIteration& iteration, Reference text, Reference length, Reference position, BlockID after)
    {
        Reference behindFirst = operatorOnNumbers(Opcode::Increment, position);
        Reference first = codeUnitAt(text, position);

        BlockID single = addBlock();
        BlockID afterLead = addBlock(BlockFrequency::Unlikely);
        branch(isSurrogate(first, 0xD800), afterLead, single);
        continueIn(afterLead);
        Reference hasSecond = operatorOnNumbers(Opcode::Less, behindFirst, length);
        BlockID second = addBlock();
        branch(hasSecond, second, single);
        continueIn(second);
        Reference inside = narrowed(behindFirst, hasSecond, OperandIndex::Less::left);
        BlockID pair = addBlock();
        branch(isSurrogate(codeUnitAt(text, inside), 0xDC00), pair, single);
        continueIn(pair);
        Reference behindPair = operatorOnNumbers(Opcode::Increment, inside);
        endStepByProtocol(iteration, after, boolean(false), pieceOfText(text, position, behindPair), behindPair);
        continueIn(single);
        Reference character = textOfCodeUnit(first);
        endStepByProtocol(iteration, after, boolean(false), character ? character : pieceOfText(text, position, behindFirst), behindFirst);
    }

    void requireAttachedUnlessDone(Reference view, Reference position)
    {
        if (std::optional<int64_t> known = integerIn(position)) {
            if (*known < Walker::doneIterationPosition)
                requireAttached(view);
            return;
        }
        BlockID asking = addBlock();
        BlockID asked = addBlock();
        branch(operatorOnNumbers(Opcode::Less, position, integer(Walker::doneIterationPosition)), asking, asked);
        continueIn(asking);
        requireAttached(view);
        jumpTo(asked);
    }

    void stepByIndex(const Walker::IndexedIteration& iteration, Reference list, Reference length, bool isEffectFree, BlockID after)
    {
        Reference position = iteration.position;
        Reference isInside = operatorOnNumbers(Opcode::Less, position, length);
        BlockID reading = addBlock();
        BlockID ending = addBlock();
        m_walker.branchOnTest(isInside, reading, ending);
        continueIn(ending);
        if (iteration.contents == ContentsStepped::View)
            requireAttachedUnlessDone(list, position);
        endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::doneIterationPosition));

        continueIn(reading);
        std::optional<int64_t> known = integerIn(position);
        Reference inside = known ? position : narrowed(position, isInside, OperandIndex::Less::left);
        if (iteration.contents == ContentsStepped::Text) {
            stepOverCodePointAt(iteration, list, length, inside, after);
            return;
        }
        std::optional<PassedTest> insideLength = m_walker.boundsProofAfterComparison(list, inside, length, isInside);
        Reference value = iteration.contents == ContentsStepped::View ? elementOfView(list, inside, insideLength) : stepValueAt(iteration, list, inside, isEffectFree, insideLength, iteration.isEntryPairElided);
        Reference next = known ? integer(*known + 1) : operatorOnNumbers(Opcode::Increment, inside);
        if (iteration.isEntryPairElided)
            endStepByProtocol(iteration, after, boolean(false), undefined(), next, inside, value);
        else
            endStepByProtocol(iteration, after, boolean(false), value, next);
    }

    template<typename StepValueFunctor>
    void endStepByProtocolOnTwoWays(const Walker::IndexedIteration& iteration, BlockID after, Reference isLast, NOESCAPE const StepValueFunctor& stepValue)
    {
        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(isLast, last, further);
        continueIn(last);
        endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::doneIterationPosition));
        continueIn(further);
        endStepByProtocol(iteration, after, boolean(false), stepValue(), integer(0));
    }

    TypeID closuresWrittenUnderNext(Reference next)
    {
        if (!m_iteratedWhoseNextIsAsked || !goesByMethodsInSight() || !m_walker.has(Mechanism::Guesses) || !m_walker.isInLoop() || !next || !next.isValue() || singleFunctionOf(m_walker.types(), m_walker.types().at(m_walker.typeOf(next))))
            return { };
        FunctionID written = functionWrittenUnderNextOfIteratorOf(m_walker, m_iteratedWhoseNextIsAsked);
        if (!written || !accessorAdmitsDirectCall(m_walker, written))
            return { };
        const LinkTables& linked = m_walker.facts().linked();
        if (written != linked.calleeAt(m_walker.currentFunction(), m_site->id()) && !linked.mayBeMentionedBy(m_walker.unit(), written))
            return { };
        TypeArena& types = m_walker.types();
        TypeID closures = types.forFunction(written);
        TypeID known = m_walker.typeOf(next);
        return !types.isSubset(known, closures) && types.overlap(known, closures) ? closures : TypeID();
    }

    void stepWhereNextIsAsWritten(const Walker::IndexedIteration& iteration, BlockID after)
    {
        TypeID closures = closuresWrittenUnderNext(iteration.next);
        if (!closures)
            return;
        BlockID byValue = addBlock(BlockFrequency::Unlikely);
        Reference known = continueIfTypeMatches(iteration.next, closures, byValue);
        m_walker.recordSpeculation();
        m_walker.count(StatisticID::DeclaredNextMethodsGuarded);
        Reference answer = requireObject(call(known, iteration.iterator), RuntimeErrorKind::IteratorResultNotObject);
        Reference done = truthOf(readOffAnswer(answer, BuiltinNames::done));
        if (!m_walker.isUnreachable())
            endStepByProtocolOnTwoWays(iteration, after, done, [&] { return readOffAnswer(answer, BuiltinNames::value); });
        continueIn(byValue);
    }

    TypeID closuresExpectedUnderNext(Reference next)
    {
        if (closuresWrittenUnderNext(next))
            return { };
        if (!m_expectedUnderNext || !goesByMethodsInSight() || !m_walker.has(Mechanism::Guesses) || !m_walker.isInLoop() || !next || !next.isValue() || !accessorAdmitsDirectCall(m_walker, m_expectedUnderNext))
            return { };
        const LinkTables& linked = m_walker.facts().linked();
        if (!linked.function(m_expectedUnderNext).isInQueueFromStart && !linked.mayBeMentionedBy(m_walker.unit(), m_expectedUnderNext))
            return { };
        TypeArena& types = m_walker.types();
        TypeID closures = types.forFunction(m_expectedUnderNext);
        TypeID known = m_walker.typeOf(next);
        return !types.isSubset(known, closures) && types.overlap(known, closures) ? closures : TypeID();
    }

    void stepWhereNextIsAsExpected(const Walker::IndexedIteration& iteration, BlockID after)
    {
        TypeID closures = closuresExpectedUnderNext(iteration.next);
        if (!closures)
            return;
        BlockID byValue = addBlock(BlockFrequency::Unlikely);
        Reference known = continueIfTypeMatches(iteration.next, closures, byValue);
        m_walker.recordSpeculation();
        m_walker.count(StatisticID::SpeculatedNextMethodsGuarded);
        Reference answer = requireObject(call(known, iteration.iterator), RuntimeErrorKind::IteratorResultNotObject);
        Reference done = truthOf(readOffAnswer(answer, BuiltinNames::done));
        if (!m_walker.isUnreachable())
            endStepByProtocolOnTwoWays(iteration, after, done, [&] { return readOffAnswer(answer, BuiltinNames::value); });
        continueIn(byValue);
    }

    static Walker::IndexedIteration overOtherOfTextAndList(const Walker::IndexedIteration& iteration)
    {
        Walker::IndexedIteration other = iteration;
        bool isOverText = iteration.contents == ContentsStepped::Text;
        other.builtinClass = isOverText ? BuiltinClasses::Array : BuiltinClasses::String;
        other.contents = isOverText ? ContentsStepped::List : ContentsStepped::Text;
        return other;
    }

    void stepOtherOfTextAndList(const Walker::IndexedIteration& iteration, BlockID after)
    {
        BlockID otherwise = std::exchange(m_whereNeitherTextNorListIsStepped, BlockID());
        if (!otherwise)
            otherwise = addBlock(BlockFrequency::Unlikely);
        SetForScope asUsualAsText(m_frequencyOfBlocksAdded, iteration.contents == ContentsStepped::Text ? BlockFrequency::Normal : m_frequencyOfBlocksAdded);
        Walker::IndexedIteration other = overOtherOfTextAndList(iteration);
        Reference holder = continueIfIndexedIteration(other, otherwise);
        stepByIndex(other, holder, lengthOf(other, holder), false, after);
        continueIn(otherwise);
    }

    static Walker::IndexedIteration overMapInsteadOfList(const Walker::IndexedIteration& iteration)
    {
        Walker::IndexedIteration maps = iteration;
        maps.builtinClass = BuiltinClasses::Map;
        maps.contents = ContentsStepped::Map;
        maps.keysProven = { };
        maps.valuesProven = { };
        return maps;
    }

    void stepMapInsteadOfList(const Walker::IndexedIteration& iteration, BlockID after)
    {
        BlockID otherwise = addBlock(BlockFrequency::Unlikely);
        SetForScope usual(m_frequencyOfBlocksAdded, BlockFrequency::Normal);
        Walker::IndexedIteration maps = overMapInsteadOfList(iteration);
        stepThroughTable(maps, continueIfIndexedIteration(maps, otherwise), after);
        continueIn(otherwise);
    }

    void stepByProtocolBesideIndex(const Walker::IndexedIteration& iteration, BlockID after)
    {
        if (iteration.isDelegating) {
            JoinOfIteration stepped = openJoin();
            m_stepsGenerator = iteration.stepsGenerator;
            step(iteration.iterator, iteration.next, m_sentToDelegate, true, stepped);
            endStepByProtocol(iteration, after, stepped.results[0], stepped.results[1], iteration.position);
            return;
        }
        if (iteration.isStringOrArray)
            stepOtherOfTextAndList(iteration, after);
        if (iteration.isArrayOrMap)
            stepMapInsteadOfList(iteration, after);
        FunctionID stepsForTheOddOne = !iteration.state && iteration.canSkipDoneChecks && m_protocolIsForTheOddOne ? findHelper(IterationHelper::StepIterator) : FunctionID();
        if (stepsForTheOddOne) {
            Reference stepValue = callHelper(stepsForTheOddOne, { iteration.iterator, iteration.next });
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(stepValue);
            endStepByProtocolOnTwoWays(iteration, after, build(WTF::move(isClosed)), [&] { return stepValue; });
            return;
        }
        if (!iteration.state && iteration.canSkipDoneChecks && iteration.wayToStep) {
            stepInWayBesideIndex(iteration, after);
            stepWhereNextIsAsExpected(iteration, after);
            stepWhereNextIsAsWritten(iteration, after);
            Reference answer = requireObject(call(iteration.next, iteration.iterator), RuntimeErrorKind::IteratorResultNotObject);
            Draft conversion = draft(Opcode::ToBoolean);
            conversion.setValueOperand(read(answer, BuiltinNames::done));
            endStepByProtocolOnTwoWays(iteration, after, build(WTF::move(conversion)), [&] { return read(answer, BuiltinNames::value); });
            return;
        }

        JoinOfIteration stepped = openJoin();
        if (iteration.state) {
            takeStateInList(iteration.state, iteration.listMadeForState);
            m_listOfStateHoldsVerdictOnNext = iteration.stateHoldsVerdictOnNext;
            stepUnlessDone({ }, false, stepped);
            endStepByProtocol(iteration, after, stepped.results[0], stepped.results[1], iteration.position);
            return;
        }

        if (iteration.canSkipDoneChecks) {
            stepWhereNextIsAsExpected(iteration, after);
            stepWhereNextIsAsWritten(iteration, after);
            m_isSteppedByCallsAlone = iteration.isSteppedByCallsAlone;
            m_goesByDriver = iteration.goesByDriver;
            step(iteration.iterator, iteration.next, { }, false, stepped);
            endStepByProtocolOnTwoWays(iteration, after, stepped.results[0], [&] { return stepped.results[1]; });
            return;
        }

        BlockID stepping = addBlock();
        BlockID finished = addBlock();
        branch(operatorOnNumbers(Opcode::Less, iteration.position, integer(Walker::doneIterationPosition)), stepping, finished);
        continueIn(finished);
        jumpTo(stepped, boolean(true), undefined());
        continueIn(stepping);
        step(iteration.iterator, iteration.next, { }, false, stepped);
        endStepByProtocolOnTwoWays(iteration, after, stepped.results[0], [&] { return stepped.results[1]; });
    }

    void stepGeneratorInWay(const Walker::IndexedIteration& iteration, BlockID after, BlockID byHelper)
    {
        if (m_walker.isUnreachable())
            return;
        BlockID isGenerator = addBlock();
        BlockID isNoGenerator = addBlock(BlockFrequency::Unlikely);
        branch(testType(iteration.wayToStep, TypeArena::fromPrimitives(Primitive::False)), isGenerator, isNoGenerator);
        continueIn(isGenerator);
        Draft step = draft(Opcode::StepGenerator);
        step.setGeneratorOperand(iteration.iterator);
        step.setResumeMode(ResumeMode::Next);
        step.setValueOperand(undefined());
        step.setCheck(GeneratorStepCheck::None);
        Reference steppedGenerator = build(WTF::move(step));
        Reference value = resultOfGeneratorStep(steppedGenerator, StepGeneratorResult::Value);
        Reference status = resultOfGeneratorStep(steppedGenerator, StepGeneratorResult::Status);
        BlockID hasYielded = addBlock();
        BlockID hasNotYielded = addBlock(BlockFrequency::Unlikely);
        BlockID isLast = addBlock();
        BlockID isNotStepped = addBlock(BlockFrequency::Rare);
        branch(hasStatus(status, GeneratorStepStatus::Yielded), hasYielded, hasNotYielded);
        continueIn(hasNotYielded);
        branch(hasStatus(status, GeneratorStepStatus::NotStepped), isNotStepped, isLast);
        continueIn(isNotStepped);
        jumpTo(byHelper);
        continueIn(isLast);
        endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::doneIterationPosition));
        continueIn(hasYielded);
        endStepByProtocol(iteration, after, boolean(false), value, integer(0));
        continueIn(isNoGenerator);
    }

    void stepInWayBesideIndex(const Walker::IndexedIteration& iteration, BlockID after)
    {
        BlockID byCall = addBlock();
        BlockID throughTable = addBlock();
        BlockID notThroughTable = addBlock();
        BlockID byHelper = addBlock();
        BlockID stepped = addBlock();
        Reference stepValue = m_walker.addParameter(stepped, { }, Representation::JSValue);
        m_walker.recordSpeculation();
        m_walker.count(StatisticID::IteratorsSteppedWithoutCall);
        SetForScope<BlockFrequency> frequency(m_frequencyOfBlocksAdded, m_frequencyOfBlocksAdded);
        if (m_walker.fileHoldsGenerators()) {
            stepGeneratorInWay(iteration, after, byHelper);
            m_frequencyOfBlocksAdded = BlockFrequency::Unlikely;
        }
        branch(testType(iteration.wayToStep, TypeArena::fromPrimitives(Primitive::True)), throughTable, notThroughTable);
        continueIn(notThroughTable);
        branch(testType(iteration.wayToStep, TypeTable::undefined), byCall, byHelper);
        continueIn(throughTable);
        Draft next = draft(Opcode::NextSetElement);
        next.setIteratorOperand(iteration.iterator);
        jumpTo(stepped, { build(WTF::move(next)) });
        continueIn(byHelper);
        if (iteration.stepsMatches)
            m_walker.count(StatisticID::MatchIterationsStepped);
        jumpTo(stepped, { callHelper(findHelper(iteration.stepsMatches ? IterationHelper::StepMatchesInWay : IterationHelper::StepIteratorInWay), { iteration.iterator, iteration.wayToStep }) });
        continueIn(stepped);
        Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
        isClosed.setTableOperand(stepValue);
        endStepByProtocolOnTwoWays(iteration, after, build(WTF::move(isClosed)), [&] { return stepValue; });
        continueIn(byCall);
    }

    void endStepByProtocol(const Walker::IndexedIteration& iteration, BlockID after, Reference done, Reference value, Reference position, Reference keyOfPair = { }, Reference valueOfPair = { })
    {
        if (iteration.table && !iteration.isProtocolOnly)
            endStepThroughTable(iteration, after, done, value, position, undefined());
        else if (m_doneBlock)
            endThreadedStep(after, done, { value, position });
        else if (iteration.isProtocolOnly)
            jumpTo(after, { done, value });
        else if (mayTakePairApartWhenDone(iteration))
            jumpTo(after, { done, value, position, keyOfPair ? keyOfPair : integer(0), valueOfPair ? valueOfPair : undefined(), boolean(!!keyOfPair) });
        else if (iteration.isEntryPairElided)
            jumpTo(after, { done, value, position, keyOfPair ? keyOfPair : integer(0), valueOfPair ? valueOfPair : undefined() });
        else
            jumpTo(after, { done, value, position });
    }

    void endThreadedStep(BlockID after, Reference done, std::initializer_list<Reference> given)
    {
        if (m_walker.hasFailed())
            return;
        RELEASE_ASSERT(done.isConstant() && m_walker.code().at(done.id()).opcode == Opcode::Boolean);
        if (m_walker.code().at(done.id()).boolean())
            jumpTo(m_doneBlock);
        else
            jumpTo(after, given);
    }

    void endStepThroughTable(const Walker::IndexedIteration& iteration, BlockID after, Reference done, Reference value, Reference position, Reference table, Reference keyOfPair = { }, Reference valueOfPair = { })
    {
        if (m_doneBlock && iteration.isEntryPairElided)
            endThreadedStep(after, done, { value, position, table, keyOfPair ? keyOfPair : undefined(), valueOfPair ? valueOfPair : undefined() });
        else if (m_doneBlock)
            endThreadedStep(after, done, { value, position, table });
        else if (mayTakePairApartWhenDone(iteration))
            jumpTo(after, { done, value, position, table, keyOfPair ? keyOfPair : undefined(), valueOfPair ? valueOfPair : undefined(), boolean(!!keyOfPair) });
        else if (iteration.isEntryPairElided)
            jumpTo(after, { done, value, position, table, keyOfPair ? keyOfPair : undefined(), valueOfPair ? valueOfPair : undefined() });
        else
            jumpTo(after, { done, value, position, table });
    }

    void takeState(Reference state)
    {
        if (!state || !state.isValue() || m_walker.code().at(state.id()).opcode != Opcode::NewHeapWords) {
            m_walker.fail("an iteration that the protocol has not opened is stepped or closed by the protocol"_s);
            return;
        }
        m_state = state;
    }

    void takeStateInList(Reference parameter, Reference listAsMade, bool holdsVerdictOnNext = false)
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
        m_listMadeForState = listAsMade;
        m_listOfStateHoldsVerdictOnNext = holdsVerdictOnNext;
    }

    std::optional<PassedTest> findWhatHoldsWordInsideListOfState(WordOfIteration word)
    {
        return m_walker.findWhatHoldsWordInsideListMadeByWalk(m_state, m_listMadeForState, integer(static_cast<uint32_t>(word)));
    }

    Reference readWord(WordOfIteration word)
    {
        if (m_stateIsList) {
            Draft read = draft(Opcode::GetArrayElement);
            read.setArrayOperand(m_state);
            read.setPositionOperand(integer(static_cast<uint32_t>(word)));
            read.setArrayStorage(ArrayStorage::Contiguous);
            read.setLevel(KnowledgeLevel::Proven);
            read.setKeyIsIntegral(true);
            if (std::optional<PassedTest> inside = findWhatHoldsWordInsideListOfState(word)) {
                read.setIsInBounds(*inside);
                read.setIsPresent(*inside);
            }
            return buildAsItIs(WTF::move(read));
        }
        Draft load = draft(Opcode::GetHeapFrameWord);
        load.setFrameOperand(m_state);
        load.setIndex(static_cast<uint32_t>(word));
        load.setValueType(word == WordOfIteration::IsDone || word == WordOfIteration::StepsInLibrary ? TypeID(TypeTable::boolean) : word == WordOfIteration::PositionInList ? m_walker.types().integer(Range { 0, Walker::doneIterationPosition }) : TypeID(TypeTable::anything));
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
            if (std::optional<PassedTest> inside = findWhatHoldsWordInsideListOfState(word))
                put.setIsInBounds(*inside);
            put.setStorageIsWritable(true);
            buildAsItIs(WTF::move(put));
            return;
        }
        Draft store = draft(Opcode::PutHeapFrameWord);
        store.setFrameOperand(m_state);
        store.setIndex(static_cast<uint32_t>(word));
        store.setValueOperand(value);
        store.setRemembersFrame(word == WordOfIteration::Iterator || word == WordOfIteration::Next || word == WordOfIteration::ListSteppedByIndex);
        build(WTF::move(store));
    }

    bool isNothingToldByHelper(Reference iterable)
    {
        if (m_kind != IteratorKind::Sync || m_wording != WordingOfIteration::Source || !iterable || !iterable.isValue() || m_walker.isUnreachable())
            return false;
        TypeID present = m_walker.types().withoutNullish(m_walker.typeOf(iterable));
        return !isNeverType(present) && !m_walker.types().at(present).hasOnlyLayouts() && findHelper(IterationHelper::OpenIterator);
    }

    OpenedIteration open(Reference iterable)
    {
        if (!isNothingToldByHelper(iterable))
            iterable = requireObjectCoercible(iterable);

        switch (m_wording) {
        case WordingOfIteration::Spread: {
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

    OpenedIteration withNextMethod(Reference iterator) { return { iterator, readMethod(iterator, m_walker.program().keyOf(BuiltinNames::next)) }; }

    OpenedIteration openReturned(Reference iterator) { return leftToSteps(withNextMethod(requireObject(iterator, RuntimeErrorKind::IteratorNotObject))); }

    Reference makeIteratorAgain(Reference iterator)
    {
        const Code& code = m_walker.code();
        const Operation& made = code.at(underlyingCellOf(code, iterator.id()));
        Draft again = draft(Opcode::CallInline);
        again.setInlineBuiltin(made.inlineBuiltin());
        again.setInlineParameters(made.inlineParameters());
        again.setOverload(made.overload());
        again.setReceiverOperand(Reference::value(made.receiverOperand()));
        return build(WTF::move(again));
    }

    OpenedIteration leftToSteps(OpenedIteration opened)
    {
        for (Reference named : { opened.iterator, opened.next }) {
            if (named && !m_walker.isUnreachable())
                m_walker.letLeave(named);
        }
        return opened;
    }

    void throwForMissingMethod(Reference method, Reference iterable, UseOfSyncIteration use)
    {
        switch (use) {
        case UseOfSyncIteration::WrappedForAsyncIteration:
            throwError(RuntimeErrorKind::AsyncFromSyncIteratorMissing);
            return;
        case UseOfSyncIteration::Stepped:
            break;
        }

        switch (m_wording) {
        case WordingOfIteration::Delegation:
            call(method, iterable);
            throwError(RuntimeErrorKind::IteratorNotObject);
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

    OpenedIteration openSync(Reference iterable, UseOfSyncIteration use)
    {
        if (Reference method = use == UseOfSyncIteration::Stepped && goesByMethodsInSight() ? readMethodInSightOfProvenObject(m_walker, iterable, m_walker.types().wellKnownSymbolKey(WellKnownSymbol::Iterator), m_origin) : Reference())
        {
            m_walker.count(StatisticID::IteratorMethodsCalledDirectly);
            return leftToSteps(withNextMethod(requireObject(call(method, iterable), RuntimeErrorKind::IteratorNotObject)));
        }
        if (FunctionID opens = use == UseOfSyncIteration::Stepped ? findHelper(IterationHelper::OpenIterator) : FunctionID()) {
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
            return withNextMethod(requireObject(iterator, RuntimeErrorKind::AsyncFromSyncIteratorNotObject));
        case UseOfSyncIteration::Stepped:
            break;
        }
        switch (m_wording) {
        case WordingOfIteration::Source:
            return withNextMethod(requireObject(iterator, RuntimeErrorKind::IteratorNotObject));
        case WordingOfIteration::Spread:
        case WordingOfIteration::Delegation:
            break;
        }

        BlockID isObject = addBlock();
        BlockID isNoObject = addBlock(BlockFrequency::Rare);
        branch(testType(iterator, TypeTable::object), isObject, isNoObject);
        continueIn(isNoObject);
        call(read(iterator, BuiltinNames::next), iterator);
        throwError(RuntimeErrorKind::IteratorNotObject);

        continueIn(isObject);
        return withNextMethod(iterator);
    }

    bool mayOpenUnmodifiedAsyncGenerator(Reference iterable)
    {
        TypeArena& types = m_walker.types();
        const BuiltinTable& table = m_walker.program().builtins();
        const Function& function = m_walker.program().at(m_walker.unit().function);
        bool isRunOften = m_walker.isInLoop() || (m_walker.unit().role == CodeRole::CoroutineBody && function.has(FunctionFlags::IsGenerator));
        return isRunOften && !m_walker.isUnreachable() && m_walker.has(Mechanism::Guesses) && m_walker.has(Mechanism::BuiltinCalls) && m_walker.opensGuesses() && hasInstanceTest(table, BuiltinClasses::AsyncGenerator)
            && types.overlap(m_walker.typeOf(iterable), types.builtinClass(BuiltinClasses::AsyncGenerator)) && engineOpensAsyncGenerators(m_walker);
    }

    void openUnmodifiedAsyncGenerator(Reference iterable, BlockID opened)
    {
        BlockID isOther = addBlock();
        Reference generator = continueIfTypeMatches(iterable, m_walker.types().builtinClass(BuiltinClasses::AsyncGenerator), isOther);
        Draft test = draft(Opcode::IsUnmodifiedAsyncGenerator);
        test.setGeneratorOperand(generator);
        BlockID isAsMade = addBlock();
        branch(build(WTF::move(test)), isAsMade, isOther);
        continueIn(isAsMade);
        Draft next = draft(Opcode::Builtin);
        next.setBuiltin(m_walker.program().builtins().canonicalID(Builtins::AsyncGeneratorPrototypeNext));
        next.setIsNotReadFromHolder(true);
        jumpTo(opened, { generator, build(WTF::move(next)), boolean(true) });
        continueIn(isOther);
    }

    OpenedIteration openAsync(Reference iterable)
    {
        if (Reference method = canSeeMethods() ? readMethodInSightOfProvenObject(m_walker, iterable, m_walker.types().wellKnownSymbolKey(WellKnownSymbol::AsyncIterator), m_origin) : Reference()) {
            m_walker.count(StatisticID::IteratorMethodsCalledDirectly);
            OpenedIteration opened = withNextMethod(requireObject(call(method, iterable), RuntimeErrorKind::IteratorNotObject));
            return { opened.iterator, opened.next, boolean(false) };
        }
        BlockID opened;
        Reference iterator;
        Reference next;
        Reference goesByDriver;
        auto addOpenedBlock = [&] {
            opened = addBlock();
            iterator = m_walker.addParameter(opened, TypeTable::anything, Representation::JSValue);
            next = m_walker.addParameter(opened, TypeTable::anything, Representation::JSValue);
            goesByDriver = m_walker.addParameter(opened, TypeTable::boolean, Representation::Boolean);
        };
        bool opensAsMade = mayOpenUnmodifiedAsyncGenerator(iterable);
        if (opensAsMade) {
            addOpenedBlock();
            openUnmodifiedAsyncGenerator(iterable, opened);
        }
        Reference method = read(iterable, WellKnownSymbol::AsyncIterator);
        BlockID absent = addBlock();
        BlockID present = addBlock();
        if (!opensAsMade)
            addOpenedBlock();
        branch(testType(method, TypeTable::nullish), absent, present);

        continueIn(absent);
        OpenedIteration sync = openSync(iterable, UseOfSyncIteration::WrappedForAsyncIteration);
        Draft wrapper = draft(Opcode::CreateAsyncFromSyncIterator);
        wrapper.setIteratorOperand(sync.iterator);
        wrapper.setNextOperand(sync.next);
        OpenedIteration wrapped = withNextMethod(build(WTF::move(wrapper)));
        jumpTo(opened, { wrapped.iterator, wrapped.next, boolean(true) });

        continueIn(present);
        OpenedIteration given = withNextMethod(requireObject(call(method, iterable), RuntimeErrorKind::IteratorNotObject));
        jumpTo(opened, { given.iterator, given.next, testType(method, m_walker.types().forBuiltin(m_walker.program().builtins().canonicalID(Builtins::AsyncIteratorPrototypeSymbolAsyncIterator))) });
        return { iterator, next, goesByDriver };
    }

    void readStepResult(Reference result, bool readsValueWhenDone, const JoinOfIteration& join) { readAwaitedStepResult(await(result), readsValueWhenDone, join); }

    void readAwaitedStepResult(Reference result, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        Reference object = requireObject(result, m_wording == WordingOfIteration::Spread ? RuntimeErrorKind::SpreadResultNotObject : RuntimeErrorKind::IteratorResultNotObject);
        Reference done = truthOf(readOffAnswer(object, BuiltinNames::done));

        if (readsValueWhenDone) {
            Reference value = readOffAnswer(object, BuiltinNames::value);
            if (keepsDoneWord())
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
        Reference value = readOffAnswer(object, BuiltinNames::value);
        if (keepsDoneWord())
            storeWord(WordOfIteration::IsDone, boolean(false));
        jumpTo(join, boolean(false), value);
    }

    void step(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (!stepIteratorOfEngineInLibrary(iterator, next, sent, readsValueWhenDone, join))
            stepGeneratorWhereNextIsOfEngine(iterator, next, sent, readsValueWhenDone, join);
        if (FunctionID steps = m_protocolIsForTheOddOne && !sent && !readsValueWhenDone ? findHelper(IterationHelper::StepIterator) : FunctionID()) {
            Reference stepValue = callHelper(steps, { iterator, next });
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(stepValue);
            endStepWithoutResultObject(build(WTF::move(isClosed)), stepValue, false, join);
            return;
        }
        std::span<const Reference> arguments;
        if (sent)
            arguments = singleElementSpan(sent);
        if (std::optional<Reference> settled = sent ? std::nullopt : awaitMethodCall(next, iterator)) {
            if (!m_walker.isUnreachable())
                readAwaitedStepResult(*settled, readsValueWhenDone, join);
            return;
        }
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
            types.forBuiltin(table.canonicalID(Builtins::RegExpStringIteratorPrototypeNext)),
            types.forBuiltin(table.canonicalID(Builtins::CompilerHelpersNextOfStepsOverText)),
        });
    }

    bool mayStepInLibrary()
    {
        return !m_protocolIsForTheOddOne && m_kind == IteratorKind::Sync && m_wording == WordingOfIteration::Source && m_walker.has(Mechanism::Guesses) && m_walker.has(Mechanism::BuiltinCalls)
            && !m_walker.isUnreachable() && findHelper(IterationHelper::StepIterator);
    }

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

    bool stepIteratorOfEngineInLibrary(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (sent || readsValueWhenDone || m_isSteppedByCallsAlone || !mayStepInLibrary())
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
            method = continueIfTypeMatches(next, methods, byCall);
        }
        m_walker.recordSpeculation();
        m_walker.count(StatisticID::IteratorsSteppedWithoutCall);
        Reference stepValue = callHelper(steps, { iterator, method });
        Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
        isClosed.setTableOperand(stepValue);
        endStepWithoutResultObject(build(WTF::move(isClosed)), stepValue, false, join);
        continueIn(byCall);
        return true;
    }

    void endStepWithoutResultObject(Reference done, Reference value, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (readsValueWhenDone) {
            if (keepsDoneWord())
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
        if (keepsDoneWord())
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

    void stepGeneratorWhereNextIsOfEngine(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        bool isAsync = m_kind == IteratorKind::Async;
        bool isGuess = m_walker.has(Mechanism::Guesses) && m_walker.has(Mechanism::BuiltinCalls);
        if (!m_mayStepGenerator || (!isAsync && !isGuess) || m_walker.isUnreachable())
            return;
        if (isAsync && !canWaitForStepOfAsyncGenerator(m_walker))
            return;

        TypeArena& types = m_walker.types();
        const BuiltinTable& table = m_walker.program().builtins();
        TypeID methods = isAsync
            ? types.unionOf(types.forBuiltin(table.canonicalID(Builtins::AsyncGeneratorPrototypeNext)), types.forBuiltin(table.canonicalID(Builtins::AsyncFromSyncIteratorPrototypeNext)))
            : types.forBuiltin(table.canonicalID(Builtins::GeneratorPrototypeNext));
        BlockID byCall = addBlock();
        bool goesByVerdict = !isAsync && (m_stepsGenerator || (m_state && !keepsDoneWord()));
        if (goesByVerdict) {
            BlockID isGenerator = addBlock();
            branch(m_stepsGenerator ? m_stepsGenerator : readWord(WordOfIteration::StepsInLibrary), isGenerator, byCall);
            continueIn(isGenerator);
        } else
            continueIfTypeMatches(next, methods, byCall);
        if (isGuess)
            m_walker.recordSpeculation();

        if (isAsync) {
            Reference generator = continueIfTypeMatches(iterator, types.unionOf(types.builtinClass(BuiltinClasses::AsyncGenerator), types.builtinClass(BuiltinClasses::AsyncFromSyncIterator)), byCall);
            bool alwaysGoesByDriver = !m_goesByDriver || types.isSubset(m_walker.typeOf(m_goesByDriver), TypeArena::fromPrimitives(Primitive::True));
            if (!alwaysGoesByDriver && m_walker.program().at(m_walker.unit().function).isModuleBody()) {
                BlockID byDriver = addBlock();
                branch(truthOf(m_goesByDriver), byDriver, byCall);
                continueIn(byDriver);
                alwaysGoesByDriver = true;
            }
            bool neverGoesByDriver = !alwaysGoesByDriver && types.isSubset(m_walker.typeOf(m_goesByDriver), TypeArena::fromPrimitives(Primitive::False));
            BuiltinID awaitedCallee = alwaysGoesByDriver ? BuiltinID() : table.canonicalID(Builtins::AsyncGeneratorPrototypeNext);
            Reference goesByDriver = alwaysGoesByDriver || neverGoesByDriver ? Reference() : truthOf(m_goesByDriver);
            Reference stepped = m_walker.isUnreachable() ? Reference() : buildStepOfAsyncGenerator(m_walker, generator, sent ? sent : constant(draft(Opcode::Absent)), m_held, awaitedCallee, goesByDriver, m_origin);
            Reference value = resultOfGeneratorStep(stepped, StepGeneratorResult::Value);
            Reference status = resultOfGeneratorStep(stepped, StepGeneratorResult::Status);
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
            step.setCheck(goesByVerdict ? GeneratorStepCheck::None : GeneratorStepCheck::IsGenerator);
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
        if (!keepsDoneWord()) {
            step(readWord(WordOfIteration::Iterator), readWord(WordOfIteration::Next), sent, readsValueWhenDone, join);
            return;
        }
        BlockID finished = addBlock();
        BlockID stepping = addBlock();
        branch(readWord(WordOfIteration::IsDone), finished, stepping);
        continueIn(finished);
        jumpTo(join, boolean(true), undefined());

        continueIn(stepping);
        storeWord(WordOfIteration::IsDone, boolean(true));
        if (stateMayHoldListSteppedByIndex())
            stepListBehindWrapper(join);
        Reference iterator = readWord(WordOfIteration::Iterator);
        Reference next = readWord(WordOfIteration::Next);
        if (m_kind == IteratorKind::Async)
            m_goesByDriver = readWord(WordOfIteration::StepsInLibrary);
        step(iterator, next, sent, readsValueWhenDone, join);
    }

    void resumeByMethod(ResumeMode mode, Reference iterator, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        bool isThrow = mode == ResumeMode::Throw;
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

    bool callVisibleReturn(Reference iterator)
    {
        if (!goesByMethodsInSight())
            return false;
        Reference method = readMethodForCallOnly(m_walker, iterator, m_walker.program().keyOf(BuiltinNames::returnKeyword), m_origin);
        if (!method || !method.isValue() || m_walker.isUnreachable() || !findOnlyCallee(m_walker, *m_site, method))
            return false;
        requireObject(call(method, iterator), RuntimeErrorKind::IteratorResultNotObject);
        return true;
    }

    void callReturn(Reference iterator, ResultOfReturnInIteration expectation)
    {
        if (goesByMethodsInSight() && findsNothingUnderNameOfProvenObject(m_walker, iterator, m_walker.program().keyOf(BuiltinNames::returnKeyword)))
            return;
        if (expectation == ResultOfReturnInIteration::MustBeObject && callVisibleReturn(iterator))
            return;
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

    void callReturnAndDropWhatItThrows(Reference iterator)
    {
        if (m_walker.isUnreachable())
            return;
        if (goesByMethodsInSight() && findsNothingUnderNameOfProvenObject(m_walker, iterator, m_walker.program().keyOf(BuiltinNames::returnKeyword)))
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
    const BodyValue* m_site { nullptr };
    FunctionID m_expectedUnderNext;
    ValueID m_iteratedWhoseNextIsAsked;
    std::optional<ClassSteppedUnseen> m_listsSteppedByIndexBehindWrapper;
    Reference m_state;
    bool m_stateIsList { false };
    Reference m_listMadeForState;
    bool m_listOfStateHoldsVerdictOnNext { false };
    bool m_isSteppedByCallsAlone { false };
    Reference m_goesByDriver;
    Reference m_stepsGenerator;
    BlockID m_handler;
    bool m_isInLoopOfSpread { false };
    bool m_mayStepGenerator { true };
    Reference m_sentToDelegate;
    bool m_stepsDelegate { false };
    bool m_protocolIsForTheOddOne { false };
    BlockID m_whereNeitherTextNorListIsStepped;
    BlockID m_doneBlock;
    ValueID m_valueThatStepHandsToArithmeticAlone;
    bool m_isStepUsedAsIndex { false };
    bool m_takesListApart { false };
    BlockFrequency m_frequencyOfBlocksAdded { BlockFrequency::Normal };
    Reference m_instanceAsMadeForGood;
    BlockID m_iteratorCallBlock;
    uint32_t m_iteratorCallBegin { 0 };
    uint32_t m_iteratorCallEnd { 0 };
    std::optional<uint32_t> m_placeWalkedBeforeCall;
    ValueID m_iteratorCallResult;
    Reference m_iteratorReturned;
};

} // anonymous namespace

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

static bool isStateOfDelegation(const BodyIndex& index, ValueID state)
{
    return !index.isParameter(state) && index.opcodeOf(state) == Opcode::IteratorOpen && index.value(state).as<Opcode::IteratorOpen>().isDelegating();
}

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
    if (FunctionID method = iterable && iterable.isValue() ? findMethodInSightOfProvenType(walker, types.withoutNullish(walker.typeOf(iterable)), types.wellKnownSymbolKey(WellKnownSymbol::Iterator)) : FunctionID())
        return walker.program().at(method).has(FunctionFlags::IsGenerator) && !walker.program().at(method).has(FunctionFlags::IsAsync);
    ExpectationOfValue expectation = findExpectation(walker, open.iterableOperand());
    return expectation.type && namesGenerators(types.withoutNullish(expectation.type));
}

static FunctionID methodThatClassWritesUnder(Walker& walker, LayoutID layout, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (findField(program, layout, key))
        return { };
    for (LayoutID above = layout; above && isDefinedByClass(program.at(above)); above = writtenBaseOf(program.at(above))) {
        std::span<const ClassMember> members = walker.facts().membersOf(above);
        for (bool isAssigned : { false, true }) {
            FunctionID written;
            bool isWritten = false;
            for (uint32_t place : walker.facts().placesOfMembersUnder(above, ClassSide::Prototype, key)) {
                if (members[place].isAssignedByProgram != isAssigned)
                    continue;
                if (isWritten || members[place].kind != MethodKind::Method)
                    return { };
                isWritten = true;
                written = members[place].function;
            }
            if (isWritten)
                return written;
        }
    }
    return { };
}

static FunctionID methodThatClassesOfFileWriteUnder(Walker& walker, TypeID objects, ModuleID file, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (!objects || !walker.types().at(objects).isJavaScriptValue())
        return { };
    FunctionID written;
    std::span<const LayoutID> members = membersOf(walker.types(), walker.types().at(objects));
    for (LayoutID layout : members) {
        if (program.at(layout).module != file)
            continue;
        bool standsBelowAnother = false;
        for (LayoutID above = writtenBaseOf(program.at(layout)); above && !standsBelowAnother; above = writtenBaseOf(program.at(above)))
            standsBelowAnother = std::ranges::binary_search(members, above);
        if (standsBelowAnother)
            continue;
        FunctionID here = methodThatClassWritesUnder(walker, layout, key);
        if (!here || (written && written != here))
            return { };
        written = here;
    }
    return written && program.at(written).module == file ? written : FunctionID();
}

static LayoutID classOfFileThatConstructionNames(Walker& walker, const BodyIndex& body, ValueID construction, ModuleID file)
{
    const Program& program = walker.program();
    ValueID callee = body.value(construction).operand(0);
    while (callee && !body.isParameter(callee) && inputPassThroughOpcodes.contains(body.opcodeOf(callee)))
        callee = body.value(callee).operand(0);
    if (!callee || body.isParameter(callee) || body.opcodeOf(callee) != Opcode::GetModuleVariable)
        return { };
    const Variable& variable = program.at(body.value(callee).as<Opcode::GetModuleVariable>().variable());
    if (variable.module != file || !variable.flags.contains(VariableFlags::IsImmutable) || !variable.declared.isEstablished())
        return { };
    TypeID held = walker.types().withOnlyObjects(variable.declared.written());
    FunctionID constructor = held && !isNeverType(held) ? singleFunctionOf(walker.types(), walker.types().at(held)) : FunctionID();
    return constructor ? classOfClassObject(program, constructor) : LayoutID();
}

static ValueID valueThatAloneIsDefinedUnder(const BodyIndex& body, ValueID literal, PropertyKeyID key)
{
    ValueID defined;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        if (!body.isReachable(BlockID(number)))
            continue;
        for (BodyValue operation : body.block(BlockID(number)).values()) {
            if (operation.opcode() != Opcode::PropDefine)
                continue;
            auto definition = operation.as<Opcode::PropDefine>();
            if (definition.key() != key || body.stripCasts(definition.objectOperand()) != literal)
                continue;
            if (defined)
                return { };
            defined = definition.valueOperand();
        }
    }
    return defined;
}

static FunctionID functionWrittenUnderNextOfIteratorOf(Walker& walker, ValueID iterated)
{
    TypeArena& types = walker.types();
    const Program& program = walker.program();
    ModuleID file = program.at(walker.currentFunction()).module;
    PropertyKeyID keyOfOpening = types.wellKnownSymbolKey(WellKnownSymbol::Iterator);
    Reference iterable = walker.referenceOf(iterated);
    FunctionID opens = iterable && iterable.isValue() ? methodThatClassesOfFileWriteUnder(walker, walker.typeOf(iterable), file, keyOfOpening) : FunctionID();
    if (!opens)
        opens = methodThatClassesOfFileWriteUnder(walker, findExpectation(walker, iterated).type, file, keyOfOpening);
    if (!opens || program.at(opens).has(FunctionFlags::IsGenerator) || program.at(opens).has(FunctionFlags::IsAsync))
        return { };
    const BodyIndex* found = walker.facts().bodies().find(opens);
    if (!found)
        return { };
    const BodyIndex& body = *found;
    PropertyKeyID next = program.keyOf(BuiltinNames::next);
    FunctionID written;
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        if (!body.isReachable(BlockID(number)))
            continue;
        BodyTerminator end = body.block(BlockID(number)).terminator();
        if (end.kind() == TerminatorKind::ReturnCall)
            return { };
        if (end.kind() != TerminatorKind::Return)
            continue;
        ValueID handedBack = end.operand(0);
        while (handedBack && !body.isParameter(handedBack) && (inputPassThroughOpcodes.contains(body.opcodeOf(handedBack)) || body.opcodeOf(handedBack) == Opcode::FinishObject))
            handedBack = body.value(handedBack).operand(0);
        if (handedBack && !body.isParameter(handedBack) && body.opcodeOf(handedBack) == Opcode::Construct) {
            LayoutID made = classOfFileThatConstructionNames(walker, body, handedBack, file);
            FunctionID here = made ? methodThatClassWritesUnder(walker, made, next) : FunctionID();
            if (!here || program.at(here).module != file || (written && written != here))
                return { };
            written = here;
            continue;
        }
        if (!handedBack || body.isParameter(handedBack) || body.opcodeOf(handedBack) != Opcode::NewObject)
            return { };
        BodyValue literal = body.value(handedBack);
        auto allocation = literal.as<Opcode::NewObject>();
        std::optional<FieldReference> field = findField(program, allocation.layout(), next);
        std::span<const ValueID> values = literal.variadicOperands();
        if (!field || field->layout != allocation.layout() || allocation.fields().size() != values.size())
            return { };
        FunctionID here;
        ValueID stored = body.opcodes().contains(Opcode::PropDefine) && !std::ranges::contains(allocation.fields(), field->field) ? valueThatAloneIsDefinedUnder(body, handedBack, next) : ValueID();
        for (size_t place = 0; place < values.size(); ++place) {
            if (allocation.fields()[place] == field->field)
                stored = values[place];
        }
        stored = stored ? body.stripCasts(stored) : ValueID();
        if (stored && !body.isParameter(stored) && body.opcodeOf(stored) == Opcode::NewClosure)
            here = body.value(stored).as<Opcode::NewClosure>().function();
        if (!here || (written && written != here))
            return { };
        written = here;
    }
    return written;
}

static ValueID onlyTakerBehindCasts(const BodyIndex& index, ValueID value)
{
    ValueID taker = index.onlyUsingValue(value);
    while (taker && inputPassThroughOpcodes.contains(index.opcodeOf(taker)))
        taker = index.onlyUsingValue(taker);
    return taker;
}

static bool isEntryPairOnlyDestructured(const BodyIndex& index, ValueID open)
{
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

bool isListOfEntriesTakenApartAtOnce(Walker& walker, const BodyValue& call)
{
    Reference called = walker.referenceOf(call.operand(OperandIndex::Call::callee));
    while (called && called.isValue() && walker.code().at(called.id()).opcode == Opcode::Narrowed) {
        ValueID tested = walker.code().at(called.id()).valueOperand();
        called = walker.code().isConstant(tested) ? Reference::constant(tested) : Reference::value(tested);
    }
    if (!called || !called.isRecord() || !walker.types().at(walker.code().at(called.id()).type).isObject() || !singleBuiltinOf(walker.types(), walker.types().at(walker.code().at(called.id()).type)))
        return false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis) || !isArrayIteratorProtocolIntact(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID open = onlyTakerBehindCasts(index, call.id());
    if (!open || index.opcodeOf(open) != Opcode::IteratorOpen || !isOpenThatMayGoByIndex(index.value(open)) || index.value(open).as<Opcode::IteratorOpen>().isDestructuring())
        return false;
    if (walker.walksStretchInsideAnother() || index.blockOf(open) != index.blockOf(call.id()))
        return false;
    WayOfIteration way = findWayOfIteration(index, open);
    return way.canBeFollowed && way.headOfLoopThatSteps && isEntryPairOnlyDestructured(index, open);
}

static bool canElideEntryPair(Walker& walker, const BodyIndex& index, const Walker::IndexedIteration& iteration, ValueID open)
{
    bool stepsPairs = (iteration.contents == ContentsStepped::Map && iteration.table) || (iteration.contents == ContentsStepped::List && !iteration.table && !iteration.isStringOrArray);
    return stepsPairs && iteration.kind == JSC::IterationKind::Entries && walker.has(Mechanism::TypedOperations) && isArrayIteratorProtocolIntact(walker) && isEntryPairOnlyDestructured(index, open);
}

bool isMadeByCallThatListsEntriesFlat(Walker& walker, Reference list)
{
    if (!list || !list.isValue())
        return false;
    const Code& code = walker.code();
    const BuiltinTable& table = walker.program().builtins();
    const Operation& made = code.at(underlyingCellOf(code, list.id()));
    return made.opcode == Opcode::CallBuiltin && table.contains(Builtins::CompilerHelpersOwnEntriesFlat) && table.at(made.overload()).builtin == table.canonicalID(Builtins::CompilerHelpersOwnEntriesFlat);
}

static Walker::IndexedIteration* findIterationWhoseStepIsTakenApart(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    ValueID pair = index.stripCasts(open.as<Opcode::IteratorOpen>().iterableOperand());
    if (!pair || index.isParameter(pair) || index.opcodeOf(pair) != Opcode::ResultAt)
        return nullptr;
    ValueID step = index.value(pair).operand(0);
    if (index.isParameter(step) || index.opcodeOf(step) != Opcode::IteratorResume)
        return nullptr;
    Reference state = walker.referenceOf(index.value(step).operand(0));
    return state && state.isVirtual() ? &walker.indexedIteration(state) : nullptr;
}

static std::optional<ClassSteppedUnseen> findListsThatAsyncIterationStepsByIndex(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    const BuiltinTable& table = walker.program().builtins();
    if (written.iteratorKind() != IteratorKind::Async || written.isDestructuring() || walker.program().admitsReplacedBuiltins || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !isArrayIteratorProtocolIntact(walker))
        return std::nullopt;
    std::optional<BuiltinID> opensAsync = findMemberAboveForIteration(walker, table.at(BuiltinClasses::Array).prototype, table.nameOf(WellKnownSymbol::AsyncIterator));
    std::optional<BuiltinID> opens = findMemberAboveForIteration(walker, table.at(BuiltinClasses::Array).prototype, table.nameOf(WellKnownSymbol::Iterator));
    if (!opensAsync || *opensAsync || !opens || !*opens)
        return std::nullopt;
    Reference iterable = walker.referenceOf(written.iterableOperand());
    if (!iterable || !iterable.isValue())
        return std::nullopt;
    TypeArena& types = walker.types();
    auto namesListsAlone = [&](TypeID type) {
        TypeID present = type ? types.withoutNullish(type) : TypeID();
        return present && !isNeverType(present) && types.isSubset(present, types.builtinClass(BuiltinClasses::Array));
    };
    if (!namesListsAlone(walker.typeOf(iterable)) && !namesListsAlone(findExpectation(walker, written.iterableOperand()).type))
        return std::nullopt;
    std::optional<ClassSteppedUnseen> lists = findClassToTestFor(walker, written.iterableOperand(), iterable, true);
    if (!lists || lists->contents != ContentsStepped::List || lists->kind != JSC::IterationKind::Values || lists->builtinClass != BuiltinClasses::Array)
        return std::nullopt;
    return lists;
}

static bool openAsyncIterationInValues(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    Reference iterable = walker.referenceOf(written.iterableOperand());
    if (written.iteratorKind() != IteratorKind::Async || written.isDelegating() || !iterable || !iterable.isValue())
        return false;
    if (findListsThatAsyncIterationStepsByIndex(walker, open))
        return false;
    if (!findWayOfIteration(walker.currentBodyIndex(), open.id()).canSkipDoneChecks)
        return false;
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Async, WordingOfIteration::Source);
    builder.standFor(open);
    Walker::IndexedIteration iteration = builder.openByProtocolAlone(iterable);
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), open.id()))
        walker.leaveAnythingAt(listed);
    walker.state().invalidateHeap();
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

ValueID findOpeningThatAloneTakesParameter(const BodyIndex& index, ValueID parameter)
{
    ValueID open = onlyTakerBehindCasts(index, parameter);
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen || index.loopOf(index.blockOf(open)) || index.holdsAnyOf(opcodesThatMakeArguments))
        return { };
    bool mayStepTable = isOpenThatMayGoByIndex(index.value(open)) && !index.value(open).as<Opcode::IteratorOpen>().isDestructuring() && findWayOfIteration(index, open).canBeFollowed;
    return mayStepTable ? open : ValueID();
}

bool isIteratorOfTableJustMade(const Walker& walker, Reference handed)
{
    if (!handed || !handed.isValue())
        return false;
    const Operation& made = walker.code().at(underlyingCellOf(walker.code(), handed.id()));
    return made.opcode == Opcode::CallInline && (made.inlineBuiltin() == InlineBuiltin::MapIterator || made.inlineBuiltin() == InlineBuiltin::SetIterator);
}

bool Walker::callCopiedAloneTakesWhatWasMadeForIt(Reference handed) const
{
    if (m_frames.size() < 2 || !m_frames.last().callCopied || m_frames.last().inlinedHeadBlocks)
        return false;
    const Frame& caller = m_frames[m_frames.size() - 2];
    const BodyIndex& index = *caller.index;
    ValueID call = m_frames.last().callCopied;
    const Operation& built = m_code.at(underlyingCellOf(m_code, handed.id()));
    uint32_t timesHanded = 0;
    bool isMadeForCall = true;
    index.value(call).forEachOperand([&](ValueID operand, uint32_t) {
        if (caller.references[operand.index()] != handed)
            return;
        ValueID made = index.stripCasts(operand);
        ++timesHanded;
        isMadeForCall &= !index.isParameter(made) && index.opcodeOf(made) == Opcode::Call && index.blockOf(made) == index.blockOf(call) && onlyTakerBehindCasts(index, made) == call
            && built.inlineFrame == caller.inlineFrame && built.position == index.value(made).origin().position;
        if (!isMadeForCall)
            return;
        Reference receiver = index.value(made).thisValueOperand() ? caller.references[index.value(made).thisValueOperand().index()] : Reference();
        isMadeForCall = receiver && receiver.isValue() && built.receiverOperand() && underlyingCellOf(m_code, receiver.id()) == underlyingCellOf(m_code, built.receiverOperand());
    });
    return timesHanded == 1 && isMadeForCall;
}

bool Walker::callCopiedAloneTakesRegExp(Reference handed) const
{
    if (m_frames.size() < 2 || !m_frames.last().callCopied || m_frames.last().inlinedHeadBlocks || currentBodyIndex().holdsAnyOf(opcodesThatMakeArguments))
        return false;
    const Frame& caller = m_frames[m_frames.size() - 2];
    const BodyIndex& index = *caller.index;
    ValueID call = m_frames.last().callCopied;
    const Operation& made = m_code.at(underlyingCellOf(m_code, handed.id()));
    uint32_t timesHanded = 0;
    bool isWrittenForCall = true;
    index.value(call).forEachOperand([&](ValueID operand, uint32_t) {
        if (caller.references[operand.index()] != handed)
            return;
        ValueID written = index.stripCasts(operand);
        ++timesHanded;
        isWrittenForCall &= !index.isParameter(written) && index.opcodeOf(written) == Opcode::NewRegExp && index.blockOf(written) == index.blockOf(call) && onlyTakerBehindCasts(index, written) == call
            && made.inlineFrame == caller.inlineFrame && made.position == index.value(written).origin().position;
    });
    return timesHanded == 1 && isWrittenForCall;
}

static bool openTableInPlaceOfIteratorHandedToCopy(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    ValueID parameter = index.stripCasts(open.as<Opcode::IteratorOpen>().iterableOperand());
    Reference iterator = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand());
    if (!index.isParameter(parameter) || index.blockOf(parameter) != entryBlock || !isIteratorOfTableJustMade(walker, iterator))
        return false;
    if (findOpeningThatAloneTakesParameter(index, parameter) != open.id() || !walker.callCopiedAloneTakesWhatWasMadeForIt(iterator))
        return false;
    TypeArena& types = walker.types();
    const Operation& made = walker.code().at(underlyingCellOf(walker.code(), iterator.id()));
    const BuiltinResult& result = walker.program().builtins().at(made.overload()).result;
    Reference table = made.receiverOperand() ? Reference::value(made.receiverOperand()) : Reference();
    const ClassPart* classOfTable = table ? singleBuiltinClassOf(types, types.at(walker.typeOf(table))) : nullptr;
    const ClassPart* classOfIterator = singleBuiltinClassOf(types, types.at(walker.typeOf(iterator)));
    if (!classOfTable || !classOfIterator || !result.iteratedOperand || !result.iteratedOperand->isReceiver() || !nobodySeesStepsOfIterators(walker, classOfIterator->builtinClass, true))
        return false;
    WayOfIteration way = findWayOfIteration(index, open.id());
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    builder.standFor(open);
    Walker::IndexedIteration iteration;
    iteration.list = table;
    iteration.builtinClass = classOfTable->builtinClass;
    iteration.contents = made.inlineBuiltin() == InlineBuiltin::MapIterator ? ContentsStepped::Map : ContentsStepped::Set;
    iteration.kind = result.iterationKind;
    iteration.position = builder.integer(0);
    iteration.table = builder.tableThatFirstStepBeginsIn(table, iteration.contents);
    iteration.canSkipDoneChecks = way.canSkipDoneChecks;
    iteration.inputHead = way.headOfLoopThatSteps;
    walker.count(StatisticID::IteratorAllocationsElidedInInlinees);
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

static bool openDelegatedIteration(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    if (written.iteratorKind() != IteratorKind::Sync || !written.isDelegating())
        return false;
    WayOfIteration way = findWayOfIteration(walker.currentBodyIndex(), open.id());
    Reference iterable = walker.referenceOf(written.iterableOperand());
    if (!way.canBeFollowed || !way.hasSingleStepAtHeader || !iterable || !iterable.isRecord() || !nobodyThrowsIntoIterators(walker) || isResultOfDelegatedCall(walker, iterable))
        return false;

    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Delegation);
    builder.standFor(open);
    TypeID present = walker.types().withoutNullish(walker.typeOf(iterable));
    if (iterable.isValue() && present != walker.typeOf(iterable) && !isNeverType(present) && findClassSteppedUnseen(walker, present)) {
        iterable = builder.requireObjectCoercible(iterable);
        if (walker.isUnreachable())
            return true;
    }
    std::optional<ClassSteppedUnseen> proven = findClassSteppedUnseen(walker, walker.typeOf(iterable));
    Walker::IndexedIteration iteration;
    if (proven && (proven->contents == ContentsStepped::Text || (proven->contents != ContentsStepped::View && isKnownPristine(walker, iterable)))) {
        walker.verifyPristineObject(iterable);
        iteration.list = iterable;
        iteration.builtinClass = proven->builtinClass;
        iteration.contents = proven->contents;
        iteration.kind = proven->kind;
        iteration.position = builder.integer(0);
        iteration.table = builder.tableThatFirstStepBeginsIn(iterable, proven->contents);
        iteration.canSkipDoneChecks = true;
    } else {
        std::optional<ClassSteppedUnseen> tested = findClassToTestFor(walker, written.iterableOperand(), iterable, true);
        if (!tested)
            return false;
        tested->textMayComeInstead = false;
        tested->mapMayComeInstead = false;
        iteration = builder.openByIndexOrProtocol(iterable, *tested, false, true, true);
        walker.recordSpeculation();
    }
    iteration.isDelegating = true;
    iteration.inputHead = way.headOfLoopThatSteps;
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

static ValueID stepAtDelegationHeader(const BodyIndex& index, BlockID inputHead, ValueID open)
{
    ValueID step;
    for (BodyValue operation : index.block(inputHead).values()) {
        if (operation.opcode() == Opcode::IteratorResume && operation.operand(0) == open)
            step = operation.id();
        else if (step ? operation.opcode() != Opcode::ResultAt || operation.as<Opcode::ResultAt>().resultsOperand() != step : operation.opcode() != Opcode::LoopHint)
            return { };
    }
    return step;
}

bool callDelegate(Walker& walker, const BodyValue& call)
{
    auto written = call.as<Opcode::Call>();
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID open = index.onlyUsingValue(call.id());
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen || !walker.isInLoop() || !walker.opensGuesses() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::BuiltinCalls))
        return false;
    auto opened = index.value(open).as<Opcode::IteratorOpen>();
    Reference callee = walker.referenceOf(written.calleeOperand());
    if (opened.iteratorKind() != IteratorKind::Sync || !opened.isDelegating() || index.blockOf(open) != index.blockOf(call.id()) || !callee || !callee.isValue() || !engineAnswersForGenerators(walker))
        return false;
    if (open.index() - index.block(index.blockOf(open)).firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    WayOfIteration way = findWayOfIteration(index, open);
    if (!way.canBeFollowed || !way.hasSingleStepAtHeader || !stepAtDelegationHeader(index, way.headOfLoopThatSteps, open))
        return false;
    Draft draft = walker.draftOf(call);
    Draft delegated(Opcode::CallDelegate, draft.origin());
    delegated.setCalleeOperand(callee);
    if (written.thisValueOperand())
        delegated.setThisValueOperand(walker.referenceOf(written.thisValueOperand()));
    Vector<Reference, 8> arguments;
    for (ValueID argument : written.argumentsOperands())
        arguments.append(walker.referenceOf(argument));
    delegated.setArgumentsOperands(arguments.span());
    if (written.calleeName())
        delegated.setCalleeName(written.calleeName());
    walker.chooseConservativeFields(delegated);
    Reference results = walker.build(WTF::move(delegated));
    if (walker.isUnreachable() || !results)
        return true;
    walker.state().invalidateHeap();
    Draft value(Opcode::ResultAt, draft.origin());
    value.setResultsOperand(results);
    value.setIndex(static_cast<uint32_t>(DelegatedCallResult::Value));
    walker.bind(call.id(), walker.build(WTF::move(value)));
    return true;
}

static bool openDelegationInValues(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    Reference iterable = walker.referenceOf(written.iterableOperand());
    if (written.iteratorKind() != IteratorKind::Sync || !written.isDelegating() || !iterable || !iterable.isValue() || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::BuiltinCalls))
        return false;
    WayOfIteration way = findWayOfIteration(walker.currentBodyIndex(), open.id());
    if (!way.canBeFollowed || !way.hasSingleStepAtHeader)
        return false;
    if (Reference hasCompleted = completionOfDelegatedCall(walker, iterable, walker.originOf(open)))
        walker.leaveCompletedDelegation(way.headOfLoopThatSteps, stepAtDelegationHeader(walker.currentBodyIndex(), way.headOfLoopThatSteps, open.id()), hasCompleted, iterable);
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Delegation);
    builder.standFor(open);
    Walker::IndexedIteration iteration = builder.openDelegation(iterable);
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), open.id()))
        walker.leaveAnythingAt(listed);
    walker.state().invalidateHeap();
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

static bool returnsNewTable(const Walker& walker, FunctionID function)
{
    const BodyIndex* body = walker.facts().bodies().find(function);
    if (!body || walker.program().at(function).isCoroutine())
        return false;
    ValueID returned;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyTerminator end = body->terminator(BlockID(number));
        if (!body->isReachable(BlockID(number)) || (end.kind() != TerminatorKind::Return && end.kind() != TerminatorKind::ReturnCall))
            continue;
        ValueID result = end.operand(0) ? body->stripCasts(end.operand(0)) : ValueID();
        if (!result || (returned && returned != result))
            return false;
        returned = result;
    }
    if (!returned || body->isParameter(returned) || body->opcodeOf(returned) != Opcode::Construct)
        return false;
    ValueID callee = body->stripCasts(body->value(returned).calleeOperand());
    if (!callee || body->isParameter(callee) || body->opcodeOf(callee) != Opcode::Builtin)
        return false;
    BuiltinID constructor = body->value(callee).as<Opcode::Builtin>().builtin();
    return constructor == Builtins::Map || constructor == Builtins::Set;
}

static void reserveTablesFilledByIteration(Walker& walker, const BodyIndex& index, const BodyValue& open, BlockID head, Reference iterable)
{
    if (!iterable || !walker.types().overlap(walker.typeOf(iterable), walker.types().builtinClass(BuiltinClasses::Array)))
        return;
    LoopID loop = index.loopOf(head);
    uint32_t firstValueOfLoop = index.block(head).firstParameter().index();
    Vector<Reference, 2> tables;
    for (uint32_t number : additionsToTables(walker)) {
        ValueID call(number);
        BlockID block = index.blockOf(call);
        ValueID receiver = index.stripCasts(index.value(call).as<Opcode::Call>().thisValueOperand());
        if (index.loopOf(block) != loop || receiver.index() >= firstValueOfLoop)
            continue;
        Reference table = walker.referenceOf(receiver);
        auto isNew = [&] {
            if (walker.state().isFresh(Reference::value(underlyingCellOf(walker.code(), table.id()))))
                return true;
            FunctionID maker = !index.isParameter(receiver) && index.opcodeOf(receiver) == Opcode::Call ? walker.facts().linked().calleeAt(walker.currentFunction(), receiver) : FunctionID();
            return maker && returnsNewTable(walker, maker);
        };
        if (!table || !table.isValue() || tables.contains(table) || !isNew())
            continue;
        TypeID type = walker.typeOf(table);
        if (!walker.types().isSubset(type, walker.types().builtinClass(BuiltinClasses::Map)) && !walker.types().isSubset(type, walker.types().builtinClass(BuiltinClasses::Set)))
            continue;
        bool growsInEveryIteration = std::ranges::all_of(walker.incomingEdgesOf(index, head), [&](const BodyIncomingEdge& edge) {
            return !index.isInLoop(edge.from, loop) || index.dominates(block, edge.from);
        });
        if (growsInEveryIteration)
            tables.append(table);
    }
    for (Reference table : tables) {
        Draft reserve(Opcode::ReserveTableCapacity, walker.originOf(open));
        walker.chooseConservativeFields(reserve);
        reserve.setTableOperand(table);
        reserve.setSourceOperand(iterable);
        walker.build(WTF::move(reserve));
        walker.count(StatisticID::TableCapacitiesReserved);
    }
}

static bool openTypedIteration(Walker& walker, const BodyValue& open)
{
    if (!isOpenThatMayGoByIndex(open))
        return openDelegatedIteration(walker, open);
    const BodyIndex& index = walker.currentBodyIndex();
    if (openTableInPlaceOfIteratorHandedToCopy(walker, index, open))
        return true;
    Walker::IndexedIteration* entries = findIterationWhoseStepIsTakenApart(walker, index, open);
    if (entries && entries->isEntryPairElided && entries->keyOfPair) {
        Walker::IndexedIteration stepping = *entries;
        BuilderOfIteration takesApart(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
        takesApart.standFor(open);
        Walker::IndexedIteration pattern = takesApart.takePartsOfPairOrStepsAtOpen(stepping, walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand()));
        if (!walker.isUnreachable())
            walker.bind(open.id(), walker.addIndexedIteration(WTF::move(pattern)));
        return true;
    }
    if (entries && entries->isOverFlatEntries) {
        Walker::IndexedIteration pair;
        pair.builtinClass = BuiltinClasses::Array;
        pair.hasPrecomputedSteps = true;
        pair.precomputedSteps.append(walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand()));
        pair.precomputedSteps.append(entries->valueBesideKey);
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(pair)));
        return true;
    }
    if (Reference list = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand()); isMadeByCallThatListsEntriesFlat(walker, list)) {
        Walker::IndexedIteration entries;
        entries.list = list;
        entries.builtinClass = BuiltinClasses::Array;
        entries.isOverFlatEntries = true;
        entries.position = BuilderOfIteration(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source).integer(0);
        entries.inputHead = findWayOfIteration(index, open.id()).headOfLoopThatSteps;
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(entries)));
        return true;
    }
    Reference iterable = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand());
    ValueID iterated = open.as<Opcode::IteratorOpen>().iterableOperand();
    bool isNamedByOpenAlone = !index.isParameter(iterated) && opcodesThatMakeArguments.contains(index.opcodeOf(iterated)) && index.onlyUsingValue(iterated) == open.id();
    bool staysAsItIsWhileIterated = isArrayIteratorProtocolIntact(walker) && (isNamedByOpenAlone || stepsOfPatternStandInRow(index, open));
    if (Reference list = staysAsItIsWhileIterated ? listStandingForFreshArguments(walker, iterable) : Reference())
        iterable = list;
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    builder.standFor(open);

    if (std::optional<Vector<Reference, 4>> parts = open.as<Opcode::IteratorOpen>().elementCount() ? partsThatPatternTakes(walker, iterable, *open.as<Opcode::IteratorOpen>().elementCount()) : std::nullopt; parts && isArrayIteratorProtocolIntact(walker) && stepsOfPatternStandInRow(index, open)) {
        Walker::IndexedIteration pattern;
        pattern.builtinClass = BuiltinClasses::Array;
        pattern.hasPrecomputedSteps = true;
        pattern.precomputedSteps = WTF::move(*parts);
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(pattern)));
        return true;
    }

    if (ValueID literal = findLiteralIteratedAlone(walker, index, open)) {
        Walker::IndexedIteration iteration;
        iteration.list = iterable;
        iteration.builtinClass = BuiltinClasses::Array;
        iteration.literal = literal;
        iteration.position = builder.integer(open.as<Opcode::IteratorOpen>().elementCount().value_or(0));
        iteration.inputHead = findWayOfIteration(index, open.id()).headOfLoopThatSteps;
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
        return true;
    }

    if (iterable && iterable.isValue()) {
        TypeID present = walker.types().withoutNullish(walker.typeOf(iterable));
        if (present != walker.typeOf(iterable) && !isNeverType(present) && findClassSteppedUnseen(walker, present)) {
            iterable = builder.requireObjectCoercible(iterable);
            if (walker.isUnreachable())
                return true;
        }
    }
    std::optional<ClassSteppedUnseen> proven = iterable ? findClassSteppedUnseen(walker, walker.typeOf(iterable)) : std::nullopt;
    bool isAsMadeForGood = proven && proven->contents != ContentsStepped::Text && linksWordCanBeHeldAgainst(walker, iterable) && (classOfIterableThatStaysAsMadeByLink(walker, open, iterated) == proven->builtinClass || (walker.shouldAssumePermanentlyPristineIterables() && open.as<Opcode::IteratorOpen>().isDestructuring()));
    bool isSurelyAsMade = proven && (proven->contents == ContentsStepped::Text || isAsMadeForGood || (proven->contents != ContentsStepped::View && isKnownPristine(walker, iterable)));
    if (iterable && !isSurelyAsMade && stepsOfPatternStandInRow(index, open)) {
        uint32_t numberOfSteps = *open.as<Opcode::IteratorOpen>().elementCount();
        bool standsInLoop = iterable.isValue() && walker.isInLoop();
        bool mayBeOfSubclass = iterable.isValue() && walker.program().someClassExtendsArray;
        std::optional<ClassSteppedUnseen> table = standsInLoop || mayBeOfSubclass ? findClassToTestFor(walker, iterated, iterable, true, keyOfIterator(walker)) : std::nullopt;
        if (table && table->subclass && table->contents == ContentsStepped::List) {
            Walker::IndexedIteration iteration = builder.takeSubclassElementsAtOpen(iterable, numberOfSteps, *table);
            walker.recordSpeculation();
            if (!walker.isUnreachable())
                walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
            return true;
        }
        if (!standsInLoop)
            table = std::nullopt;
        if (std::optional<Walker::IndexedIteration> held = table ? builder.tryOpenWithIterationGuard(open, iterable, *table) : std::nullopt) {
            if (!walker.isUnreachable())
                walker.bind(open.id(), walker.addIndexedIteration(WTF::move(*held)));
            return true;
        }
        if (table && table->contents != ContentsStepped::Map && table->contents != ContentsStepped::Set)
            table = std::nullopt;
        TypeID expected = standsInLoop && !table ? findExpectation(walker, iterated).type : TypeID();
        bool isTakenForObjectOfProgram = standsInLoop && !table && (walker.types().at(walker.typeOf(iterable)).hasOnlyLayouts() || (expected && walker.types().at(expected).hasOnlyLayouts()));
        if (isTakenForObjectOfProgram) {
            for (uint32_t number = open.id().index() + 1; numberOfSteps && number < index.numberOfValues(); ++number) {
                ValueID step(number);
                if (!index.isParameter(step) && index.opcodeOf(step) == Opcode::IteratorResume && index.value(step).operand(0) == open.id()) {
                    builder.expectUnderNext(walker.facts().held().calleeAt(walker.currentFunction(), step));
                    break;
                }
            }
            builder.expectWhatIsWrittenUnderNextOfIteratorOf(iterated);
            Walker::IndexedIteration iteration = builder.takeStepsByProtocolAtOpen(iterable, numberOfSteps);
            if (!walker.isUnreachable())
                walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
            return true;
        }
        if (table || !standsInLoop || walker.types().overlap(walker.typeOf(iterable), walker.types().builtinClass(BuiltinClasses::Array))) {
            Walker::IndexedIteration iteration = table ? builder.takeStepsThroughTableAtOpen(iterable, numberOfSteps, *table) : builder.takeStepsAtOpen(iterable, numberOfSteps);
            if (table)
                walker.recordSpeculation();
            if (!walker.isUnreachable())
                walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
            return true;
        }
    }

    WayOfIteration way = findWayOfIteration(index, open.id());
    if (!way.canBeFollowed)
        return false;
    std::optional<ClassSteppedUnseen> tested = isSurelyAsMade ? std::nullopt : findClassToTestFor(walker, open.as<Opcode::IteratorOpen>().iterableOperand(), iterable, !!way.headOfLoopThatSteps, keyOfIterator(walker));
    if (!isSurelyAsMade && !tested && (!way.canSkipDoneChecks || !iterable || !iterable.isValue()))
        return false;
    if (way.hasSingleStepAtHeader)
        reserveTablesFilledByIteration(walker, index, open, way.headOfLoopThatSteps, iterable);
    if (tested && tested->contents == ContentsStepped::List && way.hasSingleStepAtHeader && !walker.program().admitsReplacedBuiltins && walker.types().overlap(walker.typeOf(iterable), TypeTable::string) && (tested->isListForWantOfWord || writtenWordOfParameterNamesTextBesideList(walker, iterated))) {
        std::optional<ClassSteppedUnseen> texts = findClassSteppedUnseen(walker, TypeID(TypeTable::string));
        LoopID loop = index.loopOf(way.headOfLoopThatSteps);
        if (texts && (comparesStepValuesWithCharacters(walker, index, loop, open.id()) || usesStepValuesAsCharacters(walker, index, loop, open.id()))) {
            tested->textMayComeInstead = true;
            walker.count(StatisticID::ArrayIterationsWithStringFallback);
        }
    }

    Walker::IndexedIteration iteration;
    if (!isSurelyAsMade && !tested) {
        if (!isExpectedToStepGenerator(walker, open.id()))
            builder.expectNoGenerator();
        std::optional<Walker::IndexedIteration> returned = builder.tryOpenReturnedIterator(iterable, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.canSkipDoneChecks);
        iteration = returned ? WTF::move(*returned) : builder.openByProtocolAlone(iterable);
        if (!iteration.isProtocolOnly)
            iteration.isEntryPairElided = canElideEntryPair(walker, index, iteration, open.id());
        if (iteration.isProtocolOnly || iteration.hasProtocolFallback) {
            if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), open.id()))
                walker.leaveAnythingAt(listed);
            walker.state().invalidateHeap();
        }
    } else if (isSurelyAsMade) {
        walker.verifyPristineObject(iterable);
        if (isAsMadeForGood)
            holdLinksWordAgainstObjectAsMade(walker, iterable, walker.originOf(open));
        iteration.list = iterable;
        iteration.builtinClass = proven->builtinClass;
        iteration.contents = proven->contents;
        iteration.kind = proven->kind;
        iteration.position = builder.integer(0);
        iteration.table = builder.tableThatFirstStepBeginsIn(iterable, proven->contents);
        iteration.isAsMadeForGood = isAsMadeForGood;
        iteration.listHasNoHoles = isAsMadeForGood && walker.facts().linked().iterableStaysAsMadeAt(walker.currentFunction(), open.id()) && walker.facts().linked().iterableHasNoHolesAt(walker.currentFunction(), open.id());
        iteration.canSkipDoneChecks = way.canSkipDoneChecks;
        iteration.stepsOfPatternStandInRow = proven->contents == ContentsStepped::List && stepsOfPatternStandInRow(index, open);
    } else {
        if (isExpectedToBeSteppedUnseen(walker, open.id()))
            builder.expectNoStepByProtocol();
        iteration = builder.openByIndexOrProtocol(iterable, *tested, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.canSkipDoneChecks);
        iteration.isEntryPairElided = canElideEntryPair(walker, index, iteration, open.id());
        walker.recordSpeculation();
    }
    if (!iteration.isProtocolOnly)
        iteration.inputHead = way.headOfLoopThatSteps;
    bool secondCopyMayPay = iteration.contents == ContentsStepped::List || iteration.contents == ContentsStepped::Text || (iteration.contents == ContentsStepped::View && hasInlineElementAccess(walker.program().builtins().typedArrayTypeOf(iteration.builtinClass)));
    iteration.isDeadAfterLoop = !iteration.isProtocolOnly && way.hasSingleStepAtHeader && secondCopyMayPay && (!iteration.hasProtocolFallback || iteration.state || iteration.canSkipDoneChecks);
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(iteration)));
    return true;
}

static bool nameIterationUnder(Walker&, ClassSteppedUnseen& tested, BuiltinName, bool isAsMadeForGood);
static std::optional<ClassSteppedUnseen> tableToTestFor(Walker&, ValueID input, Reference receiver, BuiltinClassID tableClass);

static bool isReachedByNestedLoops(Walker& walker)
{
    const auto& linked = walker.facts().linked();
    FunctionID function = walker.unit().function;
    return walker.isInLoop() || (function && (linked.functionsFinishedInNestedIterations.get(function.index()) || linked.functionsReachedByRecursion.get(function.index())));
}

static Vector<ClassSteppedUnseen, 2> findCollectionsWhoseIteratorIsSpread(Walker& walker, const BodyValue& read, const BodyValue& call, Reference receiver, bool& isAsMadeForGood)
{
    BuiltinName name = walker.program().builtinNameOf(read.key());
    if (!name || !receiver)
        return { };
    auto isTableStepped = [&](std::optional<ClassSteppedUnseen>& tested, bool isAsMadeForGood) {
        return tested && (tested->contents == ContentsStepped::Map || tested->contents == ContentsStepped::Set) && nameIterationUnder(walker, *tested, name, isAsMadeForGood) && tested->kind != JSC::IterationKind::Entries;
    };
    std::optional<ClassSteppedUnseen> proven = findClassSteppedUnseen(walker, receiver);
    isAsMadeForGood = proven && receiverStaysAsMadeByLink(walker, call);
    std::optional<ClassSteppedUnseen> tested = isAsMadeForGood ? proven : findClassToTestFor(walker, read.objectOperand(), receiver);
    Vector<ClassSteppedUnseen, 2> tables;
    if (isTableStepped(tested, isAsMadeForGood)) {
        tables.append(*tested);
        return tables;
    }
    if (proven || (name != BuiltinNames::values && name != BuiltinNames::keys) || !isReachedByNestedLoops(walker))
        return tables;
    for (BuiltinClassID tableClass : { BuiltinClassID(BuiltinClasses::Set), BuiltinClassID(BuiltinClasses::Map) }) {
        std::optional<ClassSteppedUnseen> table = tableToTestFor(walker, read.objectOperand(), receiver, tableClass);
        if (isTableStepped(table, false))
            tables.append(*table);
    }
    if (name == BuiltinNames::keys)
        std::ranges::reverse(tables);
    return tables;
}

static bool takeCollectionCopiedByArrayFrom(Walker& walker, const BodyValue& read, ValueID callID)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<CopyByArrayFrom> copy = findCopyByArrayFrom(index, callID);
    if (!copy)
        return false;
    BodyBlock block = index.block(walker.inputBlockOf(read.id()));
    uint32_t place = read.id().index() - block.firstValue().index();
    uint32_t placeAfterTest = block.numberOfValues();
    BodyValue call = index.value(callID);
    if (placeAfterTest > walker.endOfStretchBeingWalked() || call.calleeOperand() != read.id() || call.thisValueOperand() != read.objectOperand() || !call.as<Opcode::Call>().argumentsOperands().empty())
        return false;
    Reference receiver = walker.referenceOf(read.objectOperand());
    bool isAsMadeForGood = false;
    Vector<ClassSteppedUnseen, 2> tables = findCollectionsWhoseIteratorIsSpread(walker, read, call, receiver, isAsMadeForGood);
    if (tables.isEmpty())
        return false;

    walker.count(StatisticID::IteratorAllocationsElidedInSpreads);
    for (const ClassSteppedUnseen& tested : tables)
        walker.addCopiedCollection({ walker.inlineFrameAtWork(), callID, tested.builtinClass, tested.contents, tested.kind });
    BuilderOfIteration builder(walker, walker.originOf(call), IteratorKind::Sync, WordingOfIteration::Spread);
    if (isAsMadeForGood) {
        holdLinksWordAgainstObjectAsMade(walker, receiver, walker.originOf(call));
        builder.takePermanentlyUnmodifiedCollection(receiver, *copy);
    } else {
        builder.takeCollectionOrWalkGeneric(receiver, tables.span(), *copy, block.id(), place, placeAfterTest);
        walker.recordSpeculation();
    }
    walker.walkStretch(block.id(), placeAfterTest, placeAfterTest);
    return true;
}

bool copySetByArrayFromOfItsOpcodes(Walker& walker, const BodyValue& test)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID copied = test.operand(0);
    Reference given = walker.isUnreachable() || walker.hasStringsInBuilders() ? Reference() : walker.referenceOf(copied);
    std::optional<ClassSteppedUnseen> sets = given ? findClassSteppedUnseen(walker, given) : std::nullopt;
    if (given && !sets)
        sets = findClassToTestFor(walker, copied, given);
    if (!sets || sets->contents != ContentsStepped::Set || sets->kind != JSC::IterationKind::Values)
        return false;
    BodyBlock block = index.block(walker.inputBlockOf(test.id()));
    BodyTerminator end = block.terminator();
    if (end.kind() != TerminatorKind::Branch || end.operand(0) != test.id() || !index.hasOneUse(test.id()))
        return false;
    BodyBlock spreading = index.block(end.taken().block());
    static constexpr uint32_t valuesOfSpread = 2;
    if (index.numberOfIncomingEdges(spreading.id()) != 1 || spreading.numberOfParameters() || spreading.numberOfValues() < valuesOfSpread)
        return false;
    BodyValue array = index.value(spreading.firstValue());
    ValueID spread(spreading.firstValue().index() + 1);
    if (array.opcode() != Opcode::NewArray || array.numberOfVariadicOperands() || index.opcodeOf(spread) != Opcode::AppendSpread || index.value(spread).as<Opcode::AppendSpread>().iterableOperand() != copied)
        return false;
    BuilderOfIteration builder(walker, walker.originOf(test), IteratorKind::Sync, WordingOfIteration::Spread);
    builder.testForUnmodifiedSet(test, given, *sets);
    walker.recordSpeculation();
    return true;
}

static bool replaceSpreadWithCopiedCollection(Walker& walker, const BodyValue& spread, Reference array, Reference given)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID call = index.stripCasts(spread.as<Opcode::AppendSpread>().iterableOperand());
    Vector<ClassSteppedUnseen, 2> collections;
    for (const Walker::CopiedCollection& copied : walker.copiedCollections()) {
        if (copied.call != call || copied.inlineFrame != walker.inlineFrameAtWork())
            continue;
        ClassSteppedUnseen stepped { copied.builtinClass };
        stepped.contents = copied.contents;
        stepped.kind = copied.kind;
        collections.append(stepped);
    }
    std::optional<CopyByArrayFrom> copy = collections.isEmpty() ? std::nullopt : findCopyByArrayFrom(index, call);
    if (!copy || copy->spread != spread.id())
        return false;
    BuilderOfIteration builder(walker, walker.originOf(spread), IteratorKind::Sync, WordingOfIteration::Spread);
    builder.appendCopiedCollection(array, given, collections.span());
    return true;
}

bool appendMembersInPlaceOfIteratorThatEngineMakesOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || walker.hasStringsInBuilders() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockOf(read.id()));
    ValueID callID(read.id().index() + 1);
    if (callID.index() >= block.endOfValues().index() || index.opcodeOf(callID) != Opcode::Call || index.onlyUsingValue(read.id()) != callID)
        return false;
    ValueID spreadID = onlyTakerBehindCasts(index, callID);
    if (!spreadID)
        return takeCollectionCopiedByArrayFrom(walker, read, callID);
    if (index.isParameter(spreadID) || index.blockOf(spreadID) != block.id() || index.opcodeOf(spreadID) != Opcode::AppendSpread)
        return false;
    for (uint32_t number = callID.index() + 1; number < spreadID.index(); ++number) {
        if (!opcodesThatStateTypes.contains(index.opcodeOf(ValueID(number))))
            return false;
    }
    uint32_t place = read.id().index() - block.firstValue().index();
    uint32_t placeBehindSpread = spreadID.index() - block.firstValue().index() + 1;
    BodyValue call = index.value(callID);
    auto spread = index.value(spreadID).as<Opcode::AppendSpread>();
    ValueID receiverID = read.objectOperand();
    if (placeBehindSpread > walker.endOfStretchBeingWalked() || call.calleeOperand() != read.id() || call.thisValueOperand() != receiverID || !call.as<Opcode::Call>().argumentsOperands().empty())
        return false;
    Reference array = walker.referenceOf(spread.arrayOperand());
    if (!array || index.stripCasts(spread.iterableOperand()) != callID)
        return false;
    Reference receiver = walker.referenceOf(receiverID);
    bool isAsMadeForGood = false;
    Vector<ClassSteppedUnseen, 2> tables = findCollectionsWhoseIteratorIsSpread(walker, read, call, receiver, isAsMadeForGood);
    if (tables.isEmpty())
        return false;

    walker.count(StatisticID::IteratorAllocationsElidedInSpreads);
    BuilderOfIteration builder(walker, walker.originOf(index.value(spreadID)), IteratorKind::Sync, WordingOfIteration::Spread);
    if (isAsMadeForGood) {
        holdLinksWordAgainstObjectAsMade(walker, receiver, walker.originOf(index.value(spreadID)));
        builder.appendMembersOf(array, receiver, tables[0]);
    } else {
        builder.appendMembersOrWalkAsWritten(array, receiver, tables.span(), block.id(), place, placeBehindSpread);
        walker.recordSpeculation();
    }
    walker.walkStretch(block.id(), placeBehindSpread, placeBehindSpread);
    return true;
}

static bool isReadUnderIndexesInLoop(const BodyIndex& index, BlockID head, ValueID receiver)
{
    auto isReceiver = [&](ValueID object) {
        ValueID first = index.stripCasts(object);
        ValueID second = index.stripCasts(receiver);
        if (first == second)
            return true;
        if (!first || !second || index.isParameter(first) || index.isParameter(second) || index.opcodeOf(first) != Opcode::PropGet || index.opcodeOf(second) != Opcode::PropGet)
            return false;
        BodyValue read = index.value(first);
        BodyValue other = index.value(second);
        return !read.isFreeName() && !other.isFreeName() && read.key() == other.key() && index.stripCasts(read.objectOperand()) == index.stripCasts(other.objectOperand());
    };
    return std::ranges::any_of(index.membersOf(index.loopOf(head)), [&](BlockID member) {
        return std::ranges::any_of(index.block(member).values(), [&](BodyValue value) {
            return value.opcode() == Opcode::ElemGet && isReceiver(value.objectOperand());
        });
    });
}

static std::optional<ClassSteppedUnseen> tableToTestFor(Walker& walker, ValueID input, Reference receiver, BuiltinClassID tableClass)
{
    TypeArena& types = walker.types();
    TypeID tables = types.builtinClass(tableClass);
    if (!receiver.isValue() || !types.overlap(walker.typeOf(receiver), tables) || !walker.has(Mechanism::Guesses))
        return std::nullopt;
    TypeID expected = findExpectation(walker, input).type;
    if (expected && !isNeverType(types.withoutNullish(expected)) && !types.overlap(expected, tables))
        return std::nullopt;
    return findClassSteppedUnseen(walker, tables, walker.program().admitsReplacedBuiltins ? MembersBehindWatch::AreAsBuilt : MembersBehindWatch::AreOpen);
}

static std::optional<ClassSteppedUnseen> orMapToTestFor(Walker& walker, ValueID input, Reference receiver, std::optional<ClassSteppedUnseen> tested)
{
    if (tested && !tested->isListForWantOfWord)
        return tested;
    std::optional<ClassSteppedUnseen> maps = tableToTestFor(walker, input, receiver, BuiltinClasses::Map);
    if (!maps)
        return tested;
    maps->isGuessedMap = true;
    return maps;
}

static bool nameIterationUnder(Walker& walker, ClassSteppedUnseen& tested, BuiltinName name, bool isAsMadeForGood)
{
    std::optional<JSC::IterationKind> kind = kindOfIterationThatNobodySees(walker, tested.builtinClass, false, name);
    if (!kind && !isAsMadeForGood && walker.program().admitsReplacedBuiltins) {
        BuiltinID methodThatOpens;
        kind = kindOfIterationThatNobodySees(walker, tested.builtinClass, false, name, MembersBehindWatch::AreAsBuilt, &methodThatOpens);
        restOnWatchAboveInstances(walker, tested, methodThatOpens, name);
    }
    if (kind)
        tested.kind = *kind;
    return kind.has_value();
}

static std::optional<ClassSteppedUnseen> findCollectionThatStepsAreTakenOff(Walker& walker, const BodyValue& read, const BodyValue& call, bool& isAsMadeForGood)
{
    BuiltinName name = walker.program().builtinNameOf(read.key());
    Reference receiver = walker.referenceOf(read.objectOperand());
    if (!name || !receiver || walker.isUnreachable() || walker.hasStringsInBuilders())
        return std::nullopt;
    std::optional<ClassSteppedUnseen> proven = findClassSteppedUnseen(walker, receiver);
    isAsMadeForGood = proven && receiverStaysAsMadeByLink(walker, call);
    if (!isAsMadeForGood && !walker.isInLoop())
        return std::nullopt;
    std::optional<ClassSteppedUnseen> tested = isAsMadeForGood ? proven : orMapToTestFor(walker, read.objectOperand(), receiver, findClassToTestFor(walker, read.objectOperand(), receiver, true));
    if (!tested || (tested->contents != ContentsStepped::Map && tested->contents != ContentsStepped::Set && tested->contents != ContentsStepped::List))
        return std::nullopt;
    if (!nameIterationUnder(walker, *tested, name, isAsMadeForGood))
        return std::nullopt;
    return tested;
}

static std::optional<ClassSteppedUnseen> otherTableToTest(Walker& walker, const BodyValue& read, ClassSteppedUnseen& stepped)
{
    if (!stepped.isGuessedMap)
        return std::nullopt;
    BuiltinName name = walker.program().builtinNameOf(read.key());
    std::optional<ClassSteppedUnseen> sets = tableToTestFor(walker, read.objectOperand(), walker.referenceOf(read.objectOperand()), BuiltinClasses::Set);
    if (!sets || sets->contents != ContentsStepped::Set || !nameIterationUnder(walker, *sets, name, false))
        return std::nullopt;
    if (name == BuiltinNames::values)
        std::swap(stepped, *sets);
    return sets;
}

template<typename TakeAsWritten>
static Vector<Reference, 4> takeStepsOffCollection(Walker& walker, BuilderOfIteration& builder, const BodyValue& read, const BodyValue& call, const ClassSteppedUnseen& stepped, bool isAsMadeForGood, uint32_t numberOfSteps, const TakeAsWritten& takeAsWritten)
{
    Reference receiver = walker.referenceOf(read.objectOperand());
    if (isAsMadeForGood) {
        holdLinksWordAgainstObjectAsMade(walker, receiver, walker.originOf(call));
        builder.takeInstanceForAsMade(receiver);
    } else
        walker.recordSpeculation();
    walker.count(StatisticID::IteratorAllocationsElidedInSteps);
    return builder.takeStepsOffCollection(receiver, numberOfSteps, stepped, walker.facts().linked().contentsAtOperation(walker.currentFunction(), call.id()), takeAsWritten);
}

template<typename TakeAsWritten>
static Vector<Reference, 4> takeStepsOffTables(Walker& walker, BuilderOfIteration& builder, const BodyValue& read, const BodyValue& call, ClassSteppedUnseen& stepped, bool isAsMadeForGood, uint32_t numberOfSteps, const TakeAsWritten& takeAsWritten)
{
    std::optional<ClassSteppedUnseen> other = otherTableToTest(walker, read, stepped);
    if (!other)
        return takeStepsOffCollection(walker, builder, read, call, stepped, isAsMadeForGood, numberOfSteps, takeAsWritten);
    return takeStepsOffCollection(walker, builder, read, call, stepped, false, numberOfSteps, [&](Vector<Reference, 4>& taken) {
        taken = takeStepsOffCollection(walker, builder, read, call, *other, false, numberOfSteps, takeAsWritten);
    });
}

static bool takePatternOffCollection(Walker& walker, const BodyValue& read, const BodyValue& call, const BodyValue& open)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID given = open.as<Opcode::IteratorOpen>().iterableOperand();
    if (!walker.isInLoop() || index.stripCasts(given) != call.id() || !isOpenThatMayGoByIndex(open) || !stepsOfPatternStandInRow(index, open))
        return false;
    BodyBlock block = index.block(walker.inputBlockOf(read.id()));
    if (open.id().index() - block.firstValue().index() >= walker.endOfStretchBeingWalked())
        return false;
    bool isAsMadeForGood = false;
    std::optional<ClassSteppedUnseen> stepped = findCollectionThatStepsAreTakenOff(walker, read, call, isAsMadeForGood);
    if (!stepped)
        return false;

    uint32_t numberOfSteps = *open.as<Opcode::IteratorOpen>().elementCount();
    uint32_t place = read.id().index() - block.firstValue().index();
    uint32_t placeOfOpen = open.id().index() - block.firstValue().index();
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    Walker::IndexedIteration pattern;
    pattern.builtinClass = BuiltinClasses::Array;
    pattern.hasPrecomputedSteps = true;
    pattern.precomputedSteps = takeStepsOffTables(walker, builder, read, call, *stepped, isAsMadeForGood, numberOfSteps, [&](Vector<Reference, 4>& taken) {
        walker.walkStretch(block.id(), place, placeOfOpen);
        if (!walker.isUnreachable() && !walker.hasFailed())
            builder.takeStepsOutOfLine(walker.referenceOf(given), numberOfSteps, taken);
    });
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIndexedIteration(WTF::move(pattern)));
    walker.walkStretch(block.id(), placeOfOpen + 1, placeOfOpen + 1);
    return true;
}

namespace {

struct FirstStepOfIterator {
    ValueID call;
    ValueID value;
};

} // anonymous namespace

static std::optional<FirstStepOfIterator> findFirstStepOfIterator(Walker& walker, const BodyValue& read, uint32_t end)
{
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t number = read.id().index() + 1;
    auto takeOperation = [&](Opcode opcode) { return number < end && index.opcodeOf(ValueID(number)) == opcode ? ValueID(number++) : ValueID(); };
    auto takeCastsOf = [&](ValueID value) {
        while (number < end && opcodesThatStateTypes.contains(index.opcodeOf(ValueID(number))) && index.onlyUsingValue(value) == ValueID(number) && index.stripCasts(ValueID(number)) == index.stripCasts(value))
            value = ValueID(number++);
        return value;
    };
    ValueID callID = read.isFreeName() ? ValueID() : takeOperation(Opcode::Call);
    ValueID iteratorID = callID ? takeCastsOf(callID) : ValueID();
    ValueID nextID = callID ? takeOperation(Opcode::PropGet) : ValueID();
    ValueID stepID = nextID ? takeOperation(Opcode::Call) : ValueID();
    ValueID resultID = stepID ? takeCastsOf(stepID) : ValueID();
    ValueID valueID = stepID ? takeOperation(Opcode::PropGet) : ValueID();
    if (!valueID)
        return std::nullopt;
    if (index.useCount(iteratorID) != 2 || index.onlyUsingValue(read.id()) != callID || index.onlyUsingValue(nextID) != stepID || index.onlyUsingValue(resultID) != valueID || index.isUnused(valueID))
        return std::nullopt;
    auto isCallWithoutArguments = [&](ValueID called, ValueID callee, ValueID receiver) {
        BodyValue call = index.value(called);
        return call.calleeOperand() == callee && call.thisValueOperand() == receiver && call.as<Opcode::Call>().argumentsOperands().empty();
    };
    auto isReadUnder = [&](ValueID value, BuiltinName name, ValueID object) {
        BodyValue access = index.value(value);
        return !access.isFreeName() && access.key() == walker.program().keyOf(name) && access.objectOperand() == object;
    };
    if (!isCallWithoutArguments(callID, read.id(), read.objectOperand()) || !isReadUnder(nextID, BuiltinNames::next, iteratorID) || !isCallWithoutArguments(stepID, nextID, iteratorID) || !isReadUnder(valueID, BuiltinNames::value, resultID))
        return std::nullopt;
    return FirstStepOfIterator { callID, valueID };
}

bool startsFirstStepOfIterator(Walker& walker, const BodyValue& read)
{
    BuiltinName name = walker.program().builtinNameOf(read.key());
    bool opensIterator = name == BuiltinNames::keys || name == BuiltinNames::values || name == BuiltinNames::entries;
    return opensIterator && findFirstStepOfIterator(walker, read, walker.currentBodyIndex().block(walker.inputBlockOf(read.id())).endOfValues().index());
}

bool takeFirstStepOfIteratorThatEngineMakesOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID behind(read.id().index() + 1);
    if (read.isFreeName() || behind.index() >= index.numberOfValues() || index.opcodeOf(behind) != Opcode::Call)
        return false;
    BodyBlock block = index.block(walker.inputBlockOf(read.id()));
    std::optional<FirstStepOfIterator> step = findFirstStepOfIterator(walker, read, std::min(block.endOfValues().index(), block.firstValue().index() + walker.endOfStretchBeingWalked()));
    if (!step)
        return false;
    ValueID valueID = step->value;
    BodyValue call = index.value(step->call);
    bool isAsMadeForGood = false;
    std::optional<ClassSteppedUnseen> stepped = findCollectionThatStepsAreTakenOff(walker, read, call, isAsMadeForGood);
    if (!stepped || stepped->contents == ContentsStepped::List)
        return false;

    uint32_t place = read.id().index() - block.firstValue().index();
    uint32_t placeBehindValue = valueID.index() - block.firstValue().index() + 1;
    BuilderOfIteration builder(walker, walker.originOf(call), IteratorKind::Sync, WordingOfIteration::Source);
    auto takeAsWritten = [&](Vector<Reference, 4>& given) {
        walker.walkStretch(block.id(), place, placeBehindValue);
        if (!walker.isUnreachable() && !walker.hasFailed())
            given.append(walker.referenceOf(valueID));
    };
    Vector<Reference, 4> taken = takeStepsOffTables(walker, builder, read, call, *stepped, isAsMadeForGood, 1, takeAsWritten);
    if (!walker.isUnreachable())
        walker.bind(valueID, taken[0]);
    walker.walkStretch(block.id(), placeBehindValue, placeBehindValue);
    return true;
}

static ValueID findStepAtHead(const BodyIndex& index, ValueID open, BlockID head)
{
    for (BodyValue operation : index.block(head).values()) {
        if (operation.opcode() == Opcode::IteratorResume && operation.operand(0) == open)
            return operation.id();
    }
    return { };
}

static bool loopOnlyReadsSteps(const BodyIndex& index, ValueID open, BlockID head)
{
    static constexpr OpcodeSet looks = allRelations | allEqualities | OpcodeSet { Opcode::ToBoolean, Opcode::PropGet };
    ValueID step = findStepAtHead(index, open, head);
    ValueID piece;
    for (BodyValue operation : index.block(head).values()) {
        if (step && operation.opcode() == Opcode::ResultAt && operation.as<Opcode::ResultAt>().resultsOperand() == step && operation.as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Value))
            piece = operation.id();
    }
    return piece && std::ranges::all_of(index.membersOf(index.loopOf(head)), [&](BlockID member) {
        return std::ranges::all_of(index.block(member).values(), [&](BodyValue operation) {
            if (opcodesThatStateTypes.contains(operation.opcode()) || looks.contains(operation.opcode()))
                return true;
            bool isKept = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                isKept |= operand && index.stripCasts(operand) == piece && !(operation.opcode() == Opcode::Call && operand == operation.thisValueOperand());
            });
            return !isKept;
        });
    });
}

static bool openLoopOverSpeculatedStringParts(Walker& walker, const BodyValue& read)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockOf(read.id()));
    ValueID callID = index.onlyUsingValue(read.id());
    if (!callID || index.opcodeOf(callID) != Opcode::Call || index.blockOf(callID) != block.id())
        return false;
    BodyValue call = index.value(callID);
    std::span<const ValueID> arguments = call.as<Opcode::Call>().argumentsOperands();
    ValueID receiverID = read.objectOperand();
    if (call.calleeOperand() != read.id() || call.thisValueOperand() != receiverID || arguments.size() != 1 || index.isParameter(arguments[0]))
        return false;
    const bool takesMatches = index.opcodeOf(arguments[0]) == Opcode::NewRegExp;
    if (!takesMatches && (index.opcodeOf(arguments[0]) != Opcode::String || walker.program().builtinNameOf(read.key()) != BuiltinNames::split))
        return false;
    ValueID openID = onlyTakerBehindCasts(index, callID);
    if (!openID || index.isParameter(openID) || index.blockOf(openID) != block.id() || index.opcodeOf(openID) != Opcode::IteratorOpen)
        return false;
    for (uint32_t number = read.id().index() + 1; number < openID.index(); ++number) {
        ValueID between(number);
        bool isOnlyArgument = between == arguments[0] && index.onlyUsingValue(between) == callID;
        if (between != callID && !isOnlyArgument && !opcodesThatStateTypes.contains(index.opcodeOf(between)))
            return false;
    }
    if ((takesMatches && !namesMatchAll(walker, read.key())) || walker.program().admitsReplacedBuiltins || !walker.has(Mechanism::BuiltinCalls) || !isArrayIteratorProtocolIntact(walker) || !isArrayPrototypeChainSane(walker))
        return false;
    BodyValue open = index.value(openID);
    auto opened = open.as<Opcode::IteratorOpen>();
    if (opened.isDestructuring() || opened.isDelegating() || opened.iteratorKind() != IteratorKind::Sync || index.stripCasts(opened.iterableOperand()) != callID)
        return false;
    StringID separator = takesMatches ? StringID() : index.value(arguments[0]).as<Opcode::String>().string();
    if (takesMatches) {
        auto written = index.value(arguments[0]).as<Opcode::NewRegExp>();
        if (arguments[0].index() < read.id().index() || walker.program().at(walker.unit().function).isCoroutine() || !matchesAreSteppedByExec(walker, written.pattern(), written.regExpFlags()))
            return false;
    } else if (separator == walker.program().strings.emptyString())
        return false;

    Reference receiver = walker.referenceOf(receiverID);
    TypeArena& types = walker.types();
    if (!receiver || !receiver.isValue() || types.isSubset(walker.typeOf(receiver), TypeTable::string) || !types.overlap(walker.typeOf(receiver), TypeTable::string))
        return false;
    TypeID expected = findExpectation(walker, receiverID).type;
    expected = expected ? types.withoutNullish(expected) : TypeID();
    if (expected && !isNeverType(expected) && !types.isSubset(expected, TypeTable::string))
        return false;
    WayOfIteration way = findWayOfIteration(index, openID);
    if (!way.canBeFollowed || !way.headOfLoopThatSteps || !way.hasSingleStepAtHeader || !way.canSkipDoneChecks || way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep)
        return false;

    uint32_t place = read.id().index() - block.firstValue().index();
    uint32_t placeOfOpen = openID.index() - block.firstValue().index();
    Reference subject;
    std::optional<uint32_t> placeOfPattern;
    if (takesMatches) {
        placeOfPattern = arguments[0].index() - block.firstValue().index();
        walker.walkStretch(block.id(), *placeOfPattern, *placeOfPattern + 1);
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
        std::optional<RegExpWrittenForCall> regExp = findRegExpWrittenForCall(walker, call, arguments[0]);
        if (!regExp) {
            walker.walkStretch(block.id(), place, placeOfOpen + 1);
            return true;
        }
        subject = regExp->object;
    } else {
        Draft literal(Opcode::String);
        literal.setString(separator);
        subject = walker.constant(WTF::move(literal));
    }
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    builder.setIteratorCall(block.id(), place, placeOfOpen, opened.iterableOperand(), placeOfPattern);
    Walker::IndexedIteration iteration = builder.openByIndexOrProtocol(receiver, ClassSteppedUnseen { BuiltinClasses::String, ContentsStepped::Text }, false, true);
    iteration.subject = subject;
    iteration.otherForm = takesMatches ? Walker::IndexedIteration::OtherForm::MatchesOfRegExp : Walker::IndexedIteration::OtherForm::PiecesOfSplit;
    iteration.inputHead = way.headOfLoopThatSteps;
    iteration.isDeadAfterLoop = takesMatches ? loopTakesMatchesApart(walker, findStepAtHead(index, openID, way.headOfLoopThatSteps)) : loopOnlyReadsSteps(index, openID, way.headOfLoopThatSteps);
    walker.recordSpeculation();
    walker.count(takesMatches ? StatisticID::MatchIterationsInlined : StatisticID::SplitIterationsInlined);
    if (!walker.isUnreachable())
        walker.bind(openID, walker.addIndexedIteration(WTF::move(iteration)));
    walker.walkStretch(block.id(), placeOfOpen + 1, placeOfOpen + 1);
    return true;
}

bool openLoopOverIteratorThatEngineMakesOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || walker.walksStretchInsideAnother() || walker.isUnreachable())
        return false;
    if (openLoopOverSpeculatedStringParts(walker, read))
        return true;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(walker.inputBlockOf(read.id()));
    ValueID callID(read.id().index() + 1);
    if (callID.index() >= block.endOfValues().index() || index.opcodeOf(callID) != Opcode::Call || index.onlyUsingValue(read.id()) != callID)
        return false;
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
    if (open.as<Opcode::IteratorOpen>().isDestructuring())
        return takePatternOffCollection(walker, read, call, open);
    const bool delegates = open.as<Opcode::IteratorOpen>().isDelegating();
    if (index.stripCasts(open.as<Opcode::IteratorOpen>().iterableOperand()) != callID || open.as<Opcode::IteratorOpen>().iteratorKind() != IteratorKind::Sync)
        return false;
    BuiltinName name = walker.program().builtinNameOf(read.key());
    WayOfIteration way = name ? findWayOfIteration(index, openID) : WayOfIteration();
    if (!way.canBeFollowed || (delegates && (!way.hasSingleStepAtHeader || !nobodyThrowsIntoIterators(walker))))
        return false;
    way.canSkipDoneChecks |= delegates;

    Reference receiver = walker.referenceOf(receiverID);
    std::optional<ClassSteppedUnseen> proven = findClassSteppedUnseen(walker, receiver);
    bool isAsMadeForGood = proven && receiverStaysAsMadeByLink(walker, call);
    std::optional<ClassSteppedUnseen> tested = isAsMadeForGood ? proven : findClassToTestFor(walker, receiverID, receiver, !!way.headOfLoopThatSteps, read.key());
    if (!isAsMadeForGood && way.headOfLoopThatSteps && !(tested && tested->subclass) && (name == BuiltinNames::values || name == BuiltinNames::keys) && !isReadUnderIndexesInLoop(index, way.headOfLoopThatSteps, receiverID)) {
        std::optional<ClassSteppedUnseen> maps = orMapToTestFor(walker, receiverID, receiver, tested);
        bool bothMayCome = tested && maps && tested->contents == ContentsStepped::List && maps->contents == ContentsStepped::Map && !walker.program().admitsReplacedBuiltins;
        if (bothMayCome && !delegates)
            tested->mapMayComeInstead = true;
        else
            tested = maps;
    }
    if (!tested || (tested->contents != ContentsStepped::Map && tested->contents != ContentsStepped::Set && tested->contents != ContentsStepped::List))
        return false;
    std::optional<JSC::IterationKind> kind = kindOfIterationThatNobodySees(walker, tested->builtinClass, false, name);
    if (!kind && !isAsMadeForGood && walker.program().admitsReplacedBuiltins) {
        BuiltinID methodThatOpens;
        kind = kindOfIterationThatNobodySees(walker, tested->builtinClass, false, name, MembersBehindWatch::AreAsBuilt, &methodThatOpens);
        restOnWatchAboveInstances(walker, *tested, methodThatOpens, name);
    }
    if (!kind)
        return false;
    tested->kind = *kind;
    if (tested->mapMayComeInstead && kindOfIterationThatNobodySees(walker, BuiltinClasses::Map, false, name) != kind)
        tested->mapMayComeInstead = false;

    uint32_t place = read.id().index() - block.firstValue().index();
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, delegates ? WordingOfIteration::Delegation : WordingOfIteration::Source);
    uint32_t placeOfOpen = openID.index() - block.firstValue().index();
    Walker::IndexedIteration iteration;
    if (isAsMadeForGood) {
        holdLinksWordAgainstObjectAsMade(walker, receiver, walker.originOf(open));
        iteration.list = receiver;
        iteration.builtinClass = tested->builtinClass;
        iteration.contents = tested->contents;
        iteration.kind = tested->kind;
        iteration.position = builder.integer(0);
        iteration.table = builder.tableThatFirstStepBeginsIn(receiver, tested->contents);
        iteration.isAsMadeForGood = true;
        iteration.canSkipDoneChecks = way.canSkipDoneChecks;
    } else {
        builder.setIteratorCall(block.id(), place, placeOfOpen, open.as<Opcode::IteratorOpen>().iterableOperand());
        iteration = builder.openByIndexOrProtocol(receiver, *tested, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.canSkipDoneChecks);
        iteration.isEntryPairElided = !delegates && canElideEntryPair(walker, index, iteration, openID);
        walker.recordSpeculation();
    }
    iteration.isDelegating = delegates;
    iteration.inputHead = way.headOfLoopThatSteps;
    if (iteration.contents == ContentsStepped::List && !delegates)
        iteration.isDeadAfterLoop = !iteration.isProtocolOnly && way.hasSingleStepAtHeader && (!iteration.hasProtocolFallback || iteration.state || iteration.canSkipDoneChecks);
    if (const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), callID)) {
        if (iteration.table && iteration.contents != ContentsStepped::List) {
            iteration.keysProven = contents->key;
            iteration.valuesProven = contents->value;
        } else if (iteration.contents == ContentsStepped::List && contents->value)
            iteration.elementsProven = walker.types().withPrimitives(contents->value, Primitive::Undefined);
    }
    if (!walker.isUnreachable())
        walker.bind(openID, walker.addIndexedIteration(WTF::move(iteration)));
    walker.walkStretch(block.id(), placeOfOpen + 1, placeOfOpen + 1);
    return true;
}

bool patternMayTakeListApartInCleanTurn(Walker& walker, const BodyValue& open)
{
    return isOpenThatMayGoByIndex(open) && isArrayIteratorProtocolIntact(walker) && isArrayPrototypeChainSane(walker) && stepsOfPatternStandInRow(walker.currentBodyIndex(), open)
        && isDestructuredArrayLikelyNumeric(walker, open, walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand()));
}

static bool returnsOnlyArrayLiterals(const Walker& walker, FunctionID function)
{
    const BodyIndex* body = walker.facts().bodies().find(function);
    if (!body || !walker.program().at(function).hasBody())
        return false;
    bool returnsSomething = false;
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyTerminator end = body->terminator(BlockID(number));
        if (end.kind() == TerminatorKind::ReturnCall)
            return false;
        if (end.kind() != TerminatorKind::Return)
            continue;
        ValueID returned = body->stripCasts(end.operand(0));
        if (!returned || body->isParameter(returned) || body->opcodeOf(returned) != Opcode::NewArray)
            return false;
        returnsSomething = true;
    }
    return returnsSomething;
}

bool destructuresReturnedLiteral(const Walker& walker, const BodyIndex& index, ValueID open)
{
    if (index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen || !isOpenThatMayGoByIndex(index.value(open)) || !stepsOfPatternStandInRow(index, index.value(open)))
        return false;
    ValueID call = index.stripCasts(index.value(open).as<Opcode::IteratorOpen>().iterableOperand());
    if (!call || index.isParameter(call) || index.opcodeOf(call) != Opcode::Call || index.onlyUsingValue(call) != open)
        return false;
    FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call);
    constexpr OptionSet<FunctionFlags> returnsAnotherObject { FunctionFlags::IsAsync, FunctionFlags::IsGenerator };
    return callee && !walker.program().at(callee).hasAny(returnsAnotherObject) && returnsOnlyArrayLiterals(walker, callee);
}

bool spreadAppendsElementsOfListAsMade(Walker& walker, ValueID spreadValue)
{
    Reference iterable = walker.referenceBoundTo(spreadValue);
    return iterable && iterable.isValue() && isProvenArrayIterable(walker, iterable) && isKnownPristine(walker, iterable);
}

const BitVector& Walker::listsThatCallsAloneTakeInCurrentBody()
{
    const BodyIndex& index = currentBodyIndex();
    return m_listsThatCallsAloneTake.ensure(&index, [&] {
        static constexpr OpcodeSet callsThatTakeList { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
        static constexpr OpcodeSet appends { Opcode::AppendElement, Opcode::AppendHole, Opcode::AppendSpread };
        BitVector taken(index.numberOfValues());
        if (!index.holdsAnyOf(callsThatTakeList))
            return taken;
        BitVector namedOtherwise(index.numberOfValues());
        ValueID filledOrTaken;
        uint32_t timesNamed = 0;
        auto look = [&](ValueID operand, uint32_t) {
            if (operand != filledOrTaken || timesNamed++)
                namedOtherwise.quickSet(operand.index());
        };
        for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
            BodyBlock block = index.block(BlockID(number));
            for (BodyValue operation : block.values()) {
                bool takesList = callsThatTakeList.contains(operation.opcode());
                filledOrTaken = takesList ? operation.listOperand() : appends.contains(operation.opcode()) ? operation.operand(0) : ValueID();
                timesNamed = 0;
                operation.forEachOperand(look);
                if (takesList)
                    taken.quickSet(filledOrTaken.index());
            }
            filledOrTaken = { };
            block.terminator().forEachOperand(look);
            for (BodyTarget target : block.terminator().targets()) {
                for (ValueID argument : target.arguments()) {
                    if (argument)
                        namedOtherwise.quickSet(argument.index());
                }
            }
        }
        taken.exclude(namedOtherwise);
        return taken;
    }).iterator->value;
}

static bool appendSpreadOfList(Walker& walker, const BodyValue& spread)
{
    Reference array = walker.referenceOf(spread.as<Opcode::AppendSpread>().arrayOperand());
    Reference iterable = walker.referenceOf(spread.as<Opcode::AppendSpread>().iterableOperand());
    if (array && iterable && !walker.copiedCollections().empty() && replaceSpreadWithCopiedCollection(walker, spread, array, iterable))
        return true;
    if (array && isArrayIteratorProtocolIntact(walker)) {
        if (Reference list = listStandingForFreshArguments(walker, iterable))
            iterable = list;
        else if (std::optional<Vector<Reference, 8>> elements = elementsOfFreshArguments(walker, iterable)) {
            BuilderOfIteration builder(walker, walker.originOf(spread), IteratorKind::Sync, WordingOfIteration::Spread);
            for (Reference element : *elements)
                builder.appendElement(array, element);
            return true;
        }
    }
    BuiltinClassID classAsMadeForGood = linksWordCanBeHeldAgainst(walker, iterable) ? classOfIterableThatStaysAsMadeByLink(walker, spread, spread.as<Opcode::AppendSpread>().iterableOperand()) : BuiltinClassID();
    bool isListAsMadeForGood = classAsMadeForGood == BuiltinClasses::Array && isArrayPrototypeChainSane(walker);
    bool isSurelyAsMade = isProvenArrayIterable(walker, iterable) && (isListAsMadeForGood || isKnownPristine(walker, iterable));
    std::optional<ClassSteppedUnseen> tested = !array || isSurelyAsMade ? std::nullopt : findClassToTestFor(walker, spread.as<Opcode::AppendSpread>().iterableOperand(), iterable, false, keyOfIterator(walker));
    if (!array || !iterable)
        return false;
    if (std::optional<ClassSteppedUnseen> proven = !tested && !isSurelyAsMade && classAsMadeForGood ? findClassSteppedUnseen(walker, iterable) : std::nullopt; proven && proven->builtinClass == classAsMadeForGood)
        tested = proven;
    BuilderOfIteration builder(walker, walker.originOf(spread), IteratorKind::Sync, WordingOfIteration::Spread);
    if (isSurelyAsMade)
        walker.verifyPristineObject(iterable);
    if (isSurelyAsMade && isListAsMadeForGood)
        holdLinksWordAgainstObjectAsMade(walker, iterable, walker.originOf(spread));
    if (isSurelyAsMade)
        builder.appendElementsOf(array, iterable, [&] {
            return walker.listsThatCallsAloneTakeInCurrentBody().quickGet(spread.as<Opcode::AppendSpread>().arrayOperand().index());
        });
    else if (tested && BuilderOfIteration::isListedByAppendMembers(*tested)) {
        if (classAsMadeForGood == tested->builtinClass) {
            holdLinksWordAgainstObjectAsMade(walker, iterable, walker.originOf(spread));
            builder.takeInstanceForAsMade(iterable);
        }
        builder.appendMembersOrAllOutOfLine(array, iterable, *tested);
        walker.recordSpeculation();
    } else if (tested && tested->subclass && tested->contents == ContentsStepped::List) {
        builder.appendElementsOfSubclass(array, iterable, *tested, [&] {
            return walker.listsThatCallsAloneTakeInCurrentBody().quickGet(spread.as<Opcode::AppendSpread>().arrayOperand().index());
        });
        walker.recordSpeculation();
    } else if (tested && !tested->subclass && tested->builtinClass == BuiltinClasses::Array && tested->contents == ContentsStepped::List && builder.walkCallApartFromWhatIsNoList(spread, iterable, *tested))
        walker.recordSpeculation();
    else
    {
        TypeArena& types = walker.types();
        TypeID proven = iterable.isValue() ? walker.typeOf(iterable) : TypeID();
        bool isGenerator = proven && types.isSubset(proven, types.builtinClass(BuiltinClasses::Generator));
        bool isOfProgram = proven && types.at(proven).hasOnlyLayouts() && !builtinClassBelowType(walker, proven);
        bool isInLibrary = (isGenerator && builder.tryAppendGeneratorInLibrary(array, iterable)) || (isOfProgram && builder.tryAppendObjectOfProgramInLibrary(array, iterable));
        if (!isInLibrary)
            builder.appendAllOutOfLine(array, iterable);
    }
    return true;
}

std::optional<Draft> draftOfCallWithListWrittenOut(Walker& walker, const BodyValue& call)
{
    Draft written = walker.draftOf(call);
    std::optional<Vector<Reference, 8>> arguments = elementsOfFreshList(walker, written.listOperand());
    if (!arguments)
        return std::nullopt;
    walker.checkWhatIsInSightOfList(written.listOperand(), written.origin());

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

bool stepsViewByIndexAlone(Walker& walker, const BodyIndex& index, const BodyValue& step)
{
    if (step.opcode() != Opcode::IteratorResume || step.as<Opcode::IteratorResume>().resumeMode() != ResumeMode::Next || !walker.has(Mechanism::TypedOperations) || &index != &walker.currentBodyIndex())
        return false;
    ValueID open = step.operand(0);
    Reference state = open && !index.isParameter(open) && index.opcodeOf(open) == Opcode::IteratorOpen ? walker.referenceBoundTo(open) : Reference();
    return state && state.isVirtual() && walker.indexedIteration(state).isIndexedTypedArrayIteration() && walker.indexedIteration(state).position;
}

bool stepsTextByIndexAlone(Walker& walker, const BodyIndex& index, const BodyValue& step)
{
    if (step.opcode() != Opcode::IteratorResume || step.as<Opcode::IteratorResume>().resumeMode() != ResumeMode::Next || !walker.has(Mechanism::TypedOperations) || &index != &walker.currentBodyIndex())
        return false;
    ValueID open = step.operand(0);
    Reference state = open && !index.isParameter(open) && index.opcodeOf(open) == Opcode::IteratorOpen ? walker.referenceBoundTo(open) : Reference();
    return state && state.isVirtual() && walker.indexedIteration(state).isIndexedStringIteration() && walker.indexedIteration(state).position;
}

const Walker::IndexedIteration* findIndexedArrayIteration(Walker& walker, const BodyIndex& index, const BodyValue& step, bool allowsAnyIterationKind)
{
    if (step.opcode() != Opcode::IteratorResume || step.as<Opcode::IteratorResume>().resumeMode() != ResumeMode::Next || !walker.has(Mechanism::TypedOperations) || &index != &walker.currentBodyIndex())
        return nullptr;
    ValueID open = step.operand(0);
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return nullptr;
    Reference state = walker.referenceBoundTo(open);
    if (!state || !state.isVirtual())
        return nullptr;
    const Walker::IndexedIteration& iteration = walker.indexedIteration(state);
    if (!iteration.isIndexedArrayIteration() || (!allowsAnyIterationKind && iteration.kind != JSC::IterationKind::Values) || !iteration.list || !iteration.position)
        return nullptr;
    return &iteration;
}

const Walker::IndexedIteration* findDestructuredEntriesIteration(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    if (open.opcode() != Opcode::IteratorOpen || !open.as<Opcode::IteratorOpen>().isDestructuring())
        return nullptr;
    ValueID pair = index.stripCasts(open.as<Opcode::IteratorOpen>().iterableOperand());
    if (!pair || index.isParameter(pair) || index.opcodeOf(pair) != Opcode::ResultAt)
        return nullptr;
    ValueID step = index.value(pair).operand(0);
    if (index.isParameter(step))
        return nullptr;
    const Walker::IndexedIteration* stepped = findIndexedArrayIteration(walker, index, index.value(step), true);
    return stepped && stepped->isEntryPairElided ? stepped : nullptr;
}

ValueID findIterableOfPosition(Walker& walker, const BodyIndex& index, ValueID key)
{
    ValueID given = key ? index.stripCasts(key) : ValueID();
    if (!given || index.isParameter(given) || index.opcodeOf(given) != Opcode::ResultAt || index.value(given).as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value))
        return { };
    auto stepOf = [&](ValueID result) -> ValueID {
        ValueID stepped = index.value(result).as<Opcode::ResultAt>().resultsOperand();
        return stepped && !index.isParameter(stepped) && index.opcodeOf(stepped) == Opcode::IteratorResume ? stepped : ValueID();
    };
    auto iterableOf = [&](ValueID step) -> ValueID {
        ValueID opened = index.value(step).as<Opcode::IteratorResume>().iteratorOperand();
        return opened && !index.isParameter(opened) && index.opcodeOf(opened) == Opcode::IteratorOpen ? index.stripCasts(index.value(opened).as<Opcode::IteratorOpen>().iterableOperand()) : ValueID();
    };
    ValueID step = stepOf(given);
    if (!step)
        return { };
    if (const Walker::IndexedIteration* iteration = findIndexedArrayIteration(walker, index, index.value(step), true))
        return iteration->kind == JSC::IterationKind::Keys ? iterableOf(step) : ValueID();
    auto first = index.value(step).as<Opcode::IteratorResume>();
    ValueID opened = first.iteratorOperand();
    if (!first.stepIndex() || *first.stepIndex() || !opened || index.isParameter(opened) || !findDestructuredEntriesIteration(walker, index, index.value(opened)))
        return { };
    ValueID pair = iterableOf(step);
    ValueID stepOfPair = pair ? stepOf(pair) : ValueID();
    return stepOfPair ? iterableOf(stepOfPair) : ValueID();
}

constinit const OpcodeSet opcodesThatBuildTypedIterationTakes = opcodesOfTypedIteration;
static_assert(opcodesThatBuildTypedIterationTakesInChains == opcodesOfTypedIteration);

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
        return openTypedIteration(walker, value) || openAsyncIterationInValues(walker, value) || openDelegationInValues(walker, value);
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
        walker.leaveAnythingAt(listed);
    Draft written = walker.draftOf(value);
    bool isAsync = walker.indexedIteration(state).isAsync;
    BuilderOfIteration builder(walker, written.origin(), isAsync ? IteratorKind::Async : IteratorKind::Sync, WordingOfIteration::Source, isAsync ? written.heldOperands() : std::span<const Reference>());
    builder.standFor(value);
    if (opcode == Opcode::IteratorResume)
        builder.expectUnderNext(walker.facts().held().calleeAt(walker.currentFunction(), value.id()));
    if (ValueID opened = opcode == Opcode::IteratorResume ? value.operand(0) : ValueID(); opened && !walker.currentBodyIndex().isParameter(opened) && walker.currentBodyIndex().opcodeOf(opened) == Opcode::IteratorOpen)
        builder.expectWhatIsWrittenUnderNextOfIteratorOf(walker.currentBodyIndex().value(opened).iterableOperand());
    if (opcode == Opcode::IteratorClose) {
        Walker::IndexedIteration closed = walker.indexedIteration(state);
        builder.closeByIndex(closed, value.as<Opcode::IteratorClose>().completion());
        walker.indexedIteration(state) = WTF::move(closed);
        return true;
    }
    if (walker.indexedIteration(state).isDelegating) {
        Walker::IndexedIteration stepped = walker.indexedIteration(state);
        Reference results;
        if (written.resumeMode() != ResumeMode::Next)
            results = builder.leaveDelegate(stepped, written.resumeMode(), written.sentOperand(), written.readsValueWhenDone());
        else {
            builder.sendToDelegate(written.sentOperand());
            if (builder.tryThreadTableStep(state, value) || builder.tryThreadIndexedStep(state, value))
                return true;
            results = builder.resumeByIndex(stepped, std::nullopt);
            walker.indexedIteration(state) = WTF::move(stepped);
        }
        if (results)
            walker.bind(value.id(), results);
        return true;
    }

    if (Walker::IndexedIteration& stepped = walker.indexedIteration(state); stepped.table && !stepped.keysProven && !stepped.valuesProven) {
        if (const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), value.operand(0))) {
            stepped.keysProven = contents->key;
            stepped.valuesProven = contents->value;
        }
    }
    if (Walker::IndexedIteration& stepped = walker.indexedIteration(state); stepped.contents == ContentsStepped::List && stepped.kind == JSC::IterationKind::Values)
        stepped.elementsProven = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), value.id());
    if (!isAsync && !isExpectedToStepGenerator(walker, value.operand(0)))
        builder.expectNoGenerator();
    if (!isAsync && isExpectedToBeSteppedUnseen(walker, value.operand(0)))
        builder.expectNoStepByProtocol();
    if (builder.tryThreadTableStep(state, value) || builder.tryThreadIndexedStep(state, value) || (!isAsync && (builder.tryThreadStepWithFallback(state, value) || builder.tryThreadKnownNextStep(state, value))))
        return true;
    Reference results;
    if (std::optional<uint32_t> position = walker.indexedIteration(state).literal ? placeOfStepInPattern(walker.currentBodyIndex(), value) : std::nullopt) {
        StepOfListLiteral step = stepAtPlaceOfLiteral(walker.currentBodyIndex(), walker.indexedIteration(state).literal, *position);
        results = builder.resultsOf(builder.booleanValue(step.isDone), step.element ? builder.elementOfLiteral(walker.indexedIteration(state), *position) : builder.undefinedValue());
    } else if (Walker::IndexedIteration& taken = walker.indexedIteration(state); taken.hasPrecomputedSteps)
        results = builder.resumeByIndex(taken, value.as<Opcode::IteratorResume>().stepIndex());
    else {
        Walker::IndexedIteration stepped = walker.indexedIteration(state);
        results = builder.resumeByIndex(stepped, value.as<Opcode::IteratorResume>().stepIndex());
        walker.indexedIteration(state) = WTF::move(stepped);
    }
    if (results)
        walker.bind(value.id(), results);
    return true;
}

bool buildIterationThatAsksItsListItselfOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::ResultAt)
        return handOnResultOfStep(walker, value);
    if (opcode == Opcode::IteratorOpen) {
        if (!isOpenThatMayGoByIndex(value) || !isArrayIteratorProtocolIntact(walker))
            return false;
        WayOfIteration way = findWayOfIteration(walker.currentBodyIndex(), value.id());
        if (!way.canBeFollowed || !way.hasSingleStepAtHeader)
            return false;
        Reference iterable = walker.referenceOf(value.as<Opcode::IteratorOpen>().iterableOperand());
        if (!iterable || !iterable.isValue() || !walker.types().overlap(walker.typeOf(iterable), walker.types().builtinClass(BuiltinClasses::Array)))
            return false;
        BuilderOfIteration builder(walker, walker.originOf(value), IteratorKind::Sync, WordingOfIteration::Source);
        ClassSteppedUnseen lists { BuiltinClasses::Array };
        lists.isListForWantOfWord = true;
        Walker::IndexedIteration iteration = builder.openByIndexOrProtocol(iterable, lists, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.canSkipDoneChecks);
        iteration.inputHead = way.headOfLoopThatSteps;
        if (!walker.isUnreachable())
            walker.bind(value.id(), walker.addIndexedIteration(WTF::move(iteration)));
        return true;
    }

    Reference state = walker.referenceOf(value.operand(0));
    if (!state || !state.isVirtual())
        return false;
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), value.id()))
        walker.leaveAnythingAt(listed);
    BuilderOfIteration builder(walker, walker.originOf(value), IteratorKind::Sync, WordingOfIteration::Source);
    Walker::IndexedIteration stepped = walker.indexedIteration(state);
    if (opcode == Opcode::IteratorClose) {
        builder.closeByIndex(stepped, value.as<Opcode::IteratorClose>().completion());
        walker.indexedIteration(state) = WTF::move(stepped);
        return true;
    }
    Reference results = builder.resumeByIndex(stepped, value.as<Opcode::IteratorResume>().stepIndex());
    walker.indexedIteration(state) = WTF::move(stepped);
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
        if (std::optional<ClassSteppedUnseen> lists = findListsThatAsyncIterationStepsByIndex(walker, value))
            builder.testForListSteppedByIndex(*lists);
        if (Reference state = builder.openState(written.iterableOperand()))
            walker.bind(value.id(), state);
    } else if (opcode == Opcode::IteratorResume) {
        BuilderOfIteration builder(walker, written.origin(), written.iteratorKind(), WordingOfIteration::Source, written.heldOperands());
        if (written.iteratorKind() == IteratorKind::Sync && !isExpectedToStepGenerator(walker, value.operand(0)))
            builder.expectNoGenerator();
        if (written.iteratorKind() == IteratorKind::Sync && isStateOfDelegation(walker.currentBodyIndex(), value.operand(0)))
            builder.stepDelegate();
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

    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), value.id()))
        walker.leaveAnythingAt(listed);

    walker.state().invalidateHeap();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
