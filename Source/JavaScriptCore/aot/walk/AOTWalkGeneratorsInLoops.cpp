/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */


#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBitVector.h"
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
#include "AOTWalkKnown.h"
#include "AOTWalkLoopsOverGenerators.h"
#include "AOTWalkLoopsOverGeneratorsBuilder.h"
#include "AOTWalkResultsInParts.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

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
    bool isQuietBeforeInitialYield { false };
    BlockID blockOfInitialYield;
    bool hasOneWay { false };
};

struct KeptByGeneratorsInLoops final : Walker::KeptForBuild {
    HashMap<uint32_t, GeneratorFunctionDescribed, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functions;
    HashMap<uint64_t, ValueID, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> opensOfPassedGenerators;
    std::optional<BitVector> filesWithGenerators;
    BitVector functionsCheckedForManualIteration;
    BitVector functionsWithLoopsThatStepByHand;
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

static bool returnsAtOnceAtYield(const BodyIndex& body, ValueID yield)
{
    BodyBlock block = body.block(body.blockOf(yield));
    std::optional<uint32_t> target = block.hasHandler() ? std::nullopt : targetOfReturnAtYield(body, block, yield);
    if (!target)
        return false;
    BodyBlock left = body.block(block.terminator().targets()[*target].block());
    return !left.numberOfValues() && left.terminator().kind() == TerminatorKind::Return;
}

static bool yieldsFirst(BodyBlock block)
{
    for (BodyValue operation : block.values()) {
        if (operation.opcode() == Opcode::Yield)
            return true;
        if (operation.opcode() != Opcode::LoopHint)
            return false;
    }
    return false;
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
        if (to == blockOfYield)
            return yieldsFirst(body.block(to));
        if (to.index() < blockOfYield.index())
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

static bool isPureForGenerators(const BodyValue& operation)
{
    return declaredBoundInTable(operation.opcode()).isPure() && !operandsMayRunCode(operation.opcode());
}

static constexpr bool callsCode(Opcode opcode)
{
    return opcode == Opcode::Call || opcode == Opcode::CallDirect || opcode == Opcode::CallList || opcode == Opcode::Construct || opcode == Opcode::ConstructList;
}

static constexpr uint32_t maxStepsInRow = 4;
static constexpr uint64_t mostOperationsOfUnrolledSteps = 128;
static constexpr uint64_t maxLargeGeneratorOperations = 1024;
static bool loopHoldsYield(const BodyIndex&, LoopID);

static bool isDecidedBranch(const BodyIndex& body, BodyBlock block)
{
    ValueID condition = body.stripCasts(block.terminator().operand(0));
    if (!body.isParameter(condition) && body.opcodeOf(condition) == Opcode::ToBoolean)
        condition = body.stripCasts(body.value(condition).operand(0));
    return !body.isParameter(condition) && body.opcodeOf(condition) == Opcode::Boolean;
}

static bool bindsParametersUnseen(const BodyIndex& body)
{
    BitVector isSeen(body.numberOfBlocks());
    Vector<BlockID, 8> worklist { entryBlock };
    while (!worklist.isEmpty()) {
        BodyBlock block = body.block(worklist.takeLast());
        if (isSeen.set(block.id().index()))
            continue;
        if (block.hasHandler() || body.isLoopHead(block.id()))
            return false;
        bool yields = false;
        for (BodyValue operation : block.values()) {
            yields = operation.opcode() == Opcode::InitialYield;
            if (yields)
                break;
            if (!isPureForGenerators(operation))
                return false;
        }
        if (yields)
            continue;
        BodyTerminator terminator = block.terminator();
        if (terminator.kind() != TerminatorKind::Jump && terminator.kind() != TerminatorKind::Branch)
            return false;
        for (BodyTarget target : terminator.targets())
            worklist.append(target.block());
    }
    return true;
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

    described.isQuietBeforeInitialYield = bindsParametersUnseen(*body);
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyBlock block = body->block(BlockID(number));
        if (!body->isReachable(block.id()))
            continue;
        for (BodyValue operation : block.values()) {
            if (operation.opcode() == Opcode::InitialYield)
                described.blockOfInitialYield = block.id();
            if (operation.opcode() != Opcode::Yield)
                continue;
            std::optional<uint32_t> targetOfReturn = targetOfReturnAtYield(*body, block, operation.id());
            if (!targetOfReturn || !wayOfReturnLeadsForward(*body, block.id(), block.terminator().targets()[*targetOfReturn], described.numberOfOperationsOnWaysOfReturn))
                return described;
            ++described.numberOfYields;
        }
    }
    described.bodyMayBeWalkedInConsumer = true;
    described.hasOneWay = body->numberOfLoops() <= 1 && (!body->hasLoops() || (loopHoldsYield(*body, LoopID(0u)) && walker.facts().summaries().at(id).numberOfOperations * maxStepsInRow <= mostOperationsOfUnrolledSteps));
    for (uint32_t number = 0; number < body->numberOfBlocks(); ++number) {
        BodyBlock block = body->block(BlockID(number));
        if (!body->isReachable(block.id()) || (described.isQuietBeforeInitialYield && !body->dominates(described.blockOfInitialYield, block.id())))
            continue;
        TerminatorKind end = block.terminator().kind();
        bool endsWithYield = std::ranges::any_of(block.values(), [](const BodyValue& operation) { return operation.opcode() == Opcode::Yield; });
        described.hasOneWay &= !block.hasHandler() && (end != TerminatorKind::Branch || isDecidedBranch(*body, block)) && (end != TerminatorKind::Switch || endsWithYield);
    }
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


static ValueID nameBehindCasts(const BodyIndex& index, ValueID name)
{
    while (true) {
        ValueID user = index.onlyUsingValue(name);
        if (!user || user.index() != name.index() + 1 || index.blockOf(user) != index.blockOf(name) || !opcodesThatStateTypes.contains(index.opcodeOf(user)) || index.value(user).operand(0) != name)
            return name;
        name = user;
    }
}

namespace {

class FinderOfLoopThatStepsByHand {
public:
    FinderOfLoopThatStepsByHand(const Program& program, const BodyIndex& index, ValueID generator)
        : m_index(index)
        , m_generator(generator)
        , m_next(program.keyOf(BuiltinNames::next))
        , m_done(program.keyOf(BuiltinNames::done))
        , m_value(program.keyOf(BuiltinNames::value))
    {
    }

    bool readsNext(ValueID value) const
    {
        if (m_index.isParameter(value) || m_index.opcodeOf(value) != Opcode::PropGet || !m_index.hasOneUse(value))
            return false;
        BodyValue read = m_index.value(value);
        return !read.isFreeName() && read.key() == m_next && read.objectOperand() == m_generator;
    }

    bool steps(ValueID value) const
    {
        if (m_index.isParameter(value) || m_index.opcodeOf(value) != Opcode::Call || m_index.value(value).isDoneInPlace())
            return false;
        auto call = m_index.value(value).as<Opcode::Call>();
        ValueID callee = call.calleeOperand();
        return callee && call.thisValueOperand() == m_generator && m_index.blockOf(callee) == m_index.blockOf(value) && readsNext(callee) && call.argumentsOperands().size() <= 1 && (call.argumentsOperands().empty() || call.argumentsOperands()[0] != m_generator);
    }

    ValueID stepAtEndOf(BodyBlock block) const
    {
        if (block.hasHandler() || block.terminator().kind() != TerminatorKind::Jump)
            return { };
        for (uint32_t number = block.endOfValues().index(); number-- > block.firstValue().index();) {
            if (steps(ValueID(number)))
                return ValueID(number);
            if (!opcodesThatStateTypes.contains(m_index.opcodeOf(ValueID(number))))
                return { };
        }
        return { };
    }

    std::optional<uint32_t> placeOfResultAtEndOf(BodyBlock block) const
    {
        ValueID step = stepAtEndOf(block);
        if (!step)
            return std::nullopt;
        ValueID name = nameBehindCasts(m_index, step);
        std::span<const ValueID> arguments = block.terminator().next().arguments();
        size_t place = std::ranges::find(arguments, name) - arguments.begin();
        bool isHandedOnAsItIs = name.index() + 1 == block.endOfValues().index() && m_index.hasOneUse(name) && place != arguments.size();
        return isHandedOnAsItIs ? std::optional(static_cast<uint32_t>(place)) : std::nullopt;
    }

    bool everyCycleChecksDone(BlockID blockOfStep, ValueID firstValue, ValueID result, LoopID loop, BlockID inputHead) const
    {
        Vector<ValueID, 8> results { result };
        Vector<ValueID, 8> givenOther;
        Vector<ValueID, 4> answers;
        Vector<ValueID, 4> inverseAnswers;
        Vector<BlockID, 4> latches;
        for (BlockID member : m_index.membersOf(loop)) {
            for (BodyTarget target : m_index.terminator(member).targets()) {
                if (target.block() == inputHead)
                    latches.append(member);
            }
        }
        for (BlockID member : m_index.membersOf(loop)) {
            if (!m_index.dominates(blockOfStep, member))
                continue;
            BodyBlock block = m_index.block(member);
            for (uint32_t number = member == blockOfStep ? firstValue.index() : block.firstValue().index(); number < block.endOfValues().index(); ++number) {
                ValueID value(number);
                BodyValue operation = m_index.value(value);
                Opcode opcode = operation.opcode();
                if (opcode == Opcode::PropGet && !operation.isFreeName() && operation.key() == m_done && results.contains(operation.objectOperand()))
                    answers.append(value);
                else if (opcode == Opcode::LogicalNot) {
                    if (answers.contains(operation.operand(0)))
                        inverseAnswers.append(value);
                    else if (inverseAnswers.contains(operation.operand(0)))
                        answers.append(value);
                } else if (opcode == Opcode::ToBoolean || opcodesThatStateTypes.contains(opcode) || inputPassThroughOpcodes.contains(opcode)) {
                    if (answers.contains(operation.operand(0)))
                        answers.append(value);
                    else if (inverseAnswers.contains(operation.operand(0)))
                        inverseAnswers.append(value);
                    else if (opcode != Opcode::ToBoolean && results.contains(operation.operand(0)))
                        results.append(value);
                }
            }
            BodyTerminator end = block.terminator();
            for (BodyTarget target : end.targets()) {
                std::span<const ValueID> arguments = target.arguments();
                BodyBlock to = m_index.block(target.block());
                for (uint32_t position = 0; position < arguments.size() && position < to.numberOfParameters(); ++position) {
                    ValueID parameter = to.parameter(position);
                    bool isResult = results.contains(arguments[position]) && !m_index.isLoopHead(to.id()) && to.id().index() > member.index();
                    if (!isResult)
                        givenOther.append(parameter);
                    if (isResult && !givenOther.contains(parameter) && !results.contains(parameter))
                        results.append(parameter);
                    else if (!isResult)
                        results.removeAll(parameter);
                }
            }
            m_index.forEachEdgeResult(member, [&](const BodyEdgeResult& narrowed) {
                if (narrowed.parameter && results.contains(narrowed.subject) && m_index.numberOfIncomingEdges(m_index.blockOf(narrowed.parameter)) == 1)
                    results.append(narrowed.parameter);
            });
            if (end.kind() != TerminatorKind::Branch || m_index.loopOf(member) != loop)
                continue;
            bool isInverse = inverseAnswers.contains(end.operand(0));
            if (!isInverse && !answers.contains(end.operand(0)))
                continue;
            BlockID atEnd = isInverse ? end.notTaken().block() : end.taken().block();
            BlockID turn = isInverse ? end.taken().block() : end.notTaken().block();
            if (m_index.isInLoop(atEnd, loop) || !m_index.isInLoop(turn, loop))
                continue;
            bool everyLatchFollows = turn == inputHead ? latches.size() == 1 && latches[0] == member : m_index.numberOfIncomingEdges(turn) == 1 && std::ranges::all_of(latches, [&](BlockID latch) {
                return m_index.dominates(turn, latch);
            });
            if (everyLatchFollows && resultIsOnlyReadBefore(turn, blockOfStep, firstValue, results, loop, inputHead))
                return true;
        }
        return false;
    }

    bool resultIsOnlyReadBefore(BlockID turn, BlockID blockOfStep, ValueID firstValue, const Vector<ValueID, 8>& results, LoopID loop, BlockID inputHead) const
    {
        static constexpr OpcodeSet onlyReads = opcodesThatStateTypes | inputPassThroughOpcodes | OpcodeSet { Opcode::TypeTest, Opcode::StrictEq, Opcode::ToBoolean };
        auto isResult = [&](ValueID value) { return value && results.contains(value); };
        for (BlockID member : m_index.membersOf(loop)) {
            if (!m_index.dominates(blockOfStep, member) || (turn != inputHead && m_index.dominates(turn, member)))
                continue;
            BodyBlock block = m_index.block(member);
            if (block.hasHandler() && std::ranges::any_of(block.handler().arguments(), isResult))
                return false;
            for (uint32_t number = member == blockOfStep ? firstValue.index() : block.firstValue().index(); number < block.endOfValues().index(); ++number) {
                BodyValue operation = m_index.value(ValueID(number));
                bool takesResult = false;
                operation.forEachOperand([&](ValueID operand, auto...) { takesResult |= isResult(operand); });
                if (!takesResult || onlyReads.contains(operation.opcode()))
                    continue;
                if (operation.opcode() != Opcode::PropGet || operation.isFreeName() || (operation.key() != m_done && operation.key() != m_value))
                    return false;
            }
            for (BodyTarget target : block.terminator().targets()) {
                if (!m_index.isInLoop(target.block(), loop) || target.block() == inputHead)
                    continue;
                std::span<const ValueID> arguments = target.arguments();
                BodyBlock to = m_index.block(target.block());
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    if (isResult(arguments[position]) && (position >= to.numberOfParameters() || !isResult(to.parameter(position))))
                        return false;
                }
            }
        }
        return true;
    }

    bool precedingValuesAreOnlyReadHere(BlockID inputHead, BlockID blockOfStep, ValueID step, LoopID loop) const
    {
        auto liesInFront = [&](BlockID block) { return m_index.isInLoop(block, loop) && !m_index.dominates(blockOfStep, block); };
        bool isRead = false;
        auto read = [&](ValueID value, auto...) {
            if (!value)
                return;
            BlockID block = m_index.blockOf(value);
            if (block == inputHead && m_index.isParameter(value))
                return;
            isRead |= block == blockOfStep ? value.index() < step.index() : liesInFront(block);
        };
        for (uint32_t number = 0; number < m_index.numberOfBlocks() && !isRead; ++number) {
            BodyBlock block = m_index.block(BlockID(number));
            if (!m_index.isReachable(block.id()) || liesInFront(block.id()))
                continue;
            for (uint32_t value = block.id() == blockOfStep ? step.index() + 1 : block.firstValue().index(); value < block.endOfValues().index(); ++value)
                m_index.value(ValueID(value)).forEachOperand(read);
            BodyTerminator end = block.terminator();
            end.forEachOperand(read);
            for (BodyTarget target : end.targets())
                std::ranges::for_each(target.arguments(), read);
            if (block.hasHandler())
                std::ranges::for_each(block.handler().arguments(), read);
            read(block.subject());
        }
        return !isRead;
    }

private:
    const BodyIndex& m_index;
    ValueID m_generator;
    PropertyKeyID m_next;
    PropertyKeyID m_done;
    PropertyKeyID m_value;
};

} // anonymous namespace

std::optional<LoopThatStepsByHand> findLoopThatStepsByHand(const Program& program, const BodyIndex& index, ValueID generatorCall)
{
    BodyBlock front = index.block(index.blockOf(generatorCall));
    BodyTerminator endOfFront = front.terminator();
    if (index.hasIrreducibleFlow() || front.hasHandler() || endOfFront.kind() != TerminatorKind::Jump)
        return std::nullopt;
    BlockID inputHead = endOfFront.next().block();
    if (!index.isLoopHead(inputHead) || index.isEnteredByHandlerEdge(inputHead) || index.block(inputHead).hasHandler())
        return std::nullopt;
    LoopID loop = index.loopOf(inputHead);
    if (index.loop(loop).parent != index.loopOf(front.id()))
        return std::nullopt;

    LoopThatStepsByHand found;
    found.inputHead = inputHead;
    found.generator = nameBehindCasts(index, generatorCall);
    FinderOfLoopThatStepsByHand finder(program, index, found.generator);
    ValueID stepInFront = finder.stepAtEndOf(front);
    for (uint32_t number = std::max(found.generator.index() + 1, front.firstValue().index()); number < (stepInFront ? stepInFront : front.endOfValues()).index(); ++number)
        found.frontIsSeen |= !finder.readsNext(ValueID(number)) && !isPureForGenerators(index.value(ValueID(number)));

    BodyBlock head = index.block(inputHead);
    uint32_t numberOfSteps = 0;
    uint32_t numberOfEdgesThatComeRound = 0;
    ValueID result;
    ValueID firstValue;
    if (stepInFront) {
        found.placeOfResult = finder.placeOfResultAtEndOf(front);
        if (!found.placeOfResult)
            return std::nullopt;
        found.stepInFront = stepInFront;
        found.blockOfStep = inputHead;
        result = head.parameter(*found.placeOfResult);
        firstValue = head.firstValue();
        ++numberOfSteps;
    } else {
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                if (operation.opcode() != Opcode::Call || !finder.steps(operation.id()))
                    continue;
                if (numberOfSteps++ || index.loopOf(member) != loop || index.block(member).hasHandler())
                    return std::nullopt;
                found.step = result = operation.id();
                found.blockOfStep = member;
            }
        }
        if (!numberOfSteps)
            return std::nullopt;
        firstValue = ValueID(found.step.index() + 1);
    }
    for (BlockID member : index.membersOf(loop)) {
        if (!stepInFront && !index.dominates(found.blockOfStep, member)) {
            if (index.block(member).hasHandler() || index.block(member).isHandlerBlock())
                return std::nullopt;
            found.blocksBeforeStep.append(member);
        }
        for (BodyTarget target : index.terminator(member).targets()) {
            if (target.block() != inputHead)
                continue;
            ++numberOfEdgesThatComeRound;
            if (!stepInFront) {
                if (!index.dominates(found.blockOfStep, member))
                    return std::nullopt;
                continue;
            }
            if (finder.placeOfResultAtEndOf(index.block(member)) != found.placeOfResult)
                return std::nullopt;
            found.step = finder.stepAtEndOf(index.block(member));
            ++numberOfSteps;
        }
    }
    if (!numberOfEdgesThatComeRound || index.numberOfIncomingEdges(inputHead) != numberOfEdgesThatComeRound + 1)
        return std::nullopt;
    if (index.useCount(found.generator) != 2 * numberOfSteps || 2 * numberOfSteps >= BodyIndex::maxCountedUses)
        return std::nullopt;
    if (!stepInFront && !finder.precedingValuesAreOnlyReadHere(inputHead, found.blockOfStep, found.step, loop))
        return std::nullopt;
    if (!finder.everyCycleChecksDone(found.blockOfStep, firstValue, result, loop, inputHead))
        return std::nullopt;
    auto noteWhatIsSeen = [&](BodyBlock block, ValueID end) {
        for (uint32_t number = block.firstValue().index(); number < end.index(); ++number)
            found.loopIsSeenBeforeStep |= index.opcodeOf(ValueID(number)) != Opcode::LoopHint && !finder.readsNext(ValueID(number)) && !isPureForGenerators(index.value(ValueID(number)));
    };
    if (!stepInFront) {
        found.loopIsSeenBeforeStep |= found.blockOfStep != inputHead;
        for (BlockID inFront : found.blocksBeforeStep)
            noteWhatIsSeen(index.block(inFront), index.block(inFront).endOfValues());
        noteWhatIsSeen(index.block(found.blockOfStep), found.step);
    }
    return found;
}

