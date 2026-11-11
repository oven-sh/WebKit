/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */


#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTCallingConventions.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRFunction.h"
#include "AOTIRProgram.h"
#include "AOTInliningRules.h"
#include "AOTLimits.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTThinSummary.h"
#include "AOTWalkCoroutineCallsInternal.h"
#include "AOTWalkLoopsOverGenerators.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t mostInlineFramesOfGeneratorsInLoops = defaultLimitValue(LimitID::InlineFramesPerFunction, hostArchitecture());

static constexpr OpcodeSet opcodesThatBindGeneratorToItsFrame = opcodesWhere([](Opcode opcode) {
    return boundToOf(opcode) == BoundTo::Frame && !catchOpcodes.contains(opcode) && opcode != Opcode::Yield && opcode != Opcode::InitialYield;
});

static_assert(!static_cast<uint32_t>(YieldResult::Sent) && static_cast<uint32_t>(YieldResult::Mode) == 1);
static_assert(!static_cast<uint32_t>(IteratorResumeResult::Done) && static_cast<uint32_t>(IteratorResumeResult::Value) == 1);

namespace {

struct GeneratorFunctionDescribed {
    bool bodyMayBeWalkedInConsumer { false };
    uint32_t numberOfYields { 0 };
    uint64_t numberOfOperationsOnWaysOfReturn { 0 };
    std::optional<bool> nobodyLooksAbove;
    std::optional<Vector<ValueID, 4>> onlyUsersOfEntryParameters;
    bool startsAtInitialYield { false };
};

struct KeptByGeneratorsInLoops final : Walker::KeptForBuild {
    HashMap<uint32_t, GeneratorFunctionDescribed, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functions;
};

} // anonymous namespace

static KeptByGeneratorsInLoops& keptByGeneratorsInLoops(std::unique_ptr<Walker::KeptForBuild>& kept)
{
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<KeptByGeneratorsInLoops>();
    return static_cast<KeptByGeneratorsInLoops&>(*kept);
}


static bool mayThrowAsWrittenInGeneratorsInLoops(BodyBlock block)
{
    TerminatorKind end = block.terminator().kind();
    if (end == TerminatorKind::Throw || end == TerminatorKind::Rethrow)
        return true;
    for (BodyValue operation : block.values()) {
        const Effects& bound = declaredBoundInTable(operation.opcode());
        if (bound.mayThrow || bound.mayRunOtherCode() || operandsMayRunCode(operation.opcode()))
            return true;
    }
    return false;
}

static std::optional<uint32_t> targetOfReturnAtYield(const BodyIndex& body, BodyBlock block, ValueID yield)
{
    for (uint32_t number = yield.index() + 1; number < block.endOfValues().index(); ++number) {
        BodyValue behind = body.value(ValueID(number));
        if (behind.opcode() != Opcode::ResultAt || behind.as<Opcode::ResultAt>().resultsOperand() != yield)
            return std::nullopt;
    }
    BodyTerminator terminator = block.terminator();
    if (terminator.kind() != TerminatorKind::Switch)
        return std::nullopt;
    ValueID subject = terminator.operand(0);
    if (!subject || !block.holds(subject) || subject.index() <= yield.index() || body.value(subject).as<Opcode::ResultAt>().index() != static_cast<uint32_t>(YieldResult::Mode))
        return std::nullopt;
    uint32_t target = 0;
    for (int64_t written : terminator.as<TerminatorKind::Switch>().cases()) {
        if (written == static_cast<int64_t>(ResumeMode::Return))
            return target;
        ++target;
    }
    return std::nullopt;
}

static bool wayOfReturnLeadsForward(const BodyIndex& body, BlockID blockOfYield, BodyTarget first, uint64_t& numberOfOperations)
{
    HashMap<uint32_t, std::optional<int64_t>, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> integersHandedOver;
    BitVector isSeen(body.numberOfBlocks());
    BitVector isCounted(body.numberOfBlocks());
    bool knowsLessThanWhereBlocksWereSeen = false;
    Vector<BlockID, 8> worklist;
    auto integerOf = [&](ValueID value) -> std::optional<int64_t> {
        if (!value)
            return std::nullopt;
        if (body.isParameter(value)) {
            auto found = integersHandedOver.find(value.index());
            return found == integersHandedOver.end() ? std::nullopt : found->value;
        }
        return body.opcodeOf(value) == Opcode::Integer ? std::optional<int64_t>(body.value(value).as<Opcode::Integer>().integer()) : std::nullopt;
    };
    auto visit = [&](BodyTarget target) {
        BlockID to = target.block();
        if (to.index() <= blockOfYield.index())
            return false;
        bool knowsLess = false;
        BodyBlock block = body.block(to);
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t position = 0; position < arguments.size() && position < block.numberOfParameters(); ++position) {
            std::optional<int64_t> arriving = integerOf(arguments[position]);
            auto added = integersHandedOver.add(block.parameter(position).index(), arriving);
            if (!added.isNewEntry && added.iterator->value && added.iterator->value != arriving) {
                added.iterator->value = std::nullopt;
                knowsLess = true;
                knowsLessThanWhereBlocksWereSeen = true;
            }
        }
        bool isNew = !isSeen.set(to.index());
        if (!isCounted.set(to.index()))
            numberOfOperations += block.numberOfValues() + 1;
        if (isNew || knowsLess)
            worklist.append(to);
        return true;
    };
    if (!visit(first))
        return false;
    while (!worklist.isEmpty() || knowsLessThanWhereBlocksWereSeen) {
        if (worklist.isEmpty()) {
            knowsLessThanWhereBlocksWereSeen = false;
            isSeen.clearAll();
            visit(first);
        }
        BodyBlock block = body.block(worklist.takeLast());
        BodyTerminator terminator = block.terminator();
        std::optional<int64_t> subject = terminator.kind() == TerminatorKind::Switch ? integerOf(terminator.operand(0)) : std::nullopt;
        if (subject) {
            uint32_t chosen = 0;
            for (int64_t written : terminator.as<TerminatorKind::Switch>().cases()) {
                if (written == *subject)
                    break;
                ++chosen;
            }
            if (!visit(terminator.targets()[chosen]))
                return false;
        } else {
            for (BodyTarget target : terminator.targets()) {
                if (!visit(target))
                    return false;
            }
        }
        if (block.hasHandler() && mayThrowAsWrittenInGeneratorsInLoops(block) && !visit(block.handler()))
            return false;
    }
    return true;
}

static bool isPureInGeneratorsInLoops(const BodyValue& operation)
{
    return declaredBoundInTable(operation.opcode()).isPure() && !operandsMayRunCode(operation.opcode());
}

static GeneratorFunctionDescribed describeGeneratorFunction(const Walker& walker, FunctionID id)
{
    GeneratorFunctionDescribed described;
    const Program& program = walker.program();
    const Function& function = program.at(id);
    if (!function.has(FunctionFlags::IsGenerator) || function.has(FunctionFlags::IsAsync) || !function.hasBody() || function.isModuleBody() || function.results.size() != 1)
        return described;
    if (isMarkedNeverInlined(program, function) || walker.facts().isTakenToRunOnce(id))
        return described;

    const BodyIndex* body = walker.facts().bodies().find(id);
    if (!body || walker.facts().summaries().at(id).has(SummaryTrait::HasDirectEval))
        return described;
    if (body->hasIrreducibleFlow() || body->holdsAnyOf(opcodesOfMappedArguments) || body->holdsAnyOf(opcodesThatBindGeneratorToItsFrame))
        return described;
    if (!walker.plan().conventions().exists({ id, CodeRole::Body }))
        return described;
    BodyBlock entry = body->block(entryBlock);
    if (body->numberOfIncomingEdges(entryBlock) || entry.hasHandler() || entry.numberOfParameters() != function.numberOfEntryParameters())
        return described;

    for (BodyValue operation : entry.values()) {
        described.startsAtInitialYield = operation.opcode() == Opcode::InitialYield;
        if (described.startsAtInitialYield || !isPureInGeneratorsInLoops(operation))
            break;
    }
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyBlock block = body->block(BlockID(number));
        if (!body->isReachable(block.id()))
            continue;
        for (BodyValue operation : block.values()) {
            if (operation.opcode() != Opcode::Yield)
                continue;
            std::optional<uint32_t> targetOfReturn = targetOfReturnAtYield(*body, block, operation.id());
            if (!targetOfReturn || !wayOfReturnLeadsForward(*body, block.id(), block.terminator().targets()[*targetOfReturn], described.numberOfOperationsOnWaysOfReturn))
                return described;
            ++described.numberOfYields;
        }
    }
    described.bodyMayBeWalkedInConsumer = true;
    return described;
}

