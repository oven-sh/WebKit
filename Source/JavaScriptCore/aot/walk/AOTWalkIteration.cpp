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
#include "AOTSweep.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkKnown.h"
#include "AOTWalkListReadsInLoops.h"
#include "AOTWalkTextBuilders.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

Reference buildAwaitBySavingFrame(Walker&, Reference awaited, std::span<const Reference> held, Origin);
bool canWaitForStepOfAsyncGenerator(Walker&);
bool canCallCompilerHelper(Walker&, FunctionID helper);
Reference callCompilerHelper(Walker&, FunctionID helper, std::span<const Reference> arguments, Origin);
Reference buildStepOfAsyncGenerator(Walker&, Reference generator, Reference sent, std::span<const Reference> held, BuiltinID awaitedCallee, Origin);

OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

static constexpr OpcodeSet opcodesReplacedByIteration { Opcode::IteratorOpen, Opcode::IteratorResume, Opcode::IteratorClose, Opcode::AppendSpread };
static constexpr OpcodeSet appendsToListsInIteration { Opcode::AppendElement, Opcode::AppendHole };
static constexpr OpcodeSet callsOfListsInIteration { Opcode::CallList, Opcode::ConstructList, Opcode::SuperConstructList };
static constexpr OpcodeSet opcodesOfTypedIteration = opcodesReplacedByIteration | appendsToListsInIteration | callsOfListsInIteration | OpcodeSet { Opcode::ResultAt };

static std::optional<JSC::IterationKind> kindOfIterationThatNobodySees(const Walker& walker, BuiltinClassID builtinClass, bool isPrimitive = false, BuiltinName under = { }, MembersBehindWatch aboveInstances = MembersBehindWatch::AreOpen, BuiltinID* methodThatOpens = nullptr)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!isPrimitive && !table.mayHavePristineInstances(builtinClass))
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
    if (!iteratorClass)
        return std::nullopt;
    BuiltinID prototypeOfIterators = table.at(iteratorClass).prototype;
    std::optional<BuiltinID> next = findMemberAboveForIteration(walker, prototypeOfIterators, BuiltinNames::next);
    std::optional<BuiltinID> returnMethod = findMemberAboveForIteration(walker, prototypeOfIterators, BuiltinNames::returnKeyword);
    if (!next || !*next || !returnMethod || *returnMethod)
        return std::nullopt;
    return kind;
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
    std::optional<JSC::IterationKind> kindOfListAskedForBehind;
    bool restsOnWatchAboveInstances { false };
    BuiltinID methodAskedByItsByte;
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

static TypeID typeOfInstancesStepped(Walker& walker, BuiltinClassID builtinClass, ContentsStepped contents)
{
    return contents == ContentsStepped::Text ? TypeID(TypeTable::string) : walker.types().builtinClass(builtinClass);
}

static bool isProvenListInIteration(Walker& walker, Reference value)
{
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, value);
    return stepped && stepped->contents == ContentsStepped::List;
}

static constexpr uint32_t closedOperationsOfProtocol = 3;

static Walker::KindHeldUnderName findKindHeldUnderName(Walker& walker, PropertyKeyID name)
{
    using Kind = Walker::KindHeldUnderName;
    TypeArena& types = walker.types();
    TypeID held = typeHeldUnderName(walker, name);
    TypeID present = held ? types.withoutNullish(held) : TypeID();
    if (!present || isNeverType(present))
        return Kind::Nothing;
    Kind only = Kind::Nothing;
    for (auto [instances, kind] : { std::pair { types.builtinClass(BuiltinClasses::Array), Kind::List }, std::pair { types.builtinClass(BuiltinClasses::Map), Kind::Map }, std::pair { types.builtinClass(BuiltinClasses::Set), Kind::Set }, std::pair { TypeID(TypeTable::string), Kind::Text } }) {
        if (!types.overlap(present, instances))
            continue;
        if (only != Kind::Nothing)
            return Kind::Nothing;
        only = kind;
    }
    return only;
}

static Walker::KindHeldUnderName kindHeldUnderNameThatIsRead(Walker& walker, ValueID input)
{
    PropertyKeyID name = nameUnderWhichValueIsRead(walker, input);
    if (!name)
        return Walker::KindHeldUnderName::Nothing;
    Walker::KindHeldUnderName& kept = walker.kindHeldUnderNameKept(name);
    if (kept == Walker::KindHeldUnderName::NotAsked)
        kept = findKindHeldUnderName(walker, name);
    return kept;
}