std::optional<StepsInRow> findStepsInRow(const Program& program, const BodyIndex& index, ValueID generatorCall)
{
    BodyBlock block = index.block(index.blockOf(generatorCall));
    if (block.hasHandler())
        return std::nullopt;
    StepsInRow found;
    found.generator = nameBehindCasts(index, generatorCall);
    uint32_t numberOfUses = index.useCount(found.generator);
    if (!numberOfUses || numberOfUses % 2 || numberOfUses > 2 * maxStepsInRow)
        return std::nullopt;
    FinderOfLoopThatStepsByHand finder(program, index, found.generator);
    for (uint32_t number = found.generator.index() + 1; number < block.endOfValues().index() && 2 * found.steps.size() < numberOfUses; ++number) {
        if (finder.steps(ValueID(number)))
            found.steps.append(ValueID(number));
        else if (found.steps.isEmpty())
            found.frontIsSeen |= !finder.readsNext(ValueID(number)) && !isPureForGenerators(index.value(ValueID(number)));
    }
    return 2 * found.steps.size() == numberOfUses ? std::optional(WTF::move(found)) : std::nullopt;
}

static Vector<ValueID, 4> valuesReadAfterNextStep(const BodyIndex& index, std::span<const ValueID> steps)
{
    BodyBlock block = index.block(index.blockOf(steps[0]));
    Vector<ValueID, 4> found;
    size_t next = 0;
    for (uint32_t number = steps[0].index(); number < steps.back().index(); ++number) {
        while (steps[next].index() <= number)
            ++next;
        uint32_t numberOfUses = 0;
        for (uint32_t user = number + 1; user <= steps[next].index(); ++user) {
            index.value(ValueID(user)).forEachOperand([&](ValueID operand, auto...) {
                numberOfUses += operand == ValueID(number);
            });
        }
        bool isNarrowedAtEnd = false;
        index.forEachEdgeResult(block.id(), [&](const BodyEdgeResult& result) {
            isNarrowedAtEnd |= result.subject == ValueID(number);
        });
        if (isNarrowedAtEnd || index.useCount(ValueID(number)) != numberOfUses || numberOfUses >= BodyIndex::maxCountedUses)
            found.append(ValueID(number));
    }
    return found;
}

static bool isOnlySteppedByHand(const Program& program, const BodyIndex& index, ValueID generatorCall)
{
    ValueID generator = nameBehindCasts(index, generatorCall);
    FinderOfLoopThatStepsByHand finder(program, index, generator);
    uint32_t numberOfSteps = 0;
    for (uint32_t block = 0; block < index.numberOfBlocks(); ++block) {
        for (BodyValue operation : index.block(BlockID(block)).values())
            numberOfSteps += operation.opcode() == Opcode::Call && finder.steps(operation.id());
    }
    return index.useCount(generator) == 2 * numberOfSteps && 2 * numberOfSteps < BodyIndex::maxCountedUses;
}

bool onlyStepsParameter(const Program& program, const BodyIndex& index, ValueID parameter)
{
    uint32_t numberOfUses = index.useCount(nameBehindCasts(index, parameter));
    return numberOfUses && !(numberOfUses % 2) && isOnlySteppedByHand(program, index, parameter) && findLoopThatStepsByHand(program, index, parameter);
}

bool Walker::loopStepsGeneratorTaken(BlockID inputHead) const
{
    return m_hasTakenGeneratorsSteppedByHand && m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.isSteppedByHand && taken.inputHead == inputHead && taken.inlineFrame == m_frames.last().inlineFrame;
    });
}

enum class ConsumerAfterCall : uint8_t { IsNone, IsNamedOnce, MayStepByHand };

static ConsumerAfterCall consumerAfterCall(const BodyIndex& index, ValueID call)
{
    uint32_t uses = index.useCount(call);
    if (!uses)
        return ConsumerAfterCall::IsNone;
    ValueID next(call.index() + 1);
    if (next.index() < index.numberOfValues() && !index.isParameter(next) && index.opcodeOf(next) == Opcode::IsPristineArray)
        return ConsumerAfterCall::IsNamedOnce;
    ValueID taker = index.onlyUsingValue(call);
    if (taker && (index.opcodeOf(taker) == Opcode::IteratorOpen || index.opcodeOf(taker) == Opcode::AppendSpread))
        return ConsumerAfterCall::IsNamedOnce;
    bool mayBeStepped = taker ? opcodesThatStateTypes.contains(index.opcodeOf(taker)) : !(uses % 2);
    return mayBeStepped ? ConsumerAfterCall::MayStepByHand : ConsumerAfterCall::IsNone;
}

bool Walker::holdsLoopsThatStepByHand(FunctionID id)
{
    KeptByGeneratorsInLoops& kept = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops);
    if (kept.functionsCheckedForManualIteration.set(id.index()))
        return kept.functionsWithLoopsThatStepByHand.get(id.index());
    const LinkTables& linked = facts().linked();
    const BodyIndex* body = facts().bodies().find(id);
    if (!body || id.index() + 1 >= linked.firstCallOfCaller.size())
        return false;
    uint32_t first = linked.firstCallOfCaller[id.index()];
    bool holdsSome = std::ranges::any_of(linked.calls.span().subspan(first, linked.firstCallOfCaller[id.index() + 1] - first), [&](const LinkedCall& call) {
        return program().at(call.callee).has(FunctionFlags::IsGenerator) && !body->isParameter(call.site) && body->opcodeOf(call.site) == Opcode::Call && (findLoopThatStepsByHand(program(), *body, call.site) || findStepsInRow(program(), *body, call.site));
    });
    if (holdsSome)
        kept.functionsWithLoopsThatStepByHand.set(id.index());
    return holdsSome;
}

bool Walker::answerStepOfGeneratorTaken(const BodyValue& operation)
{
    bool isRead = operation.opcode() == Opcode::PropGet;
    if (isRead && operation.isFreeName())
        return false;
    if (operation.opcode() == Opcode::IsPristineArray) {
        Reference tested = operation.operand(0) ? referenceBoundTo(operation.operand(0)) : Reference();
        if (!tested || !tested.isVirtual() || !standsForFrameOfGenerator(tested))
            return false;
        bind(operation.id(), BuilderForGenerators(*this, originOf(operation)).boolean(true));
        return true;
    }
    ValueID named = isRead ? operation.objectOperand() : operation.as<Opcode::Call>().thisValueOperand();
    Reference state = named ? referenceBoundTo(named) : Reference();
    if (!state || !state.isVirtual() || !standsForFrameOfGenerator(state))
        return false;
    if (isRead) {
        buildFirstReadsThatOpenOfGeneratorLeavesOut(*this, originOf(operation), false);
        if (!isUnreachable())
            bind(operation.id(), Reference());
        return true;
    }
    std::span<const ValueID> arguments = operation.as<Opcode::Call>().argumentsOperands();
    Reference sent = arguments.empty() ? Reference() : referenceOf(arguments[0]);
    if (!sent) {
        Draft nothing(Opcode::Undefined, originOf(operation));
        nothing.representation = Representation::JSValue;
        sent = constant(WTF::move(nothing));
    }
    if (m_endOfManualIteration && m_endOfManualIteration->generator == state) {
        m_endOfManualIteration->isReached = true;
        m_endOfManualIteration->block = m_block;
        m_endOfManualIteration->sent = sent;
        m_endOfManualIteration->state = WTF::move(m_state);
        m_block = { };
        m_state.clear();
        return true;
    }
    size_t placeOfRow = m_generatorsTakenByConsumers.findIf([&](const GeneratorTakenByConsumer& taken) {
        return taken.state == state && !taken.stepsInRow.isEmpty();
    });
    if (placeOfRow != notFound)
        return walkStepsInRow(placeOfRow, operation);
    auto isSteppedByHand = [&](const auto& generator) { return generator.state == state && generator.isSteppedByHand; };
    if (!m_generatorsTakenByConsumers.containsIf(isSteppedByHand) && !m_generatorsWalkedInConsumers.containsIf(isSteppedByHand)) {
        fail("a generator that is walked in its consumer is stepped where no step of it is expected"_s);
        return true;
    }
    const Frame& frame = m_frames.last();
    ValueID last = nameBehindCasts(*frame.index, operation.id());
    for (uint32_t name = operation.id().index(); name <= last.index(); ++name)
        bind(ValueID(name), sent);
    m_frames.last().walkedUpTo = std::max(frame.walkedUpTo, last.index() + 1 - frame.index->block(frame.index->blockOf(last)).firstValue().index());
    return true;
}

bool Walker::walkOperationOfGeneratorInLoop(const BodyValue& value)
{
    switch (value.opcode()) {
    case Opcode::Call: {
        ConsumerAfterCall consumer = consumerAfterCall(currentBodyIndex(), value.id());
        if (consumer == ConsumerAfterCall::IsNone || (consumer == ConsumerAfterCall::MayStepByHand && !holdsLoopsThatStepByHand(currentFunction())))
            return false;
        return takeGeneratorForConsumer(value) || walkGeneratorCallTwice(value);
    }
    case Opcode::IteratorOpen:
        return openGeneratorTaken(value) || takeGeneratorForConsumer(value);
    case Opcode::IteratorResume:
        return walkMatchIteration(*this, value) || stepGeneratorTakenOutsideLoops(value);
    case Opcode::IteratorClose:
        return walkMatchIteration(*this, value) || closeGeneratorTaken(value);
    case Opcode::AppendSpread:
        return spreadGeneratorTaken(value) || takeGeneratorForConsumer(value);
    case Opcode::InitialYield:
        chooseInlineFrameOfStep(true);
        return walkInitialYieldInConsumer(value);
    case Opcode::Yield:
        return walkYieldOfGeneratorInConsumer(value);
    case Opcode::PropGet:
        return m_frames.size() > 1 && foldGeneratorRead(value);
    case Opcode::InstanceOf:
        return m_frames.size() > 1 && foldGeneratorInstanceOf(value);
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

static bool stepsWalkConsumer(const Program& program, const BodyIndex& index, ValueID generatorCall)
{
    if (findLoopThatStepsByHand(program, index, generatorCall))
        return true;
    std::optional<StepsInRow> row = findStepsInRow(program, index, generatorCall);
    return row && row->steps.size() > 1;
}

bool Walker::loopHoldsGenerators(const BodyIndex& index, LoopID loop)
{
    if (!has(Mechanism::Handlers) || !has(Mechanism::DirectCalls))
        return false;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            FunctionID callee;
            if (operation.opcode() == Opcode::Call) {
                callee = facts().linked().calleeAt(currentFunction(), operation.id());
                if (!callee || !program().at(callee).has(FunctionFlags::IsGenerator) || !stepsWalkConsumer(program(), index, operation.id()))
                    continue;
            } else {
                if (operation.opcode() != Opcode::IteratorOpen)
                    continue;
                auto opened = operation.as<Opcode::IteratorOpen>();
                if (opened.iteratorKind() != IteratorKind::Sync || opened.isDelegating() || opened.isDestructuring())
                    continue;
                ValueID iterable = index.stripCasts(opened.iterableOperand());
                bool isCall = iterable && !index.isParameter(iterable) && index.opcodeOf(iterable) == Opcode::Call;
                callee = isCall ? facts().linked().calleeAt(currentFunction(), iterable) : FunctionID();
                if (!callee || !program().at(callee).has(FunctionFlags::IsGenerator))
                    callee = facts().linked().calleeAt(currentFunction(), operation.id());
                if (!callee || !program().at(callee).has(FunctionFlags::IsGenerator))
                    continue;
            }
            const GeneratorFunctionDescribed& described = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.ensure(callee.index(), [&] {
                return describeGeneratorFunction(*this, callee);
            }).iterator->value;
            if (described.bodyMayBeWalkedInConsumer)
                return true;
        }
    }
    return false;
}

bool Walker::isQuietBeforeInitialYield(FunctionID generatorFunction)
{
    return keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.ensure(generatorFunction.index(), [&] {
        return describeGeneratorFunction(*this, generatorFunction);
    }).iterator->value.isQuietBeforeInitialYield;
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

ValueID Walker::onlyTakerOfEntryParameter(FunctionID id, uint32_t entryParameter)
{
    const BodyIndex* body = facts().bodies().find(id);
    if (!body || program().at(id).isCoroutine() || !program().at(id).hasBody() || body->holdsAnyOf(opcodesThatMakeArguments) || body->holdsAnyOf(opcodesOfMappedArguments) || facts().summaries().at(id).has(SummaryTrait::HasDirectEval))
        return { };
    BodyBlock entry = body->block(entryBlock);
    if (entryParameter >= entry.numberOfParameters() || body->numberOfIncomingEdges(entryBlock))
        return { };
    ValueID name = entry.parameter(entryParameter);
    ValueID taker = followCastsToOnlyTaker(*body, name);
    if (!taker || (body->opcodeOf(taker) != Opcode::IteratorOpen && body->opcodeOf(taker) != Opcode::AppendSpread))
        return { };
    ValueID taken = body->opcodeOf(taker) == Opcode::IteratorOpen ? body->value(taker).as<Opcode::IteratorOpen>().iterableOperand() : body->value(taker).as<Opcode::AppendSpread>().iterableOperand();
    return taken == name ? taker : ValueID();
}

namespace {

class UsesOfPassedGenerator {
public:
    UsesOfPassedGenerator(const Walker& walker, FunctionID function, unsigned depth)
        : m_walker(walker)
        , m_function(function)
        , m_depth(depth)
        , m_body(walker.facts().bodies().find(function))
    {
    }

    bool visit(uint32_t entryParameter);
    ValueID findOnlyOpen() const;
    std::optional<bool> findReturnedTruth() const;

private:
    struct Use {
        BlockID block;
        ValueID user;
        bool isTest { false };
    };
    static constexpr unsigned maxDepth = 2;

    bool isGenerator(ValueID value) const { return value && m_generators.get(value.index()); }
    void note(BlockID block, ValueID user, bool isTest) { m_uses.append({ block, user, isTest }); }
    void noteTruth(ValueID value, bool truth) { m_truths.set(value.index(), truth); }
    std::optional<bool> truthOf(ValueID value) const
    {
        auto found = m_truths.find(value.index());
        return found == m_truths.end() ? std::nullopt : std::optional(found->value);
    }
    void visitOperation(BlockID, const BodyValue&);
    std::optional<bool> truthOfTypeTest(TypeID held, TypeID tested) const;
    bool isInstanceOfOtherClass(const BodyValue& test) const;
    void follow(BlockID from, const BodyTarget&, ValueID test, Vector<BlockID, 16>& worklist);
    BitVector blocksReachableFrom(BlockID) const;

    const Walker& m_walker;
    FunctionID m_function;
    unsigned m_depth;
    const BodyIndex* m_body;
    BitVector m_generators;
    BitVector m_isUndefined;
    BitVector m_isReached;
    HashMap<uint32_t, bool, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_truths;
    Vector<std::pair<BlockID, BlockID>, 16> m_edges;
    Vector<Use, 8> m_uses;
    Vector<std::pair<BlockID, ValueID>, 1> m_opens;
};

} // anonymous namespace

static BuiltinID findUnrelatedClass(const Walker& walker, TypeID typeOfConstructor)
{
    const Program& program = walker.program();
    const BuiltinTable& table = program.builtins();
    if (!typeOfConstructor || !walker.types().at(typeOfConstructor).isObject())
        return { };
    BuiltinID constructor = singleBuiltinOf(walker.types(), walker.types().at(typeOfConstructor));
    if (!constructor || table.at(constructor).kind != BuiltinKind::Constructor)
        return { };
    BuiltinID hasInstance = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(table.nameOf(WellKnownSymbol::HasInstance)));
    BuiltinID location = findConstantMemberFrom(program, walker.facts(), constructor, program.keyOf(BuiltinNames::prototype));
    if (!hasInstance || table.canonicalID(hasInstance) != Builtins::FunctionPrototypeSymbolHasInstance || !location)
        return { };
    BuiltinID holder = table.canonicalID(table.at(BuiltinClasses::Generator).prototype);
    while (holder) {
        if (holder == table.canonicalID(location) || !table.hasLockedPrototypeLink(holder))
            return { };
        if (table.at(holder).prototypeLink == BuiltinPrototypeLink::Null)
            return constructor;
        if (table.at(holder).prototypeLink != BuiltinPrototypeLink::Builtin)
            return { };
        holder = table.canonicalID(table.at(holder).inheritsFrom);
    }
    return { };
}

static bool generatorsHaveNothingUnder(const Walker& walker, PropertyKeyID key)
{
    std::optional<BuiltinID> member = findMemberUnderKey(walker, walker.program().builtins().at(BuiltinClasses::Generator).prototype, key);
    return member && !*member;
}

std::optional<bool> UsesOfPassedGenerator::truthOfTypeTest(TypeID held, TypeID tested) const
{
    if (m_walker.types().isSubset(held, tested))
        return true;
    if (!m_walker.types().overlap(held, tested))
        return false;
    return std::nullopt;
}