static Vector<ValueID, 4> findOnlyUsersOfEntryParameters(const BodyIndex& body)
{
    uint32_t firstParameter = body.block(entryBlock).firstParameter().index();
    uint32_t numberOfParameters = body.block(entryBlock).numberOfParameters();
    Vector<ValueID, 4> users;
    Vector<uint32_t, 4> numbersOfUses;
    users.fill(ValueID(), numberOfParameters);
    numbersOfUses.fill(0, numberOfParameters);
    auto noteUse = [&](ValueID operand, ValueID user) {
        ValueID named = operand ? body.stripCasts(operand) : ValueID();
        if (!named || !body.isParameter(named) || named.index() < firstParameter || named.index() >= firstParameter + numberOfParameters)
            return;
        users[named.index() - firstParameter] = ++numbersOfUses[named.index() - firstParameter] == 1 ? user : ValueID();
    };
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        BodyBlock block = body.block(BlockID(number));
        if (!body.isReachable(block.id()))
            continue;
        for (BodyValue operation : block.values()) {
            if (opcodesThatStateTypes.contains(operation.opcode()) || operation.opcode() == Opcode::InitialYield)
                continue;
            if (operation.opcode() == Opcode::Yield) {
                noteUse(operation.as<Opcode::Yield>().valueOperand(), ValueID());
                continue;
            }
            operation.forEachOperand([&](ValueID operand, auto...) {
                noteUse(operand, operation.id());
            });
        }
        auto noteUseAtEnd = [&](ValueID operand, auto...) {
            noteUse(operand, ValueID());
        };
        BodyTerminator end = block.terminator();
        end.forEachOperand(noteUseAtEnd);
        for (BodyTarget target : end.targets())
            std::ranges::for_each(target.arguments(), noteUseAtEnd);
        if (block.hasHandler())
            std::ranges::for_each(block.handler().arguments(), noteUseAtEnd);
    }
    return users;
}

static bool loopHoldsYield(const BodyIndex& body, LoopID loop)
{
    for (BlockID member : body.membersOf(loop)) {
        for (BodyValue operation : body.block(member).values()) {
            if (operation.opcode() == Opcode::Yield)
                return true;
        }
    }
    return false;
}


static bool stepStandsUnderHandlerOfCall(const BodyIndex& index, BodyBlock blockOfCall, BodyBlock blockOfStep)
{
    if (blockOfCall.hasHandler() != blockOfStep.hasHandler())
        return false;
    if (!blockOfCall.hasHandler())
        return true;
    std::span<const ValueID> atCall = blockOfCall.handler().arguments();
    std::span<const ValueID> atStep = blockOfStep.handler().arguments();
    if (blockOfCall.handler().block() != blockOfStep.handler().block() || atCall.size() != atStep.size())
        return false;
    std::span<const ValueID> entering = blockOfCall.terminator().next().arguments();
    for (size_t position = 0; position < atCall.size(); ++position) {
        if (atCall[position] == atStep[position])
            continue;
        ValueID parameter = atStep[position];
        if (!index.isParameter(parameter) || index.blockOf(parameter) != blockOfStep.id())
            return false;
        uint32_t place = parameter.index() - blockOfStep.firstParameter().index();
        if (place >= entering.size() || entering[place] != atCall[position])
            return false;
    }
    return true;
}

static bool blockOfStepOnlySteps(const BodyIndex& index, BodyBlock blockOfCall, ValueID open, const LoopOverIterator& found)
{
    BlockID inputHead = found.blockOfStep;
    if (index.hasIrreducibleFlow() || index.isEnteredByHandlerEdge(inputHead))
        return false;
    bool standsInBlockOfCall = inputHead == blockOfCall.id();
    if (!standsInBlockOfCall) {
        BodyTerminator end = blockOfCall.terminator();
        if (open.index() + 1 != blockOfCall.endOfValues().index() || end.kind() != TerminatorKind::Jump || end.next().block() != inputHead)
            return false;
        if (!stepStandsUnderHandlerOfCall(index, blockOfCall, index.block(inputHead)))
            return false;
    }

    BodyBlock head = index.block(inputHead);
    uint32_t numberOfReads = 0;
    for (BodyValue operation : head.values()) {
        if (operation.id() == found.step || (standsInBlockOfCall && operation.id().index() <= open.index()))
            continue;
        if (operation.id().index() < found.step.index()) {
            if (operation.opcode() != Opcode::LoopHint)
                return false;
            continue;
        }
        if (operation.opcode() != Opcode::ResultAt || operation.as<Opcode::ResultAt>().resultsOperand() != found.step)
            return false;
        ++numberOfReads;
    }
    if (numberOfReads != index.useCount(found.step) || numberOfReads >= BodyIndex::maxCountedUses)
        return false;

    if (!found.stepStandsInHeadOfLoop)
        return !index.isLoopHead(inputHead) && (standsInBlockOfCall || index.numberOfIncomingEdges(inputHead) == 1);
    LoopID loop = index.loopOf(inputHead);
    BodyTerminator branch = head.terminator();
    if (index.isInLoop(branch.taken().block(), loop) || !index.isInLoop(branch.notTaken().block(), loop))
        return false;
    uint32_t numberOfEdgesThatComeRound = 0;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyTarget target : index.terminator(member).targets())
            numberOfEdgesThatComeRound += target.block() == inputHead;
    }
    return index.numberOfIncomingEdges(inputHead) == numberOfEdgesThatComeRound + 1;
}

static Vector<BlockID, 8> collectRegionOfIteration(const BodyIndex& index, const BodyLiveness& liveness, BlockID blockOfStep, LoopID loop, ValueID open)
{
    Vector<BlockID, 8> region;
    Vector<BlockID, 8> behindLoop;
    Vector<BlockID, 8> worklist;
    auto visit = [&](BlockID to) {
        if ((loop && index.isInLoop(to, loop)) || !liveness.isLiveAtHead(open, to) || behindLoop.contains(to))
            return;
        behindLoop.append(to);
        worklist.append(to);
    };
    auto visitWaysOutOf = [&](BlockID from) {
        BodyBlock block = index.block(from);
        for (BodyTarget target : block.terminator().targets())
            visit(target.block());
        if (block.hasHandler())
            visit(block.handler().block());
    };
    if (!loop) {
        region.append(blockOfStep);
        visitWaysOutOf(blockOfStep);
    } else {
        for (BlockID member : index.membersOf(loop)) {
            region.append(member);
            visitWaysOutOf(member);
        }
    }
    while (!worklist.isEmpty())
        visitWaysOutOf(worklist.takeLast());
    region.appendVector(behindLoop);
    std::ranges::sort(region, { }, [](BlockID block) { return block.index(); });
    return region;
}


bool Walker::walkOperationOfGeneratorInLoop(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::Call:
        return takeGeneratorForConsumer(value);
    case Opcode::IteratorOpen:
        return openGeneratorTaken(value) || takeGeneratorForConsumer(value);
    case Opcode::IteratorResume:
        return stepGeneratorTakenOutsideLoops(value);
    case Opcode::IteratorClose:
        return closeGeneratorTaken(value);
    case Opcode::AppendSpread:
        return spreadGeneratorTaken(value) || takeGeneratorForConsumer(value);
    case Opcode::InitialYield:
        return placeOfGeneratorOnTop() != notFound;
    case Opcode::Yield:
        return walkYieldOfGeneratorInConsumer(value);
    default:
        return false;
    }
}

size_t Walker::placeOfGeneratorOnTop() const
{
    size_t top = m_frames.size() - 1;
    for (size_t place = m_generatorsWalkedInConsumers.size(); place--;) {
        if (m_generatorsWalkedInConsumers[place].frameOfGenerator == top)
            return place;
    }
    return notFound;
}

bool Walker::loopHoldsGenerators(const BodyIndex& index, LoopID loop)
{
    if (!has(Mechanism::Handlers) || !has(Mechanism::DirectCalls))
        return false;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() != Opcode::IteratorOpen)
                continue;
            auto opened = operation.as<Opcode::IteratorOpen>();
            if (opened.iteratorKind() != IteratorKind::Sync || opened.isDelegating() || opened.isDestructuring())
                continue;
            ValueID iterable = index.stripCasts(opened.iterableOperand());
            bool isCall = iterable && !index.isParameter(iterable) && index.opcodeOf(iterable) == Opcode::Call;
            FunctionID callee = isCall ? facts().linked().calleeAt(currentFunction(), iterable) : FunctionID();
            if (!callee || !program().at(callee).has(FunctionFlags::IsGenerator))
                callee = facts().linked().calleeAt(currentFunction(), operation.id());
            if (!callee || !program().at(callee).has(FunctionFlags::IsGenerator))
                continue;
            const GeneratorFunctionDescribed& described = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.ensure(callee.index(), [&] {
                return describeGeneratorFunction(*this, callee);
            }).iterator->value;
            if (described.bodyMayBeWalkedInConsumer)
                return true;
        }
    }
    return false;
}

bool Walker::startsAtInitialYield(FunctionID generatorFunction)
{
    return keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.ensure(generatorFunction.index(), [&] {
        return describeGeneratorFunction(*this, generatorFunction);
    }).iterator->value.startsAtInitialYield;
}