static TypeID instancesOfKindHeldUnderName(Walker& walker, Walker::KindHeldUnderName kind)
{
    switch (kind) {
    case Walker::KindHeldUnderName::List:
        return walker.types().builtinClass(BuiltinClasses::Array);
    case Walker::KindHeldUnderName::Map:
        return walker.types().builtinClass(BuiltinClasses::Map);
    case Walker::KindHeldUnderName::Set:
        return walker.types().builtinClass(BuiltinClasses::Set);
    case Walker::KindHeldUnderName::Text:
        return TypeTable::string;
    case Walker::KindHeldUnderName::NotAsked:
    case Walker::KindHeldUnderName::Nothing:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<ClassSteppedUnseen> findClassHeldUnderNameThatIsRead(Walker& walker, ValueID input)
{
    TypeID instances = instancesOfKindHeldUnderName(walker, kindHeldUnderNameThatIsRead(walker, input));
    return instances ? findClassSteppedUnseen(walker, instances) : std::nullopt;
}

TypeID partHeldUnderNameThatLoopWalksUnseen(Walker& walker, ValueID input)
{
    Walker::KindHeldUnderName kind = kindHeldUnderNameThatIsRead(walker, input);
    TypeID instances = kind == Walker::KindHeldUnderName::List ? TypeID() : instancesOfKindHeldUnderName(walker, kind);
    return instances && findClassSteppedUnseen(walker, instances) ? walker.types().intersectionOf(typeHeldUnderName(walker, nameUnderWhichValueIsRead(walker, input)), instances) : TypeID();
}

static std::optional<ClassSteppedUnseen> findClassToTestFor(Walker& walker, ValueID input, Reference iterable, bool stepsStandInLoop = false)
{
    if (!iterable || !iterable.isValue())
        return std::nullopt;
    TypeArena& types = walker.types();
    bool isDeclared = true;
    MembersBehindWatch aboveInstances = walker.program().admitsReplacedBuiltins ? MembersBehindWatch::AreAsBuilt : MembersBehindWatch::AreOpen;
    std::optional<ClassSteppedUnseen> stepped = findClassSteppedUnseen(walker, iterable, aboveInstances);
    if (!stepped) {
        ExpectationOfValue expectation = findExpectation(walker, input);
        isDeclared = expectation.type && expectation.isDeclared;
        if (expectation.type)
            stepped = findClassSteppedUnseen(walker, types.withoutNullish(expectation.type), aboveInstances);
        else if ((stepped = findClassHeldUnderNameThatIsRead(walker, input))) {
            isDeclared = false;
            if (stepped->contents != ContentsStepped::List && listsAreIteratedByIndex(walker))
                stepped->kindOfListAskedForBehind = JSC::IterationKind::Values;
        }
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

namespace {

struct WayOfIteration {
    bool canBeFollowed { false };
    BlockID headOfLoopThatSteps;
    bool stepsStandUnderOwnCleanup { false };
    bool closesStandBelowOnlyStep { false };
    bool onlyStepStandsAtHeadOfLoop { false };
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
                if (operation.as<Opcode::ResultAt>().resultsOperand() != lastStep || (operation.as<Opcode::ResultAt>().index() != static_cast<uint32_t>(IteratorResumeResult::Value) && !index.isUnused(operation.id())))
                    return false;
                break;
            case Opcode::IteratorClose:
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
        if (!way.canBeFollowed || way.headOfLoopThatSteps != head.id() || !way.isOpenAtEveryStepAndClose)
            return std::nullopt;
        return ForOfByText { open, operation.id() };
    }
    return std::nullopt;
}

bool openOverListAsMadeGoesByIndex(Walker& walker, const BodyIndex& index, ValueID open)
{
    return walker.has(Mechanism::TypedOperations) && listsAreIteratedByIndex(walker) && isOpenThatMayGoByIndex(index.value(open)) && findWayOfIteration(index, open).canBeFollowed;
}

static ValueID findLiteralIteratedAlone(Walker& walker, const BodyIndex& index, const BodyValue& open)
{
    ValueID literal = open.as<Opcode::IteratorOpen>().iterableOperand();
    if (!walker.has(Mechanism::TypedOperations) || !isOpenThatMayGoByIndex(open) || index.isParameter(literal) || index.opcodeOf(literal) != Opcode::NewArray || index.onlyUsingValue(literal) != open.id())
        return { };
    if (index.blockOf(literal) != index.blockOf(open.id()) || !listsAreIteratedByIndex(walker))
        return { };
    if (walker.hasTextsInStringBuilders()) {
        for (ValueID element : index.value(literal).as<Opcode::NewArray>().elementsOperands()) {
            if (isNameOfTextInStringBuilder(walker, element))
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
    const Operation& made = code.at(rootOfCellInSweptCode(code, list.id()));
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
        if (mentions == BodyIndex::maxCountedUses || operandsMayRunCode(opcode) || !bound.alwaysCompletes() || !bound.writes.containsOnly(heapsWhoseWritesNobodySees))
            break;
        mentionsToCome.append(mentions);
    }
    return end;
}

namespace {

enum class WordingOfIteration : uint8_t { Source, Spread, Delegation };
enum class UseOfSyncIteration : uint8_t { Stepped, WrappedForAsyncIteration };
enum class ResultOfReturnInIteration : uint8_t { MustBeObject, IsIgnored };

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
    BuilderOfIteration(Walker& walker, Origin origin, IteratorKind kind, WordingOfIteration wording, std::span<const Reference> held = { })
        : m_walker(walker)
        , m_origin(origin)
        , m_kind(kind)
        , m_wording(wording)
        , m_held(held)
    {
        m_origin.isTailCall = false;
    }

    void standFor(const BodyValue& site) { m_site = &site; }
    void expectUnderNext(FunctionID function) { m_expectedUnderNext = function; }

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

    Walker::IterationByIndex openByIndexOrProtocol(Reference iterable, const ClassSteppedUnseen& stepped, bool keepsStateInMemory, bool isOpenAtEveryStepAndClose = false)
    {
        Walker::IterationByIndex iteration;
        iteration.isOpenAtEveryStepAndClose = !keepsStateInMemory && isOpenAtEveryStepAndClose;
        iteration.builtinClass = stepped.builtinClass;
        iteration.contents = stepped.contents;
        iteration.kind = stepped.kind;
        iteration.mayGoByProtocol = true;
        iteration.position = integer(0);

        if (!keepsStateInMemory)
            iteration.kindOfListAskedForBehind = stepped.kindOfListAskedForBehind;

        BlockID byProtocol = addBlock();
        BlockID isOfOtherKind = iteration.kindOfListAskedForBehind ? addBlock() : byProtocol;
        Reference instance = goOnWhereInstanceIsAsMade(iterable, stepped, isOfOtherKind);
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
            if (iteration.isOpenAtEveryStepAndClose && laysVerdictOnNext(iterable) && findHelper(IterationHelper::WayToStepIterator) && findHelper(IterationHelper::StepIteratorInWay)) {
                iteration.wayToStep = m_walker.addParameter(after, { }, Representation::JSValue);
                byIndex.append(integer(0));
            }
        }
        if (stepsThroughTable(stepped.contents)) {
            iteration.table = m_walker.addParameter(after, { }, Representation::JSValue);
            byIndex.append(table);
        }
        jumpTo(after, byIndex.span());

        if (iteration.kindOfListAskedForBehind) {
            continueIn(isOfOtherKind);
            byIndex[0] = goOnWhereInstanceIsAsMade(iterable, ClassSteppedUnseen { BuiltinClasses::Array }, byProtocol);
            if (iteration.table)
                byIndex.last() = undefined();
            jumpTo(after, byIndex.span());
        }

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
        } else {
            openedByProtocol.appendList({ opened.iterator, opened.next });
            if (iteration.wayToStep)
                openedByProtocol.append(callHelper(findHelper(IterationHelper::WayToStepIterator), { opened.iterator, opened.next }));
        }
        if (iteration.table)
            openedByProtocol.append(undefined());
        jumpTo(after, openedByProtocol.span());
        return iteration;
    }

    Walker::IterationByIndex openByProtocolAlone(Reference iterable)
    {
        Walker::IterationByIndex iteration;
        iteration.isOpenAtEveryStepAndClose = true;
        iteration.mayGoByProtocol = true;
        iteration.goesByProtocolAlone = true;
        iteration.isAsync = m_kind == IteratorKind::Async;
        iteration.position = integer(0);
        iteration.isSteppedByCallsAlone = !laysVerdictOnNext(iterable);
        bool asksWay = !iteration.isSteppedByCallsAlone && findHelper(IterationHelper::WayToStepIterator) && findHelper(IterationHelper::StepIteratorInWay);
        OpenedIteration opened = open(iterable);
        iteration.iterator = opened.iterator;
        iteration.next = opened.next;
        if (asksWay)
            iteration.wayToStep = callHelper(findHelper(IterationHelper::WayToStepIterator), { opened.iterator, opened.next });
        return iteration;
    }

    Reference tableThatFirstStepBeginsIn(Reference instance, ContentsStepped contents) { return beginByIndex(instance, contents); }

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

    Walker::IterationByIndex takeStepsByProtocolAtOpen(Reference iterable, uint32_t numberOfSteps)
    {
        Walker::IterationByIndex iteration;
        iteration.isTakenAtOpen = true;
        OpenedIteration opened = open(iterable);
        if (m_walker.isUnreachable())
            return iteration;
        BlockID after = addBlock();
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            iteration.takenAtOpen.append(m_walker.addParameter(after, { }, Representation::JSValue));
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
            Reference known = goOnWhereTypeTestPasses(opened.next, closures, byValue);
            m_walker.noteGuessOpened();
            m_walker.count(StatisticID::NextMethodsTestedForFunctionExpected);
            takeSteps(known);
            continueIn(byValue);
        }
        takeSteps(opened.next);
        continueIn(after);
        if (!m_walker.isUnreachable())
            m_walker.state().invalidateHeap();
        return iteration;
    }

    Walker::IterationByIndex takeStepsThroughTableAtOpen(Reference iterable, uint32_t numberOfSteps, const ClassSteppedUnseen& stepped)
    {
        BlockID outOfLine = addBlock(BlockFrequency::Unlikely);
        Walker::IterationByIndex inLine;
        inLine.list = goOnWhereInstanceIsAsMade(iterable, stepped, outOfLine);
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

        Walker::IterationByIndex iteration;
        iteration.isTakenAtOpen = true;
        BlockID after = addBlock();
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            iteration.takenAtOpen.append(m_walker.addParameter(after, { }, Representation::JSValue));
        jumpTo(after, taken.span());
        continueIn(outOfLine);
        taken.shrink(0);
        takeStepsOutOfLine(iterable, numberOfSteps, taken);
        jumpTo(after, taken.span());
        return iteration;
    }

    Reference resumeByIndex(Walker::IterationByIndex& iteration, std::optional<uint32_t> stepIndex)
    {
        if (iteration.isTakenAtOpen) {
            RELEASE_ASSERT(stepIndex && *stepIndex < iteration.takenAtOpen.size());
            return resultsOf(boolean(false), iteration.takenAtOpen[*stepIndex]);
        }
        if (iteration.holdsFlatEntries)
            return resumeThroughFlatEntries(iteration);
        if (iteration.goesByProtocolAlone) {
            BlockID stepped = addBlock();
            Reference isLast = m_walker.addParameter(stepped, { }, m_walker.formOfParameter(TypeTable::boolean, { }));
            Reference handedOut = m_walker.addParameter(stepped, { }, Representation::JSValue);
            stepByProtocolBesideIndex(iteration, stepped);
            return resultsOf(isLast, handedOut);
        }
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
                Reference value = handedOutAt(iteration, iteration.list, iteration.position, runsNoCode, false);
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
            BlockID isOfOtherKind = iteration.kindOfListAskedForBehind ? addBlock() : byProtocol;
            list = goOnWhereParameterHoldsInstance(iteration, isOfOtherKind);
            BlockID byIndex = m_walker.isUnreachable() ? BlockID() : m_walker.currentBlock();
            if (iteration.kindOfListAskedForBehind) {
                continueIn(isOfOtherKind);
                Walker::IterationByIndex throughList = iteration;
                throughList.builtinClass = BuiltinClasses::Array;
                throughList.contents = ContentsStepped::List;
                throughList.kind = *iteration.kindOfListAskedForBehind;
                Reference listBehind = goOnWhereParameterHoldsInstance(throughList, byProtocol);
                stepByIndex(throughList, listBehind, lengthOf(throughList, listBehind), false, after);
            }
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

    Reference handedOutAt(const Walker::IterationByIndex& iteration, Reference list, Reference position, bool runsNoCode, bool isInsideLength)
    {
        if (iteration.kind == JSC::IterationKind::Keys)
            return position;
        bool restsOnLinksWord = runsNoCode && !iteration.literal && !(isFreshInIteration(m_walker, iteration.list) && isListAsMade(m_walker, iteration.list));
        Reference element = elementAt(list, position, runsNoCode, isInsideLength, iteration.elementsProven, restsOnLinksWord);
        if (iteration.kind != JSC::IterationKind::Entries || m_walker.isUnreachable())
            return element;
        Draft pair = draft(Opcode::NewArray);
        pair.setContents(TypeTable::anything);
        pair.setElementsOperands(std::array { position, element });
        return build(WTF::move(pair));
    }

    bool stepListOnWaysApart(Reference state, const BodyValue& resume)
    {
        const Walker::IterationByIndex& record = m_walker.iterationByIndex(state);
        if (!record.goesThroughListByIndexAlone() || !record.head || !findWayOfIteration(m_walker.currentBodyIndex(), resume.operand(0)).onlyStepStandsAtHeadOfLoop)
            return false;
        Walker::IterationByIndex entered = record;
        const BodyIndex& index = m_walker.currentBodyIndex();
        BodyTerminator end = index.terminator(index.blockOf(resume.id()));
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        if (!m_walker.has(Mechanism::Folding) || !condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
            return false;
        auto asked = index.value(condition).as<Opcode::ResultAt>();
        if (asked.resultsOperand() != resume.id() || asked.index() != static_cast<uint32_t>(IteratorResumeResult::Done))
            return false;
        if (integerIn(entered.position) || !m_walker.armsMayEndWithBranchBehind(resume))
            return false;
        bool runsNoCode = readsOfListRunNoCode(entered);
        Reference isInside = operatorOnNumbers(Opcode::Less, entered.position, lengthOf(entered, entered.list));
        if (m_walker.isUnreachable())
            return true;
        BlockID giving = addBlock();
        BlockID finished = addBlock();
        branch(isInside, giving, finished);

        continueIn(finished);
        m_walker.iterationByIndex(state).position = integer(Walker::positionBehindEveryEnd);
        m_walker.bind(resume.id(), resultsOf(boolean(true), undefined()));
        if (!m_walker.endArmWithBranchBehind(resume)) {
            m_walker.fail("a way of a step does not walk the end of its block"_s);
            return true;
        }

        continueIn(giving);
        Reference inside = narrowed(entered.position, isInside, OperandIndex::Less::left);
        Reference given = handedOutAt(entered, entered.list, inside, runsNoCode, true);
        m_walker.iterationByIndex(state).position = operatorOnNumbers(Opcode::Increment, inside);
        if (m_walker.isUnreachable())
            return true;
        m_walker.bind(resume.id(), resultsOf(boolean(false), given));
        if (entered.kind == JSC::IterationKind::Entries)
            noteFreshList(m_walker, given);
        if (!m_walker.endArmWithBranchBehind(resume))
            m_walker.fail("a way of a step does not walk the end of its block"_s);
        return true;
    }

    bool stepByMethodInSightOnWaysApart(Reference state, const BodyValue& resume)
    {
        const Walker::IterationByIndex& entered = m_walker.iterationByIndex(state);
        if (!entered.goesByProtocolAlone || !entered.isOpenAtEveryStepAndClose || entered.wayToStep || !goesByMethodsInSight())
            return false;
        Reference next = entered.next;
        Reference iterator = entered.iterator;
        if (!findOnlyCallee(m_walker, resume, next))
            return false;
        const BodyIndex& index = m_walker.currentBodyIndex();
        BodyTerminator end = index.terminator(index.blockOf(resume.id()));
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        if (!m_walker.has(Mechanism::Folding) || !condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
            return false;
        auto asked = index.value(condition).as<Opcode::ResultAt>();
        if (asked.resultsOperand() != resume.id() || asked.index() != static_cast<uint32_t>(IteratorResumeResult::Done) || !m_walker.armsMayEndWithBranchBehind(resume))
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
        if (!m_walker.endArmWithBranchBehind(resume)) {
            m_walker.fail("a way of a step does not walk the end of its block"_s);
            return true;
        }

        continueIn(further);
        Reference value = readOffAnswer(answer, BuiltinNames::value);
        if (m_walker.isUnreachable())
            return true;
        m_walker.bind(resume.id(), resultsOf(boolean(false), value));
        if (!m_walker.endArmWithBranchBehind(resume))
            m_walker.fail("a way of a step does not walk the end of its block"_s);
        return true;
    }

    bool stepTableOnWaysApart(Reference state, const BodyValue& resume)
    {
        const Walker::IterationByIndex& record = m_walker.iterationByIndex(state);
        if (record.mayGoByProtocol || record.isTakenAtOpen || record.holdsFlatEntries || record.literal || !record.table || !stepsThroughTable(record.contents) || !record.head || !findWayOfIteration(m_walker.currentBodyIndex(), resume.operand(0)).onlyStepStandsAtHeadOfLoop)
            return false;
        Walker::IterationByIndex entered = record;
        const BodyIndex& index = m_walker.currentBodyIndex();
        BodyTerminator end = index.terminator(index.blockOf(resume.id()));
        ValueID condition = end.kind() == TerminatorKind::Branch ? end.operand(0) : ValueID();
        if (!m_walker.has(Mechanism::Folding) || !condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::ResultAt)
            return false;
        auto asked = index.value(condition).as<Opcode::ResultAt>();
        if (asked.resultsOperand() != resume.id() || asked.index() != static_cast<uint32_t>(IteratorResumeResult::Done))
            return false;
        if (entered.otherForm != Walker::IterationByIndex::OtherForm::None || !m_walker.armsMayEndWithBranchBehind(resume))
            return false;
        Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
        isClosed.setTableOperand(entered.table);
        Reference wasClosed = build(WTF::move(isClosed));
        if (m_walker.isUnreachable())
            return true;
        BlockID finished = addBlock();
        BlockID stepping = addBlock();
        branch(wasClosed, finished, stepping);

        continueIn(stepping);
        bool isMap = entered.contents == ContentsStepped::Map;
        Draft next = draft(isMap ? Opcode::MapNext : Opcode::SetNext);
        next.setOperand(0, entered.list);
        next.setTableOperand(entered.table);
        next.setEntryOperand(entered.position);
        Reference found = build(WTF::move(next));
        BlockID giving = addBlock();
        branch(resultAt(found, MapNextResult::HasEntry), giving, finished);

        continueIn(finished);
        m_walker.iterationByIndex(state).table = build(draft(Opcode::CollectionIteratorClosedMark));
        m_walker.bind(resume.id(), resultsOf(boolean(true), undefined()));
        if (!m_walker.endArmWithBranchBehind(resume)) {
            m_walker.fail("a way of a step does not walk the end of its block"_s);
            return true;
        }

        continueIn(giving);
        Reference key = resultAt(found, MapNextResult::Key, isMap ? entered.keysProven : entered.valuesProven);
        Reference value = isMap ? resultAt(found, MapNextResult::Value, entered.valuesProven) : key;
        Reference given = entered.kind == JSC::IterationKind::Keys ? key : value;
        if (entered.kind == JSC::IterationKind::Entries) {
            Draft pair = draft(Opcode::NewArray);
            pair.setContents(TypeTable::anything);
            pair.setElementsOperands(std::array { key, value });
            given = build(WTF::move(pair));
        }
        m_walker.iterationByIndex(state).position = operatorOnNumbers(Opcode::Increment, resultAt(found, MapNextResult::Entry));
        m_walker.iterationByIndex(state).table = tableOf(entered.list);
        if (m_walker.isUnreachable())
            return true;
        m_walker.bind(resume.id(), resultsOf(boolean(false), given));
        if (entered.kind == JSC::IterationKind::Entries)
            noteFreshList(m_walker, given);
        if (!m_walker.endArmWithBranchBehind(resume))
            m_walker.fail("a way of a step does not walk the end of its block"_s);
        return true;
    }

    void closeByIndex(Walker::IterationByIndex& iteration, IteratorCompletion completion)
    {
        if (!iteration.mayGoByProtocol)
            return;
        if (completion == IteratorCompletion::Throw && iteration.isOpenAtEveryStepAndClose && findHelper(IterationHelper::CloseIteratorAfterThrow)) {
            callReturnAndDropWhatItThrows(iteration.iterator);
            return;
        }
        if (iteration.goesByProtocolAlone) {
            if (completion == IteratorCompletion::Normal)
                callReturn(iteration.iterator, ResultOfReturnInIteration::MustBeObject);
            else
                callReturnAndDropWhatItThrows(iteration.iterator);
            return;
        }
        BlockID byProtocol = addBlock(BlockFrequency::Unlikely);
        BlockID after = addBlock();
        if (iteration.kindOfListAskedForBehind)
            goOnWhereTypeTestPasses(iteration.list, m_walker.types().unionOf(typeOfInstancesStepped(m_walker, iteration.builtinClass, iteration.contents), m_walker.types().builtinClass(BuiltinClasses::Array)), byProtocol);
        else
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

    Reference elementOfLiteral(const Walker::IterationByIndex& iteration, uint32_t position)
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
    void openWhatCallGivesByProtocol(BlockID inputBlock, uint32_t begin, uint32_t end, ValueID given)
    {
        m_blockOfCallThatMakesIterator = inputBlock;
        m_placeOfReadOfMethod = begin;
        m_placeBehindCallThatMakesIterator = end;
        m_givenByCallThatMakesIterator = given;
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

    bool goesByMethodsInSight() const { return m_site && m_kind == IteratorKind::Sync && m_wording == WordingOfIteration::Source && !m_handler && m_walker.has(Mechanism::DirectCalls) && !m_walker.isUnreachable(); }

    Reference readMethod(Reference object, PropertyKeyID key)
    {
        Reference method = goesByMethodsInSight() ? readMethodAboveProvenObject(m_walker, object, key, m_origin) : Reference();
        return method ? method : read(object, key);
    }

    Reference readOffAnswer(Reference answer, BuiltinName name)
    {
        Reference held = goesByMethodsInSight() ? readFilledFieldOfProvenObject(m_walker, answer, m_walker.program().keyOf(name), m_origin) : Reference();
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
        if (FunctionID callee = arguments.empty() && goesByMethodsInSight() ? findOnlyCallee(m_walker, *m_site, method) : FunctionID()) {
            if (callMethodOfProtocol(m_walker, *m_site, callee, receiver, method))
                return m_walker.isUnreachable() ? Reference() : m_walker.referenceBoundTo(m_site->id());
        }
        Draft invocation = draft(Opcode::Call);
        invocation.setCalleeOperand(method);
        invocation.setThisValueOperand(receiver);
        invocation.setArgumentsOperands(arguments);
        return build(WTF::move(invocation));
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
        BlockID block = m_walker.addBlock(frequency);
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
        operation.setForm(formAllowedForTypedOperator(m_walker, operation));
        return boxedUnlessNumbersAreUnboxed(m_walker, m_walker.build(WTF::move(operation)), m_origin);
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
        if (formAllowedForTypedOperator(m_walker, sum) != OperatorForm::Integers)
            return { };
        sum.setForm(OperatorForm::Integers);
        Reference result = m_walker.build(WTF::move(sum));
        return result && m_walker.types().at(m_walker.typeOf(result)).isInteger() ? result : Reference();
    }

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

    Reference goOnWhereInstanceIsAsMade(Reference iterable, const ClassSteppedUnseen& stepped, BlockID otherwise)
    {
        TypeID instances = typeOfInstancesStepped(m_walker, stepped.builtinClass, stepped.contents);
        Reference instance = m_walker.types().isSubset(m_walker.typeOf(iterable), instances) ? iterable : goOnWhereTypeTestPasses(iterable, instances, otherwise);
        if (stepped.contents == ContentsStepped::Text || (instance == iterable && iterable == m_instanceAsMadeForGood && !stepped.restsOnWatchAboveInstances))
            return instance;
        bool isList = stepped.contents == ContentsStepped::List;
        Draft test = draft(Opcode::IsPristine);
        test.setValueOperand(instance);
        test.setBuiltinClass(stepped.builtinClass);
        PristineAspectSet aspects = isList ? PristineAspectSet { PristineAspects::Storage, PristineAspects::Names } : PristineAspectSet { PristineAspects::Names };
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

    Reference lengthOf(const Walker::IterationByIndex& iteration, Reference list)
    {
        if (m_walker.isUnreachable())
            return { };
        auto heldAgainstMemory = [&](auto&& read) -> Reference {
            uint32_t numberOfRecordsInFront = m_walker.code().numberOfValues();
            Reference believed = buildAsItIs(read());
            if (m_walker.checksProofs() && believed && !m_walker.isBuiltWhereItStandsSince(numberOfRecordsInFront, believed))
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

    Reference elementAt(Reference list, Reference position, bool runsNoCode, bool isInsideLength = false, TypeID provenByLink = { }, bool restsOnLinksWord = false)
    {
        if (m_walker.isUnreachable())
            return { };
        if (std::optional<int64_t> known = integerIn(position)) {
            Reference held = elementOfListInSight(m_walker, list, static_cast<uint32_t>(*known));
            if (held && !(m_walker.types().at(m_walker.typeOf(held)).primitives & Primitive::Absent))
                return held;
        }

        if (!runsNoCode)
            m_walker.letLeave(list);
        TypeID before = m_walker.exchangeResultOfReadBeingTyped(provenByLink);
        Reference built;
        if (std::optional<ListTestedInFrontOfLoop> tested = findListTestedInFrontOfLoop(m_walker, list)) {
            Draft read = readOfListTestedInFrontOfLoop(list, position, *tested, isInsideLength);
            takeStorageAndLengthFromInFrontOfLoop(m_walker, read, *tested);
            built = m_walker.build(WTF::move(read));
        } else {
            Draft read = readOfElementAt(list, position, runsNoCode, isInsideLength);
            read.setAsksCellWhereNothingIsFound(restsOnLinksWord);
            built = m_walker.build(WTF::move(read));
        }
        m_walker.exchangeResultOfReadBeingTyped(before);
        return built;
    }

    Draft readOfListTestedInFrontOfLoop(Reference list, Reference position, const ListTestedInFrontOfLoop& tested, bool isInsideLength)
    {
        Draft read = draft(Opcode::GetArrayElement);
        read.setArrayOperand(list);
        read.setPositionOperand(position);
        nameKindOfListTestedInFrontOfLoop(read, tested);
        read.setLevel(KnowledgeLevel::Proven);
        read.setKeyIsIntegral(true);
        read.setIsInBounds(isInsideLength);
        return read;
    }

    Draft readOfElementAt(Reference list, Reference position, bool runsNoCode, bool isInsideLength)
    {
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

    void stepThroughTable(const Walker::IterationByIndex& iteration, Reference collection, BlockID after)
    {
        static_assert(static_cast<uint32_t>(MapNextResult::HasEntry) == static_cast<uint32_t>(SetNextResult::HasEntry) && static_cast<uint32_t>(MapNextResult::Entry) == static_cast<uint32_t>(SetNextResult::Entry) && static_cast<uint32_t>(MapNextResult::Key) == static_cast<uint32_t>(SetNextResult::Element));
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

    void stepOverCodePointAt(const Walker::IterationByIndex& iteration, Reference text, Reference length, Reference position, BlockID after)
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
        endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::positionBehindEveryEnd));

        continueIn(reading);
        std::optional<int64_t> known = integerIn(position);
        Reference inside = known ? position : narrowed(position, isInside, OperandIndex::Less::left);
        if (iteration.contents == ContentsStepped::Text) {
            stepOverCodePointAt(iteration, list, length, inside, after);
            return;
        }
        Reference value = iteration.contents == ContentsStepped::View ? elementOfView(list, inside) : handedOutAt(iteration, list, inside, runsNoCode, true);
        Reference next = known ? integer(*known + 1) : operatorOnNumbers(Opcode::Increment, inside);
        endStepByProtocol(iteration, after, boolean(false), value, next);
    }

    template<typename HandedOut>
    void endStepByProtocolOnTwoWays(const Walker::IterationByIndex& iteration, BlockID after, Reference isLast, NOESCAPE const HandedOut& handedOut)
    {
        BlockID last = addBlock();
        BlockID further = addBlock();
        branch(isLast, last, further);
        continueIn(last);
        endStepByProtocol(iteration, after, boolean(true), undefined(), integer(Walker::positionBehindEveryEnd));
        continueIn(further);
        endStepByProtocol(iteration, after, boolean(false), handedOut(), integer(0));
    }

    TypeID closuresExpectedUnderNext(Reference next)
    {
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

    void stepWhereNextIsAsExpected(const Walker::IterationByIndex& iteration, BlockID after)
    {
        TypeID closures = closuresExpectedUnderNext(iteration.next);
        if (!closures)
            return;
        BlockID byValue = addBlock(BlockFrequency::Unlikely);
        Reference known = goOnWhereTypeTestPasses(iteration.next, closures, byValue);
        m_walker.noteGuessOpened();
        m_walker.count(StatisticID::NextMethodsTestedForFunctionExpected);
        Reference answer = requireObject(call(known, iteration.iterator), RuntimeErrorKind::IteratorResultNotObject);
        Reference done = truthOf(readOffAnswer(answer, BuiltinNames::done));
        if (!m_walker.isUnreachable())
            endStepByProtocolOnTwoWays(iteration, after, done, [&] { return readOffAnswer(answer, BuiltinNames::value); });
        continueIn(byValue);
    }

    void stepByProtocolBesideIndex(const Walker::IterationByIndex& iteration, BlockID after)
    {
        FunctionID stepsForTheOddOne = !iteration.state && iteration.isOpenAtEveryStepAndClose && m_protocolIsForTheOddOne ? findHelper(IterationHelper::StepIterator) : FunctionID();
        if (stepsForTheOddOne) {
            Reference handedOut = callHelper(stepsForTheOddOne, { iteration.iterator, iteration.next });
            Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
            isClosed.setTableOperand(handedOut);
            endStepByProtocolOnTwoWays(iteration, after, build(WTF::move(isClosed)), [&] { return handedOut; });
            return;
        }
        if (!iteration.state && iteration.isOpenAtEveryStepAndClose && iteration.wayToStep) {
            stepInWayBesideIndex(iteration, after);
            stepWhereNextIsAsExpected(iteration, after);
            Reference answer = requireObject(call(iteration.next, iteration.iterator), RuntimeErrorKind::IteratorResultNotObject);
            Draft conversion = draft(Opcode::ToBoolean);
            conversion.setValueOperand(read(answer, BuiltinNames::done));
            endStepByProtocolOnTwoWays(iteration, after, build(WTF::move(conversion)), [&] { return read(answer, BuiltinNames::value); });
            return;
        }

        JoinOfIteration stepped = openJoin();
        if (iteration.state) {
            takeStateInList(iteration.state);
            m_listOfStateHoldsVerdictOnNext = iteration.stateHoldsVerdictOnNext;
            stepUnlessDone({ }, false, stepped);
            endStepByProtocol(iteration, after, stepped.results[0], stepped.results[1], iteration.position);
            return;
        }

        if (iteration.isOpenAtEveryStepAndClose) {
            stepWhereNextIsAsExpected(iteration, after);
            m_isSteppedByCallsAlone = iteration.isSteppedByCallsAlone;
            step(iteration.iterator, iteration.next, { }, false, stepped);
            endStepByProtocolOnTwoWays(iteration, after, stepped.results[0], [&] { return stepped.results[1]; });
            return;
        }

        BlockID stepping = addBlock();
        BlockID finished = addBlock();
        branch(operatorOnNumbers(Opcode::Less, iteration.position, integer(Walker::positionBehindEveryEnd)), stepping, finished);
        continueIn(finished);
        jumpTo(stepped, boolean(true), undefined());
        continueIn(stepping);
        step(iteration.iterator, iteration.next, { }, false, stepped);
        endStepByProtocolOnTwoWays(iteration, after, stepped.results[0], [&] { return stepped.results[1]; });
    }

    void stepInWayBesideIndex(const Walker::IterationByIndex& iteration, BlockID after)
    {
        BlockID byCall = addBlock();
        BlockID inLibrary = addBlock();
        branch(operatorOnNumbers(Opcode::StrictEq, iteration.wayToStep, integer(0)), byCall, inLibrary);
        continueIn(inLibrary);
        m_walker.noteGuessOpened();
        m_walker.count(StatisticID::IteratorsSteppedWithoutCall);
        Reference handedOut = callHelper(findHelper(IterationHelper::StepIteratorInWay), { iteration.iterator, iteration.wayToStep });
        Draft isClosed = draft(Opcode::IsCollectionIteratorClosedMark);
        isClosed.setTableOperand(handedOut);
        endStepByProtocolOnTwoWays(iteration, after, build(WTF::move(isClosed)), [&] { return handedOut; });
        continueIn(byCall);
    }

    void endStepByProtocol(const Walker::IterationByIndex& iteration, BlockID after, Reference done, Reference value, Reference position)
    {
        if (iteration.goesByProtocolAlone)
            jumpTo(after, { done, value });
        else if (iteration.table)
            jumpTo(after, { done, value, position, undefined() });
        else
            jumpTo(after, { done, value, position });
    }

    void takeState(Reference state)
    {
        if (!state || !state.isValue() || m_walker.code().at(state.id()).opcode != Opcode::NewHeapWords) {
            m_walker.fail("an iteration that the protocol has not opened is stepped or closed by the protocol"_s);
            return;
        }
        m_state = state;
    }

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

    OpenedIteration open(Reference iterable)
    {
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
            throwError(RuntimeErrorKind::NoSyncIteratorForAsyncIteration);
            return;
        case UseOfSyncIteration::Stepped:
            break;
        }

        switch (m_wording) {
        case WordingOfIteration::Delegation:
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

    OpenedIteration openSync(Reference iterable, UseOfSyncIteration use)
    {
        if (Reference method = use == UseOfSyncIteration::Stepped && goesByMethodsInSight() ? readMethodAboveProvenObject(m_walker, iterable, m_walker.types().wellKnownSymbolKey(WellKnownSymbol::Iterator), m_origin) : Reference())
        {
            m_walker.count(StatisticID::IterationsOpenedByMethodsInSight);
            return leftToSteps(withNextMethod(requireObject(call(method, iterable), RuntimeErrorKind::IteratorIsNotAnObject)));
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

        BlockID isObject = addBlock();
        BlockID isNoObject = addBlock(BlockFrequency::Rare);
        branch(testType(iterator, TypeTable::object), isObject, isNoObject);
        continueIn(isNoObject);
        call(read(iterator, BuiltinNames::next), iterator);
        throwError(RuntimeErrorKind::IteratorIsNotAnObject);

        continueIn(isObject);
        return withNextMethod(iterator);
    }

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

    void readStepResult(Reference result, bool readsValueWhenDone, const JoinOfIteration& join) { readAwaitedStepResult(await(result), readsValueWhenDone, join); }

    void readAwaitedStepResult(Reference result, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        Reference object = requireObject(result, m_wording == WordingOfIteration::Spread ? RuntimeErrorKind::SpreadResultNotObject : RuntimeErrorKind::IteratorResultNotObject);
        Reference done = truthOf(readOffAnswer(object, BuiltinNames::done));

        if (readsValueWhenDone) {
            Reference value = readOffAnswer(object, BuiltinNames::value);
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
        Reference value = readOffAnswer(object, BuiltinNames::value);
        if (m_state)
            storeWord(WordOfIteration::IsDone, boolean(false));
        jumpTo(join, boolean(false), value);
    }

    void step(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (!stepIteratorOfEngineInLibrary(iterator, next, sent, readsValueWhenDone, join))
            stepGeneratorWhereNextIsOfEngine(iterator, next, sent, readsValueWhenDone, join);
        if (FunctionID steps = m_protocolIsForTheOddOne && !sent && !readsValueWhenDone ? findHelper(IterationHelper::StepIterator) : FunctionID()) {
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

    void stepGeneratorWhereNextIsOfEngine(Reference iterator, Reference next, Reference sent, bool readsValueWhenDone, const JoinOfIteration& join)
    {
        if (!m_mayStepGenerator || !m_walker.has(Mechanism::Guesses) || !m_walker.has(Mechanism::BuiltinCalls) || m_walker.isUnreachable())
            return;
        bool isAsync = m_kind == IteratorKind::Async;
        if (isAsync && !canWaitForStepOfAsyncGenerator(m_walker))
            return;

        TypeArena& types = m_walker.types();
        const BuiltinTable& table = m_walker.program().builtins();
        TypeID methods = isAsync
            ? types.unionOf(types.forBuiltin(table.canonicalID(Builtins::AsyncGeneratorPrototypeNext)), types.forBuiltin(table.canonicalID(Builtins::AsyncFromSyncIteratorPrototypeNext)))
            : types.forBuiltin(table.canonicalID(Builtins::GeneratorPrototypeNext));
        BlockID byCall = addBlock();
        goOnWhereTypeTestPasses(next, methods, byCall);
        m_walker.noteGuessOpened();

        if (isAsync) {
            Reference generator = goOnWhereTypeTestPasses(iterator, types.unionOf(types.builtinClass(BuiltinClasses::AsyncGenerator), types.builtinClass(BuiltinClasses::AsyncFromSyncIterator)), byCall);
            Reference stepped = m_walker.isUnreachable() ? Reference() : buildStepOfAsyncGenerator(m_walker, generator, sent ? sent : constant(draft(Opcode::Absent)), m_held, { }, m_origin);
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
        storeWord(WordOfIteration::IsDone, boolean(true));
        Reference iterator = readWord(WordOfIteration::Iterator);
        Reference next = readWord(WordOfIteration::Next);
        step(iterator, next, sent, readsValueWhenDone, join);
    }

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

    void callReturn(Reference iterator, ResultOfReturnInIteration expectation)
    {
        if (goesByMethodsInSight() && findsNothingUnderNameOfProvenObject(m_walker, iterator, m_walker.program().keyOf(BuiltinNames::returnKeyword)))
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
    Reference m_state;
    bool m_stateIsList { false };
    bool m_listOfStateHoldsVerdictOnNext { false };
    bool m_isSteppedByCallsAlone { false };
    BlockID m_handler;
    bool m_isInLoopOfSpread { false };
    bool m_mayStepGenerator { true };
    bool m_protocolIsForTheOddOne { false };
    Reference m_instanceAsMadeForGood;
    BlockID m_blockOfCallThatMakesIterator;
    uint32_t m_placeOfReadOfMethod { 0 };
    uint32_t m_placeBehindCallThatMakesIterator { 0 };
    ValueID m_givenByCallThatMakesIterator;
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

static ValueID onlyTakerBehindCasts(const BodyIndex& index, ValueID value)
{
    ValueID taker = index.onlyUsingValue(value);
    while (taker && inputOperationsThatHandOn.contains(index.opcodeOf(taker)))
        taker = index.onlyUsingValue(taker);
    return taker;
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
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::EscapeAnalysis) || !listsAreIteratedByIndex(walker))
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID open = onlyTakerBehindCasts(index, call.id());
    if (!open || index.opcodeOf(open) != Opcode::IteratorOpen || !isOpenThatMayGoByIndex(index.value(open)) || index.value(open).as<Opcode::IteratorOpen>().isDestructuring())
        return false;
    if (walker.walksStretchInsideAnother() || index.blockOf(open) != index.blockOf(call.id()))
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

static bool isMadeByCallThatListsEntriesFlat(Walker& walker, Reference list)
{
    if (!list || !list.isValue())
        return false;
    const Code& code = walker.code();
    const BuiltinTable& table = walker.program().builtins();
    const Operation& made = code.at(rootOfCellInSweptCode(code, list.id()));
    return made.opcode == Opcode::CallBuiltin && table.contains(Builtins::CompilerHelpersOwnEntriesFlat) && table.at(made.overload()).builtin == table.canonicalID(Builtins::CompilerHelpersOwnEntriesFlat);
}

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

static bool openAsyncIterationInValues(Walker& walker, const BodyValue& open)
{
    auto written = open.as<Opcode::IteratorOpen>();
    Reference iterable = walker.referenceOf(written.iterableOperand());
    if (written.iteratorKind() != IteratorKind::Async || written.isDelegating() || !iterable || !iterable.isValue())
        return false;
    if (!findWayOfIteration(walker.currentBodyIndex(), open.id()).isOpenAtEveryStepAndClose)
        return false;
    Walker::IterationByIndex iteration = BuilderOfIteration(walker, walker.originOf(open), IteratorKind::Async, WordingOfIteration::Source).openByProtocolAlone(iterable);
    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), open.id()))
        walker.leaveAnythingAt(listed);
    walker.state().invalidateHeap();
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
    return true;
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
    if (Reference list = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand()); isMadeByCallThatListsEntriesFlat(walker, list)) {
        Walker::IterationByIndex entries;
        entries.list = list;
        entries.builtinClass = BuiltinClasses::Array;
        entries.holdsFlatEntries = true;
        entries.position = BuilderOfIteration(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source).integer(0);
        entries.inputHead = findWayOfIteration(index, open.id()).headOfLoopThatSteps;
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(entries)));
        return true;
    }
    Reference iterable = walker.referenceOf(open.as<Opcode::IteratorOpen>().iterableOperand());
    ValueID iterated = open.as<Opcode::IteratorOpen>().iterableOperand();
    bool isNamedByOpenAlone = !index.isParameter(iterated) && opcodesThatMakeArguments.contains(index.opcodeOf(iterated)) && index.onlyUsingValue(iterated) == open.id();
    bool staysAsItIsWhileIterated = listsAreIteratedByIndex(walker) && (isNamedByOpenAlone || stepsOfPatternStandInRow(index, open));
    if (Reference list = staysAsItIsWhileIterated ? listStandingForFreshArguments(walker, iterable) : Reference())
        iterable = list;
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    builder.standFor(open);

    if (ValueID literal = findLiteralIteratedAlone(walker, index, open)) {
        Walker::IterationByIndex iteration;
        iteration.list = iterable;
        iteration.builtinClass = BuiltinClasses::Array;
        iteration.literal = literal;
        iteration.position = builder.integer(open.as<Opcode::IteratorOpen>().elementCount().value_or(0));
        iteration.inputHead = findWayOfIteration(index, open.id()).headOfLoopThatSteps;
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
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
    bool isAsMadeForGood = proven && proven->contents != ContentsStepped::Text && linksWordCanBeHeldAgainst(walker, iterable) && classOfIterableThatStaysAsMadeByLink(walker, open, iterated) == proven->builtinClass;
    bool isSurelyAsMade = proven && (proven->contents == ContentsStepped::Text || isAsMadeForGood || (proven->contents != ContentsStepped::View && isListAsMade(walker, iterable)));
    if (iterable && !isSurelyAsMade && stepsOfPatternStandInRow(index, open)) {
        uint32_t numberOfSteps = *open.as<Opcode::IteratorOpen>().elementCount();
        bool standsInLoop = iterable.isValue() && walker.isInLoop();
        std::optional<ClassSteppedUnseen> table = standsInLoop ? findClassToTestFor(walker, iterated, iterable, true) : std::nullopt;
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
            Walker::IterationByIndex iteration = builder.takeStepsByProtocolAtOpen(iterable, numberOfSteps);
            if (!walker.isUnreachable())
                walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
            return true;
        }
        if (table || !standsInLoop || walker.types().overlap(walker.typeOf(iterable), walker.types().builtinClass(BuiltinClasses::Array))) {
            Walker::IterationByIndex iteration = table ? builder.takeStepsThroughTableAtOpen(iterable, numberOfSteps, *table) : builder.takeStepsAtOpen(iterable, numberOfSteps);
            if (table)
                walker.noteGuessOpened();
            if (!walker.isUnreachable())
                walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
            return true;
        }
    }

    WayOfIteration way = findWayOfIteration(index, open.id());
    if (!way.canBeFollowed)
        return false;
    std::optional<ClassSteppedUnseen> tested = isSurelyAsMade ? std::nullopt : findClassToTestFor(walker, open.as<Opcode::IteratorOpen>().iterableOperand(), iterable, !!way.headOfLoopThatSteps);
    if (!isSurelyAsMade && !tested && (!way.isOpenAtEveryStepAndClose || !iterable || !iterable.isValue()))
        return false;

    Walker::IterationByIndex iteration;
    if (!isSurelyAsMade && !tested) {
        if (!isExpectedToStepGenerator(walker, open.id()))
            builder.expectNoGenerator();
        iteration = builder.openByProtocolAlone(iterable);
        if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), open.id()))
            walker.leaveAnythingAt(listed);
        walker.state().invalidateHeap();
    } else if (isSurelyAsMade) {
        if (isAsMadeForGood)
            holdLinksWordAgainstObjectAsMade(walker, iterable, walker.originOf(open));
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
        if (tested->kindOfListAskedForBehind && (way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep))
            tested = ClassSteppedUnseen { BuiltinClasses::Array };
        iteration = builder.openByIndexOrProtocol(iterable, *tested, way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep, way.isOpenAtEveryStepAndClose);
        walker.noteGuessOpened();
    }
    if (!iteration.goesByProtocolAlone)
        iteration.inputHead = way.headOfLoopThatSteps;
    iteration.isDeadBehindLoop = !iteration.goesByProtocolAlone && way.onlyStepStandsAtHeadOfLoop && iteration.contents == ContentsStepped::List && (!iteration.mayGoByProtocol || iteration.state || iteration.isOpenAtEveryStepAndClose);
    if (!walker.isUnreachable())
        walker.bind(open.id(), walker.addIterationByIndex(WTF::move(iteration)));
    return true;
}

bool openLoopOverIteratorThatEngineMakesOfItsOpcodes(Walker& walker, const BodyValue& read)
{
    if (read.isFreeName() || walker.walksStretchInsideAnother() || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(read.id()));
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
    if (index.valueBehindCasts(open.as<Opcode::IteratorOpen>().iterableOperand()) != callID || !isOpenThatMayGoByIndex(open) || open.as<Opcode::IteratorOpen>().isDestructuring())
        return false;
    BuiltinName name = walker.program().builtinNameOf(read.key());
    WayOfIteration way = name ? findWayOfIteration(index, openID) : WayOfIteration();
    if (!way.canBeFollowed)
        return false;

    Reference receiver = walker.referenceOf(receiverID);
    std::optional<ClassSteppedUnseen> proven = findClassSteppedUnseen(walker, receiver);
    bool isAsMadeForGood = proven && receiverStaysAsMadeByLink(walker, call);
    std::optional<ClassSteppedUnseen> tested = isAsMadeForGood ? proven : findClassToTestFor(walker, receiverID, receiver, !!way.headOfLoopThatSteps);
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
    if (tested->kindOfListAskedForBehind)
        tested->kindOfListAskedForBehind = kindOfIterationThatNobodySees(walker, BuiltinClasses::Array, false, name);
    if (tested->kindOfListAskedForBehind && (way.stepsStandUnderOwnCleanup || !way.closesStandBelowOnlyStep))
        tested = ClassSteppedUnseen { BuiltinClasses::Array, ContentsStepped::List, *tested->kindOfListAskedForBehind };

    uint32_t place = read.id().index() - block.firstValue().index();
    BuilderOfIteration builder(walker, walker.originOf(open), IteratorKind::Sync, WordingOfIteration::Source);
    uint32_t placeOfOpen = openID.index() - block.firstValue().index();
    Walker::IterationByIndex iteration;
    if (isAsMadeForGood) {
        holdLinksWordAgainstObjectAsMade(walker, receiver, walker.originOf(open));
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
    if (const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), callID)) {
        if (iteration.table) {
            iteration.keysProven = contents->key;
            iteration.valuesProven = contents->value;
        } else if (iteration.contents == ContentsStepped::List && contents->value)
            iteration.elementsProven = walker.types().withPrimitives(contents->value, Primitive::Undefined);
    }
    if (!walker.isUnreachable())
        walker.bind(openID, walker.addIterationByIndex(WTF::move(iteration)));
    walker.walkStretch(block.id(), placeOfOpen + 1, placeOfOpen + 1);
    return true;
}

bool spreadAppendsElementsOfListAsMade(Walker& walker, ValueID spreadValue)
{
    Reference iterable = walker.referenceBoundTo(spreadValue);
    return iterable && iterable.isValue() && isProvenListInIteration(walker, iterable) && isListAsMade(walker, iterable);
}

static bool appendSpreadOfList(Walker& walker, const BodyValue& spread)
{
    Reference array = walker.referenceOf(spread.as<Opcode::AppendSpread>().arrayOperand());
    Reference iterable = walker.referenceOf(spread.as<Opcode::AppendSpread>().iterableOperand());
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
    BuiltinClassID classAsMadeForGood = linksWordCanBeHeldAgainst(walker, iterable) ? classOfIterableThatStaysAsMadeByLink(walker, spread, spread.as<Opcode::AppendSpread>().iterableOperand()) : BuiltinClassID();
    bool isListAsMadeForGood = classAsMadeForGood == BuiltinClasses::Array && indexesAreAbsentAboveLists(walker);
    bool isSurelyAsMade = isProvenListInIteration(walker, iterable) && (isListAsMadeForGood || isListAsMade(walker, iterable));
    std::optional<ClassSteppedUnseen> tested = !array || isSurelyAsMade ? std::nullopt : findClassToTestFor(walker, spread.as<Opcode::AppendSpread>().iterableOperand(), iterable);
    if (array && iterable && !isSurelyAsMade && !tested) {
        std::optional<ClassSteppedUnseen> held = findClassHeldUnderNameThatIsRead(walker, spread.as<Opcode::AppendSpread>().iterableOperand());
        if (held && BuilderOfIteration::isListedByAppendMembers(*held)) {
            tested = findClassToTestFor(walker, spread.as<Opcode::AppendSpread>().iterableOperand(), iterable, true);
            if (tested && !BuilderOfIteration::isListedByAppendMembers(*tested))
                tested = std::nullopt;
        }
    }
    if (!array || !iterable)
        return false;
    if (std::optional<ClassSteppedUnseen> proven = !tested && !isSurelyAsMade && classAsMadeForGood ? findClassSteppedUnseen(walker, iterable) : std::nullopt; proven && proven->builtinClass == classAsMadeForGood)
        tested = proven;
    BuilderOfIteration builder(walker, walker.originOf(spread), IteratorKind::Sync, WordingOfIteration::Spread);
    if (isSurelyAsMade && isListAsMadeForGood)
        holdLinksWordAgainstObjectAsMade(walker, iterable, walker.originOf(spread));
    if (isSurelyAsMade)
        builder.appendElementsOf(array, iterable);
    else if (tested && BuilderOfIteration::isListedByAppendMembers(*tested)) {
        if (classAsMadeForGood == tested->builtinClass) {
            holdLinksWordAgainstObjectAsMade(walker, iterable, walker.originOf(spread));
            builder.takeInstanceForAsMade(iterable);
        }
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

const Walker::IterationByIndex* findListSteppedByIndexAlone(Walker& walker, const BodyIndex& index, const BodyValue& step)
{
    if (step.opcode() != Opcode::IteratorResume || !walker.has(Mechanism::TypedOperations) || &index != &walker.currentBodyIndex())
        return nullptr;
    ValueID open = step.operand(0);
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return nullptr;
    Reference state = walker.referenceBoundTo(open);
    if (!state || !state.isVirtual())
        return nullptr;
    const Walker::IterationByIndex& iteration = walker.iterationByIndex(state);
    if (!iteration.goesThroughListByIndexAlone() || iteration.kind != JSC::IterationKind::Values || !iteration.list || !iteration.position)
        return nullptr;
    return &iteration;
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
        return openTypedIteration(walker, value) || openAsyncIterationInValues(walker, value);
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
    bool isAsync = walker.iterationByIndex(state).isAsync;
    BuilderOfIteration builder(walker, written.origin(), isAsync ? IteratorKind::Async : IteratorKind::Sync, WordingOfIteration::Source, isAsync ? written.heldOperands() : std::span<const Reference>());
    builder.standFor(value);
    if (opcode == Opcode::IteratorResume)
        builder.expectUnderNext(walker.facts().held().calleeAt(walker.currentFunction(), value.id()));
    if (opcode == Opcode::IteratorClose) {
        Walker::IterationByIndex closed = walker.iterationByIndex(state);
        builder.closeByIndex(closed, value.as<Opcode::IteratorClose>().completion());
        walker.iterationByIndex(state) = WTF::move(closed);
        return true;
    }

    if (Walker::IterationByIndex& stepped = walker.iterationByIndex(state); stepped.table && !stepped.keysProven && !stepped.valuesProven) {
        if (const LinkedContentsAtOperation* contents = walker.facts().linked().contentsAtOperation(walker.currentFunction(), value.operand(0))) {
            stepped.keysProven = contents->key;
            stepped.valuesProven = contents->value;
        }
    }
    if (Walker::IterationByIndex& stepped = walker.iterationByIndex(state); stepped.contents == ContentsStepped::List && stepped.kind == JSC::IterationKind::Values)
        stepped.elementsProven = walker.facts().linked().resultOfReadByKey(walker.currentFunction(), value.id());
    if (!isAsync && !isExpectedToStepGenerator(walker, value.operand(0)))
        builder.expectNoGenerator();
    if (!isAsync && isExpectedToBeSteppedUnseen(walker, value.operand(0)))
        builder.expectNoStepByProtocol();
    if (builder.stepTableOnWaysApart(state, value) || builder.stepListOnWaysApart(state, value) || (!isAsync && builder.stepByMethodInSightOnWaysApart(state, value)))
        return true;
    Reference results;
    if (std::optional<uint32_t> position = walker.iterationByIndex(state).literal ? placeOfStepInPattern(walker.currentBodyIndex(), value) : std::nullopt) {
        StepOfListLiteral step = stepAtPlaceOfLiteral(walker.currentBodyIndex(), walker.iterationByIndex(state).literal, *position);
        results = builder.resultsOf(builder.booleanValue(step.isDone), step.element ? builder.elementOfLiteral(walker.iterationByIndex(state), *position) : builder.undefinedValue());
    } else if (Walker::IterationByIndex& taken = walker.iterationByIndex(state); taken.isTakenAtOpen)
        results = builder.resumeByIndex(taken, value.as<Opcode::IteratorResume>().stepIndex());
    else {
        Walker::IterationByIndex stepped = walker.iterationByIndex(state);
        results = builder.resumeByIndex(stepped, value.as<Opcode::IteratorResume>().stepIndex());
        walker.iterationByIndex(state) = WTF::move(stepped);
    }
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

    if (FunctionID listed = walker.facts().linked().calleeAt(walker.currentFunction(), value.id()))
        walker.leaveAnythingAt(listed);

    walker.state().invalidateHeap();
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