bool UsesOfPassedGenerator::isInstanceOfOtherClass(const BodyValue& test) const
{
    ValueID constructor = test.as<Opcode::InstanceOf>().constructorOperand();
    if (m_body->isParameter(constructor) || m_body->opcodeOf(constructor) != Opcode::Convert)
        return false;
    return !!findUnrelatedClass(m_walker, m_body->value(constructor).as<Opcode::Convert>().declared().written());
}

void UsesOfPassedGenerator::visitOperation(BlockID block, const BodyValue& operation)
{
    Opcode opcode = operation.opcode();
    ValueID id = operation.id();
    if (opcode == Opcode::Boolean) {
        noteTruth(id, operation.as<Opcode::Boolean>().boolean());
        return;
    }
    if (opcode == Opcode::LogicalNot || opcode == Opcode::ToBoolean) {
        if (std::optional<bool> truth = truthOf(operation.operand(0)))
            noteTruth(id, *truth == (opcode == Opcode::ToBoolean));
        if (!isGenerator(operation.operand(0)))
            return;
        note(block, id, true);
        noteTruth(id, opcode == Opcode::ToBoolean);
        return;
    }
    if (opcode == Opcode::TypeTest) {
        auto test = operation.as<Opcode::TypeTest>();
        bool testsGenerator = isGenerator(test.valueOperand());
        if (!testsGenerator && !m_isUndefined.get(test.valueOperand().index()))
            return;
        if (testsGenerator)
            note(block, id, true);
        if (std::optional<bool> truth = truthOfTypeTest(testsGenerator ? m_walker.types().builtinClass(BuiltinClasses::Generator) : TypeTable::undefined, test.testedType()))
            noteTruth(id, *truth);
        return;
    }
    bool namesGenerator = false;
    operation.forEachOperand([&](ValueID operand, auto...) {
        namesGenerator |= isGenerator(operand);
    });
    if (!namesGenerator)
        return;
    if (opcodesThatStateTypes.contains(opcode)) {
        if (isGenerator(operation.operand(0)))
            m_generators.set(id.index());
        return;
    }
    if (opcode == Opcode::PropGet && !operation.isFreeName() && generatorsHaveNothingUnder(m_walker, operation.key())) {
        note(block, id, true);
        m_isUndefined.set(id.index());
        return;
    }
    if (opcode == Opcode::InstanceOf && isGenerator(operation.as<Opcode::InstanceOf>().valueOperand()) && isInstanceOfOtherClass(operation)) {
        note(block, id, true);
        noteTruth(id, false);
        return;
    }
    if (opcode == Opcode::IteratorOpen && isGenerator(operation.as<Opcode::IteratorOpen>().iterableOperand())) {
        m_opens.append({ block, id });
        return;
    }
    if (opcode == Opcode::Call && m_depth < maxDepth) {
        auto call = operation.as<Opcode::Call>();
        FunctionID callee = m_walker.facts().linked().calleeAt(m_function, id);
        std::span<const ValueID> arguments = call.argumentsOperands();
        uint32_t timesNamed = isGenerator(call.calleeOperand()) + isGenerator(call.thisValueOperand());
        std::optional<uint32_t> position;
        for (uint32_t index = 0; index < arguments.size(); ++index) {
            if (!isGenerator(arguments[index]))
                continue;
            ++timesNamed;
            position = index;
        }
        if (callee && position && timesNamed == 1 && *position < m_walker.program().at(callee).numberOfPositionalParameters()) {
            UsesOfPassedGenerator uses(m_walker, callee, m_depth + 1);
            std::optional<bool> truth = uses.visit(m_walker.program().at(callee).firstParameterIndex() + *position) ? uses.findReturnedTruth() : std::nullopt;
            if (truth) {
                note(block, id, true);
                noteTruth(id, *truth);
                return;
            }
        }
    }
    note(block, id, false);
}

void UsesOfPassedGenerator::follow(BlockID from, const BodyTarget& target, ValueID test, Vector<BlockID, 16>& worklist)
{
    BodyBlock to = m_body->block(target.block());
    std::span<const ValueID> arguments = target.arguments();
    std::span<const uint32_t> proofs = target.proofs();
    for (uint32_t index = 0; index < to.numberOfParameters() && index < arguments.size() + proofs.size(); ++index) {
        ValueID arriving = index < arguments.size() ? arguments[index] : test && !m_body->isParameter(test) ? m_body->value(test).operand(proofs[index - arguments.size()]) : ValueID();
        if (!isGenerator(arriving))
            continue;
        if (m_body->numberOfIncomingEdges(to.id()) == 1)
            m_generators.set(to.parameter(index).index());
        else if (index < arguments.size())
            note(from, { }, false);
    }
    m_edges.append({ from, to.id() });
    if (!m_isReached.set(to.id().index()))
        worklist.append(to.id());
}

bool UsesOfPassedGenerator::visit(uint32_t entryParameter)
{
    const Function& function = m_walker.program().at(m_function);
    if (!m_body || function.isCoroutine() || !function.hasBody() || m_body->holdsAnyOf(opcodesThatMakeArguments) || m_body->holdsAnyOf(opcodesOfMappedArguments) || m_walker.facts().summaries().at(m_function).has(SummaryTrait::HasDirectEval))
        return false;
    BodyBlock entry = m_body->block(entryBlock);
    if (entryParameter >= entry.numberOfParameters() || m_body->numberOfIncomingEdges(entryBlock))
        return false;
    m_generators.set(entry.parameter(entryParameter).index());
    m_isReached.set(entryBlock.index());
    Vector<BlockID, 16> worklist { entryBlock };
    while (!worklist.isEmpty()) {
        BodyBlock block = m_body->block(worklist.takeLast());
        for (BodyValue operation : block.values())
            visitOperation(block.id(), operation);
        if (block.hasHandler())
            follow(block.id(), block.handler(), { }, worklist);
        BodyTerminator end = block.terminator();
        if (end.kind() == TerminatorKind::Branch) {
            ValueID test = end.operand(0);
            std::optional<bool> truth = truthOf(test);
            if (!truth || *truth)
                follow(block.id(), end.taken(), test, worklist);
            if (!truth || !*truth)
                follow(block.id(), end.notTaken(), test, worklist);
            continue;
        }
        end.forEachOperand([&](ValueID operand, auto...) {
            if (isGenerator(operand))
                note(block.id(), { }, false);
        });
        for (BodyTarget target : end.targets())
            follow(block.id(), target, { }, worklist);
    }
    return true;
}

BitVector UsesOfPassedGenerator::blocksReachableFrom(BlockID start) const
{
    BitVector reachable;
    Vector<BlockID, 16> worklist { start };
    while (!worklist.isEmpty()) {
        BlockID from = worklist.takeLast();
        for (auto [source, to] : m_edges) {
            if (source == from && !reachable.set(to.index()))
                worklist.append(to);
        }
    }
    return reachable;
}

ValueID UsesOfPassedGenerator::findOnlyOpen() const
{
    if (m_opens.size() != 1)
        return { };
    auto [blockOfOpen, open] = m_opens[0];
    auto opened = m_body->value(open).as<Opcode::IteratorOpen>();
    if (opened.iteratorKind() != IteratorKind::Sync || opened.isDelegating() || opened.isDestructuring())
        return { };
    BitVector isBehindOpen = blocksReachableFrom(blockOfOpen);
    if (isBehindOpen.get(blockOfOpen.index()))
        return { };
    for (const Use& use : m_uses) {
        if (isBehindOpen.get(use.block.index()))
            return { };
        if (use.block == blockOfOpen) {
            if (!use.isTest || use.user.index() > open.index())
                return { };
            continue;
        }
        if (!use.isTest && blocksReachableFrom(use.block).get(blockOfOpen.index()))
            return { };
    }
    return open;
}

std::optional<bool> UsesOfPassedGenerator::findReturnedTruth() const
{
    if (!m_opens.isEmpty() || !std::ranges::all_of(m_uses, &Use::isTest))
        return std::nullopt;
    std::optional<bool> returned;
    for (size_t number : m_isReached) {
        BodyTerminator end = m_body->terminator(BlockID(static_cast<uint32_t>(number)));
        if (end.kind() != TerminatorKind::Return)
            continue;
        std::optional<bool> truth = end.operand(0) ? truthOf(end.operand(0)) : std::nullopt;
        if (!truth || (returned && *returned != *truth))
            return std::nullopt;
        returned = truth;
    }
    return returned;
}

ValueID Walker::openOfPassedGenerator(FunctionID callee, uint32_t entryParameter) const
{
    return keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).opensOfPassedGenerators.ensure(uint64_t { callee.index() } << 32 | entryParameter, [&] {
        UsesOfPassedGenerator uses(*this, callee, 0);
        return uses.visit(entryParameter) ? uses.findOnlyOpen() : ValueID();
    }).iterator->value;
}

bool Walker::fileHoldsGenerators() const
{
    std::optional<BitVector>& files = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).filesWithGenerators;
    if (!files) {
        files.emplace();
        for (const Function& function : program().functions) {
            if (function.has(FunctionFlags::IsGenerator) && !function.has(FunctionFlags::IsAsync) && function.hasBody())
                files->set(function.module.index());
        }
    }
    return files->get(program().at(currentFunction()).module.index());
}

bool Walker::isPassedGenerator(Reference value) const
{
    if (!value || !value.isRecord() || m_frames.size() < 2)
        return false;
    const Operation& made = m_code.at(value.id());
    if (made.opcode != Opcode::CallDirect || made.role() != CodeRole::Body)
        return false;
    const Function& function = program().at(made.function());
    return function.has(FunctionFlags::IsGenerator) && !function.has(FunctionFlags::IsAsync);
}

bool Walker::isOnlyTestedSinceCall(Reference generator) const
{
    static constexpr OpcodeSet tests { Opcode::IsUnmodifiedGenerator, Opcode::TypeTest };
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Box, Opcode::Unbox };
    BitVector generators;
    generators.set(generator.id().index());
    for (uint32_t number = generator.id().index() + 1; number < m_code.numberOfValues(); ++number) {
        const Operation& operation = m_code.at(ValueID(number));
        bool namesGenerator = false;
        operation.forEachOperand([&](ValueID operand, auto...) {
            namesGenerator |= operand && generators.get(operand.index());
        });
        if (!namesGenerator || tests.contains(operation.opcode))
            continue;
        if (!names.contains(operation.opcode))
            return false;
        generators.set(number);
    }
    return true;
}

bool Walker::runsNothingSinceCall(Reference generator) const
{
    BlockID madeIn = m_code.blockOf(generator.id());
    for (BlockID block = m_block; block != madeIn;) {
        EdgeID into = m_code.at(block).firstEdgeIn();
        if (!into || m_code.at(into).nextInto())
            return false;
        block = m_code.at(into).from;
        if (TerminatorKind kind = m_code.at(block).terminator.kind; kind != TerminatorKind::Jump && kind != TerminatorKind::Branch)
            return false;
    }
    for (uint32_t number = generator.id().index() + 1; number < m_code.numberOfValues(); ++number) {
        if (m_code.isConstant(ValueID(number)))
            continue;
        EffectTraits traits = m_code.at(ValueID(number)).traits();
        if (traits.mayWriteObservableState() || traits.mayRunOtherCode())
            return false;
    }
    return true;
}

Reference Walker::functionOfGenerator(Reference generator) const
{
    const Operation& made = m_code.at(generator.id());
    std::optional<unsigned> place = program().at(made.function()).calleeParameterIndex();
    if (!place || !made.isRemovableWhenUnused() || *place >= made.numberOfVariadicOperands() || !runsNothingSinceCall(generator))
        return { };
    ValueID function = made.variadicOperands()[*place];
    static constexpr OpcodeSet allocations { Opcode::NewClosure };
    if (!function || m_code.isConstant(function) || allocations.contains(m_code.at(function).opcode))
        return { };
    return singleFunctionOf(m_types, m_types.at(m_code.at(function).type)) == made.function() ? Reference::value(function) : Reference();
}

Reference Walker::makeGeneratorAgain(Reference generator)
{
    const Operation& made = m_code.at(generator.id());
    Draft again(Opcode::CallDirect, made.origin());
    chooseConservativeFields(again);
    again.standsForNoValueOfInput = true;
    again.setFunction(made.function());
    again.setRole(made.role());
    again.setIsRemovableWhenUnused(true);
    Vector<Reference, 8> operands;
    for (ValueID operand : made.variadicOperands())
        operands.append(m_code.isConstant(operand) ? Reference::constant(operand) : Reference::value(operand));
    again.setVariadicOperands(operands.span());
    Effects effects = effectsOfDirectCall(*this, made.function(), operands.span());
    return buildCallThatMayBorrowScopes(WTF::move(again), &effects);
}

bool Walker::isOnlyPassedToCopy(Reference generator)
{
    const Frame& leaving = frameLeftForCall();
    size_t copy = m_frames.size() - 1;
    while (copy && &m_frames[copy] != &leaving)
        --copy;
    if (!copy)
        return false;
    const Frame& caller = m_frames[copy - 1];
    const BodyIndex& index = *caller.index;
    ValueID call = leaving.callCopied;
    if (!call || index.isParameter(call) || index.opcodeOf(call) != Opcode::Call || facts().linked().calleeAt(caller.function, call) != leaving.function)
        return false;
    for (ValueID argument : index.value(call).as<Opcode::Call>().argumentsOperands()) {
        ValueID creation = index.stripCasts(argument);
        if (referenceBoundIn(caller, creation) != generator)
            continue;
        ValueID name = creation;
        return !index.isParameter(creation) && index.opcodeOf(creation) == Opcode::Call && index.blockOf(creation) == index.blockOf(call) && followCastsToOnlyTaker(index, name) == call;
    }
    return false;
}

bool Walker::checkPassedGenerator(Reference generator, Origin origin, Reference* holderOfReturn)
{
    origin.isTailCall = false;
    Reference function = isOnlyPassedToCopy(generator) ? functionOfGenerator(generator) : Reference();
    Draft test(function ? Opcode::MakesPlainGenerators : Opcode::IsUnmodifiedGenerator, origin);
    chooseConservativeFields(test);
    if (function)
        test.setFunctionOperand(function);
    else
        test.setValueOperand(generator);
    Reference passes = build(WTF::move(test));
    if (!passes)
        return false;
    recordSpeculation();
    if (function)
        frameLeftForCall().generatorMadeAgain = generator;
    BlockID passed = addBlock();
    branch(passes, passed, blockWhereCopyLeavesForCall());
    continueIn(passed);
    if (holderOfReturn && !isUnreachable())
        *holderOfReturn = function ? buildGeneratorPrototype(function, origin) : generator;
    return !isUnreachable();
}

Reference Walker::buildGeneratorPrototype(Reference function, Origin origin)
{
    origin.isTailCall = false;
    Draft read(Opcode::GetGeneratorPrototype, origin);
    chooseConservativeFields(read);
    read.setFunctionOperand(function);
    return build(WTF::move(read));
}

bool Walker::readGlobalInCopy(const BodyValue& read)
{
    const BodyIndex& index = currentBodyIndex();
    if (!index.opcodes().contains(Opcode::IteratorOpen))
        return false;
    BodyBlock entry = index.block(entryBlock);
    for (uint32_t parameter = 0; parameter < entry.numberOfParameters(); ++parameter) {
        if (isPassedGenerator(referenceBoundTo(entry.parameter(parameter))) && openOfPassedGenerator(currentFunction(), parameter))
            return mayLeaveCopyForCall() && readFiledGlobal(*this, read);
    }
    return false;
}

bool Walker::foldGeneratorRead(const BodyValue& read)
{
    if (!m_frames.last().callCopied || !has(Mechanism::Guesses))
        return false;
    if (read.isFreeName())
        return readGlobalInCopy(read);
    Reference generator = referenceOf(read.objectOperand());
    if (!isPassedGenerator(generator) || !mayLeaveCopyForCall() || !generatorsHaveNothingUnder(*this, read.key()) || !engineAnswersForGenerators(*this))
        return false;
    if (checkPassedGenerator(generator, originOf(read)))
        bind(read.id(), BuilderForGenerators(*this, originOf(read)).undefined());
    return true;
}

bool Walker::foldGeneratorInstanceOf(const BodyValue& test)
{
    if (!m_frames.last().callCopied || !has(Mechanism::Guesses))
        return false;
    auto written = test.as<Opcode::InstanceOf>();
    Reference generator = referenceOf(written.valueOperand());
    Reference constructor = referenceOf(written.constructorOperand());
    if (!isPassedGenerator(generator) || !constructor || !constructor.isRecord() || !mayLeaveCopyForCall() || !engineAnswersForGenerators(*this))
        return false;
    BuiltinID expected = findUnrelatedClass(*this, typeOf(constructor));
    bool isProven = !!expected;
    if (!isProven)
        expected = findUnrelatedClass(*this, findExpectation(*this, written.constructorOperand()).type);
    if (!expected)
        return false;
    Origin origin = originOf(test);
    origin.isTailCall = false;
    if (!isProven) {
        Draft identity(Opcode::TypeTest, origin);
        chooseConservativeFields(identity);
        identity.setValueOperand(constructor);
        identity.setTestedType(m_types.forBuiltin(expected));
        Reference isExpected = build(WTF::move(identity));
        if (!isExpected)
            return isUnreachable();
        recordSpeculation();
        BlockID passed = addBlock();
        branch(isExpected, passed, blockWhereCopyLeavesForCall());
        continueIn(passed);
        if (isUnreachable())
            return true;
    }
    if (checkPassedGenerator(generator, origin))
        bind(test.id(), BuilderForGenerators(*this, origin).boolean(false));
    return true;
}

bool Walker::onlyStepsEntryParameter(FunctionID id, uint32_t entryParameter)
{
    const BodyIndex* body = facts().bodies().find(id);
    if (!body || program().at(id).isCoroutine() || !program().at(id).hasBody() || body->holdsAnyOf(opcodesThatMakeArguments) || body->holdsAnyOf(opcodesOfMappedArguments) || facts().summaries().at(id).has(SummaryTrait::HasDirectEval))
        return false;
    BodyBlock entry = body->block(entryBlock);
    return entryParameter < entry.numberOfParameters() && !body->numberOfIncomingEdges(entryBlock) && onlyStepsParameter(program(), *body, entry.parameter(entryParameter));
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
    if (function.has(FunctionFlags::IsAsync) || position >= function.numberOfPositionalParameters() || call.calleeOperand() == passed || call.thisValueOperand() == passed)
        return false;
    ValueID user = function.has(FunctionFlags::IsGenerator) ? onlyUserOfEntryParameter(callee, function.firstParameterIndex() + position) : onlyTakerOfEntryParameter(callee, function.firstParameterIndex() + position);
    if (!user)
        return onlyStepsEntryParameter(callee, function.firstParameterIndex() + position);
    BodyValue iteration = facts().bodies().at(callee).value(user);
    if (iteration.opcode() != Opcode::AppendSpread && iteration.opcode() != Opcode::IteratorOpen)
        return false;
    if (iteration.opcode() == Opcode::AppendSpread)
        return facts().bodies().at(callee).stripCasts(iteration.as<Opcode::AppendSpread>().arrayOperand()).index() != facts().bodies().at(callee).block(entryBlock).firstParameter().index() + function.firstParameterIndex() + position;
    return iteration.opcode() == Opcode::IteratorOpen && iteration.as<Opcode::IteratorOpen>().iteratorKind() == IteratorKind::Sync;
}