ValueID Walker::onlyUserOfEntryParameter(FunctionID generatorFunction, uint32_t entryParameter)
{
    GeneratorFunctionDescribed& described = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.ensure(generatorFunction.index(), [&] {
        return describeGeneratorFunction(*this, generatorFunction);
    }).iterator->value;
    if (!described.bodyMayBeWalkedInConsumer)
        return { };
    if (!described.onlyUsersOfEntryParameters)
        described.onlyUsersOfEntryParameters = findOnlyUsersOfEntryParameters(facts().bodies().at(generatorFunction));
    return entryParameter < described.onlyUsersOfEntryParameters->size() ? described.onlyUsersOfEntryParameters->at(entryParameter) : ValueID();
}

bool Walker::passesGeneratorForIteration(const BodyIndex& caller, ValueID generatorCall, FunctionID callerID)
{
    if (caller.isParameter(generatorCall) || caller.opcodeOf(generatorCall) != Opcode::Call)
        return false;
    ValueID passed = generatorCall;
    ValueID taker = followCastsToOnlyTaker(caller, passed);
    if (!taker || caller.opcodeOf(taker) != Opcode::Call)
        return false;
    auto call = caller.value(taker).as<Opcode::Call>();
    std::span<const ValueID> arguments = call.argumentsOperands();
    uint32_t position = static_cast<uint32_t>(std::ranges::find(arguments, passed) - arguments.begin());
    FunctionID callee = facts().linked().calleeAt(callerID, taker);
    if (position == arguments.size() || !callee || !facts().linked().function(callee).everyCallHasOneCallee)
        return false;
    const Function& function = program().at(callee);
    if (!function.has(FunctionFlags::IsGenerator) || function.has(FunctionFlags::IsAsync) || position >= function.numberOfPositionalParameters() || call.calleeOperand() == passed || call.thisValueOperand() == passed)
        return false;
    ValueID user = onlyUserOfEntryParameter(callee, function.firstParameterIndex() + position);
    if (!user)
        return false;
    BodyValue iteration = facts().bodies().at(callee).value(user);
    if (iteration.opcode() == Opcode::AppendSpread)
        return facts().bodies().at(callee).stripCasts(iteration.as<Opcode::AppendSpread>().arrayOperand()).index() != facts().bodies().at(callee).block(entryBlock).firstParameter().index() + function.firstParameterIndex() + position;
    return iteration.opcode() == Opcode::IteratorOpen && iteration.as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync;
}

Reference Walker::findPassedGenerator(ValueID open, ValueID iterable, ValueID& callThatMadeIt, uint32_t& frameOfCall)
{
    size_t place = placeOfGeneratorOnTop();
    if (place == notFound || !m_generatorsWalkedInConsumers[place].call)
        return { };
    const GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[place];
    const BodyIndex& body = currentBodyIndex();
    ValueID parameter = body.stripCasts(iterable);
    if (!body.isParameter(parameter) || body.blockOf(parameter) != entryBlock || body.loopDepthOf(body.blockOf(open)))
        return { };
    if (onlyUserOfEntryParameter(currentFunction(), parameter.index() - body.block(entryBlock).firstParameter().index()) != open)
        return { };
    Reference generator = referenceBoundTo(parameter);
    if (!generator || !generator.isRecord())
        return { };
    const Operation& made = m_code.at(generator.id());
    if (made.opcode == Opcode::CallDirect && !startsAtInitialYield(made.function()))
        return { };
    if (made.opcode != Opcode::CallDirect || made.role() != CodeRole::Body || made.numberOfVariadicOperands() != program().at(made.function()).numberOfEntryParameters())
        return { };

    const Frame& caller = m_frames[walked.frameOfCall];
    const BodyIndex& index = *caller.index;
    for (ValueID argument : index.value(walked.call).as<Opcode::Call>().argumentsOperands()) {
        ValueID call = index.stripCasts(argument);
        if (referenceBoundIn(caller, call) != generator)
            continue;
        ValueID nameInCaller = call;
        if (index.isParameter(call) || index.opcodeOf(call) != Opcode::Call || index.blockOf(call) != index.blockOf(walked.call) || followCastsToOnlyTaker(index, nameInCaller) != walked.call)
            return { };
        callThatMadeIt = call;
        frameOfCall = walked.frameOfCall;
        return generator;
    }
    return { };
}

bool Walker::takeGeneratorForConsumer(const BodyValue& call)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    bool takerCallsMethodItself = call.opcode() != Opcode::Call;
    ValueID taker = takerCallsMethodItself ? call.id() : index.onlyUsingValue(call.id());
    if (!taker || (index.opcodeOf(taker) != Opcode::IteratorOpen && index.opcodeOf(taker) != Opcode::AppendSpread))
        return false;
    if (!has(Mechanism::TypedOperations) || !has(Mechanism::Folding) || !has(Mechanism::Handlers) || !m_opensGuesses || m_coldCopyDepth || (!takerCallsMethodItself && call.isDoneInPlace()))
        return false;
    if (hasStringsInBuilders() || !m_versionedLoops.isEmpty() || !m_frames[frameIndex].blocksBeingTailDuplicated.isEmpty())
        return false;
    BodyBlock blockOfCall = index.block(index.blockOf(call.id()));
    if ((!takerCallsMethodItself && taker.index() != call.id().index() + 1) || !blockOfCall.holds(taker) || taker.index() - blockOfCall.firstValue().index() >= endOfStretchBeingWalked() || walksStretchInsideAnother())
        return false;

    FunctionID callee;
    Reference iterable;
    Reference passedGenerator;
    ValueID callOfGenerator = takerCallsMethodItself ? ValueID() : call.id();
    uint32_t frameOfCall = static_cast<uint32_t>(frameIndex);
    PropertyKeyID keyOfMethod = types().wellKnownSymbolKey(WellKnownSymbol::Iterator);
    if (takerCallsMethodItself) {
        ValueID iterated = call.opcode() == Opcode::IteratorOpen ? call.as<Opcode::IteratorOpen>().iterableOperand() : call.as<Opcode::AppendSpread>().iterableOperand();
        iterable = referenceOf(iterated);
        passedGenerator = has(Mechanism::DirectCalls) ? findPassedGenerator(call.id(), iterated, callOfGenerator, frameOfCall) : Reference();
        TypeID present = iterable && iterable.isValue() ? m_types.withoutNullish(typeOf(iterable)) : TypeID();
        if (passedGenerator)
            callee = m_code.at(passedGenerator.id()).function();
        else
            callee = has(Mechanism::DirectCalls) ? findMethodInSightOfProvenType(*this, present, keyOfMethod) : FunctionID();
        if (callee && !passedGenerator && callee != facts().linked().calleeAt(currentFunction(), call.id()) && !facts().linked().mayBeMentionedBy(unit(), callee))
            return false;
        if (m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& around) { return around.numberOfYields > 1 && around.frameOfTurn == frameIndex; }))
            return false;
    } else if (Reference closure = referenceOf(call.as<Opcode::Call>().calleeOperand()))
        callee = findOnlyCallee(*this, call, closure);
    if (!callee || isBeingWalkedOtherThanAtYield(callee))
        return false;
    const GeneratorFunctionDescribed& found = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.ensure(callee.index(), [&] {
        return describeGeneratorFunction(*this, callee);
    }).iterator->value;
    if (!found.bodyMayBeWalkedInConsumer)
        return false;
    if (!found.nobodyLooksAbove) {
        bool nobodyLooksAbove = nobodyLooksAboveGeneratorsOf(*this, callee, nullptr, scopedLambda<bool(const BodyIndex&, const LinkedCall&, FunctionID)>([&](const BodyIndex& caller, const LinkedCall& passed, FunctionID callerID) {
            return passesGeneratorForIteration(caller, passed.site, callerID);
        }));
        keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.find(callee.index())->value.nobodyLooksAbove = nobodyLooksAbove;
    }
    const GeneratorFunctionDescribed& described = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.find(callee.index())->value;
    if (!*described.nobodyLooksAbove || !engineAnswersForGenerators(*this))
        return false;
    const LinkTables& linked = facts().linked();
    if (!linked.function(callee).namesOnlyWhatEveryFunctionLeadsTo && !linked.leadsTo(unit(), callee))
        return false;
    if (m_code.inlineFrames().size() >= mostInlineFramesOfGeneratorsInLoops)
        return false;

    GeneratorTakenByConsumer taken;
    taken.inlineFrame = m_frames[frameIndex].inlineFrame;
    taken.taker = taker;
    taken.function = callee;
    taken.call = callOfGenerator;
    taken.frameOfCall = frameOfCall;
    taken.takerCallsMethodItself = takerCallsMethodItself && !passedGenerator;
    taken.numberOfYields = described.numberOfYields;
    uint64_t numberOfOperations = facts().summaries().at(callee).numberOfOperations;

    if (index.opcodeOf(taker) == Opcode::AppendSpread) {
        auto spread = index.value(taker).as<Opcode::AppendSpread>();
        if (!takerCallsMethodItself && (spread.iterableOperand() != call.id() || spread.arrayOperand() == call.id()))
            return false;
        taken.callPosition = index.value(taker).position();
    } else {
        auto opened = index.value(taker).as<Opcode::IteratorOpen>();
        if (opened.iteratorKind() != IteratorKind::Sync || opened.isDelegating() || opened.isDestructuring())
            return false;
        std::optional<LoopOverIterator> found = findLoopOverIterator(index, taker);
        if (!found || !index.value(found->step).as<Opcode::IteratorResume>().heldOperands().empty() || !blockOfStepOnlySteps(index, blockOfCall, taker, *found))
            return false;
        if (found->blockOfStep == blockOfCall.id() && found->step.index() - blockOfCall.firstValue().index() >= endOfStretchBeingWalked())
            return false;
        taken.step = found->step;
        taken.inputHead = found->blockOfStep;
        taken.callPosition = index.value(found->step).position();
        taken.hasBackEdge = found->stepStandsInHeadOfLoop;
        LoopID loop = taken.hasBackEdge ? index.loopOf(taken.inputHead) : LoopID();
        size_t numberOfRegionsBeingWalked = m_frames[frameIndex].references.size() > index.numberOfValues();
        for (const GeneratorWalkedInConsumer& around : m_generatorsWalkedInConsumers)
            numberOfRegionsBeingWalked += around.inputHead && (around.frameOfTurn == UINT32_MAX ? around.frameOfConsumer : around.frameOfTurn) == frameIndex;
        if (m_frames[frameIndex].loopsBeingVersioned.size() != numberOfRegionsBeingWalked)
            return false;

        if (!m_liveness.isComputedFor(index))
            m_liveness.compute(index);
        taken.region = collectRegionOfIteration(index, m_liveness, taken.inputHead, loop, taker);
        uint64_t numberOfOperationsOfRegion = 0;
        uint64_t numberOfCloses = 0;
        bool holdsWhatIsWalkedInPlaceOfItself = false;
        auto isInRegion = [&](BlockID block) {
            return std::ranges::binary_search(taken.region, block.index(), { }, [](BlockID each) { return each.index(); });
        };
        if (m_restArrayAllocator && m_frames[frameIndex].isOfUnit && isInRegion(m_restArrayAllocationBlock))
            return false;
        auto noteName = [&](ValueID named, auto...) {
            if (named && !isInRegion(index.blockOf(named)))
                taken.valuesThatRegionNamesFromOutside.append(named);
        };
        for (BlockID block : taken.region) {
            numberOfOperationsOfRegion += index.block(block).numberOfValues() + 1;
            BodyTerminator end = index.terminator(block);
            end.forEachOperand(noteName);
            for (BodyTarget target : end.targets())
                std::ranges::for_each(target.arguments(), noteName);
            if (index.block(block).hasHandler())
                std::ranges::for_each(index.block(block).handler().arguments(), noteName);
            for (BodyValue operation : index.block(block).values()) {
                operation.forEachOperand(noteName);
                Opcode opcode = operation.opcode();
                numberOfCloses += opcode == Opcode::IteratorClose && operation.as<Opcode::IteratorClose>().iteratorOperand() == taker;
                holdsWhatIsWalkedInPlaceOfItself |= opcode == Opcode::Yield;
                taken.regionHoldsIterations |= opcode == Opcode::Yield || opcode == Opcode::IteratorOpen || opcode == Opcode::AppendSpread;
                if ((opcode != Opcode::IteratorOpen && opcode != Opcode::AppendSpread) || operation.id() == taker)
                    continue;
                ValueID iterable = opcode == Opcode::IteratorOpen ? operation.as<Opcode::IteratorOpen>().iterableOperand() : operation.as<Opcode::AppendSpread>().iterableOperand();
                holdsWhatIsWalkedInPlaceOfItself |= iterable && !index.isParameter(iterable) && index.opcodeOf(iterable) == Opcode::Call;
            }
        }
        if (described.numberOfYields > 1 && holdsWhatIsWalkedInPlaceOfItself)
            return false;
        numberOfOperations += numberOfOperationsOfRegion * std::max(1u, described.numberOfYields) - numberOfOperationsOfRegion;
        taken.numberOfOperationsOfRegion = numberOfOperationsOfRegion;
        numberOfOperations += numberOfCloses * described.numberOfOperationsOnWaysOfReturn;

        std::ranges::sort(taken.valuesThatRegionNamesFromOutside, { }, &ValueID::index);
        taken.valuesThatRegionNamesFromOutside.shrink(std::ranges::unique(taken.valuesThatRegionNamesFromOutside).begin() - taken.valuesThatRegionNamesFromOutside.begin());

        ValueID firstValue = taken.hasBackEdge ? index.block(taken.inputHead).firstParameter() : ValueID(taken.step.index() + 1);
        ValueID endOfValues = firstValue;
        for (BlockID member : taken.region)
            endOfValues = std::max(endOfValues, index.block(member).endOfValues(), [](ValueID one, ValueID other) { return one.index() < other.index(); });
        auto isDefinedInRegion = [&](ValueID value) {
            return value.index() >= firstValue.index() && isInRegion(index.blockOf(value));
        };
        for (BlockID member : taken.region) {
            for (BodyTarget target : index.terminator(member).targets()) {
                if (isInRegion(target.block()))
                    continue;
                m_liveness.forEachLiveAtHead(target.block(), firstValue, endOfValues, [&](ValueID live) {
                    if (isDefinedInRegion(live) && !taken.valuesReadBehindRegion.contains(live))
                        taken.valuesReadBehindRegion.append(live);
                });
            }
        }
        for (BlockID member : taken.region) {
            BodyBlock block = index.block(member);
            for (uint32_t value = std::max(block.firstParameter().index(), firstValue.index()); value < block.endOfValues().index(); ++value) {
                uint32_t numberOutsideRegion = numberOfHandlersThatRead(m_frames[frameIndex], ValueID(value));
                if (!numberOutsideRegion)
                    continue;
                for (BlockID inside : taken.region)
                    numberOutsideRegion -= index.block(inside).isHandlerBlock() && m_liveness.isLiveAtHead(ValueID(value), inside);
                if (numberOutsideRegion)
                    return false;
            }
        }
    }
    if (!operationsFitWindowOfCallWrittenInLoop(*this, numberOfOperations))
        return false;

    Draft site = takerCallsMethodItself ? Draft(Opcode::Call, originOf(call)) : draftOf(call);
    if (takerCallsMethodItself && !passedGenerator) {
        if (m_types.at(typeOf(iterable)).primitives & Primitive::UndefinedOrNull) {
            Origin origin = originOf(call);
            origin.isTailCall = false;
            Draft check(Opcode::RequireObjectCoercible, origin);
            chooseConservativeFields(check);
            check.setValueOperand(iterable);
            check.setNullishAccess(call.opcode() == Opcode::AppendSpread ? NullishAccess::Spread : NullishAccess::Iterate);
            iterable = build(WTF::move(check));
            if (!iterable)
                return isUnreachable();
        }
        Reference method = readMethodInSightOfProvenObject(*this, iterable, keyOfMethod, originOf(call));
        if (!method || findOnlyCallee(*this, call, method) != callee)
            return false;
        chooseConservativeFields(site);
        site.setCalleeOperand(method);
        site.setThisValueOperand(iterable);
    }
    if (passedGenerator) {
        m_environmentCellsOfUnit.mayNameSome |= isHandedEnvironmentWithByte(*this, callee);
        for (ValueID operand : m_code.at(passedGenerator.id()).variadicOperands())
            taken.entryArguments.append(Reference::value(operand));
    } else {
        Vector<Reference, 8> passedOperands = collectPassedOperands(site, program().at(callee));
        if (hasFailed() || !callMayEnterCopyOfBody(*this, site, callee, passedOperands.span()))
            return false;

        m_environmentCellsOfUnit.mayNameSome |= isHandedEnvironmentWithByte(*this, callee);
        taken.entryArguments = buildEntryArgumentsOfCopy(*this, site, callee, passedOperands.span());
        if (isUnreachable())
            return true;
    }
    taken.state = addIndexedIteration(IndexedIteration { });

    if (!depthOfCopies())
        m_operationsCopiedAtWrittenCall = 0;
    m_operationsCopiedAtWrittenCall += static_cast<uint32_t>(std::min<uint64_t>(numberOfOperations, UINT32_MAX / 2));
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;
    Reference state = taken.state;
    m_generatorsTakenByConsumers.removeAllMatching([&](const GeneratorTakenByConsumer& earlier) {
        return earlier.inlineFrame == taken.inlineFrame && earlier.taker == taken.taker;
    });
    m_generatorsTakenByConsumers.append(WTF::move(taken));
    if (!takerCallsMethodItself) {
        bind(call.id(), state);
        return true;
    }
    if (!passedGenerator)
        count(StatisticID::GeneratorMethodsInlinedIntoIterations);
    if (call.opcode() == Opcode::AppendSpread)
        return spreadGeneratorTakenAt(m_generatorsTakenByConsumers.size() - 1, call);
    buildFirstReadsThatOpenOfGeneratorLeavesOut(*this, originOf(call), !!passedGenerator);
    if (!isUnreachable())
        bind(call.id(), state);
    return true;
}