Reference Walker::findPassedGenerator(ValueID taker, ValueID iterable, ValueID& creatingCall, uint32_t& frameOfCall)
{
    size_t place = placeOfGeneratorOnTop();
    ValueID callOfCopy;
    uint32_t frameOfCaller = 0;
    if (place != notFound) {
        callOfCopy = m_generatorsWalkedInConsumers[place].call;
        frameOfCaller = m_generatorsWalkedInConsumers[place].frameOfCall;
    } else if (size_t top = m_frames.size() - 1) {
        callOfCopy = m_frames[top].callCopied;
        frameOfCaller = static_cast<uint32_t>(top - 1);
        const Frame& caller = m_frames[frameOfCaller];
        if (!callOfCopy || caller.index->isParameter(callOfCopy) || caller.index->opcodeOf(callOfCopy) != Opcode::Call || facts().linked().calleeAt(caller.function, callOfCopy) != currentFunction())
            return { };
    }
    if (!callOfCopy)
        return { };
    const BodyIndex& body = currentBodyIndex();
    ValueID parameter = body.stripCasts(iterable);
    bool stepsByHand = body.opcodeOf(taker) == Opcode::PropGet;
    if (!body.isParameter(parameter) || body.blockOf(parameter) != entryBlock || (!stepsByHand && body.loopDepthOf(body.blockOf(taker))))
        return { };
    uint32_t entryParameter = parameter.index() - body.block(entryBlock).firstParameter().index();
    bool isOnlyUse = stepsByHand ? place == notFound && onlyStepsEntryParameter(currentFunction(), entryParameter) : (place != notFound ? onlyUserOfEntryParameter(currentFunction(), entryParameter) : onlyTakerOfEntryParameter(currentFunction(), entryParameter)) == taker;
    if (!isOnlyUse && (stepsByHand || place != notFound || body.opcodeOf(taker) != Opcode::IteratorOpen || openOfPassedGenerator(currentFunction(), entryParameter) != taker))
        return { };
    Reference generator = referenceBoundTo(parameter);
    if (!generator || !generator.isRecord() || (!isOnlyUse && !isOnlyTestedSinceCall(generator)))
        return { };
    const Operation& made = m_code.at(generator.id());
    if (made.opcode == Opcode::CallDirect && !isQuietBeforeInitialYield(made.function()))
        return { };
    if (made.opcode != Opcode::CallDirect || made.role() != CodeRole::Body || made.numberOfVariadicOperands() != program().at(made.function()).numberOfEntryParameters())
        return { };

    const Frame& caller = m_frames[frameOfCaller];
    const BodyIndex& index = *caller.index;
    for (ValueID argument : index.value(callOfCopy).as<Opcode::Call>().argumentsOperands()) {
        ValueID call = index.stripCasts(argument);
        if (referenceBoundIn(caller, call) != generator)
            continue;
        ValueID nameInCaller = call;
        if (index.isParameter(call) || index.opcodeOf(call) != Opcode::Call || index.blockOf(call) != index.blockOf(callOfCopy) || followCastsToOnlyTaker(index, nameInCaller) != callOfCopy)
            return { };
        creatingCall = call;
        frameOfCall = frameOfCaller;
        return generator;
    }
    return { };
}

bool Walker::stepsStandApart(const StepsInRow& row) const
{
    const BodyIndex& index = currentBodyIndex();
    for (const GeneratorTakenByConsumer& taken : m_generatorsTakenByConsumers) {
        if (taken.inlineFrame != m_frames.last().inlineFrame || taken.stepsInRow.size() < 2 || index.blockOf(taken.stepsInRow[0]) != index.blockOf(row.generator))
            continue;
        if (row.steps[0].index() < taken.stepsInRow.last().index() && row.steps.last().index() > taken.stepsInRow[0].index())
            return false;
    }
    if (row.steps.size() < 2)
        return true;
    for (uint32_t number = row.steps[0].index(); number <= row.steps.last().index(); ++number) {
        BodyValue operation = index.value(ValueID(number));
        bool stepsOther = false;
        operation.forEachOperand([&](ValueID operand, auto...) {
            Reference bound = operand && operand.index() < row.generator.index() ? referenceBoundTo(operand) : Reference();
            stepsOther |= bound && bound.isVirtual() && standsForFrameOfGenerator(bound);
        });
        if (stepsOther)
            return false;
    }
    return std::ranges::none_of(valuesReadAfterNextStep(index, row.steps.span()), [&](ValueID value) {
        return index.opcodeOf(value) == Opcode::IteratorOpen || index.opcodeOf(value) == Opcode::IteratorResume;
    });
}

bool Walker::takePassedGeneratorForSteps(const BodyValue& read)
{
    if (read.isFreeName() || read.key() != program().keyOf(BuiltinNames::next) || !m_frames.last().callCopied || !has(Mechanism::DirectCalls) || !has(Mechanism::Inlining))
        return false;
    const BodyIndex& index = currentBodyIndex();
    ValueID parameter = index.stripCasts(read.objectOperand());
    if (!index.isParameter(parameter) || index.blockOf(parameter) != entryBlock || isUnreachable())
        return false;
    Reference passed = referenceBoundTo(parameter);
    return passed && passed.isRecord() && m_code.at(passed.id()).opcode == Opcode::CallDirect && takeGeneratorForConsumer(read);
}

void Walker::takePassedGeneratorForLoop(BlockID inputHead)
{
    const Frame& frame = m_frames.last();
    if (!frame.callCopied || !has(Mechanism::DirectCalls) || !has(Mechanism::Inlining) || isUnreachable())
        return;
    const BodyIndex& index = *frame.index;
    BodyBlock entry = index.block(entryBlock);
    BodyTerminator end = entry.terminator();
    if (end.kind() != TerminatorKind::Jump || end.next().block() != inputHead)
        return;
    for (uint32_t number = 0; number < entry.numberOfParameters(); ++number) {
        Reference passed = referenceBoundTo(entry.parameter(number));
        if (!passed || !passed.isRecord() || m_code.at(passed.id()).opcode != Opcode::CallDirect || !program().at(m_code.at(passed.id()).function()).has(FunctionFlags::IsGenerator))
            continue;
        std::optional<LoopThatStepsByHand> byHand = onlyStepsParameter(program(), index, entry.parameter(number)) ? findLoopThatStepsByHand(program(), index, entry.parameter(number)) : std::nullopt;
        if (!byHand || byHand->placeOfResult || byHand->inputHead != inputHead)
            continue;
        ValueID read = index.value(byHand->step).as<Opcode::Call>().calleeOperand();
        if (takePassedGeneratorForSteps(index.value(read)))
            return;
    }
}

static ValueID findSpreadOfArrayFrom(const BodyIndex& index, ValueID generatorCall)
{
    BodyBlock block = index.block(index.blockOf(generatorCall));
    ValueID test(generatorCall.index() + 1);
    BodyTerminator end = block.terminator();
    if (test.index() + 1 != block.endOfValues().index() || index.opcodeOf(test) != Opcode::IsPristineArray || index.value(test).operand(0) != generatorCall || end.kind() != TerminatorKind::Branch || end.operand(0) != test || block.hasHandler())
        return { };
    BodyBlock spreading = index.block(end.taken().block());
    if (index.numberOfIncomingEdges(spreading.id()) != 1 || spreading.numberOfParameters() || spreading.numberOfValues() != 2 || spreading.hasHandler() || index.isEnteredByHandlerEdge(spreading.id()))
        return { };
    BodyValue array = index.value(spreading.firstValue());
    ValueID spread(spreading.firstValue().index() + 1);
    if (array.opcode() != Opcode::NewArray || array.numberOfVariadicOperands() || index.opcodeOf(spread) != Opcode::AppendSpread)
        return { };
    auto append = index.value(spread).as<Opcode::AppendSpread>();
    return append.arrayOperand() == array.id() && append.iterableOperand() == generatorCall ? spread : ValueID();
}

struct LoopOverMethodCall {
    LoopID loop;
    ValueID open;
};

static std::optional<LoopOverMethodCall> findLoopOverCall(const BodyIndex& index, const BodyValue& read)
{
    BodyBlock block = index.block(index.blockOf(read.id()));
    bool isCall = read.opcode() == Opcode::Call;
    ValueID call = isCall ? read.id() : index.onlyUsingValue(read.id());
    if (!call || !block.holds(call) || index.opcodeOf(call) != Opcode::Call || (!isCall && (index.value(call).as<Opcode::Call>().calleeOperand() != read.id() || index.value(call).as<Opcode::Call>().thisValueOperand() != read.objectOperand())))
        return std::nullopt;
    ValueID open = index.onlyUsingValue(call);
    if (!open || open.index() != call.index() + 1 || !block.holds(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return std::nullopt;
    auto opened = index.value(open).as<Opcode::IteratorOpen>();
    if (opened.iteratorKind() != IteratorKind::Sync || opened.isDelegating() || opened.isDestructuring())
        return std::nullopt;
    std::optional<LoopOverIterator> found = findLoopOverIterator(index, open);
    if (!found || !found->stepStandsInHeadOfLoop || found->blockOfStep == block.id() || !index.value(found->step).as<Opcode::IteratorResume>().heldOperands().empty() || !blockOfStepOnlySteps(index, block, open, *found))
        return std::nullopt;
    for (uint32_t number = read.id().index() + 1; number < call.index(); ++number) {
        ValueID between(number);
        const Effects& bound = declaredBoundInTable(index.opcodeOf(between));
        if (bound.mayRunOtherCode() || operandsMayRunCode(index.opcodeOf(between)) || bound.writes.contains(AbstractHeap::NamedProperties))
            return std::nullopt;
        ValueID user = index.onlyUsingValue(between);
        if (index.useCount(between) > 1 || (index.useCount(between) && (!user || user.index() > call.index() || !block.holds(user))))
            return std::nullopt;
    }
    return LoopOverMethodCall { index.loopOf(found->blockOfStep), open };
}

bool Walker::findReadsAfterRegion(std::span<const BlockID> region, ValueID firstValue, Vector<ValueID, 4>& readAfterRegion)
{
    Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    auto isInRegion = [&](BlockID block) {
        return std::ranges::binary_search(region, block.index(), { }, [](BlockID each) { return each.index(); });
    };
    ValueID endOfValues = firstValue;
    for (BlockID member : region)
        endOfValues = std::max(endOfValues, index.block(member).endOfValues(), [](ValueID one, ValueID other) { return one.index() < other.index(); });
    for (BlockID member : region) {
        for (BodyTarget target : index.terminator(member).targets()) {
            if (isInRegion(target.block()))
                continue;
            m_liveness.forEachLiveAtHead(target.block(), firstValue, endOfValues, [&](ValueID live) {
                if (live.index() >= firstValue.index() && isInRegion(index.blockOf(live)) && !readAfterRegion.contains(live))
                    readAfterRegion.append(live);
            });
        }
    }
    for (BlockID member : region) {
        BodyBlock block = index.block(member);
        for (uint32_t value = std::max(block.firstParameter().index(), firstValue.index()); value < block.endOfValues().index(); ++value) {
            uint32_t numberOutsideRegion = numberOfHandlersThatRead(frame, ValueID(value));
            if (!numberOutsideRegion)
                continue;
            for (BlockID inside : region)
                numberOutsideRegion -= index.block(inside).isHandlerBlock() && m_liveness.isLiveAtHead(ValueID(value), inside);
            if (numberOutsideRegion)
                return false;
        }
    }
    return true;
}

static bool mayOverrideReturnByName(const Walker& walker)
{
    const LookupsPastProgramObjects& lookups = walker.facts().linked().lookups;
    PropertyKeyID key = walker.program().keyOf(BuiltinNames::returnKeyword);
    return key && (lookups.namesWrittenThroughUnseenByProgram.get(key.index()) || lookups.namesWrittenThroughLostBuiltins.get(key.index()));
}

static BuiltinID findCalledBuiltin(const Walker& walker, const BodyIndex& body, const BodyValue& call, bool isInWalkedFrame)
{
    ValueID callee = call.opcode() == Opcode::Call ? body.stripCasts(call.as<Opcode::Call>().calleeOperand()) : ValueID();
    if (!callee || body.isParameter(callee))
        return { };
    if (body.opcodeOf(callee) == Opcode::Builtin)
        return body.value(callee).as<Opcode::Builtin>().builtin();
    if (body.opcodeOf(callee) != Opcode::PropGet || body.value(callee).isFreeName())
        return { };
    BodyValue read = body.value(callee);
    ValueID object = body.stripCasts(read.objectOperand());
    if (!body.isParameter(object) && body.opcodeOf(object) == Opcode::Builtin)
        return findConstantMemberFrom(walker.program(), walker.facts(), body.value(object).as<Opcode::Builtin>().builtin(), read.key());
    Reference reference = isInWalkedFrame ? walker.referenceBoundTo(object) : Reference();
    return reference && !reference.isVirtual() ? findMemberOnObjectAsMade(walker, walker.typeOf(reference), read.key()) : BuiltinID();
}

static bool mayStoreUnderUnknownKey(const Walker& walker, BuiltinID builtin)
{
    const BuiltinTable& table = walker.program().builtins();
    std::span<const BuiltinOverload> overloads = table.overloadsOf(table.canonicalID(builtin));
    return overloads.empty() || std::ranges::any_of(overloads, [](const BuiltinOverload& overload) {
        return overload.isUndeclared() || !overload.flow.callbacks.empty() || !overload.flow.handlerCalls.empty() || !overload.flow.propertyActions.empty() || overload.effects.writes.contains(AbstractHeap::NamedProperties) || overload.effects.writes.contains(AbstractHeap::PrototypeLinks);
    });
}

static bool mayStoreUnderUnknownKey(const Walker& walker, FunctionID function, const BodyIndex& body, BodyBlock block, bool isInWalkedFrame, bool inspectsCallees)
{
    const Facts& facts = walker.facts();
    bool storesUnderComputedKey = facts.summaries().at(function).has(SummaryTrait::StoresUnderComputedKey);
    for (BodyValue operation : block.values()) {
        if (operation.opcode() == Opcode::ElemSet && storesUnderComputedKey)
            return true;
        if (!callsCode(operation.opcode()))
            continue;
        FunctionID callee = facts.linked().calleeAt(function, operation.id());
        if (!callee) {
            BuiltinID builtin = findCalledBuiltin(walker, body, operation, isInWalkedFrame);
            if (!builtin || mayStoreUnderUnknownKey(walker, builtin))
                return true;
            continue;
        }
        Effects effects = facts.effectsOnReturn(callee);
        effects.merge(facts.effectsOnThrow(callee));
        if (!effects.mayRunOtherCode() && !effects.writes.contains(AbstractHeap::NamedProperties) && !effects.writes.contains(AbstractHeap::PrototypeLinks))
            continue;
        const BodyIndex* bodyOfCallee = inspectsCallees ? facts.bodies().find(callee) : nullptr;
        if (!bodyOfCallee)
            return true;
        for (uint32_t number = 0; number < bodyOfCallee->numberOfBlocks(); ++number) {
            if (mayStoreUnderUnknownKey(walker, callee, *bodyOfCallee, bodyOfCallee->block(BlockID(number)), false, false))
                return true;
        }
    }
    return false;
}

static bool mayOverrideReturnInIteration(const Walker& walker, FunctionID consumer, const BodyIndex& index, std::span<const BlockID> region, FunctionID generator)
{
    const LookupsPastProgramObjects& lookups = walker.facts().linked().lookups;
    if (!lookups.writesUnderAnyNameThroughUnseenByProgram && !lookups.writesUnderAnyNameThroughLostBuiltins)
        return false;
    for (BlockID block : region) {
        if (mayStoreUnderUnknownKey(walker, consumer, index, index.block(block), true, true))
            return true;
    }
    const BodyIndex& body = walker.facts().bodies().at(generator);
    for (uint32_t number = 0; number < body.numberOfBlocks(); ++number) {
        if (mayStoreUnderUnknownKey(walker, generator, body, body.block(BlockID(number)), false, true))
            return true;
    }
    return false;
}

bool Walker::mayWalkMethodGenerator(const BodyValue& read, FunctionID method)
{
    size_t frameIndex = m_frames.size() - 1;
    Frame& frame = m_frames[frameIndex];
    const BodyIndex& index = *frame.index;
    if (!program().at(method).has(FunctionFlags::IsGenerator) || isBeingWalkedOtherThanAtYield(method) || m_code.inlineFrames().size() >= mostInlineFramesOfGeneratorsInLoops || mayOverrideReturnByName(*this))
        return false;
    if (!has(Mechanism::Folding) || !has(Mechanism::Handlers) || !m_opensGuesses || m_coldCopyDepth || hasStringsInBuilders() || walksStretchInsideAnother() || !m_versionedLoops.isEmpty() || !frame.loopsBeingVersioned.isEmpty() || !frame.blocksBeingTailDuplicated.isEmpty() || !m_generatorsWalkedInConsumers.isEmpty())
        return false;
    std::optional<LoopOverMethodCall> found = findLoopOverCall(index, read);
    if (!found || endOfStretchBeingWalked() < index.block(index.blockOf(read.id())).numberOfValues())
        return false;
    auto& kept = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops);
    const GeneratorFunctionDescribed& described = kept.functions.ensure(method.index(), [&] { return describeGeneratorFunction(*this, method); }).iterator->value;
    if (!described.bodyMayBeWalkedInConsumer || !described.isQuietBeforeInitialYield)
        return false;
    const LinkTables& linked = facts().linked();
    if (!engineAnswersForGenerators(*this) || !linked.mayBeMentionedBy(unit(), method) || (!linked.function(method).namesOnlyWhatEveryFunctionLeadsTo && !linked.leadsTo(unit(), method)))
        return false;
    if (!m_liveness.isComputedFor(index))
        m_liveness.compute(index);
    BlockID head = index.loop(found->loop).head;
    Vector<BlockID, 8> region = collectRegionOfIteration(index, m_liveness, head, found->loop, found->open);
    if (mayOverrideReturnInIteration(*this, currentFunction(), index, region.span(), method))
        return false;
    uint64_t numberOfOperations = facts().summaries().at(method).numberOfOperations;
    for (BlockID member : region) {
        numberOfOperations += index.block(member).numberOfValues() + 1;
        if (m_restArrayAllocator && frame.isOfUnit && member == m_restArrayAllocationBlock)
            return false;
    }
    Vector<ValueID, 4> readAfterRegion;
    return operationsFitWindowOfCallWrittenInLoop(*this, numberOfOperations) && findReadsAfterRegion(region.span(), index.block(head).firstParameter(), readAfterRegion);
}

void Walker::walkIterationAsWritten(BlockID start, const BodyValue& read)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock block = index.block(index.blockOf(read.id()));
    LoopOverMethodCall found = *findLoopOverCall(index, read);
    BlockID head = index.loop(found.loop).head;
    Vector<BlockID, 8> region = collectRegionOfIteration(index, m_liveness, head, found.loop, found.open);
    Vector<ValueID, 4> readAfterRegion;
    findReadsAfterRegion(region.span(), index.block(head).firstParameter(), readAfterRegion);
    uint32_t namesAtHead = inputBlock(frameIndex, head).firstNarrowerName;
    uint32_t walkedUpTo = m_frames[frameIndex].walkedUpTo;
    m_generatorMethodsTested.append({ m_frames[frameIndex].inlineFrame, read.id() });
    for (ValueID value : readAfterRegion)
        m_frames[frameIndex].addDuplicatedValue(value);
    m_frames[frameIndex].loopsBeingVersioned.append(found.loop);

    NamesOfWay names;
    Vector<std::pair<ValueID, Reference>, 8> boundOnWay;
    if (Frame::DuplicatedValues* listed = m_frames[frameIndex].duplicatedValues.get()) {
        names = listed->namesOfWay;
        for (ValueID value : listed->boundOnWay) {
            if (Reference name = m_frames[frameIndex].references[value.index()]; name && listed->contains(value))
                boundOnWay.append({ value, name });
        }
    }
    Origin originInFront = m_origin;
    BlockID behindTests = m_block;
    WalkState stateAfterTests = m_state;
    m_block = { };
    m_state.clear();
    continueIn(start);
    {
        ClosedArm closedArm(*this);
        SetForScope coldCopyScope(m_coldCopyDepth, m_coldCopyDepth + 1);
        if (!isUnreachable())
            walkStretch(block.id(), read.id().index() - block.firstValue().index(), block.numberOfValues());
        if (!isUnreachable() && !m_hasFailed) {
            Vector<Reference, 8> entering;
            for (ValueID argument : block.terminator().next().arguments())
                entering.append(referenceOf(argument));
            walkCopyOfLoop(head, entering.span());
            for (BlockID member : region) {
                if (!m_hasFailed && !index.isInLoop(member, found.loop))
                    walkBlock(member);
            }
        }
        for (BlockID member : region) {
            inputBlock(frameIndex, member) = InputBlock { };
            BodyBlock walked = index.block(member);
            for (uint32_t value = walked.firstParameter().index(); value < walked.endOfValues().index(); ++value)
                bindInFrame(frameIndex, ValueID(value), Reference());
        }
    }
    inputBlock(frameIndex, head).firstNarrowerName = namesAtHead;
    m_frames[frameIndex].loopsBeingVersioned.removeLast();
    m_frames[frameIndex].walkedUpTo = walkedUpTo;
    switchToInputBlock(block.id());
    if (m_frames[frameIndex].duplicatedValues) {
        forgetNamesOfWay(m_frames[frameIndex]);
        takeNamesOfWay(block.id(), WTF::move(names));
        for (auto [value, name] : boundOnWay)
            bind(value, name);
    }
    m_origin = originInFront;
    m_block = behindTests;
    m_state = WTF::move(stateAfterTests);
}