bool Walker::isBeingWalkedOtherThanAtYield(FunctionID function) const
{
    for (size_t index = 0; index < m_frames.size(); ++index) {
        if (m_frames[index].function != function)
            continue;
        bool standsAtYield = m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) {
            return walked.frameOfGenerator == index && walked.yieldStoodAt;
        });
        if (!standsAtYield)
            return true;
    }
    return false;
}

bool Walker::leavesRegionOfIteration(size_t frameIndex, BlockID to) const
{
    return m_generatorsWalkedInConsumers.containsIf([&](const GeneratorWalkedInConsumer& walked) {
        if (!walked.inputHead || (walked.frameOfTurn == UINT32_MAX ? walked.frameOfConsumer : walked.frameOfTurn) != frameIndex)
            return false;
        return !std::ranges::binary_search(walked.region, to.index(), { }, [](BlockID each) { return each.index(); });
    });
}

size_t Walker::placeOfGeneratorTaken(ValueID taker, ValueID iterable) const
{
    uint32_t inlineFrame = m_frames.last().inlineFrame;
    size_t place = m_generatorsTakenByConsumers.findIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.taker == taker && taken.inlineFrame == inlineFrame;
    });
    if (place == notFound || referenceBoundTo(iterable) != m_generatorsTakenByConsumers[place].state)
        return notFound;
    return place;
}

bool Walker::openGeneratorTaken(const BodyValue& open)
{
    if (m_generatorsTakenByConsumers.isEmpty())
        return false;
    size_t place = placeOfGeneratorTaken(open.id(), open.as<Opcode::IteratorOpen>().iterableOperand());
    if (place == notFound)
        return false;
    Reference state = m_generatorsTakenByConsumers[place].state;
    buildFirstReadsThatOpenOfGeneratorLeavesOut(*this, originOf(open));
    if (!isUnreachable())
        bind(open.id(), state);
    return true;
}


void Walker::layFrameOfGenerator(FunctionID callee, uint32_t callPosition, uint32_t loopDepthAtCall, uint32_t numberOfCarriedValues)
{
    const Function& function = program().at(callee);
    const BodyIndex& body = facts().bodies().at(callee);
    Frame frame;
    frame.function = callee;
    frame.body = &body.body();
    frame.index = &body;
    frame.references.lay(body.numberOfValues() + numberOfCarriedValues);
    frame.loopDepthAtCall = loopDepthAtCall;
    frame.inlineFrame = m_code.addInlineFrame({
        .callee = callee,
        .callPosition = callPosition,
        .parent = m_frames.last().inlineFrame,
        .module = function.module,
    });
    for (uint32_t place = 0; place < numberOfCarriedValues; ++place)
        frame.addDuplicatedValue(ValueID(body.numberOfValues() + place));
    if (numberOfCarriedValues)
        frame.loopsBeingVersioned.append(LoopID(body.numberOfLoops()));
    m_frames.append(WTF::move(frame));
}

Vector<Reference, 4> Walker::carriedValuesInFrame(size_t frameIndex) const
{
    const Frame& frame = m_frames[frameIndex];
    Vector<Reference, 4> carried;
    for (uint32_t value = frame.index->numberOfValues(); value < frame.references.size(); ++value)
        carried.append(frame.references[value]);
    return carried;
}

Vector<Reference, 4> Walker::valuesThatLoopBringsRound(size_t frameIndex, BlockID inputHead) const
{
    const Frame& frame = m_frames[frameIndex];
    Vector<Reference, 4> carried;
    BodyBlock head = frame.index->block(inputHead);
    for (uint32_t parameter = 0; frame.index->isLoopHead(inputHead) && parameter < head.numberOfParameters(); ++parameter)
        carried.append(referenceBoundIn(frame, head.parameter(parameter)));
    carried.appendVector(carriedValuesInFrame(frameIndex));
    return carried;
}

void Walker::bindValuesThatLoopBringsRound(BlockID inputHead, std::span<const Reference> carried)
{
    const Frame& frame = m_frames.last();
    BodyBlock head = frame.index->block(inputHead);
    uint32_t numberOfParameters = frame.index->isLoopHead(inputHead) ? head.numberOfParameters() : 0;
    uint32_t firstCarried = frame.index->numberOfValues();
    for (uint32_t place = 0; place < carried.size(); ++place)
        bind(place < numberOfParameters ? head.parameter(place) : ValueID(firstCarried + place - numberOfParameters), carried[place]);
}

void Walker::bindInFrame(size_t frameIndex, ValueID input, Reference reference)
{
    Reference& bound = m_frames[frameIndex].references.placeOf<NamesOfValues::Writer::LogsWhatStoodThere>(input.index());
    if (m_numberOfClosedArms || m_trialVisitDepth)
        m_bindings.append(Binding { static_cast<uint32_t>(frameIndex), input, bound });
    bound = reference;
}

void Walker::moveFrame(size_t from, size_t to)
{
    RELEASE_ASSERT(m_hasFailed || (!m_versionedLoops.containsIf([&](const VersionedLoop& versionedLoop) { return versionedLoop.frameIndex == from; }) && !m_sharedGenericVersions.containsIf([&](const SharedGenericVersion& copy) { return copy.frameIndex == from; })));
    if (to == m_frames.size()) {
        size_t lowestNotAsked = from + 1;
        while (lowestNotAsked && m_frames[lowestNotAsked - 1].numberOfFramesUpToHandlerBelow == Frame::notAskedYet)
            --lowestNotAsked;
        for (size_t index = lowestNotAsked; index <= from; ++index)
            m_frames[index].numberOfFramesUpToHandlerBelow = !index ? 0 : m_frames[index - 1].hasHandler ? static_cast<uint32_t>(index) : m_frames[index - 1].numberOfFramesUpToHandlerBelow;
        Frame moved = WTF::move(m_frames[from]);
        m_frames[from].hasHandler = false;
        m_frames[from].isOfUnit = false;
        m_frames.append(WTF::move(moved));
    } else {
        Frame moved = WTF::move(m_frames[from]);
        RELEASE_ASSERT(from + 1 == m_frames.size());
        m_frames.removeLast();
        m_frames[to] = WTF::move(moved);
    }
    for (GeneratorWalkedInConsumer& walked : m_generatorsWalkedInConsumers) {
        if (walked.frameOfGenerator == from)
            walked.frameOfGenerator = static_cast<uint32_t>(to);
        if (walked.frameOfTurn == from)
            walked.frameOfTurn = static_cast<uint32_t>(to);
    }
    for (size_t place = 0; place < m_stringsInBuilders.size(); ++place) {
        if (std::as_const(m_stringsInBuilders)[place].frameIndex == from)
            m_stringsInBuilders[place].frameIndex = static_cast<uint32_t>(to);
    }
    for (size_t& resumption : m_resumptionsToLeave) {
        if (resumption == from)
            resumption = to;
    }
    for (ExpectationOfArgument& expected : m_expectedOfArgumentsOfCopies) {
        if (expected.frameOfCopy == from)
            expected.frameOfCopy = static_cast<uint32_t>(to);
    }
    for (size_t place = 0; place < m_openLoops.size(); ++place) {
        if (m_openLoops[place].frame == from)
            m_openLoops.changeable(place).frame = static_cast<uint32_t>(to);
    }
    renameFrameInTries(from, to);
}


void Walker::forgetRegionOfIteration(size_t frameIndex, const GeneratorWalkedInConsumer& walked)
{
    const BodyIndex& index = *m_frames[frameIndex].index;
    for (BlockID member : walked.region) {
        inputBlock(frameIndex, member) = InputBlock { };
        BodyBlock block = index.block(member);
        for (uint32_t value = member == walked.inputHead ? walked.step.index() + 1 : block.firstParameter().index(); value < block.endOfValues().index(); ++value)
            bindInFrame(frameIndex, ValueID(value), Reference());
    }
    inputBlock(frameIndex, walked.inputHead).firstNarrowerName = walked.namesAtHead;
    uint32_t numberInFront = m_numberOfIterationsInFrontOfTheirLoops;
    for (size_t place = m_indexedIterations.size(); place-- && numberInFront;) {
        const IndexedIteration& iteration = std::as_const(m_indexedIterations)[place];
        if (iteration.head || !iteration.inputHead)
            continue;
        --numberInFront;
        if (iteration.inlineFrame != m_frames[frameIndex].inlineFrame || !std::ranges::binary_search(walked.region, iteration.inputHead.index(), { }, [](BlockID each) { return each.index(); }))
            continue;
        m_indexedIterations[place].inputHead = { };
        --m_numberOfIterationsInFrontOfTheirLoops;
    }
    restoreBindingsBeforeLoop(walked);
}

void Walker::restoreBindingsBeforeLoop(const GeneratorWalkedInConsumer& walked)
{
    for (auto [value, name] : walked.bindingsBeforeLoop) {
        if (referenceBoundIn(m_frames.last(), value) != name)
            bind(value, name);
    }
}