bool Walker::walkGeneratorCallTwice(const BodyValue& call)
{
    if (isInLoop() || isNestedTooDeeply() || isUnreachable() || m_hasFailed || !has(Mechanism::TypedOperations) || !has(Mechanism::DirectCalls) || !has(Mechanism::Guesses) || call.isDoneInPlace())
        return false;
    Reference closure = referenceOf(call.as<Opcode::Call>().calleeOperand());
    FunctionID function = closure && closure.isRecord() ? findOnlyCallee(*this, call, closure) : FunctionID();
    if (!function || m_generatorMethodsTested.contains(std::pair { m_frames.last().inlineFrame, call.id() }))
        return false;
    auto& kept = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops);
    auto described = kept.functions.find(function.index());
    if (described == kept.functions.end() || !described->value.nobodyLooksAbove || *described->value.nobodyLooksAbove || !mayWalkMethodGenerator(call, function))
        return false;

    Origin origin = originOf(call);
    origin.isTailCall = false;
    BlockID asWritten = addBlock(BlockFrequency::Rare);
    auto continueIfPassed = [&](Reference passes) {
        BlockID passed = addBlock();
        branch(passes, passed, asWritten);
        continueIn(passed);
    };
    recordSpeculation();
    count(StatisticID::GeneratorCallsTestedBeforeLoops);
    if (Reference fails = drawWhetherGuessFails(origin)) {
        BlockID passed = addBlock();
        branch(fails, asWritten, passed);
        continueIn(passed);
    }
    Draft test(Opcode::MakesPlainGenerators, origin);
    chooseConservativeFields(test);
    test.setFunctionOperand(closure);
    if (Reference passes = build(WTF::move(test)); passes && !isUnreachable())
        continueIfPassed(passes);
    if (!m_hasFailed)
        walkIterationAsWritten(asWritten, call);
    return m_hasFailed || isUnreachable() || takeGeneratorForConsumer(call);
}

bool Walker::takeGeneratorForConsumer(const BodyValue& call)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    const bool stepsPassed = call.opcode() == Opcode::PropGet;
    bool takerCallsMethodItself = call.opcode() != Opcode::Call;
    ValueID taker = takerCallsMethodItself ? call.id() : index.onlyUsingValue(call.id());
    std::optional<LoopThatStepsByHand> byHand;
    std::optional<StepsInRow> row;
    bool copiesIntoArray = false;
    if (stepsPassed) {
        byHand = findLoopThatStepsByHand(program(), index, index.stripCasts(call.objectOperand()));
        if (!byHand)
            return false;
        taker = byHand->generator;
    } else if (ValueID spread = !takerCallsMethodItself && isInLoop() ? findSpreadOfArrayFrom(index, call.id()) : ValueID()) {
        taker = spread;
        copiesIntoArray = true;
    } else if (!taker || (index.opcodeOf(taker) != Opcode::IteratorOpen && index.opcodeOf(taker) != Opcode::AppendSpread)) {
        if (takerCallsMethodItself || (taker ? !opcodesThatStateTypes.contains(index.opcodeOf(taker)) : !index.useCount(call.id()) || index.useCount(call.id()) % 2) || !holdsLoopsThatStepByHand(currentFunction()))
            return false;
        byHand = findLoopThatStepsByHand(program(), index, call.id());
        if (!byHand && isInLoop())
            row = findStepsInRow(program(), index, call.id());
        if (!byHand && !row)
            return false;
        taker = byHand ? byHand->generator : row->generator;
    }
    if (!has(Mechanism::TypedOperations) || !has(Mechanism::Folding) || !has(Mechanism::Handlers) || !m_opensGuesses || m_coldCopyDepth || (!takerCallsMethodItself && call.isDoneInPlace()))
        return false;
    bool isPatternOrSpread = !byHand && (index.opcodeOf(taker) == Opcode::AppendSpread || (index.opcodeOf(taker) == Opcode::IteratorOpen && index.value(taker).as<Opcode::IteratorOpen>().isDestructuring()));
    if (hasStringsInBuilders() || (!m_versionedLoops.isEmpty() && !isPatternOrSpread && !(row && row->steps.size() == 1) && !frameMayMove(frameIndex)) || !m_frames[frameIndex].blocksBeingTailDuplicated.isEmpty())
        return false;
    BodyBlock blockOfCall = index.block(index.blockOf(call.id()));
    if ((!takerCallsMethodItself && !byHand && !row && !copiesIntoArray && taker.index() != call.id().index() + 1) || walksStretchInsideAnother())
        return false;
    if (copiesIntoArray ? endOfStretchBeingWalked() < blockOfCall.numberOfValues() : !stepsPassed && (!blockOfCall.holds(taker) || taker.index() - blockOfCall.firstValue().index() >= endOfStretchBeingWalked()))
        return false;
    if (byHand && !(stepsPassed && !byHand->placeOfResult) && endOfStretchBeingWalked() < blockOfCall.numberOfValues())
        return false;
    if (byHand && m_generatorsTakenByConsumers.containsIf([&](const GeneratorTakenByConsumer& taken) { return taken.inlineFrame == m_frames[frameIndex].inlineFrame && taken.inputHead == byHand->inputHead && taken.taker != taker; }))
        return false;
    if (row && (row->steps.last().index() - blockOfCall.firstValue().index() >= endOfStretchBeingWalked() || !stepsStandApart(*row)))
        return false;

    FunctionID callee;
    Reference iterable;
    Reference passedGenerator;
    ValueID callOfGenerator = takerCallsMethodItself ? ValueID() : call.id();
    uint32_t frameOfCall = static_cast<uint32_t>(frameIndex);
    PropertyKeyID keyOfMethod = types().wellKnownSymbolKey(WellKnownSymbol::Iterator);
    if (takerCallsMethodItself) {
        ValueID iterated = stepsPassed ? call.objectOperand() : call.opcode() == Opcode::IteratorOpen ? call.as<Opcode::IteratorOpen>().iterableOperand() : call.as<Opcode::AppendSpread>().iterableOperand();
        iterable = referenceOf(iterated);
        passedGenerator = has(Mechanism::DirectCalls) ? findPassedGenerator(call.id(), iterated, callOfGenerator, frameOfCall) : Reference();
        if (stepsPassed && !passedGenerator)
            return false;
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
            return passesGeneratorForIteration(caller, passed.site, callerID) || (!caller.isParameter(passed.site) && findSpreadOfArrayFrom(caller, passed.site)) || (!caller.isParameter(passed.site) && caller.opcodeOf(passed.site) == Opcode::Call && isOnlySteppedByHand(program(), caller, passed.site));
        }));
        keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.find(callee.index())->value.nobodyLooksAbove = nobodyLooksAbove;
    }
    const GeneratorFunctionDescribed& described = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.find(callee.index())->value;
    if (!engineAnswersForGenerators(*this))
        return false;
    Reference functionTested = !takerCallsMethodItself && (m_generatorMethodsTested.contains(std::pair { m_frames[frameIndex].inlineFrame, call.id() }) || m_generatorMethodsTested.contains(std::pair { m_frames[frameIndex].inlineFrame, index.stripCasts(call.as<Opcode::Call>().calleeOperand()) })) ? referenceOf(call.as<Opcode::Call>().calleeOperand()) : Reference();
    bool needsModificationGuard = !*described.nobodyLooksAbove && !functionTested;
    if (needsModificationGuard && (!passedGenerator || stepsPassed || call.opcode() != Opcode::IteratorOpen || !has(Mechanism::Guesses) || !mayLeaveCopyForCall() || mayOverrideReturnByName(*this)))
        return false;
    if (((byHand && byHand->frontIsSeen) || (row && row->frontIsSeen)) && !described.isQuietBeforeInitialYield)
        return false;
    if (row && row->steps.size() > 1 && !described.hasOneWay)
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
    bool consumerCalls = row || byHand || copiesIntoArray;

    if (row) {
        uint32_t numberOfOperationsBetween = row->steps.last().index() - row->steps[0].index();
        numberOfOperations += numberOfOperationsBetween * described.numberOfYields;
        for (uint32_t value = row->steps[0].index(); value < row->steps.last().index(); ++value) {
            if (numberOfHandlersThatRead(m_frames[frameIndex], ValueID(value)))
                return false;
        }
        taken.callPosition = index.value(row->steps[0]).position();
        taken.stepsInRow = WTF::move(row->steps);
    } else if (!byHand && index.opcodeOf(taker) == Opcode::AppendSpread) {
        auto spread = index.value(taker).as<Opcode::AppendSpread>();
        if (!takerCallsMethodItself && (spread.iterableOperand() != call.id() || spread.arrayOperand() == call.id()))
            return false;
        taken.callPosition = index.value(taker).position();
    } else if (!byHand && index.value(taker).as<Opcode::IteratorOpen>().isDestructuring()) {
        auto pattern = index.value(taker).as<Opcode::IteratorOpen>();
        RestOfPattern rest;
        consumerCalls = true;
        if (pattern.iteratorKind() != IteratorKind::Sync || pattern.isDelegating() || !stepsOfPatternStandInRow(index, index.value(taker), &rest) || *pattern.elementCount() > maxGeneratorPatternSteps)
            return false;
        if (!*pattern.elementCount() && !rest.array && !described.isQuietBeforeInitialYield)
            return false;
        taken.restArray = rest.array;
        taken.numberOfPatternSteps = *pattern.elementCount();
        taken.callPosition = index.value(taker).position();
        numberOfOperations += described.numberOfYields * (4 * uint64_t { taken.numberOfPatternSteps }) + described.numberOfOperationsOnWaysOfReturn;
    } else {
        if (byHand) {
            taken.isSteppedByHand = true;
            taken.headPrecedesLoop = byHand->loopIsSeenBeforeStep && !described.isQuietBeforeInitialYield;
            bool holdsIterations = facts().bodies().at(callee).holdsAnyOf(OpcodeSet { Opcode::IteratorOpen, Opcode::AppendSpread }) || holdsLoopsThatStepByHand(callee);
            if (byHand->stepInFront && !holdsIterations)
                taken.stepInFront = byHand->stepInFront;
            taken.placeOfResult = byHand->placeOfResult;
            taken.blockOfStep = byHand->blockOfStep;
            taken.blocksBeforeStep = WTF::move(byHand->blocksBeforeStep);
            taken.step = byHand->step;
            taken.inputHead = byHand->inputHead;
            taken.hasBackEdge = true;
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
            taken.hasBackEdge = found->stepStandsInHeadOfLoop;
        }
        taken.callPosition = index.value(taken.step).position();
        LoopID loop = taken.hasBackEdge ? index.loopOf(taken.inputHead) : LoopID();
        size_t numberOfRegionsBeingWalked = m_frames[frameIndex].references.size() > index.numberOfValues();
        for (const GeneratorWalkedInConsumer& around : m_generatorsWalkedInConsumers)
            numberOfRegionsBeingWalked += around.inputHead && (around.frameOfTurn == UINT32_MAX ? around.frameOfConsumer : around.frameOfTurn) == frameIndex;
        if (m_frames[frameIndex].loopsBeingVersioned.size() != numberOfRegionsBeingWalked)
            return false;

        if (!m_liveness.isComputedFor(index))
            m_liveness.compute(index);
        taken.region = collectRegionOfIteration(index, m_liveness, taken.inputHead, loop, taker);
        if (needsModificationGuard && mayOverrideReturnInIteration(*this, currentFunction(), index, taken.region.span(), callee))
            return false;
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
                consumerCalls |= callsCode(opcode);
                numberOfCloses += opcode == Opcode::IteratorClose && operation.as<Opcode::IteratorClose>().iteratorOperand() == taker;
                holdsWhatIsWalkedInPlaceOfItself |= opcode == Opcode::Yield;
                taken.regionHoldsIterations |= opcode == Opcode::Yield || opcode == Opcode::IteratorOpen || opcode == Opcode::AppendSpread;
                if (FunctionID called = opcode == Opcode::Call ? facts().linked().calleeAt(currentFunction(), operation.id()) : FunctionID(); called && program().at(called).has(FunctionFlags::IsGenerator) && operation.id() != call.id() && (findLoopThatStepsByHand(program(), index, operation.id()) || findStepsInRow(program(), index, operation.id()))) {
                    taken.regionHoldsIterations = true;
                    holdsWhatIsWalkedInPlaceOfItself = true;
                }
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
    const bool isLarge = !operationsFitWindowOfCallWrittenInLoop(*this, numberOfOperations);
    if (isLarge && (depthOfCopies() || consumerCalls || !facts().bodies().at(callee).hasLoops() || numberOfOperations > maxLargeGeneratorOperations))
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
    if (needsModificationGuard && !checkPassedGenerator(passedGenerator, originOf(call), &taken.holderOfReturn))
        return true;
    if (functionTested) {
        taken.holderOfReturn = buildGeneratorPrototype(functionTested, originOf(call));
        if (!taken.holderOfReturn)
            return true;
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
    if (isLarge)
        count(StatisticID::LargeGeneratorsInlinedIntoLoops);
    else
        m_operationsCopiedAtWrittenCall += static_cast<uint32_t>(std::min<uint64_t>(numberOfOperations, UINT32_MAX / 2));
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = callee;
    Reference state = taken.state;
    m_generatorsTakenByConsumers.removeAllMatching([&](const GeneratorTakenByConsumer& earlier) {
        return earlier.inlineFrame == taken.inlineFrame && earlier.taker == taken.taker;
    });
    m_generatorsTakenByConsumers.append(WTF::move(taken));
    if (stepsPassed) {
        m_hasTakenGeneratorsSteppedByHand = true;
        for (uint32_t cast = index.stripCasts(call.objectOperand()).index(); cast <= taker.index(); ++cast)
            bind(ValueID(cast), state);
        return true;
    }
    if (!takerCallsMethodItself) {
        bind(call.id(), state);
        m_hasTakenGeneratorsSteppedByHand |= copiesIntoArray;
        if (byHand || row) {
            m_hasTakenGeneratorsSteppedByHand = true;
            for (uint32_t cast = call.id().index() + 1; cast <= taker.index(); ++cast)
                bind(ValueID(cast), state);
            m_frames[frameIndex].walkedUpTo = std::max(m_frames[frameIndex].walkedUpTo, taker.index() + 1 - blockOfCall.firstValue().index());
        }
        return true;
    }
    if (!passedGenerator)
        count(StatisticID::GeneratorMethodsInlinedIntoIterations);
    if (call.opcode() == Opcode::AppendSpread)
        return spreadGeneratorTakenAt(m_generatorsTakenByConsumers.size() - 1, call);
    if (m_generatorsTakenByConsumers.last().numberOfPatternSteps || m_generatorsTakenByConsumers.last().restArray)
        return patternTakesGeneratorAt(m_generatorsTakenByConsumers.size() - 1, call);
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
    if (m_generatorsTakenByConsumers[place].numberOfPatternSteps || m_generatorsTakenByConsumers[place].restArray)
        return patternTakesGeneratorAt(place, open);
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

bool Walker::frameMayMove(size_t from) const
{
    return !m_versionedLoops.containsIf([&](const VersionedLoop& versionedLoop) { return versionedLoop.frameIndex == from; }) && !m_sharedGenericVersions.containsIf([&](const SharedGenericVersion& copy) { return copy.frameIndex == from; });
}

void Walker::moveFrame(size_t from, size_t to)
{
    RELEASE_ASSERT(m_hasFailed || frameMayMove(from));
    if (to == m_frames.size()) {
        size_t lowestNotAsked = from + 1;
        while (lowestNotAsked && m_frames[lowestNotAsked - 1].numberOfFramesUpToHandlerBelow == Frame::notAskedYet)
            --lowestNotAsked;
        for (size_t index = lowestNotAsked; index <= from; ++index)
            m_frames[index].numberOfFramesUpToHandlerBelow = !index ? 0 : m_frames[index - 1].hasHandler ? static_cast<uint32_t>(index) : m_frames[index - 1].numberOfFramesUpToHandlerBelow;
        Frame moved = WTF::move(m_frames[from]);
        m_frames[from].callCopied = std::exchange(moved.callCopied, ValueID());
        m_frames[from].hasHandler = false;
        m_frames[from].isOfUnit = false;
        m_frames.append(WTF::move(moved));
    } else {
        Frame moved = WTF::move(m_frames[from]);
        RELEASE_ASSERT(from + 1 == m_frames.size());
        m_frames.removeLast();
        moved.callCopied = m_frames[to].callCopied;
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
        uint32_t firstValue = member != walked.inputHead ? block.firstParameter().index() : walked.isSteppedByHand ? block.firstValue().index() : walked.step.index() + 1;
        for (uint32_t value = firstValue; value < block.endOfValues().index(); ++value)
            bindInFrame(frameIndex, ValueID(value), Reference());
    }
    inputBlock(frameIndex, walked.inputHead).firstNarrowerName = walked.namesAtHead;
    if (walked.blockOfStep)
        inputBlock(frameIndex, walked.blockOfStep).firstNarrowerName = walked.namesAtHead;
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

void Walker::leaveCompletedDelegation(BlockID inputHead, ValueID step, Reference hasCompleted, Reference result)
{
    size_t frameIndex = m_frames.size() - 1;
    BlockID blockOfOpen = m_frames[frameIndex].block;
    uint32_t walkedUpTo = m_frames[frameIndex].walkedUpTo;
    Origin origin = m_origin;
    BodyBlock head = m_frames[frameIndex].index->block(inputHead);

    BlockID isCompleted = addBlock();
    BlockID isOpen = addBlock();
    branch(hasCompleted, isCompleted, isOpen);
    continueIn(isCompleted);
    Draft nothing(Opcode::Undefined, m_origin);
    nothing.representation = Representation::JSValue;
    Vector<Reference, 4> carried;
    carried.fill(constant(WTF::move(nothing)), head.numberOfParameters());
    walkHeadOfLoopBehindStep(inputHead, step, carried.span(), true, result);
    if (m_hasFailed)
        return;

    m_block = { };
    m_frames[frameIndex].block = { };
    setCurrentInputBlock(blockOfOpen);
    m_frames[frameIndex].walkedUpTo = walkedUpTo;
    m_origin = origin;
    continueIn(isOpen);
}

void Walker::walkManualIterationAfterStep(const GeneratorWalkedInConsumer& walked, std::span<const Reference> carried, bool isDone, Reference stepValue)
{
    size_t frameIndex = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameIndex].index;
    BodyBlock block = index.block(walked.blockOfStep);
    m_frames[frameIndex].block = { };
    setCurrentInputBlock(walked.blockOfStep);
    m_origin = originOf(index.value(walked.step));
    m_origin.isTailCall = false;

    bindValuesThatLoopBringsRound(walked.inputHead, carried);
    Draft written(Opcode::Boolean, m_origin);
    written.setBoolean(isDone);
    written.representation = Representation::JSValue;
    Reference result = buildResultOfStep(*this, stepValue, constant(WTF::move(written)), m_origin);
    if (isUnreachable() || hasFailed())
        return;
    noteFreshObject(*this, result, 2);
    ValueID named = walked.placeOfResult ? block.parameter(*walked.placeOfResult) : walked.step;
    bind(named, result);
    storeValueBehindItsDefinition(named);
    uint32_t begin = !walked.placeOfResult ? walked.step.index() + 1 - block.firstValue().index() : block.numberOfValues() && index.opcodeOf(block.firstValue()) == Opcode::LoopHint;
    walkStretch(walked.blockOfStep, begin, block.numberOfValues());
    walkTerminator(block.terminator());
    for (size_t number = 0; number < walked.region.size() && !m_hasFailed; ++number) {
        BlockID member = walked.region[number];
        if (member != walked.blockOfStep && member != walked.inputHead && !walked.blocksBeforeStep.contains(member))
            walkBlock(member);
    }
}

Walker::EndOfManualIteration Walker::walkManualIterationToStep(const GeneratorWalkedInConsumer& walked)
{
    EndOfManualIteration end;
    end.generator = walked.state;
    SetForScope turn(m_endOfManualIteration, &end);
    BodyBlock head = currentBodyIndex().block(walked.inputHead);
    size_t numberOfLoopsAround = m_openLoops.size();
    m_frames.last().block = { };
    setCurrentInputBlock(walked.inputHead);
    uint32_t begin = head.numberOfValues() && currentBodyIndex().opcodeOf(head.firstValue()) == Opcode::LoopHint;
    walkStretch(walked.inputHead, begin, head.numberOfValues());
    if (walked.blockOfStep != walked.inputHead) {
        walkTerminator(head.terminator());
        for (size_t number = 0; number < walked.blocksBeforeStep.size() && !m_hasFailed; ++number) {
            if (walked.blocksBeforeStep[number] != walked.inputHead)
                walkBlock(walked.blocksBeforeStep[number]);
        }
        if (!m_hasFailed)
            walkBlock(walked.blockOfStep);
    }
    endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
    m_block = end.isReached ? end.block : BlockID();
    if (end.isReached)
        m_state = WTF::move(end.state);
    else
        m_state.clear();
    return end;
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

bool Walker::leaveHandlerOfGenerator(size_t frameOfGenerator)
{
    if (!std::exchange(m_frames[frameOfGenerator].hasHandler, false))
        return false;
    if (m_code.at(m_block).handler)
        jumpTo(addBlock());
    return true;
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

    if (Reference list = m_generatorsWalkedInConsumers[place].list; list && m_generatorsWalkedInConsumers[place].numberOfPatternSteps) {
        walkYieldOfGeneratorInPatternWithRest(place, yield, yielded, originAtYield);
        return true;
    } else if (list) {
        appendYielded(list, yielded, originAtYield);
        if (Reference resumption = isUnreachable() ? Reference() : resumptionOfGenerator({ }, ResumeMode::Next, originAtYield))
            bind(yield.id(), resumption);
        return true;
    }

    if (m_generatorsWalkedInConsumers[place].numberOfPatternSteps) {
        walkYieldOfGeneratorInPattern(place, yield, yielded, originAtYield);
        return true;
    }
    if (!m_generatorsWalkedInConsumers[place].stepsInRow.isEmpty()) {
        walkYieldOfGeneratorInRow(place, yield, yielded, originAtYield);
        return true;
    }

    const size_t frameOfGenerator = m_frames.size() - 1;
    const size_t frameOfConsumer = m_generatorsWalkedInConsumers[place].frameOfConsumer;
    const size_t frameOfTurn = m_frames.size();
    const BlockID inputHead = m_generatorsWalkedInConsumers[place].inputHead;
    const ValueID step = m_generatorsWalkedInConsumers[place].step;
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfGenerator);
    const bool countsSteps = m_generatorsWalkedInConsumers[place].countsSteps;
    if (countsSteps)
        carried.removeLast();

    bool generatorStoodUnderHandler = leaveHandlerOfGenerator(frameOfGenerator);
    m_generatorsWalkedInConsumers[place].yieldStoodAt = yield.id();
    moveFrame(frameOfConsumer, frameOfTurn);
    m_generatorsWalkedInConsumers[place].frameOfTurn = static_cast<uint32_t>(frameOfTurn);
    RELEASE_ASSERT(!m_frames[frameOfTurn].loopsBeingVersioned.isEmpty());
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = true;
    forgetRegionOfIteration(frameOfTurn, m_generatorsWalkedInConsumers[place]);

    size_t numberOfLoopsAround = m_openLoops.size();
    m_generatorsWalkedInConsumers[place].numberOfLoopsAroundTurn = static_cast<uint32_t>(numberOfLoopsAround);
    const bool isSteppedByHand = m_generatorsWalkedInConsumers[place].isSteppedByHand;
    const std::optional<uint32_t> placeOfResult = m_generatorsWalkedInConsumers[place].placeOfResult;
    Reference sent;
    if (isSteppedByHand)
        walkManualIterationAfterStep(GeneratorWalkedInConsumer(m_generatorsWalkedInConsumers[place]), carried.span(), false, yielded);
    else
        walkHeadOfLoopBehindStep(inputHead, step, carried.span(), false, yielded);
    for (size_t number = 0; !isSteppedByHand && number < m_generatorsWalkedInConsumers[place].region.size() && !m_hasFailed; ++number) {
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
        if (isSteppedByHand && !placeOfResult && !m_hasFailed) {
            EndOfManualIteration end = walkManualIterationToStep(GeneratorWalkedInConsumer(m_generatorsWalkedInConsumers[place]));
            hasBackEdge = end.isReached;
            sent = end.sent;
        }
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

    if (placeOfResult) {
        Draft nothing(Opcode::Undefined, originAtYield);
        nothing.representation = Representation::JSValue;
        sent = std::exchange(carried[*placeOfResult], constant(WTF::move(nothing)));
    }
    if (sent && representationOf(sent) != Representation::JSValue)
        sent = convert(sent, Representation::JSValue, originAtYield);
    if (countsSteps)
        carried.append(BuilderForGenerators(*this, originAtYield).integer(1));
    uint32_t firstCarried = m_frames[frameOfGenerator].index->numberOfValues();
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(firstCarried + number), carried[number]);
    if (Reference resumption = resumptionOfGenerator(sent, ResumeMode::Next, originAtYield))
        bind(yield.id(), resumption);
    return true;
}


bool Walker::walkInitialYieldInConsumer(const BodyValue& initialYield)
{
    size_t place = placeOfGeneratorOnTop();
    if (place == notFound)
        return false;
    if (!m_generatorsWalkedInConsumers[place].headPrecedesLoop)
        return true;

    Origin origin = m_origin;
    origin.isTailCall = false;
    const size_t frameOfGenerator = m_frames.size() - 1;
    const size_t frameOfConsumer = m_generatorsWalkedInConsumers[place].frameOfConsumer;
    const size_t frameOfTurn = m_frames.size();
    const BlockID inputHead = m_generatorsWalkedInConsumers[place].inputHead;
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfGenerator);

    bool generatorStoodUnderHandler = leaveHandlerOfGenerator(frameOfGenerator);
    m_generatorsWalkedInConsumers[place].yieldStoodAt = initialYield.id();
    moveFrame(frameOfConsumer, frameOfTurn);
    m_generatorsWalkedInConsumers[place].frameOfTurn = static_cast<uint32_t>(frameOfTurn);
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = true;
    forgetRegionOfIteration(frameOfTurn, m_generatorsWalkedInConsumers[place]);
    m_generatorsWalkedInConsumers[place].numberOfLoopsAroundTurn = static_cast<uint32_t>(m_openLoops.size());
    bindValuesThatLoopBringsRound(inputHead, carried.span());
    bool isReached = walkManualIterationToStep(GeneratorWalkedInConsumer(m_generatorsWalkedInConsumers[place])).isReached;
    restoreBindingsBeforeLoop(m_generatorsWalkedInConsumers[place]);
    m_frames[frameOfTurn].block = { };
    setCurrentInputBlock(inputHead);
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = false;
    m_generatorsWalkedInConsumers[place].frameOfTurn = UINT32_MAX;
    moveFrame(frameOfTurn, frameOfConsumer);
    m_generatorsWalkedInConsumers[place].yieldStoodAt = { };
    m_frames[frameOfGenerator].hasHandler = generatorStoodUnderHandler;
    m_origin = origin;
    if (m_hasFailed || !isReached) {
        m_block = { };
        m_state.clear();
    }
    return true;
}

void Walker::walkYieldOfGeneratorInPattern(size_t place, const BodyValue& yield, Reference yielded, Origin origin)
{
    const uint32_t numberOfSteps = m_generatorsWalkedInConsumers[place].numberOfPatternSteps;
    const size_t frameOfGenerator = m_frames.size() - 1;
    const ValueID count(static_cast<uint32_t>(m_frames[frameOfGenerator].references.size()) - numberOfSteps - 1);
    auto nameOfStep = [&](uint32_t step) { return ValueID(count.index() + 1 + step); };
    BuilderForGenerators builder(*this, origin);
    Reference numberThatHasCome = referenceBoundTo(count);
    const Operation* written = numberThatHasCome && numberThatHasCome.isRecord() && m_code.isConstant(numberThatHasCome.id()) ? &m_code.at(numberThatHasCome.id()) : nullptr;
    std::optional<int64_t> known = written && written->opcode == Opcode::Integer ? std::optional<int64_t>(written->integer()) : std::nullopt;

    auto leave = [&] {
        bool generatorStoodUnderHandler = leaveHandlerOfGenerator(frameOfGenerator);
        m_generatorsWalkedInConsumers[place].yieldStoodAt = yield.id();
        buildFirstReadThatCloseOfGeneratorLeavesOut(*this, origin);
        if (!isUnreachable())
            resumeGeneratorToLeave(place, ResumeMode::Return, false, origin);
        m_generatorsWalkedInConsumers[place].yieldStoodAt = { };
        if (!isUnreachable() && !m_hasFailed) {
            noteCarriedValuesAtReturnOfGenerator();
            m_frames[frameOfGenerator].returnsOfCopy->append(Frame::ReturnOfCopy { m_block, Reference(), WTF::move(m_state), false, !m_opensGuesses });
        }
        m_frames[frameOfGenerator].hasHandler = generatorStoodUnderHandler;
        m_block = { };
        m_state.clear();
    };

    if (known ? *known + 1 == numberOfSteps : numberOfSteps == 1) {
        bind(nameOfStep(numberOfSteps - 1), yielded);
        leave();
        return;
    }
    if (known) {
        bind(nameOfStep(static_cast<uint32_t>(*known)), yielded);
        bind(count, builder.integer(*known + 1));
    } else {
        BlockID goesOn = builder.addBlock();
        BlockID isFilled = builder.addBlock();
        builder.branch(builder.isInteger(numberThatHasCome, numberOfSteps - 1), isFilled, goesOn);
        Vector<Reference, 4> inFront;
        for (uint32_t step = 0; step <= numberOfSteps; ++step)
            inFront.append(referenceBoundTo(ValueID(count.index() + step)));
        builder.continueIn(isFilled);
        bind(nameOfStep(numberOfSteps - 1), yielded);
        leave();
        for (uint32_t step = 0; step <= numberOfSteps; ++step)
            bind(ValueID(count.index() + step), inFront[step]);
        builder.continueIn(goesOn);
        if (isUnreachable() || m_hasFailed)
            return;

        BlockID isTaken = builder.addBlock();
        Vector<Reference, 4> parameters;
        parameters.append(addParameter(isTaken, { }, formOfParameter(m_types.integer(Range { 1, numberOfSteps - 1 }), { })));
        for (uint32_t step = 0; step + 1 < numberOfSteps; ++step)
            parameters.append(addParameter(isTaken, { }, Representation::JSValue));
        auto handOn = [&](uint32_t placeOfYielded) {
            Vector<Reference, 4> arguments;
            arguments.append(builder.integer(placeOfYielded + 1));
            for (uint32_t step = 0; step + 1 < numberOfSteps; ++step)
                arguments.append(step == placeOfYielded ? yielded : inFront[step + 1]);
            jumpTo(isTaken, arguments.span());
        };
        for (uint32_t step = 0; step + 2 < numberOfSteps; ++step) {
            BlockID isOfStep = builder.addBlock();
            BlockID isLater = builder.addBlock();
            builder.branch(builder.isInteger(numberThatHasCome, step), isOfStep, isLater);
            builder.continueIn(isOfStep);
            handOn(step);
            builder.continueIn(isLater);
        }
        handOn(numberOfSteps - 2);
        builder.continueIn(isTaken);
        if (isUnreachable() || m_hasFailed)
            return;
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            bind(ValueID(count.index() + step), parameters[step]);
    }
    if (isUnreachable() || m_hasFailed)
        return;
    if (Reference resumption = resumptionOfGenerator({ }, ResumeMode::Next, origin))
        bind(yield.id(), resumption);
}

void Walker::appendYielded(Reference list, Reference yielded, Origin origin)
{
    Draft append(Opcode::AppendElement, origin);
    chooseConservativeFields(append);
    append.setArrayOperand(list);
    append.setValueOperand(yielded);
    build(WTF::move(append));
}

void Walker::walkYieldOfGeneratorInPatternWithRest(size_t place, const BodyValue& yield, Reference yielded, Origin origin)
{
    const uint32_t numberOfSteps = m_generatorsWalkedInConsumers[place].numberOfPatternSteps;
    const Reference list = m_generatorsWalkedInConsumers[place].list;
    const ValueID count(static_cast<uint32_t>(m_frames.last().references.size()) - numberOfSteps - 1);
    auto nameOfStep = [&](uint32_t step) { return ValueID(count.index() + 1 + step); };
    BuilderForGenerators builder(*this, origin);
    Reference numberThatHasCome = referenceBoundTo(count);
    Type typeOfNumber = numberThatHasCome && numberThatHasCome.isRecord() ? m_types.at(ownTypeOf(numberThatHasCome)) : Type();
    std::optional<int64_t> known = typeOfNumber.isInteger() && typeOfNumber.integers.minimum == typeOfNumber.integers.maximum ? std::optional<int64_t>(typeOfNumber.integers.minimum) : std::nullopt;
    if (known && *known >= numberOfSteps)
        appendYielded(list, yielded, origin);
    else if (known) {
        bind(nameOfStep(static_cast<uint32_t>(*known)), yielded);
        bind(count, builder.integer(*known + 1));
    } else {
        BlockID isTaken = builder.addBlock();
        Vector<Reference, 4> parameters;
        parameters.append(addParameter(isTaken, { }, formOfParameter(m_types.integer(Range { 1, numberOfSteps }), { })));
        for (uint32_t step = 0; step < numberOfSteps; ++step)
            parameters.append(addParameter(isTaken, { }, Representation::JSValue));
        auto takeAs = [&](uint32_t placeOfYielded) {
            Vector<Reference, 4> arguments;
            arguments.append(builder.integer(std::min(placeOfYielded + 1, numberOfSteps)));
            for (uint32_t step = 0; step < numberOfSteps; ++step)
                arguments.append(step == placeOfYielded ? yielded : referenceBoundTo(nameOfStep(step)));
            jumpTo(isTaken, arguments.span());
        };
        for (uint32_t step = 0; step < numberOfSteps; ++step) {
            BlockID isOfStep = builder.addBlock();
            BlockID isLater = builder.addBlock();
            builder.branch(builder.isInteger(numberThatHasCome, step), isOfStep, isLater);
            builder.continueIn(isOfStep);
            takeAs(step);
            builder.continueIn(isLater);
        }
        appendYielded(list, yielded, origin);
        takeAs(numberOfSteps);
        builder.continueIn(isTaken);
        if (isUnreachable() || m_hasFailed)
            return;
        for (uint32_t step = 0; step <= numberOfSteps; ++step)
            bind(ValueID(count.index() + step), parameters[step]);
    }
    if (isUnreachable() || m_hasFailed)
        return;
    if (Reference resumption = resumptionOfGenerator({ }, ResumeMode::Next, origin))
        bind(yield.id(), resumption);
}

bool Walker::patternTakesGeneratorAt(size_t placeOfTaken, const BodyValue& open)
{
    GeneratorTakenByConsumer taken = WTF::move(m_generatorsTakenByConsumers[placeOfTaken]);
    m_generatorsTakenByConsumers.removeAt(placeOfTaken);

    const size_t frameOfConsumer = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameOfConsumer].index;
    Origin origin = originOf(open);
    origin.isTailCall = false;
    buildFirstReadsThatOpenOfGeneratorLeavesOut(*this, origin, !taken.takerCallsMethodItself);
    if (isUnreachable())
        return true;

    GeneratorWalkedInConsumer walked;
    walked.frameOfConsumer = static_cast<uint32_t>(frameOfConsumer);
    walked.frameOfGenerator = static_cast<uint32_t>(frameOfConsumer + 1);
    walked.call = taken.call;
    walked.frameOfCall = taken.frameOfCall;
    walked.state = taken.state;
    walked.numberOfPatternSteps = taken.numberOfPatternSteps;
    walked.numberOfYields = taken.numberOfYields;
    if (taken.restArray) {
        Draft made(Opcode::NewArray, originOf(index.value(taken.restArray)));
        chooseConservativeFields(made);
        made.standsForNoValueOfInput = true;
        made.setContents(TypeTable::anything);
        walked.list = build(WTF::move(made));
        if (!walked.list)
            return true;
    }
    const Reference restList = walked.list;
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfConsumer);
    for (const GeneratorWalkedInConsumer& around : m_generatorsWalkedInConsumers) {
        if (around.frameOfGenerator == frameOfConsumer)
            walked.typesOfCarriedValues.appendVector(around.typesOfCarriedValues);
    }
    const uint32_t numberCarriedBeforeStep = static_cast<uint32_t>(carried.size());
    BuilderForGenerators builder(*this, origin);
    carried.append(builder.integer(0));
    walked.typesOfCarriedValues.append(m_types.integer(Range { 0, taken.numberOfPatternSteps }));
    for (uint32_t step = 0; step < taken.numberOfPatternSteps; ++step) {
        carried.append(builder.undefined());
        walked.typesOfCarriedValues.append(TypeTable::anything);
    }

    layFrameOfGenerator(taken.function, taken.callPosition, m_frames[frameOfConsumer].loopDepthAtCall + index.loopDepthOf(index.blockOf(open.id())), static_cast<uint32_t>(carried.size()));
    m_generatorsWalkedInConsumers.append(WTF::move(walked));
    nameCallUpToInitialYield(taken);
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
    m_calleeAccountedFor = facts().linked().calleeAt(currentFunction(), open.id());
    if (m_hasFailed || isUnreachable())
        return true;
    uint32_t firstCarried = index.numberOfValues();
    for (uint32_t number = 0; number < numberCarriedBeforeStep; ++number)
        bind(ValueID(firstCarried + number), carried[number]);
    IndexedIteration pattern;
    pattern.hasPrecomputedSteps = true;
    for (uint32_t step = 0; step < taken.numberOfPatternSteps; ++step)
        pattern.precomputedSteps.append(carried[numberCarriedBeforeStep + 1 + step]);
    pattern.restList = restList;
    pattern.restArray = taken.restArray;
    bind(open.id(), addIndexedIteration(WTF::move(pattern)));
    return true;
}