void Walker::walkHeadOfLoopBehindStep(BlockID inputHead, ValueID step, std::span<const Reference> carried, bool isDone, Reference stepValue)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock head = index.block(inputHead);
    m_frames[frameIndex].block = { };
    setCurrentInputBlock(inputHead);
    m_origin = originOf(index.value(step));
    m_origin.isTailCall = false;

    bindValuesThatLoopBringsRound(inputHead, carried);
    Draft answer(Opcode::Boolean, m_origin);
    answer.setBoolean(isDone);
    answer.representation = Representation::JSValue;
    Reference done = constant(WTF::move(answer));
    for (uint32_t number = step.index() + 1; number < head.endOfValues().index(); ++number) {
        bool readsDone = index.value(ValueID(number)).as<Opcode::ResultAt>().index() == static_cast<uint32_t>(IteratorResumeResult::Done);
        bind(ValueID(number), readsDone ? done : stepValue);
        storeValueBehindItsDefinition(ValueID(number));
    }
    if (head.hasHandler())
        noteStateAtHeadForHandler();
    m_frames[frameIndex].walkedUpTo = head.numberOfValues();
    walkTerminator(head.terminator());
}

Reference Walker::resumptionOfGenerator(Reference sent, ResumeMode mode, Origin origin)
{
    if (!sent) {
        Draft nothing(Opcode::Undefined, origin);
        nothing.representation = Representation::JSValue;
        sent = constant(WTF::move(nothing));
    }
    Draft written(Opcode::Integer, origin);
    written.setInteger(static_cast<int64_t>(mode));
    written.representation = Representation::JSValue;
    Draft results(Opcode::Results, origin);
    chooseConservativeFields(results);
    results.setValuesOperands(std::array { sent, constant(WTF::move(written)) });
    return build(WTF::move(results));
}

bool Walker::walkYieldOfGeneratorInConsumer(const BodyValue& yield)
{
    if (!m_resumptionsToLeave.isEmpty() && m_resumptionsToLeave.last() == m_frames.size() - 1) {
        Frame& frame = m_frames.last();
        frame.returnsOfCopy->append(Frame::ReturnOfCopy { m_block, referenceOf(yield.as<Opcode::Yield>().valueOperand()), WTF::move(m_state), frame.hasHandler, !m_opensGuesses });
        m_block = { };
        m_state.clear();
        return true;
    }
    size_t place = placeOfGeneratorOnTop();
    if (place == notFound)
        return false;

    Origin originAtYield = m_origin;
    originAtYield.isTailCall = false;
    Reference yielded = referenceOf(yield.as<Opcode::Yield>().valueOperand());
    if (!yielded || !yielded.isRecord()) {
        fail("a generator that is walked in its consumer yields a value that nothing was built for"_s);
        return true;
    }

    if (Reference list = m_generatorsWalkedInConsumers[place].list) {
        Draft append(Opcode::AppendElement, originAtYield);
        chooseConservativeFields(append);
        append.setArrayOperand(list);
        append.setValueOperand(yielded);
        build(WTF::move(append));
        if (Reference resumption = isUnreachable() ? Reference() : resumptionOfGenerator({ }, ResumeMode::Next, originAtYield))
            bind(yield.id(), resumption);
        return true;
    }

    const size_t frameOfGenerator = m_frames.size() - 1;
    const size_t frameOfConsumer = m_generatorsWalkedInConsumers[place].frameOfConsumer;
    const size_t frameOfTurn = m_frames.size();
    const BlockID inputHead = m_generatorsWalkedInConsumers[place].inputHead;
    const ValueID step = m_generatorsWalkedInConsumers[place].step;
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfGenerator);

    bool generatorStoodUnderHandler = std::exchange(m_frames[frameOfGenerator].hasHandler, false);
    m_generatorsWalkedInConsumers[place].yieldStoodAt = yield.id();
    moveFrame(frameOfConsumer, frameOfTurn);
    m_generatorsWalkedInConsumers[place].frameOfTurn = static_cast<uint32_t>(frameOfTurn);
    RELEASE_ASSERT(!m_frames[frameOfTurn].loopsBeingVersioned.isEmpty());
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = true;
    forgetRegionOfIteration(frameOfTurn, m_generatorsWalkedInConsumers[place]);

    size_t numberOfLoopsAround = m_openLoops.size();
    m_generatorsWalkedInConsumers[place].numberOfLoopsAroundTurn = static_cast<uint32_t>(numberOfLoopsAround);
    walkHeadOfLoopBehindStep(inputHead, step, carried.span(), false, yielded);
    for (size_t number = 0; number < m_generatorsWalkedInConsumers[place].region.size() && !m_hasFailed; ++number) {
        BlockID member = m_generatorsWalkedInConsumers[place].region[number];
        if (member != inputHead)
            walkBlock(member);
    }
    endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    m_block = { };
    m_state.clear();

    bool hasBackEdge = !m_hasFailed && inputBlock(frameOfTurn, inputHead).firstOpenEdge != noOpenEdge;
    if (hasBackEdge) {
        m_frames[frameOfTurn].block = { };
        setCurrentInputBlock(inputHead);
        m_origin = originOf(m_frames[frameOfTurn].index->value(step));
        m_origin.isTailCall = false;
        Vector<uint32_t, 4> edges = takeOpenEdges(inputHead);
        closeEdgesInto(addBlockFor(inputHead), inputHead, edges.span(), false);
        carried = valuesThatLoopBringsRound(frameOfTurn, inputHead);
    }
    restoreBindingsBeforeLoop(m_generatorsWalkedInConsumers[place]);
    m_frames[frameOfTurn].block = { };
    setCurrentInputBlock(inputHead);
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = false;
    m_generatorsWalkedInConsumers[place].frameOfTurn = UINT32_MAX;
    moveFrame(frameOfTurn, frameOfConsumer);
    m_generatorsWalkedInConsumers[place].yieldStoodAt = { };
    m_frames[frameOfGenerator].hasHandler = generatorStoodUnderHandler;
    m_origin = originAtYield;
    if (m_hasFailed || !hasBackEdge) {
        m_block = { };
        m_state.clear();
        return true;
    }

    uint32_t firstCarried = m_frames[frameOfGenerator].index->numberOfValues();
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(firstCarried + number), carried[number]);
    if (Reference resumption = resumptionOfGenerator({ }, ResumeMode::Next, originAtYield))
        bind(yield.id(), resumption);
    return true;
}


bool Walker::loopCarriesValuesOfConsumerRound(BlockID inputHead) const
{
    const BodyIndex& body = currentBodyIndex();
    return placeOfGeneratorOnTop() != notFound && loopHoldsYield(body, body.loopOf(inputHead));
}

bool Walker::isConsumerSmallEnoughToVersion(BlockID inputHead, uint64_t largestLoop) const
{
    const GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[placeOfGeneratorOnTop()];
    if (walked.regionHoldsIterations)
        return false;
    const BodyIndex& body = currentBodyIndex();
    uint64_t numberOfOperations = 0;
    for (BlockID member : body.membersOf(body.loopOf(inputHead))) {
        numberOfOperations += body.block(member).numberOfValues() + 1;
        for (BodyValue operation : body.block(member).values()) {
            if (operation.opcode() == Opcode::IteratorOpen || operation.opcode() == Opcode::AppendSpread)
                return false;
            numberOfOperations += operation.opcode() == Opcode::Yield ? walked.numberOfOperationsOfRegion : 0;
        }
    }
    return numberOfOperations <= largestLoop;
}

void Walker::carryValuesRoundLoopOfGenerator(BlockID inputHead, BlockID head, Vector<Reference, 8>& entering, Vector<ValueID, 8>& parameters)
{
    if (!loopCarriesValuesOfConsumerRound(inputHead))
        return;
    size_t place = placeOfGeneratorOnTop();
    const BodyIndex& body = currentBodyIndex();
    GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[place];
    for (uint32_t number = 0; number < walked.typesOfCarriedValues.size(); ++number) {
        ValueID name(body.numberOfValues() + number);
        Reference enters = referenceBoundTo(name);
        TypeID arriving = enters ? ownTypeOf(enters) : TypeID();
        TypeID type = typeInRowOfValueCarriedRound(inputHead, parameters.size(), arriving ? arriving : TypeID(TypeTable::anything));
        entering.append(enters);
        parameters.append(m_code.appendParameter(head, type, formOfParameter(type, { }), m_origin));
        bind(name, Reference::value(parameters.last()));
    }
    walked.headsThatCarryValues.append(head);
    walked.inputHeadsThatCarryValues.append(inputHead);
}

void Walker::appendCarriedValuesThatComeRound(BlockID head, Vector<Reference, 8>& arguments)
{
    size_t place = placeOfGeneratorOnTop();
    if (place == notFound)
        return;
    size_t number = m_generatorsWalkedInConsumers[place].headsThatCarryValues.find(head);
    if (number == notFound)
        return;
    arguments.appendVector(carriedValuesInFrame(m_frames.size() - 1));
}

bool Walker::takeCarriedValuesOntoHandlerEdge(size_t frameIndex, Vector<std::pair<ValueID, Reference>, 2>& names)
{
    const Frame& frame = m_frames[frameIndex];
    for (uint32_t value = frame.index->numberOfValues(); value < frame.references.size(); ++value) {
        Reference reference = m_frames[frameIndex].references[value];
        TypeID type = ownTypeOf(reference);
        if (!type) {
            fail("a handler edge hands on a carried value that nothing was built for"_s);
            return false;
        }
        Representation form = boxedFormOf(type);
        if (!isSameWord(representationOf(reference), form))
            reference = convert(reference, form, m_origin);
        if (!reference)
            return false;
        names.append({ ValueID(value), reference });
    }
    return true;
}

void Walker::bindParametersOfHeadForHandlerOfStep(size_t frameIndex)
{
    for (size_t place = m_generatorsWalkedInConsumers.size(); place--;) {
        const GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[place];
        if (walked.frameOfConsumer < frameIndex || walked.frameOfTurn != UINT32_MAX || walked.frameOfGenerator >= m_frames.size())
            continue;
        Frame& frame = m_frames[walked.frameOfConsumer];
        const Frame& generator = m_frames[walked.frameOfGenerator];
        uint32_t numberOfParameters = walked.inputHead && frame.index->isLoopHead(walked.inputHead) ? frame.index->block(walked.inputHead).numberOfParameters() : 0;
        uint32_t firstCarried = generator.index->numberOfValues();
        for (uint32_t number = 0; firstCarried + number < generator.references.size(); ++number) {
            uint32_t name = number < numberOfParameters ? frame.index->block(walked.inputHead).parameter(number).index() : frame.index->numberOfValues() + number - numberOfParameters;
            if (frame.duplicatedValues)
                frame.duplicatedValues->noteBoundOnWay(ValueID(name), generator.references[firstCarried + number]);
            bindInFrame(walked.frameOfConsumer, ValueID(name), generator.references[firstCarried + number]);
        }
    }
}

void Walker::nameCarriedValuesOnEdge(OpenEdge& open, size_t frameIndex)
{
    Frame& frame = m_frames[frameIndex];
    for (uint32_t value = frame.index->numberOfValues(); value < frame.references.size(); ++value)
        frame.duplicatedValues->nameOtherwiseOn(open.names, ValueID(value), frame.references[value]);
}

BlockID Walker::handlerOfCodeInForce(size_t numberOfFramesUpToHandler) const
{
    for (size_t index = m_frames.size(); index-- > numberOfFramesUpToHandler;) {
        if (BlockID handler = m_frames[index].handlerOfCodeBelow)
            return handler;
    }
    return { };
}

void Walker::noteCarriedValuesAtReturnOfGenerator()
{
    size_t place = placeOfGeneratorOnTop();
    if (place != notFound)
        m_generatorsWalkedInConsumers[place].carriedAtReturns.append(carriedValuesInFrame(m_frames.size() - 1));
}


Vector<Reference, 4> Walker::walkBodyOfGeneratorToItsEnd(size_t place, Origin originInFront)
{
    const BodyIndex& body = currentBodyIndex();
    Vector<Frame::ReturnOfCopy, 2> returns;
    m_frames.last().returnsOfCopy = &returns;
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFailed; ++number)
        walkBlock(BlockID(number));
    m_block = { };
    m_state.clear();
    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_frames.removeLast();
    m_origin = originInFront;

    Vector<Vector<Reference, 4>, 2> carriedAtReturns = WTF::move(m_generatorsWalkedInConsumers[place].carriedAtReturns);
    if (m_hasFailed || returns.isEmpty())
        return { };
    if (carriedAtReturns.size() != returns.size()) {
        fail("a return of a generator that is walked in its consumer was not noted"_s);
        return { };
    }
    if (returns.size() == 1 && !returns[0].isUnderHandlerOfCallee) {
        m_block = returns[0].block;
        m_state = WTF::move(returns[0].state);
        return WTF::move(carriedAtReturns[0]);
    }

    BlockID ended = addBlock();
    Vector<Reference, 4> carried;
    Vector<Representation, 2> forms;
    for (size_t number = 0; number < carriedAtReturns[0].size(); ++number) {
        TypeID type;
        forms.shrink(0);
        for (const Vector<Reference, 4>& atReturn : carriedAtReturns) {
            TypeID arriving = ownTypeOf(atReturn[number]);
            type = type ? joinTypes(type, arriving) : arriving;
            forms.append(representationOf(atReturn[number]));
        }
        carried.append(addParameter(ended, type, formOfParameter(type, forms.span())));
    }
    for (size_t number = 0; number < returns.size() && !m_hasFailed; ++number) {
        m_block = returns[number].block;
        m_state = WTF::move(returns[number].state);
        jumpTo(ended, carriedAtReturns[number].span());
        if (number + 1 < returns.size())
            leaveBlockPending();
    }
    return carried;
}

bool Walker::walkGeneratorInPlaceOfLoop(BlockID inputHead, std::span<const TypeID> typesOfParameters, std::span<const Reference> entering)
{
    const size_t frameOfConsumer = m_frames.size() - 1;
    uint32_t inlineFrameOfConsumer = m_frames[frameOfConsumer].inlineFrame;
    size_t placeOfTaken = m_generatorsTakenByConsumers.findIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.inputHead == inputHead && taken.inlineFrame == inlineFrameOfConsumer;
    });
    if (placeOfTaken == notFound)
        return false;
    GeneratorTakenByConsumer taken = WTF::move(m_generatorsTakenByConsumers[placeOfTaken]);
    m_generatorsTakenByConsumers.removeAt(placeOfTaken);
    if (referenceBoundTo(taken.taker) != taken.state)
        return false;

    const BodyIndex& index = *m_frames[frameOfConsumer].index;
    Origin originInFront = m_origin;

    GeneratorWalkedInConsumer walked;
    walked.frameOfConsumer = static_cast<uint32_t>(frameOfConsumer);
    walked.frameOfGenerator = static_cast<uint32_t>(frameOfConsumer + 1);
    walked.call = taken.call;
    walked.frameOfCall = taken.frameOfCall;
    walked.inputHead = inputHead;
    walked.step = taken.step;
    walked.state = taken.state;
    walked.numberOfYields = taken.numberOfYields;
    walked.numberOfOperationsOfRegion = taken.numberOfOperationsOfRegion;
    walked.regionHoldsIterations = taken.regionHoldsIterations;
    walked.region = WTF::move(taken.region);
    walked.namesAtHead = inputBlock(frameOfConsumer, inputHead).firstNarrowerName;
    for (ValueID value : taken.valuesThatRegionNamesFromOutside) {
        if (Reference name = referenceBoundIn(m_frames[frameOfConsumer], value))
            walked.bindingsBeforeLoop.append({ value, name });
    }
    for (size_t parameter = 0; parameter < entering.size(); ++parameter) {
        TypeID type = typesOfParameters[parameter];
        TypeID arriving = ownTypeOf(entering[parameter]);
        if (!type || !arriving || !m_types.isSubset(arriving, type)) {
            fail("what enters a loop over a generator lies outside the type of the head's parameter"_s);
            return true;
        }
        walked.typesOfCarriedValues.append(type);
    }
    for (const GeneratorWalkedInConsumer& around : m_generatorsWalkedInConsumers) {
        if (around.frameOfGenerator == frameOfConsumer)
            walked.typesOfCarriedValues.appendVector(around.typesOfCarriedValues);
    }
    Vector<Reference, 4> carried = valuesThatLoopBringsRound(frameOfConsumer, inputHead);
    if (carried.size() != walked.typesOfCarriedValues.size()) {
        fail("a loop over a generator brings round other values than it has types for"_s);
        return true;
    }

    {
        Frame& frame = m_frames[frameOfConsumer];
        for (ValueID value : taken.valuesReadBehindRegion)
            frame.addDuplicatedValue(value);
        frame.loopsBeingVersioned.append(taken.hasBackEdge ? index.loopOf(inputHead) : LoopID(index.numberOfLoops()));
        frame.isInPreheader = false;
    }

    layFrameOfGenerator(taken.function, taken.callPosition, m_frames[frameOfConsumer].loopDepthAtCall + index.loopDepthOf(inputHead) - taken.hasBackEdge, static_cast<uint32_t>(carried.size()));
    m_generatorsWalkedInConsumers.append(WTF::move(walked));
    const size_t place = m_generatorsWalkedInConsumers.size() - 1;

    const BodyIndex& body = currentBodyIndex();
    BodyBlock entry = body.block(entryBlock);
    for (uint32_t parameter = 0; parameter < taken.entryArguments.size(); ++parameter)
        bind(entry.parameter(parameter), taken.entryArguments[parameter]);
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(body.numberOfValues() + number), carried[number]);

    carried = walkBodyOfGeneratorToItsEnd(place, originInFront);
    if (!m_hasFailed && !isUnreachable()) {
        forgetRegionOfIteration(frameOfConsumer, m_generatorsWalkedInConsumers[place]);
        Draft nothing(Opcode::Undefined, m_origin);
        nothing.representation = Representation::JSValue;
        walkHeadOfLoopBehindStep(inputHead, taken.step, carried.span(), true, constant(WTF::move(nothing)));
    }

    forgetRegionOfIteration(frameOfConsumer, m_generatorsWalkedInConsumers[place]);
    for (BlockID member : m_generatorsWalkedInConsumers[place].region) {
        inputBlock(frameOfConsumer, member).firstNarrowerName = m_generatorsWalkedInConsumers[place].namesAtHead;
        inputBlock(frameOfConsumer, member).isEntered = true;
    }
    m_frames[frameOfConsumer].loopsBeingVersioned.removeLast();
    m_generatorsWalkedInConsumers.removeLast();
    m_block = { };
    m_state.clear();
    return true;
}