void Walker::walkYieldOfGeneratorInRow(size_t place, const BodyValue& yield, Reference yielded, Origin origin)
{
    const uint32_t numberOfSteps = static_cast<uint32_t>(m_generatorsWalkedInConsumers[place].stepsInRow.size());
    const uint32_t numberOfKept = static_cast<uint32_t>(m_generatorsWalkedInConsumers[place].valuesKeptOverSteps.size());
    const size_t frameOfGenerator = m_frames.size() - 1;
    const ValueID isDone(static_cast<uint32_t>(m_frames[frameOfGenerator].references.size()) - numberOfKept - 2);
    const ValueID count(isDone.index() + 1);
    BuilderForGenerators builder(*this, origin);
    std::optional<uint32_t> step = numberOfSteps == 1 ? std::optional(0u) : numberOfStepsTaken(referenceBoundTo(count));
    if (!step || *step >= numberOfSteps) {
        fail("a generator that is stepped in a row yields where the number of steps is not known"_s);
        return;
    }
    if (*step + 1 == numberOfSteps) {
        bind(isDone, builder.boolean(false));
        noteCarriedValuesAtReturnOfGenerator();
        Frame& frame = m_frames[frameOfGenerator];
        frame.returnsOfCopy->append(Frame::ReturnOfCopy { m_block, yielded, WTF::move(m_state), frame.hasHandler, !m_opensGuesses });
        m_block = { };
        m_state.clear();
        return;
    }

    m_generatorsWalkedInConsumers[place].yieldStoodAt = yield.id();
    bool generatorStoodUnderHandler = leaveHandlerOfGenerator(frameOfGenerator);
    Reference sent = walkToNextStepInRow(place, *step, yielded);
    m_frames[frameOfGenerator].hasHandler = generatorStoodUnderHandler;
    m_generatorsWalkedInConsumers[place].yieldStoodAt = { };
    m_origin = origin;
    if (!sent || isUnreachable() || m_hasFailed)
        return;
    bind(count, builder.integer(*step + 1));
    if (Reference resumption = resumptionOfGenerator(sent, ResumeMode::Next, origin))
        bind(yield.id(), resumption);
}

std::optional<uint32_t> Walker::numberOfStepsTaken(Reference count) const
{
    if (!count || !count.isRecord())
        return std::nullopt;
    Type type = m_types.at(ownTypeOf(count));
    return type.isInteger() && type.integers.minimum == type.integers.maximum ? std::optional(static_cast<uint32_t>(type.integers.minimum)) : std::nullopt;
}

void Walker::nameCallUpToInitialYield(const GeneratorTakenByConsumer& taken)
{
    if (!taken.call || isQuietBeforeInitialYield(taken.function))
        return;
    GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers.last();
    const Frame& consumer = m_frames[walked.frameOfConsumer];
    uint32_t positionOfCall = consumer.index->value(taken.call).position();
    Frame& generator = m_frames.last();
    walked.inlineFrameOfStep = generator.inlineFrame;
    if (m_code.inlineFrames()[generator.inlineFrame].callPosition != positionOfCall) {
        walked.inlineFrameOfCall = m_code.addInlineFrame({
            .callee = taken.function,
            .callPosition = positionOfCall,
            .parent = consumer.inlineFrame,
            .module = program().at(taken.function).module,
        });
    }
}

void Walker::chooseInlineFrameOfStep(bool isPastInitialYield)
{
    size_t place = placeOfGeneratorOnTop();
    if (place == notFound)
        return;
    const GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[place];
    Frame& frame = m_frames.last();
    if (walked.inlineFrameOfCall) {
        BlockID initial = keptByGeneratorsInLoops(m_keptByGeneratorsInLoops).functions.find(frame.function.index())->value.blockOfInitialYield;
        bool bindsParameters = !isPastInitialYield && (frame.block == initial || !frame.index->dominates(initial, frame.block));
        m_origin.inlineFrame = frame.inlineFrame = bindsParameters ? walked.inlineFrameOfCall : walked.inlineFrameOfStep;
        if (bindsParameters)
            return;
    }
    if (walked.inlineFramesOfSteps.isEmpty())
        return;
    std::optional<uint32_t> step = numberOfStepsTaken(referenceBoundTo(ValueID(static_cast<uint32_t>(frame.references.size() - walked.valuesKeptOverSteps.size()) - 1)));
    if (walked.countsSteps || (step && *step < walked.inlineFramesOfSteps.size()))
        m_origin.inlineFrame = frame.inlineFrame = walked.inlineFramesOfSteps[walked.countsSteps && step != 0u ? 1 : *step];
}

Reference Walker::walkBetweenStepsInRow(std::span<const ValueID> steps, uint32_t step, Reference value, bool isDone)
{
    const BodyIndex& index = currentBodyIndex();
    m_origin = originOf(index.value(steps[step]));
    m_origin.isTailCall = false;
    BuilderForGenerators builder(*this, m_origin);
    Reference result = buildResultOfStep(*this, value, builder.boolean(isDone), m_origin);
    Reference sent;
    if (!isUnreachable() && !m_hasFailed) {
        noteFreshObject(*this, result, 2);
        bind(steps[step], result);
        BlockID block = index.blockOf(steps[step]);
        uint32_t firstValue = index.block(block).firstValue().index();
        size_t numberOfLoopsAround = m_openLoops.size();
        walkStretch(block, steps[step].index() + 1 - firstValue, steps[step + 1].index() - firstValue);
        endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
        std::span<const ValueID> arguments = index.value(steps[step + 1]).as<Opcode::Call>().argumentsOperands();
        sent = isUnreachable() || m_hasFailed || arguments.empty() ? Reference() : referenceOf(arguments[0]);
        if (!isUnreachable() && !m_hasFailed && !sent)
            sent = builder.undefined();
        if (sent && representationOf(sent) != Representation::JSValue)
            sent = convert(sent, Representation::JSValue, m_origin);
    }
    if (isUnreachable() || m_hasFailed || !sent) {
        m_block = { };
        m_state.clear();
        return { };
    }
    return sent;
}

Reference Walker::walkToNextStepInRow(size_t place, uint32_t step, Reference yielded)
{
    const size_t frameOfGenerator = m_frames.size() - 1;
    const size_t frameOfConsumer = m_generatorsWalkedInConsumers[place].frameOfConsumer;
    const size_t frameOfTurn = m_frames.size();
    const Vector<ValueID, 4> steps = m_generatorsWalkedInConsumers[place].stepsInRow;
    const Vector<ValueID, 4> kept = m_generatorsWalkedInConsumers[place].valuesKeptOverSteps;
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfGenerator);
    moveFrame(frameOfConsumer, frameOfTurn);
    m_generatorsWalkedInConsumers[place].frameOfTurn = static_cast<uint32_t>(frameOfTurn);
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = true;
    const uint32_t firstOuterName = currentBodyIndex().numberOfValues();
    const uint32_t numberOfOuterNames = static_cast<uint32_t>(m_frames[frameOfTurn].references.size()) - firstOuterName;
    for (uint32_t number = 0; number < numberOfOuterNames; ++number)
        bind(ValueID(firstOuterName + number), carried[number]);
    for (uint32_t number = 0; number < kept.size(); ++number) {
        if (kept[number].index() < steps[step].index())
            bind(kept[number], carried[numberOfOuterNames + 2 + number]);
    }

    Reference sent = walkBetweenStepsInRow(steps.span(), step, yielded, false);
    if (sent) {
        for (uint32_t number = 0; number < numberOfOuterNames; ++number)
            carried[number] = referenceBoundTo(ValueID(firstOuterName + number));
        for (uint32_t number = 0; number < kept.size(); ++number) {
            if (kept[number].index() < steps[step + 1].index())
                carried[numberOfOuterNames + 2 + number] = referenceOf(kept[number]);
        }
    }
    if (!frameOfConsumer)
        m_frames[frameOfTurn].returnsInPlaceOfUnit = false;
    m_generatorsWalkedInConsumers[place].frameOfTurn = UINT32_MAX;
    moveFrame(frameOfTurn, frameOfConsumer);
    if (sent) {
        uint32_t firstCarried = m_frames[frameOfGenerator].index->numberOfValues();
        for (uint32_t number = 0; number < carried.size(); ++number)
            bind(ValueID(firstCarried + number), carried[number]);
    }
    return sent;
}

void Walker::finishStepsInRow(const GeneratorWalkedInConsumer& walked, uint32_t step, Reference returned, Vector<Frame::ReturnOfCopy, 2>& ways, Vector<Vector<Reference, 4>, 2>& carriedOnWays)
{
    BuilderForGenerators builder(*this, m_origin);
    Origin origin = builder.origin();
    const uint32_t numberOfSteps = static_cast<uint32_t>(walked.stepsInRow.size());
    for (; step + 1 < numberOfSteps && !isUnreachable() && !m_hasFailed; ++step) {
        walkBetweenStepsInRow(walked.stepsInRow.span(), step, returned, true);
        returned = builder.undefined();
    }
    m_origin = origin;
    if (isUnreachable() || m_hasFailed) {
        m_block = { };
        m_state.clear();
        return;
    }
    Vector<Reference, 4> carried;
    for (uint32_t number = currentBodyIndex().numberOfValues(); number < m_frames.last().references.size(); ++number)
        carried.append(referenceBoundTo(ValueID(number)));
    for (ValueID kept : walked.valuesKeptOverSteps)
        carried.append(referenceOf(kept));
    carried.append(builder.boolean(true));
    carriedOnWays.append(WTF::move(carried));
    ways.append(Frame::ReturnOfCopy { m_block, returned, WTF::move(m_state), false, !m_opensGuesses });
    m_block = { };
    m_state.clear();
}

bool Walker::walkStepsInRow(size_t placeOfTaken, const BodyValue& firstStep)
{
    const GeneratorTakenByConsumer taken = m_generatorsTakenByConsumers[placeOfTaken];
    if (firstStep.id() != taken.stepsInRow[0]) {
        fail("a generator that is stepped in a row is stepped by another step first"_s);
        return true;
    }

    const size_t frameOfConsumer = m_frames.size() - 1;
    const BodyIndex& index = *m_frames[frameOfConsumer].index;
    Origin origin = originOf(firstStep);
    origin.isTailCall = false;

    GeneratorWalkedInConsumer walked;
    walked.frameOfConsumer = static_cast<uint32_t>(frameOfConsumer);
    walked.frameOfGenerator = static_cast<uint32_t>(frameOfConsumer + 1);
    walked.call = taken.call;
    walked.frameOfCall = taken.frameOfCall;
    walked.state = taken.state;
    walked.numberOfYields = taken.numberOfYields;
    walked.stepsInRow = taken.stepsInRow;
    if (walked.stepsInRow.size() > 1)
        walked.valuesKeptOverSteps = valuesReadAfterNextStep(index, walked.stepsInRow.span());
    Vector<Reference, 4> carried = carriedValuesInFrame(frameOfConsumer);
    for (const GeneratorWalkedInConsumer& around : m_generatorsWalkedInConsumers) {
        if (around.frameOfGenerator == frameOfConsumer)
            walked.typesOfCarriedValues.appendVector(around.typesOfCarriedValues);
    }
    const uint32_t numberOfOuterNames = static_cast<uint32_t>(carried.size());
    const uint32_t numberOfKept = static_cast<uint32_t>(walked.valuesKeptOverSteps.size());
    BuilderForGenerators builder(*this, origin);
    carried.append(builder.boolean(true));
    walked.typesOfCarriedValues.append(TypeTable::boolean);
    carried.append(builder.integer(0));
    walked.typesOfCarriedValues.append(m_types.integer(Range { 0, static_cast<int64_t>(walked.stepsInRow.size()) }));
    for (uint32_t number = 0; number < numberOfKept; ++number) {
        carried.append(builder.undefined());
        walked.typesOfCarriedValues.append(TypeTable::anything);
    }

    layFrameOfGenerator(taken.function, taken.callPosition, m_frames[frameOfConsumer].loopDepthAtCall + index.loopDepthOf(index.blockOf(firstStep.id())), static_cast<uint32_t>(carried.size()));
    walked.inlineFramesOfSteps.append(m_frames.last().inlineFrame);
    for (size_t step = 1; step < walked.stepsInRow.size(); ++step) {
        walked.inlineFramesOfSteps.append(m_code.addInlineFrame({
            .callee = taken.function,
            .callPosition = index.value(walked.stepsInRow[step]).position(),
            .parent = m_frames[frameOfConsumer].inlineFrame,
            .module = program().at(taken.function).module,
        }));
    }
    m_generatorsWalkedInConsumers.append(walked);
    nameCallUpToInitialYield(taken);
    const size_t place = m_generatorsWalkedInConsumers.size() - 1;
    const BodyIndex& body = currentBodyIndex();
    BodyBlock entry = body.block(entryBlock);
    for (uint32_t parameter = 0; parameter < taken.entryArguments.size(); ++parameter)
        bind(entry.parameter(parameter), taken.entryArguments[parameter]);
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(body.numberOfValues() + number), carried[number]);

    Vector<Frame::ReturnOfCopy, 2> returns;
    Vector<Vector<Reference, 4>, 2> carriedAtReturns = walkBodyOfGenerator(place, origin, returns);
    m_generatorsWalkedInConsumers.removeLast();
    m_callIsAccountedFor = true;
    m_calleeAccountedFor = facts().linked().calleeAt(currentFunction(), firstStep.id());
    if (m_hasFailed)
        return true;

    Vector<Frame::ReturnOfCopy, 2> ways;
    Vector<Vector<Reference, 4>, 2> carriedOnWays;
    Vector<Frame::ReturnOfCopy, 2> ends;
    Vector<Vector<Reference, 4>, 2> carriedAtEnds;
    for (size_t number = 0; number < returns.size(); ++number) {
        Vector<Reference, 4>& atReturn = carriedAtReturns[number];
        Reference isDone = atReturn[numberOfOuterNames];
        bool hasYielded = isDone && isDone.isRecord() && ownTypeOf(isDone) == TypeArena::fromPrimitives(Primitive::False);
        if (!hasYielded && walked.stepsInRow.size() > 1) {
            ends.append(WTF::move(returns[number]));
            carriedAtEnds.append(WTF::move(atReturn));
            continue;
        }
        Vector<Reference, 4> onWay(atReturn.subspan(0, numberOfOuterNames));
        onWay.append(atReturn.subspan(numberOfOuterNames + 2));
        onWay.append(builder.boolean(!hasYielded));
        carriedOnWays.append(WTF::move(onWay));
        ways.append(WTF::move(returns[number]));
    }
    if (!ends.isEmpty()) {
        Reference returned;
        Vector<Reference, 4> atEnd = joinReturnsOfGenerator(ends, carriedAtEnds, origin, &returned);
        auto bindNamesAtEnd = [&] {
            for (uint32_t number = 0; number < numberOfOuterNames; ++number)
                bind(ValueID(index.numberOfValues() + number), atEnd[number]);
            for (uint32_t number = 0; number < numberOfKept; ++number)
                bind(walked.valuesKeptOverSteps[number], atEnd[numberOfOuterNames + 2 + number]);
        };
        if (!isUnreachable() && !m_hasFailed) {
            std::optional<uint32_t> step = numberOfStepsTaken(atEnd[numberOfOuterNames + 1]);
            if (!step) {
                fail("a generator that is stepped in a row returns where the number of steps is not known"_s);
                return true;
            }
            bindNamesAtEnd();
            finishStepsInRow(walked, *step, returned, ways, carriedOnWays);
        }
    }
    m_origin = origin;
    if (m_hasFailed)
        return true;
    Reference value;
    carried = joinReturnsOfGenerator(ways, carriedOnWays, origin, &value);
    if (m_hasFailed || isUnreachable()) {
        m_block = { };
        m_state.clear();
        return true;
    }
    for (uint32_t number = 0; number < numberOfOuterNames; ++number)
        bind(ValueID(index.numberOfValues() + number), carried[number]);
    for (uint32_t number = 0; number < numberOfKept; ++number)
        bind(walked.valuesKeptOverSteps[number], carried[numberOfOuterNames + number]);
    Reference done = carried[numberOfOuterNames + numberOfKept];
    if (representationOf(done) != Representation::JSValue)
        done = convert(done, Representation::JSValue, origin);
    Reference result = buildResultOfStep(*this, value, done, origin);
    if (isUnreachable() || m_hasFailed)
        return true;
    noteFreshObject(*this, result, 2);
    bind(walked.stepsInRow.last(), result);
    uint32_t firstValue = index.block(index.blockOf(firstStep.id())).firstValue().index();
    m_frames[frameOfConsumer].walkedUpTo = std::max(m_frames[frameOfConsumer].walkedUpTo, walked.stepsInRow.last().index() + 1 - firstValue);
    return true;
}

bool Walker::loopCarriesValuesOfConsumerRound(BlockID inputHead) const
{
    const BodyIndex& body = currentBodyIndex();
    return placeOfGeneratorOnTop() != notFound && loopHoldsYield(body, body.loopOf(inputHead));
}

static bool everyCycleYields(const BodyIndex& body, LoopID loop)
{
    auto yields = [&](BlockID block) {
        return std::ranges::any_of(body.block(block).values(), [](const BodyValue& operation) { return operation.opcode() == Opcode::Yield; });
    };
    BlockID head = body.loop(loop).head;
    BitVector isSeen(body.numberOfBlocks());
    Vector<BlockID, 8> worklist { head };
    while (!worklist.isEmpty()) {
        BlockID from = worklist.takeLast();
        if (isSeen.set(from.index()) || yields(from))
            continue;
        for (BodyTarget target : body.terminator(from).targets()) {
            if (target.block() == head)
                return false;
            if (body.isInLoop(target.block(), loop))
                worklist.append(target.block());
        }
    }
    return true;
}

std::optional<uint32_t> Walker::iterationsTakenByPattern(LoopID loop) const
{
    size_t place = m_generatorsWalkedInConsumers.isEmpty() ? notFound : placeOfGeneratorOnTop();
    if (place == notFound)
        return std::nullopt;
    const GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[place];
    if (walked.countsSteps) {
        bool isFirstStep = numberOfStepsTaken(referenceBoundTo(ValueID(static_cast<uint32_t>(m_frames.last().references.size()) - 1))) == 0u;
        const BodyIndex& body = currentBodyIndex();
        return isFirstStep && !walked.regionHoldsIterations && body.loop(loop).numberOfLoopsWithin == 1 && loopHoldsYield(body, loop) && everyCycleYields(body, loop) ? std::optional(0u) : std::nullopt;
    }
    const uint32_t numberOfSteps = walked.numberOfPatternSteps ? walked.numberOfPatternSteps : static_cast<uint32_t>(walked.stepsInRow.size());
    const uint32_t numberOfNamesAfterCount = walked.numberOfPatternSteps ? numberOfSteps : static_cast<uint32_t>(walked.valuesKeptOverSteps.size());
    const BodyIndex& body = currentBodyIndex();
    if (!numberOfSteps || body.loop(loop).numberOfLoopsWithin != 1 || !loopHoldsYield(body, loop) || !everyCycleYields(body, loop))
        return std::nullopt;
    Reference count = referenceBoundTo(ValueID(static_cast<uint32_t>(m_frames.last().references.size()) - numberOfNamesAfterCount - 1));
    if (!count || !count.isRecord() || !m_code.isConstant(count.id()) || m_code.at(count.id()).opcode != Opcode::Integer)
        return std::nullopt;
    uint32_t numberThatHasCome = static_cast<uint32_t>(m_code.at(count.id()).integer());
    return numberThatHasCome < numberOfSteps ? std::optional(numberOfSteps - numberThatHasCome - 1) : std::nullopt;
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
        Representation form = formOfParameter(type, { });
        parameters.append(m_code.appendParameter(head, type, walked.numberOfPatternSteps && form == Representation::OptionalInt64 ? Representation::JSValue : form, m_origin));
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
            if (name >= frame.references.size())
                break;
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


Vector<Reference, 4> Walker::walkBodyOfGeneratorToItsEnd(size_t place, Origin originInFront, Reference* returned)
{
    Vector<Frame::ReturnOfCopy, 2> returns;
    Vector<Vector<Reference, 4>, 2> carriedAtReturns = walkBodyOfGenerator(place, originInFront, returns);
    return joinReturnsOfGenerator(returns, carriedAtReturns, originInFront, returned);
}

Vector<Vector<Reference, 4>, 2> Walker::walkBodyOfGenerator(size_t place, Origin originInFront, Vector<Frame::ReturnOfCopy, 2>& returns)
{
    const BodyIndex& body = currentBodyIndex();
    m_frames.last().returnsOfCopy = &returns;
    for (uint32_t number = 0; number < body.numberOfBlocks() && !m_hasFailed; ++number)
        walkBlock(BlockID(number));
    m_block = { };
    m_state.clear();
    endBlocksInFrontOfLoops(m_frames.size() - 1, { });
    m_frames.removeLast();
    m_origin = originInFront;

    Vector<Vector<Reference, 4>, 2> carriedAtReturns = WTF::move(m_generatorsWalkedInConsumers[place].carriedAtReturns);
    if (!m_hasFailed && carriedAtReturns.size() != returns.size())
        fail("a return of a generator that is walked in its consumer was not noted"_s);
    return carriedAtReturns;
}

Vector<Reference, 4> Walker::joinReturnsOfGenerator(Vector<Frame::ReturnOfCopy, 2>& returns, Vector<Vector<Reference, 4>, 2>& carriedAtReturns, Origin originInFront, Reference* returned)
{
    m_block = { };
    m_state.clear();
    if (m_hasFailed || returns.isEmpty())
        return { };
    auto valueOfReturn = [&](size_t number) {
        if (Reference value = returns[number].value; value && value.isRecord())
            return value;
        Draft nothing(Opcode::Undefined, originInFront);
        nothing.representation = Representation::JSValue;
        return constant(WTF::move(nothing));
    };
    if (returns.size() == 1 && !returns[0].isUnderHandlerOfCallee) {
        m_block = returns[0].block;
        m_state = WTF::move(returns[0].state);
        if (returned)
            *returned = valueOfReturn(0);
        return WTF::move(carriedAtReturns[0]);
    }
    for (size_t number = 0; returned && number < returns.size(); ++number)
        carriedAtReturns[number].append(valueOfReturn(number));

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
    if (returned)
        *returned = carried.takeLast();
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
    walked.isSteppedByHand = taken.isSteppedByHand;
    walked.headPrecedesLoop = taken.headPrecedesLoop;
    walked.placeOfResult = taken.placeOfResult;
    walked.blockOfStep = taken.blockOfStep;
    walked.blocksBeforeStep = WTF::move(taken.blocksBeforeStep);
    walked.state = taken.state;
    walked.numberOfYields = taken.numberOfYields;
    walked.holderOfReturn = taken.holderOfReturn;
    walked.numberOfOperationsOfRegion = taken.numberOfOperationsOfRegion;
    walked.regionHoldsIterations = taken.regionHoldsIterations;
    walked.region = WTF::move(taken.region);
    walked.namesAtHead = inputBlock(frameOfConsumer, inputHead).firstNarrowerName;
    for (ValueID value : taken.valuesThatRegionNamesFromOutside) {
        if (Reference name = referenceBoundIn(m_frames[frameOfConsumer], value))
            walked.bindingsBeforeLoop.append({ value, name });
    }
    for (size_t parameter = 0; parameter < entering.size(); ++parameter) {
        TypeID arriving = ownTypeOf(entering[parameter]);
        TypeID type = walked.placeOfResult == parameter ? arriving : typesOfParameters[parameter];
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

    m_generatorsWalkedInConsumers.append(WTF::move(walked));
    const size_t place = m_generatorsWalkedInConsumers.size() - 1;
    auto endRegion = [&] {
        forgetRegionOfIteration(frameOfConsumer, m_generatorsWalkedInConsumers[place]);
        for (BlockID member : m_generatorsWalkedInConsumers[place].region) {
            inputBlock(frameOfConsumer, member).firstNarrowerName = m_generatorsWalkedInConsumers[place].namesAtHead;
            inputBlock(frameOfConsumer, member).isEntered = true;
        }
        m_frames[frameOfConsumer].loopsBeingVersioned.removeLast();
        m_generatorsWalkedInConsumers.removeLast();
        m_block = { };
        m_state.clear();
    };
    if (taken.isSteppedByHand && !taken.placeOfResult && !taken.headPrecedesLoop) {
        uint32_t frameOfGenerator = std::exchange(m_generatorsWalkedInConsumers[place].frameOfGenerator, UINT32_MAX);
        walkManualIterationToStep(GeneratorWalkedInConsumer(m_generatorsWalkedInConsumers[place]));
        m_generatorsWalkedInConsumers[place].frameOfGenerator = frameOfGenerator;
        if (m_hasFailed || isUnreachable()) {
            endRegion();
            return true;
        }
    }

    if (taken.stepInFront) {
        m_generatorsWalkedInConsumers[place].countsSteps = true;
        m_generatorsWalkedInConsumers[place].typesOfCarriedValues.append(m_types.integer(Range { 0, 1 }));
        carried.append(BuilderForGenerators(*this, m_origin).integer(0));
    }
    layFrameOfGenerator(taken.function, taken.stepInFront ? index.value(taken.stepInFront).position() : taken.callPosition, m_frames[frameOfConsumer].loopDepthAtCall + index.loopDepthOf(inputHead) - taken.hasBackEdge, static_cast<uint32_t>(carried.size()));
    if (taken.stepInFront) {
        m_generatorsWalkedInConsumers[place].inlineFramesOfSteps.append(m_frames.last().inlineFrame);
        m_generatorsWalkedInConsumers[place].inlineFramesOfSteps.append(m_code.addInlineFrame({
            .callee = taken.function,
            .callPosition = taken.callPosition,
            .parent = inlineFrameOfConsumer,
            .module = program().at(taken.function).module,
        }));
    }
    nameCallUpToInitialYield(taken);
    const BodyIndex& body = currentBodyIndex();
    BodyBlock entry = body.block(entryBlock);
    for (uint32_t parameter = 0; parameter < taken.entryArguments.size(); ++parameter)
        bind(entry.parameter(parameter), taken.entryArguments[parameter]);
    for (uint32_t number = 0; number < carried.size(); ++number)
        bind(ValueID(body.numberOfValues() + number), carried[number]);

    Reference returned;
    carried = walkBodyOfGeneratorToItsEnd(place, originInFront, taken.isSteppedByHand ? &returned : nullptr);
    m_generatorsWalkedInConsumers[place].frameOfGenerator = UINT32_MAX;
    if (!m_hasFailed && !isUnreachable()) {
        if (taken.stepInFront)
            carried.removeLast();
        forgetRegionOfIteration(frameOfConsumer, m_generatorsWalkedInConsumers[place]);
        if (taken.isSteppedByHand) {
            size_t numberOfLoopsAround = m_openLoops.size();
            walkManualIterationAfterStep(GeneratorWalkedInConsumer(m_generatorsWalkedInConsumers[place]), carried.span(), true, returned);
            endBlocksInFrontOfLoopsFrom(numberOfLoopsAround);
            if (!m_hasFailed && inputBlock(frameOfConsumer, inputHead).firstOpenEdge != noOpenEdge) {
                m_block = { };
                m_frames[frameOfConsumer].block = { };
                setCurrentInputBlock(inputHead);
                Vector<uint32_t, 4> edges = takeOpenEdges(inputHead);
                closeEdgesInto(addBlockFor(inputHead), inputHead, edges.span(), false);
                if (!m_hasFailed && !isUnreachable())
                    endAsUnreachable(m_origin);
            }
        } else {
            Draft nothing(Opcode::Undefined, m_origin);
            nothing.representation = Representation::JSValue;
            walkHeadOfLoopBehindStep(inputHead, taken.step, carried.span(), true, constant(WTF::move(nothing)));
        }
    }

    endRegion();
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
    nameCallUpToInitialYield(taken);
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
    IteratorCompletion completion = close.as<Opcode::IteratorClose>().completion();
    BlockID after;
    if (const GeneratorWalkedInConsumer& walked = m_generatorsWalkedInConsumers[place]; Reference holder = walked.holderOfReturn) {
        bool leavesAtOnce = returnsAtOnceAtYield(*m_frames[walked.frameOfGenerator].index, walked.yieldStoodAt);
        BlockID inPlace = addBlock();
        after = addBlock();
        closeByChangedReturn(holder, completion, leavesAtOnce, inPlace, after, origin);
        continueIn(inPlace);
    }
    buildFirstReadThatCloseOfGeneratorLeavesOut(*this, origin);
    if (!isUnreachable())
        resumeGeneratorToLeave(place, ResumeMode::Return, completion == IteratorCompletion::Throw, origin);
    if (after && !m_hasFailed)
        jumpTo(after);
    return true;
}

void Walker::closeByChangedReturn(Reference holder, IteratorCompletion completion, bool leavesAtOnce, BlockID inPlace, BlockID after, Origin origin)
{
    BuilderForGenerators builder(*this, origin);
    Draft test(m_code.at(holder.id()).opcode == Opcode::GetGeneratorPrototype ? Opcode::IsPlainGeneratorPrototype : Opcode::IsUnmodifiedGenerator, builder.origin());
    test.setOperand(0, holder);
    Reference isPlain = builder.build(WTF::move(test));
    BlockID changed = addBlock(BlockFrequency::Rare);
    builder.branch(isPlain, inPlace, changed);
    builder.continueIn(changed);
    Draft take(Opcode::TakeGeneratorLeftInPlace, builder.origin());
    take.setHolderOperand(holder);
    take.setLeavesAtOnce(leavesAtOnce);
    Reference generator = builder.build(WTF::move(take));
    BlockID called = addBlock(BlockFrequency::Rare);
    builder.branch(builder.testType(generator, TypeTable::undefined), inPlace, called);
    builder.continueIn(called);
    if (completion == IteratorCompletion::Throw)
        builder.beginToDropWhatIsThrown();
    Reference method = builder.read(generator, BuiltinNames::returnKeyword);
    BlockID isOther = addBlock(BlockFrequency::Rare);
    builder.branch(builder.testType(method, m_types.forBuiltin(program().builtins().canonicalID(Builtins::GeneratorPrototypeReturn))), inPlace, isOther);
    builder.continueIn(isOther);
    BlockID isPresent = addBlock(BlockFrequency::Rare);
    builder.branch(builder.testType(method, TypeTable::nullish), after, isPresent);
    builder.continueIn(isPresent);
    Reference result = builder.call(method, generator);
    if (completion == IteratorCompletion::Throw) {
        builder.dropWhatWasThrown(after);
        return;
    }
    builder.requireObject(result, RuntimeErrorKind::IteratorResultNotObject);
    builder.jumpTo(after);
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
    inputBlock(m_frames.size() - 1, block.id()).isEntered = !yieldsFirst(block);
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
    if (!m_hasFailed && inputBlock(m_frames.size() - 1, block.id()).firstOpenEdge != noOpenEdge) {
        setCurrentInputBlock(block.id());
        Vector<uint32_t, 4> edges = takeOpenEdges(block.id());
        closeEdgesInto(addBlockFor(block.id()), block.id(), edges.span(), false);
        returns.append(Frame::ReturnOfCopy { m_block, Reference(), WTF::move(m_state), m_frames.last().hasHandler, !m_opensGuesses });
        m_block = { };
        m_state.clear();
    }
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