bool Walker::stepGeneratorTakenOutsideLoops(const BodyValue& step)
{
    if (m_generatorsTakenByConsumers.isEmpty())
        return false;
    uint32_t inlineFrame = m_frames.last().inlineFrame;
    if (!m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) { return taken.step == step.id() && taken.inlineFrame == inlineFrame && !taken.hasBackEdge; }))
        return false;
    BlockID blockOfStep = m_frames.last().block;
    return walkGeneratorInPlaceOfLoop(blockOfStep, { }, { });
}

bool Walker::spreadGeneratorTaken(const BodyValue& spread)
{
    if (m_generatorsTakenByConsumers.isEmpty())
        return false;
    size_t placeOfTaken = placeOfGeneratorTaken(spread.id(), spread.as<Opcode::AppendSpread>().iterableOperand());
    return placeOfTaken != notFound && spreadGeneratorTakenAt(placeOfTaken, spread);
}

bool Walker::spreadGeneratorTakenAt(size_t placeOfTaken, const BodyValue& spread)
{
    GeneratorTakenByConsumer taken = WTF::move(m_generatorsTakenByConsumers[placeOfTaken]);
    m_generatorsTakenByConsumers.removeAt(placeOfTaken);

    const size_t frameOfConsumer = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameOfConsumer].index;
    Origin origin = originOf(spread);
    origin.isTailCall = false;
    Reference list = referenceOf(spread.as<Opcode::AppendSpread>().arrayOperand());
    if (!list || !list.isRecord()) {
        fail("a spread appends to a list that nothing was built for"_s);
        return true;
    }
    buildFirstReadsThatOpenOfGeneratorLeavesOut(*this, origin, !taken.takerCallsMethodItself);
    if (isUnreachable())
        return true;

    GeneratorWalkedInConsumer walked;
    walked.frameOfConsumer = static_cast<uint32_t>(frameOfConsumer);
    walked.frameOfGenerator = static_cast<uint32_t>(frameOfConsumer + 1);
    walked.call = taken.call;
    walked.frameOfCall = taken.frameOfCall;
    walked.state = taken.state;
    walked.list = list;
    walked.numberOfYields = taken.numberOfYields;
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfConsumer);
    for (const GeneratorWalkedInConsumer& around : m_generatorsWalkedInConsumers) {
        if (around.frameOfGenerator == frameOfConsumer)
            walked.typesOfCarriedValues.appendVector(around.typesOfCarriedValues);
    }

    layFrameOfGenerator(taken.function, taken.callPosition, m_frames[frameOfConsumer].loopDepthAtCall + index.loopDepthOf(index.blockOf(spread.id())), static_cast<uint32_t>(carried.size()));
    m_generatorsWalkedInConsumers.append(WTF::move(walked));
    const size_t place = m_generatorsWalkedInConsumers.size() - 1;
    const BodyIndex& body = currentBodyIndex();
    BodyBlock entry = body.block(entryBlock);
    for (uint32_t parameter = 0; parameter < taken.entryArguments.size(); ++parameter)
        bind(entry.parameter(parameter), taken.entryArguments[parameter]);
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(body.numberOfValues() + number), carried[number]);

    carried = walkBodyOfGeneratorToItsEnd(place, origin);
    m_generatorsWalkedInConsumers.removeLast();
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = facts().linked().calleeAt(currentFunction(), spread.id());
    if (m_hasFailed || isUnreachable())
        return true;
    uint32_t firstCarried = index.numberOfValues();
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(firstCarried + number), carried[number]);
    bind(spread.id(), list);
    return true;
}


bool Walker::closeGeneratorTaken(const BodyValue& close)
{
    if (m_generatorsWalkedInConsumers.isEmpty())
        return false;
    Reference state = referenceBoundTo(close.as<Opcode::IteratorClose>().iteratorOperand());
    if (!state || !state.isVirtual())
        return false;
    size_t place = m_generatorsWalkedInConsumers.size();
    while (place && m_generatorsWalkedInConsumers[place - 1].state != state)
        --place;
    if (!place--)
        return false;
    if (!m_generatorsWalkedInConsumers[place].yieldStoodAt) {
        fail("a loop over a generator is closed where the generator stands at no yield"_s);
        return true;
    }

    Origin origin = originOf(close);
    origin.isTailCall = false;
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = facts().linked().calleeAt(currentFunction(), close.id());
    buildFirstReadThatCloseOfGeneratorLeavesOut(*this, origin);
    if (!isUnreachable())
        resumeGeneratorToLeave(place, ResumeMode::Return, close.as<Opcode::IteratorClose>().completion() == IteratorCompletion::Throw, origin);
    return true;
}

void Walker::resumeGeneratorToLeave(size_t place, ResumeMode mode, bool dropsWhatIsThrown, Origin origin)
{
    const size_t frameOfCaller = m_frames.size() - 1;
    const ValueID yield = m_generatorsWalkedInConsumers[place].yieldStoodAt;
    const BodyIndex& indexOfCaller = *m_frames[frameOfCaller].index;

    BlockID handler;
    if (dropsWhatIsThrown) {
        handler = m_code.addBlock(BlockRole::CatchHandler, BlockFrequency::Rare);
        m_code.setLoopMarks(handler, isInLoop(), false);
        jumpTo(addBlock());
    }

    {
        const Frame& suspended = m_frames[m_generatorsWalkedInConsumers[place].frameOfGenerator];
        Frame frame;
        frame.function = suspended.function;
        frame.body = suspended.body;
        frame.index = suspended.index;
        frame.references = suspended.references;
        frame.references.shrink(suspended.index->numberOfValues());
        if (suspended.duplicatedValues) {
            for (uint32_t number = 0; number < suspended.duplicatedValues->slots.size(); ++number) {
                const Frame::DuplicatedValues::Slot& slot = suspended.duplicatedValues->slots[number];
                if (slot.value.index() < frame.references.size())
                    frame.references.placeOf<NamesOfValues::Writer::LaysFrame>(slot.value.index()) = referenceBoundIn(suspended, slot.value);
            }
        }
        frame.loopDepthAtCall = m_frames[frameOfCaller].loopDepthAtCall + indexOfCaller.loopDepthOf(m_frames[frameOfCaller].block);
        frame.inlineFrame = suspended.inlineFrame;
        frame.handlerOfCodeBelow = handler;
        if (handler)
            frame.numberOfFramesUpToHandlerBelow = static_cast<uint32_t>(m_frames.size());
        m_frames.append(WTF::move(frame));
    }
    m_numberOfFramesOverHandlersOfCode += !!handler;
    m_resumptionsToLeave.append(m_frames.size() - 1);

    const BodyIndex& body = currentBodyIndex();
    BodyBlock block = body.block(body.blockOf(yield));
    Vector<Frame::ReturnOfCopy, 2> returns;
    m_frames.last().returnsOfCopy = &returns;
    inputBlock(m_frames.size() - 1, block.id()).isEntered = true;
    inputBlock(m_frames.size() - 1, block.id()).firstNarrowerName = noNarrowerName;
    setCurrentInputBlock(block.id());
    m_origin = origin;
    m_origin.inlineFrame = m_frames.last().inlineFrame;
    if (Reference resumption = resumptionOfGenerator({ }, mode, m_origin))
        bind(yield, resumption);
    if (block.hasHandler())
        noteStateAtHeadForHandler();
    walkStretch(block.id(), yield.index() + 1 - block.firstValue().index(), block.numberOfValues());
    walkTerminator(block.terminator());
    for (uint32_t number = block.id().index() + 1; number < body.numberOfBlocks() && !m_hasFailed; ++number)
        walkBlock(BlockID(number));
    m_block = { };
    m_state.clear();
    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_resumptionsToLeave.removeLast();
    m_numberOfFramesOverHandlersOfCode -= !!handler;
    m_frames.removeLast();
    m_origin = origin;
    if (m_hasFailed)
        return;

    BlockID after = addBlock();
    bool someWayArrives = false;
    for (Frame::ReturnOfCopy& returned : returns) {
        if (m_hasFailed)
            return;
        leaveBlockPending();
        m_block = returned.block;
        m_state = WTF::move(returned.state);
        jumpTo(after);
        someWayArrives = true;
    }
    if (handler && m_code.at(handler).firstEdgeIn()) {
        continueIn(handler);
        m_state.clear();
        Draft caught(Opcode::CatchValue, origin);
        chooseConservativeFields(caught);
        build(WTF::move(caught));
        jumpTo(after);
        m_state.clear();
        someWayArrives = true;
    } else if (handler)
        m_code.end(handler, TerminatorDraft(TerminatorKind::Unreachable, origin), 0);
    if (!someWayArrives) {
        m_block = { };
        m_state.clear();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
