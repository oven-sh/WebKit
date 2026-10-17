/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTAirPasses.h"
#include "AOTInstructionsInCode.h"
#include "AOTOutliner.h"
#include "AOTProgramData.h"
#include "AOTTypedObject.h"
#include "AirBlockInsertionSet.h"
#include "AirCode.h"
#include "AirGenerate.h"
#include "AirInstInlines.h"
#include "AirInsertionSet.h"
#include "B3BasicBlockInlines.h"
#include "B3CompilationCohort.h"
#include "B3Generate.h"
#include "B3InsertionSetInlines.h"
#include "B3ProcedureFingerprint.h"
#include "B3SlotBaseValue.h"
#include "B3StackmapGenerationParams.h"
#include "B3ValueInlines.h"
#include "B3Variable.h"
#include "CCallHelpers.h"
#include "JSCInlines.h"
#include "LinkBuffer.h"

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

Lowering::Lowering(Procedure& proc, const Program& program, FunctionID id, const Function& function, LoweringContext& context)
    : Emitter(proc)
    , m_program(program)
    , m_function(function)
    , m_id(id)
    , m_isEntry(program.isEntry(id))
    , m_context(context)
    , m_builtins(Builtins::singleton())
    , m_length(program.find(asBytes("length"_s.span8())).value_or(StringID()))
{
    m_out.recordFoldedConstantsIn(m_context.wideIntegerConstants);
    m_context.emptyGenerator = m_emptyGenerator;
    m_isCompact = !function.isSharedOutlinedBody && program.isColdInCallGraph(id);
}

LType Lowering::b3TypeOf(Rep rep)
{
    switch (rep) {
    case Rep::Void:
        return Void;
    case Rep::Int32:
        return Int32;
    case Rep::Int64:
    case Rep::OptionalInt64:
    case Rep::Cell:
    case Rep::JSValue:
    case Rep::LiteralString:
        return Int64;
    case Rep::Double:
    case Rep::OptionalDouble:
        return Double;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LBasicBlock Lowering::newUnlikelyBlock()
{
    LBasicBlock block = m_out.newBlock();
    block->setFrequency(std::min(block->frequency(), 0.25));
    return block;
}

LBasicBlock Lowering::newColdBlock()
{
    LBasicBlock block = m_out.newBlock();
    block->setFrequency(0);
    return block;
}

Integer Lowering::integer(int64_t value)
{
    m_context.wideIntegerConstants.add(value);
    return Integer(m_out.constInt64(value));
}

std::span<const Target> Lowering::successorsOf(const Terminator& terminator) const
{
    std::span<const Target> targets = terminator.targets.span();
    if (terminator.kind != TerminatorKind::Branch)
        return targets;
    uint16_t primitives = typeOf(terminator.value).primitives;
    bool mayBeTrue = primitives & Primitive::True;
    bool mayBeFalse = primitives & Primitive::False;
    return targets.subspan(mayBeTrue ? 0 : 1, mayBeTrue + mayBeFalse);
}

Vector<BlockID> Lowering::reversePostorder() const
{
    Vector<BlockID> order;
    BitVector seen(m_function.blocks.size());
    Vector<std::pair<BlockID, unsigned>> stack;
    seen.set(0);
    stack.append({ BlockID(0u), 0 });
    while (!stack.isEmpty()) {
        auto& [block, next] = stack.last();
        auto targets = successorsOf(m_function.at(block).terminator);
        if (next < targets.size()) {
            BlockID successor = targets[targets.size() - 1 - next++].block;
            if (!seen.set(successor.index()))
                stack.append({ successor, 0 });
            continue;
        }
        order.append(block);
        stack.removeLast();
    }
    order.reverse();
    return order;
}

LBasicBlock Lowering::edgeTo(const Target& target, bool isBranch)
{
    auto& parameters = m_function.at(target.block).parameters;
    Vector<LValue, 4> arguments;
    Vector<std::pair<LValue, LValue>, 2> cells;
    ValueID readKnownDefined = std::exchange(m_readKnownDefinedOnEdge, ValueID { });
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (m_stringBuilderAnalysis.isState(parameters[i])) {
            arguments.append(m_out.int64Zero);
            if (LValue phi = m_cellPhis.get(parameters[i].index()))
                cells.append({ phi, m_out.int64Zero });
            continue;
        }
        m_readKnownDefinedOnEdge = readKnownDefined;
        Operand argument = in(target.arguments[i], m_function.at(parameters[i]).rep);
        m_readKnownDefinedOnEdge = { };
        if (LValue phi = m_cellPhis.get(parameters[i].index())) {
            Parts parts = partsOf(argument.as<Boxed>());
            arguments.append(parts.word);
            cells.append({ phi, parts.cell });
            continue;
        }
        arguments.append(argument.value);
    }
    Vector<std::pair<LValue, LValue>, 2> truthiness;
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_truthinessPhis.get(parameters[i].index()).isTruthy)
            truthiness.append({ phi, lowerToBoolean(target.arguments[i]).value() });
    }
    Vector<std::pair<LValue, LValue>, 2> integers;
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_integerPhis.get(parameters[i].index()))
            integers.append({ phi, integerOrMarker(target.arguments[i]).value() });
    }
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_int32Phis.get(parameters[i].index()))
            integers.append({ phi, toInt32(target.arguments[i]).value() });
    }
    for (unsigned i = 0; i < target.arguments.size(); ++i) {
        if (LValue phi = m_numberPhis.get(parameters[i].index()))
            integers.append({ phi, number(static_cast<double>(m_function.at(target.arguments[i]).integer())).value() });
    }
    auto& phis = m_phis[target.block.index()];
    LBasicBlock block = m_blocks[target.block.index()];
    auto anchorAll = [&] {
        initializeStringBuildersOnEdge(target);
        for (unsigned i = 0; i < arguments.size(); ++i) {
            if (!m_cellTypes.contains(arguments[i]))
                m_cellTypes.remove(phis[i]);
            m_out.addIncomingToPhi(phis[i], m_out.anchor(arguments[i]));
        }
        for (auto [phi, isTruthy] : truthiness)
            m_out.addIncomingToPhi(phi, m_out.anchor(isTruthy));
        for (auto [phi, kept] : integers)
            m_out.addIncomingToPhi(phi, m_out.anchor(kept));
        for (auto [phi, cell] : cells)
            m_out.addIncomingToPhi(phi, m_out.anchor(cell));
    };
    if (!isBranch || arguments.isEmpty()) {
        anchorAll();
        return block;
    }
    LBasicBlock here = m_out.m_block;
    LBasicBlock edge = m_out.newBlock();
    edge->setFrequency(block->frequency());
    m_out.appendTo(edge);
    anchorAll();
    m_out.jump(block);
    m_out.appendTo(here);
    return edge;
}

std::optional<unsigned> Lowering::nullishSuccessorOf(const Terminator& terminator) const
{
    bool isNegated = false;
    ValueID id = terminator.value;
    for (; m_function.at(id).opcode == Opcode::LogicalNot; id = m_function.at(id).child(0))
        isNegated = !isNegated;
    auto& condition = m_function.at(id);
    constexpr uint16_t cells = Primitive::String | Primitive::Symbol | Primitive::BigInt;
    auto isReference = [&](ValueID operand, uint16_t others) {
        auto& type = m_program.typeOf(m_function, operand);
        return type.primitives & Primitive::UndefinedOrNull && type.hasCells() && !(type.primitives & ~(Primitive::UndefinedOrNull | others));
    };
    auto isNil = [&](ValueID operand) {
        return m_function.at(operand).opcode == Opcode::Undefined || m_function.at(operand).opcode == Opcode::Null;
    };
    bool isTrueForNullish;
    switch (condition.opcode) {
    case Opcode::IsUndefinedOrNull:
        if (!isReference(condition.child(0), cells))
            return std::nullopt;
        isTrueForNullish = true;
        break;
    case Opcode::StrictEq:
    case Opcode::Eq:
        if (!(isNil(condition.child(1)) && isReference(condition.child(0), cells)) && !(isNil(condition.child(0)) && isReference(condition.child(1), cells)))
            return std::nullopt;
        isTrueForNullish = true;
        break;
    case Opcode::ToBoolean:
        if (!isReference(condition.child(0), 0))
            return std::nullopt;
        isTrueForNullish = false;
        break;
    default:
        return std::nullopt;
    }
    return isTrueForNullish != isNegated ? 0 : 1;
}

std::optional<std::pair<ValueID, unsigned>> Lowering::nullishTestOf(const Terminator& terminator) const
{
    bool isNegated = false;
    ValueID id = terminator.value;
    for (; m_function.at(id).opcode == Opcode::LogicalNot; id = m_function.at(id).child(0))
        isNegated = !isNegated;
    auto& condition = m_function.at(id);
    ValueID tested;
    bool isTrueForNullish = true;
    switch (condition.opcode) {
    case Opcode::IsUndefinedOrNull:
        tested = condition.child(0);
        break;
    case Opcode::StrictEq:
    case Opcode::Eq:
        for (auto [operand, constant] : { std::pair { condition.child(0), condition.child(1) }, std::pair { condition.child(1), condition.child(0) } }) {
            Opcode opcode = m_function.at(constant).opcode;
            if (opcode != Opcode::Undefined && opcode != Opcode::Null)
                continue;
            if (condition.opcode == Opcode::StrictEq && m_program.typeOf(m_function, operand).primitives & (opcode == Opcode::Undefined ? Primitive::Null : Primitive::Undefined))
                continue;
            tested = operand;
        }
        break;
    case Opcode::ToBoolean:
        tested = condition.child(0);
        isTrueForNullish = false;
        break;
    default:
        break;
    }
    if (!tested)
        return std::nullopt;
    return std::pair { tested, isTrueForNullish != isNegated ? 1u : 0u };
}

static DFG::Node* originForMap(const Function&, const Value&);

void Lowering::lowerTerminator(const Terminator& terminator)
{
    if (std::exchange(m_needsPoll, false))
        poll();
    openStringsBeforeLoopsEnteredBy(terminator);
    hoistTypedArrayFieldsBeforeLoops(terminator);
    convertNumberArraysBeforeLoops(terminator, false);
    switch (terminator.kind) {
    case TerminatorKind::Unreachable:
        if (Options::auditAOTTypedAssertions()) [[unlikely]]
            AOT_TYPED_OPERATION(ThrowReachedUnreachable);
        m_out.unreachable();
        return;
    case TerminatorKind::Jump:
        m_out.jump(edgeTo(terminator.targets[0], false));
        return;
    case TerminatorKind::Branch: {
        auto successors = successorsOf(terminator);
        if (successors.empty()) {
            m_out.unreachable();
            return;
        }
        if (successors.size() == 1) {
            m_out.jump(edgeTo(successors[0], false));
            return;
        }
        if (isLazilyFetched(terminator.value)) {
            lowerBranchOnLiteral();
            return;
        }
        if (m_isFusedWithBranch.get(terminator.value.index())) {
            LBasicBlock isOne = edgeTo(terminator.targets[0], true);
            LBasicBlock isNone = edgeTo(terminator.targets[1], true);
            if (Options::aotMapFilePath()) [[unlikely]]
                m_out.setOrigin(std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(m_function, m_function.at(terminator.value))) | static_cast<uintptr_t>(terminator.value.index() + 1) << 32));
            if (isAwaitMayContinue(m_function.at(terminator.value)))
                branchOnAwaitMayContinue(m_function.at(terminator.value), terminator.targets[0].block, isOne, isNone);
            else if (isReceiverGuard(m_function.at(terminator.value)))
                branchOnOriginalStructure(m_function.at(terminator.value), terminator, isOne, isNone);
            else if (isForInStampCheck(m_function.at(terminator.value)))
                branchOnForInStamp(m_function.at(terminator.value), isOne, isNone);
            else if (isHostObjectCacheTest(m_function.at(terminator.value)))
                branchOnHostObjectCache(m_function.at(terminator.value), isOne, isNone);
            else if (isJSONParserTest(m_function.at(terminator.value)))
                branchOnJSONParserTest(m_function.at(terminator.value), isOne, isNone);
            else if (m_function.at(terminator.value).opcode != Opcode::IsClosureOf)
                branchOnRelational(m_function.at(terminator.value), isOne, isNone);
            else
                branchOnClosureOf(m_function.at(terminator.value), isOne, isNone);
            return;
        }
        Word32 condition = get(terminator.value).as<Word32>();
        LBasicBlock taken = edgeTo(terminator.targets[0], true);
        ValueID provenDefined = readProvenDefinedWhenFalse(terminator);
        m_readKnownDefinedOnEdge = provenDefined;
        LBasicBlock notTaken = edgeTo(terminator.targets[1], true);
        if (provenDefined && terminator.targets[1].block.index() && numberOfPredecessorsOf(terminator.targets[1].block) == 1)
            m_blocksWhereReadIsDefined.add(provenDefined.index(), Vector<BlockID, 1> { }).iterator->value.append(terminator.targets[1].block);
        if (auto test = nullishTestOf(terminator); test && terminator.targets[test->second].block.index() && numberOfPredecessorsOf(terminator.targets[test->second].block) == 1) {
            ValueID tested = test->first;
            while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
                tested = m_function.at(tested).child(0);
            didPassNullishTest(tested, terminator.targets[test->second].block);
        }
        if (auto& test = m_function.at(terminator.value); test.opcode == Opcode::IsUndefinedOrNull && terminator.targets[0].block.index() && terminator.targets[0].block != terminator.targets[1].block && numberOfPredecessorsOf(terminator.targets[0].block) == 1) {
            ValueID tested = test.child(0);
            while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
                tested = m_function.at(tested).child(0);
            m_knownNullishIn.set(terminator.targets[0].block.index(), tested);
        }
        if (auto nullishSuccessor = nullishSuccessorOf(terminator)) {
            LBasicBlock& onNullish = *nullishSuccessor ? notTaken : taken;
            if (onNullish->frequency() >= 1) {
                LBasicBlock here = m_out.m_block;
                LBasicBlock edge = newUnlikelyBlock();
                m_out.appendTo(edge);
                m_out.jump(onNullish);
                m_out.appendTo(here);
                onNullish = edge;
            }
        }
        if (std::exchange(m_truthinessDeferredToBranch, ValueID()) == terminator.value) {
            std::pair<FTL::WeightedTarget, FTL::WeightedTarget> successorsWithWeights { unsure(taken), unsure(notTaken) };
            if (m_moduleEvaluatingBlocks.get(terminator.targets[0].block.index()) && notTaken->frequency())
                successorsWithWeights = { rarely(taken), usually(notTaken) };
            else if (m_moduleEvaluatingBlocks.get(terminator.targets[1].block.index()) && taken->frequency())
                successorsWithWeights = { usually(taken), rarely(notTaken) };
            SetForScope indexOfTest(m_currentIndexInBlock, m_currentBlock->values.size() - 1);
            m_truthinessSuccessors = successorsWithWeights;
            condition = lowerToBoolean(m_function.at(terminator.value).child(0));
            m_truthinessSuccessors = std::nullopt;
            if (condition.value())
                m_out.branch(condition.value(), successorsWithWeights.first, successorsWithWeights.second);
            return;
        }
        if (m_moduleEvaluatingBlocks.get(terminator.targets[0].block.index()) && notTaken->frequency())
            m_out.branch(condition.value(), rarely(taken), usually(notTaken));
        else if (m_moduleEvaluatingBlocks.get(terminator.targets[1].block.index()) && taken->frequency())
            m_out.branch(condition.value(), usually(taken), rarely(notTaken));
        else if (((m_function.at(terminator.value).opcode == Opcode::HasShapeOf && m_function.at(terminator.value).immediates[1] == shapeTestGuardsFastPath) || m_function.at(terminator.value).guardsFastPath) && taken->frequency())
            m_out.branch(condition.value(), usually(taken), rarely(notTaken));
        else
            m_out.branch(condition.value(), unsure(taken), unsure(notTaken));
        return;
    }
    case TerminatorKind::Return: {
        if (m_isEntry) {
            m_out.ret(terminator.value ? in<Boxed>(terminator.value).value() : undefined().value());
            return;
        }
        if (!m_function.resultTypes.isEmpty()) {
            if (m_function.at(terminator.value).isJumpToContinuation())
                m_out.unreachable();
            else
                returnResults(m_function.at(terminator.value));
            return;
        }
        Rep rep = resultRepOf(m_program, m_id);
        if (rep == Rep::Void) {
            m_out.m_block->clearSuccessors();
            m_out.m_block->appendNew<B3::Value>(m_proc, B3::Return, m_out.origin());
            return;
        }
        if (terminator.value && rep == Rep::JSValue && tryTailCallBoxNumber(terminator.value))
            return;
        if (terminator.value)
            m_out.ret(in(terminator.value, rep).value);
        else
            m_out.ret(convert(undefined(), Type::fromPrimitives(Primitive::Undefined), rep).value);
        return;
    }
    case TerminatorKind::Throw: {
        if (LValue caught = terminator.targets.isEmpty() ? nullptr : m_caughtValues.get(terminator.targets[0].block.index())) {
            Boxed thrown = isErrorThrownToIgnoringHandler(*m_currentBlock, terminator.value) ? undefined() : in<Boxed>(terminator.value);
            if (m_program.propagatesExceptionObjects && typeOf(terminator.value).isAnything() && !exceptionCaughtIn(terminator.targets[0].block))
                thrown = AOT_TYPED_OPERATION(ExceptionValue, thrown);
            LBasicBlock handler = edgeTo(terminator.targets[0], false);
            m_out.addIncomingToPhi(caught, m_out.anchor(thrown.value()));
            m_out.jump(handler);
            return;
        }
        std::optional<Boxed> thrown;
        if (!isLazilyFetched(terminator.value))
            thrown = in<Boxed>(terminator.value);
        if (!terminator.targets.isEmpty())
            m_activeHandler = &terminator.targets[0];
        if (thrown) {
            if (!terminator.cases.isEmpty())
                m_throwPosition = LoweringContext::SitePosition { static_cast<uint32_t>(terminator.cases[0]), terminator.cases.size() > 1 ? static_cast<uint32_t>(terminator.cases[1]) : 0 };
            if (m_activeHandler)
                AOT_TYPED_OPERATION(ThrowToHandlerInFunction, *thrown);
            else
                AOT_TYPED_OPERATION(Throw, *thrown);
            m_throwPosition = std::nullopt;
        } else
            throwNewError(m_function.at(terminator.value));
        didLowerOperationWithHandler();
        m_out.unreachable();
        return;
    }
    case TerminatorKind::Invoke:
        m_out.jump(edgeTo(terminator.targets[0], false));
        return;
    case TerminatorKind::Switch: {
        Integer value = in<Integer>(terminator.value);
        Vector<FTL::SwitchCase> cases;
        for (unsigned i = 0; i < terminator.cases.size(); ++i)
            cases.append(FTL::SwitchCase(m_out.constInt64(terminator.cases[i]), edgeTo(terminator.targets[i], true), FTL::Weight()));
        m_out.switchInstruction(value.value(), cases, edgeTo(terminator.targets.last(), true), FTL::Weight());
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Lowering::PredecessorLists Lowering::computePredecessors(const Vector<BlockID>& order) const
{
    unsigned numberOfBlocks = m_function.blocks.size();
    PredecessorLists result;
    result.placeInOrder.fill(0, numberOfBlocks);
    for (unsigned i = 0; i < order.size(); ++i)
        result.placeInOrder[order[i].index()] = i;
    result.first.fill(0, numberOfBlocks + 1);
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator))
            ++result.first[target.block.index() + 1];
    }
    for (unsigned i = 0; i < numberOfBlocks; ++i)
        result.first[i + 1] += result.first[i];
    result.blocks.grow(result.first[numberOfBlocks]);
    Vector<unsigned, 32> next(result.first.span().first(numberOfBlocks));
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator))
            result.blocks[next[target.block.index()]++] = block.index();
    }
    return result;
}

void Lowering::computeLoopBlocks(const Vector<BlockID>& order, const PredecessorLists& predecessors)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    auto& place = predecessors.placeInOrder;
    for (BlockID block : order) {
        for (auto& target : successorsOf(m_function.at(block).terminator)) {
            if (place[target.block.index()] > place[block.index()])
                continue;
            BitVector seen(numberOfBlocks);
            seen.set(target.block.index());
            Vector<unsigned, 16> worklist;
            if (!seen.set(block.index()))
                worklist.append(block.index());
            while (!worklist.isEmpty()) {
                for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                    if (!seen.set(predecessor))
                        worklist.append(predecessor);
                }
            }
            m_loopBlocks.merge(seen);
        }
    }
}

void Lowering::computeDominators(const Vector<BlockID>& order, const PredecessorLists& predecessors)
{
    unsigned numberOfBlocks = m_function.blocks.size();
    static constexpr unsigned unknown = UINT_MAX;
    auto& place = predecessors.placeInOrder;
    Vector<unsigned> dominator;
    dominator.fill(unknown, numberOfBlocks);
    dominator[order[0].index()] = order[0].index();
    for (bool changed = !tryFindImmediateDominators(order.size(), [&](unsigned i) { return order[i].index(); }, predecessors, dominator); changed;) {
        changed = false;
        for (unsigned i = 1; i < order.size(); ++i) {
            unsigned found = unknown;
            for (unsigned other : predecessors[order[i].index()]) {
                if (dominator[other] == unknown)
                    continue;
                if (found == unknown) {
                    found = other;
                    continue;
                }
                while (found != other) {
                    while (place[other] > place[found])
                        other = dominator[other];
                    while (place[found] > place[other])
                        found = dominator[found];
                }
            }
            if (dominator[order[i].index()] != found) {
                dominator[order[i].index()] = found;
                changed = true;
            }
        }
    }
    Vector<Vector<unsigned, 2>> dominated(numberOfBlocks);
    for (unsigned i = 1; i < order.size(); ++i)
        dominated[dominator[order[i].index()]].append(order[i].index());
    m_dominatorTreeEnter.fill(0, numberOfBlocks);
    m_dominatorTreeLeave.fill(0, numberOfBlocks);
    unsigned clock = 0;
    Vector<std::pair<unsigned, unsigned>> stack { { order[0].index(), 0u } };
    m_dominatorTreeEnter[order[0].index()] = clock++;
    while (!stack.isEmpty()) {
        auto& [block, next] = stack.last();
        if (next == dominated[block].size()) {
            m_dominatorTreeLeave[block] = clock++;
            stack.removeLast();
            continue;
        }
        unsigned child = dominated[block][next++];
        m_dominatorTreeEnter[child] = clock++;
        stack.append({ child, 0u });
    }
}

bool Lowering::verifyDefinitions(const Vector<BlockID>& order) const
{
    BitVector isWatched;
    for (BlockID blockID : order) {
        auto& block = m_function.at(blockID);
        if (block.terminator.kind == TerminatorKind::Invoke && !block.values.isEmpty())
            isWatched.set(block.values.last().index());
    }
    Vector<unsigned> numberOfPredecessors;
    if (!isWatched.isEmpty()) {
        numberOfPredecessors.fill(0, m_function.blocks.size());
        for (BlockID blockID : order) {
            for (auto& target : successorsOf(m_function.at(blockID).terminator))
                ++numberOfPredecessors[target.block.index()];
        }
    }
    BitVector isDefined(m_function.values.size());
    bool isRight = true;
    for (BlockID blockID : order) {
        auto& block = m_function.at(blockID);
        bool hasThrowingValue = block.terminator.kind == TerminatorKind::Invoke && !block.values.isEmpty();
        auto use = [&](ValueID operand, bool isBeforeThrowingValue) {
            if (!operand)
                return;
            BlockID definedIn = m_definingBlock[operand.index()];
            if (definedIn == blockID) {
                isRight &= isDefined.quickGet(operand.index()) && !(isBeforeThrowingValue && operand == block.values.last());
                return;
            }
            if (!definedIn || !dominates(definedIn, blockID)) {
                isRight = false;
                return;
            }
            if (isWatched.get(operand.index())) {
                BlockID next = m_function.at(definedIn).terminator.targets[0].block;
                isRight &= numberOfPredecessors[next.index()] == 1 && dominates(next, blockID);
            }
        };
        for (ValueID parameter : block.parameters)
            isRight &= !isDefined.quickSet(parameter.index());
        for (ValueID id : block.values) {
            m_function.at(id).forEachChild([&](ValueID child) {
                use(child, false);
            });
            isRight &= !isDefined.quickSet(id.index());
        }
        use(block.terminator.value, hasThrowingValue);
        for (auto& target : successorsOf(block.terminator)) {
            bool leadsToHandler = hasThrowingValue && &target == &block.terminator.targets[1];
            for (ValueID argument : target.arguments)
                use(argument, leadsToHandler);
        }
    }
    return isRight;
}

void Lowering::didPassNullishTest(ValueID id, BlockID block)
{
    auto& blocks = m_blocksAfterNullishTest.add(id.index(), Vector<BlockAfterNullishTest, 1> { }).iterator->value;
    unsigned enter = m_dominatorTreeEnter[block.index()];
    unsigned leave = m_dominatorTreeLeave[block.index()];
    size_t place = std::ranges::upper_bound(blocks, enter, { }, &BlockAfterNullishTest::enter) - blocks.begin();
    blocks.insert(place, BlockAfterNullishTest { enter, place ? std::max(leave, blocks[place - 1].maxLeave) : leave });
    for (size_t later = place + 1; later < blocks.size() && blocks[later].maxLeave < leave; ++later)
        blocks[later].maxLeave = leave;
}

bool Lowering::hasPassedNullishTest(ValueID id) const
{
    if (!m_currentBlock || (m_currentIndexInBlock < m_currentBlock->values.size() && m_currentBlock->values[m_currentIndexInBlock] == id))
        return false;
    for (ValueID value = id;; value = m_function.at(value).child(0)) {
        if (auto& created = m_function.at(value); created.opcode == Opcode::CallBuiltin) {
            auto& implementation = m_builtins.at(created.builtin()).implementation;
            if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::RegExpNew)
                return true;
        }
        if (auto found = m_blocksAfterNullishTest.find(value.index()); found != m_blocksAfterNullishTest.end()) {
            unsigned enter = m_dominatorTreeEnter[m_currentBlockID.index()];
            auto behind = std::ranges::upper_bound(found->value, enter, { }, &BlockAfterNullishTest::enter);
            if (behind != found->value.begin() && (behind - 1)->maxLeave >= enter)
                return true;
        }
        if (m_function.at(value).opcode != Opcode::Cast || m_function.at(value).castReason() == CastReason::Coerce)
            return false;
    }
}

void Lowering::widenLiteralStringOperands(const Value& value)
{
    switch (value.opcode) {
    case Opcode::Cast:
    case Opcode::StrictEq:
    case Opcode::ToBoolean:
    case Opcode::IsUndefinedOrNull:
    case Opcode::PutField:
    case Opcode::PutById:
    case Opcode::PutModuleVariable:
    case Opcode::NewObjectWithShape:
    case Opcode::NewClosure:
    case Opcode::NewClosureOnce:
    case Opcode::CallDirect:
    case Opcode::Results:
        return;
    default:
        break;
    }
    value.forEachChild([&](ValueID child) {
        widenLiteralString(child);
    });
}

void Lowering::widenLiteralString(ValueID id)
{
    if (!typeOf(id).hasLiteralString())
        return;
    auto& asString = *m_widenedLiteralStringTypes.ensure(m_function.at(id).type.index(), [&] {
        return makeUniqueWithoutFastMallocCheck<Type>(m_program.at(m_function.at(id).type).withLiteralStringWidened());
    }).iterator->value;
    m_assumptions.append(Assumption { id, convert(get(id), m_program.at(m_function.at(id).type), repOf(asString)), &asString });
}

const Type& Lowering::withoutNullish(TypeID type) const
{
    return *m_typesWithoutNullish.ensure(type.index(), [&] {
        return makeUniqueWithoutFastMallocCheck<Type>(m_program.at(type).withoutPrimitives(Primitive::UndefinedOrNull));
    }).iterator->value;
}

CellPointer Lowering::nonNullish(ValueID receiver, StringID name)
{
    Operand operand = get(receiver);
    if (operand.rep == Rep::Cell || !typeOf(receiver).mayBeUndefinedOrNull())
        return nonNullish(operand, typeOf(receiver), name);
    bool isUnconditional = m_out.m_block == m_unconditionalBlock;
    CellPointer cell = nonNullish(operand, typeOf(receiver), name);
    bool isWatched = m_currentBlock->terminator.kind == TerminatorKind::Invoke && m_currentIndexInBlock + 1 == m_currentBlock->values.size();
    if (isUnconditional) {
        m_unconditionalBlock = m_out.m_block;
        if (!isWatched) {
            ValueID tested = receiver;
            while (m_function.at(tested).opcode == Opcode::Cast && m_function.at(tested).castReason() != CastReason::As && m_function.at(tested).castReason() != CastReason::Coerce)
                tested = m_function.at(tested).child(0);
            didPassNullishTest(tested, m_currentBlockID);
        }
    }
    return cell;
}

static constexpr unsigned firstCastForMap = 256;
static constexpr unsigned firstTerminatorForMap = 288;
static constexpr unsigned firstBuiltinForMap = 320;

static DFG::Node* originForMap(unsigned kind, bool isInlinedBlock)
{
    return std::bit_cast<DFG::Node*>(static_cast<uintptr_t>((kind + 1) << 1 | isInlinedBlock) << 8);
}

static DFG::Node* originForMap(const Function& function, const Value& value)
{
    unsigned kind = static_cast<unsigned>(value.opcode);
    if (value.opcode == Opcode::Cast)
        kind = firstCastForMap + static_cast<unsigned>(value.castReason());
    else if (value.opcode == Opcode::CallBuiltin)
        kind = firstBuiltinForMap + value.builtin().index();
    uint32_t outermost = value.inlineFrame;
    while (outermost && function.inlineFrames[outermost].parent)
        outermost = function.inlineFrames[outermost].parent;
    uintptr_t inlineReasons = value.inlineFrame ? function.inlineFrames[outermost].licence | function.inlineFrames[value.inlineFrame].licence << 4 : 0;
    return std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(kind, !!value.inlineFrame)) | inlineReasons << 24);
}

static String bytesByOperation(const B3::PCToOriginMap& map, LinkBuffer& linkBuffer, size_t codeSize)
{
    UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> bytes;
    auto* start = static_cast<const uint8_t*>(linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr());
    auto& ranges = map.ranges();
    auto offsetOf = [&](size_t i) {
        return i < ranges.size() ? std::min<size_t>(static_cast<const uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(ranges[i].label).untaggedPtr()) - start, codeSize) : codeSize;
    };
    std::array<uint32_t, 256> bytesByInlineReason { };
    bytes.add(0, 0).iterator->value += offsetOf(0);
    bytesByInlineReason[0] += offsetOf(0);
    for (size_t i = 0; i < ranges.size(); ++i) {
        uintptr_t origin = std::bit_cast<uintptr_t>(ranges[i].origin.dfgOrigin());
        bytes.add(static_cast<uint32_t>(origin >> 8 & 0xffff), 0).iterator->value += offsetOf(i + 1) - offsetOf(i);
        bytesByInlineReason[origin >> 24 & 0xff] += offsetOf(i + 1) - offsetOf(i);
    }
    Vector<std::pair<uint32_t, uint32_t>> sorted;
    for (auto& entry : bytes)
        sorted.append({ entry.key, entry.value });
    std::ranges::sort(sorted);
    StringPrintStream out;
    for (auto& [origin, size] : sorted) {
        if (!size)
            continue;
        out.print("\t");
        unsigned kind = (origin >> 1) - 1;
        if (!origin)
            out.print("-");
        else if (kind >= firstBuiltinForMap) {
            auto& builtin = Builtins::singleton().at(BuiltinID(kind - firstBuiltinForMap));
            out.print("CallBuiltin.", builtin.holder, builtin.holder.isEmpty() ? "" : ".", builtin.name);
        } else if (kind >= firstTerminatorForMap)
            out.print("->", kind - firstTerminatorForMap);
        else if (kind >= firstCastForMap)
            out.print("Cast.", kind - firstCastForMap);
        else
            out.print(nameOf(static_cast<Opcode>(kind)));
        out.print(origin & 1 ? "+" : "", "\t", size);
    }
    out.print("\nL");
    for (unsigned inlineReasons = 0; inlineReasons < bytesByInlineReason.size(); ++inlineReasons) {
        if (bytesByInlineReason[inlineReasons])
            out.print("\t", inlineReasonLetters[inlineReasons & 15], inlineReasonLetters[inlineReasons >> 4], "\t", bytesByInlineReason[inlineReasons]);
    }
    out.print("\nS");
    if (offsetOf(0))
        out.print("\t0\t0\t0\t", offsetOf(0));
    for (size_t i = 0; i < ranges.size(); ++i) {
        uintptr_t origin = std::bit_cast<uintptr_t>(ranges[i].origin.dfgOrigin());
        if (offsetOf(i + 1) > offsetOf(i))
            out.print("\t", origin >> 8 & 0xffff, "\t", origin >> 32 | (origin & 1) << 31, "\t", offsetOf(i), "\t", offsetOf(i + 1) - offsetOf(i));
    }
    return out.toString();
}

void Lowering::lowerBlock(BlockID id)
{
    m_out.appendTo(m_blocks[id.index()]);
    m_currentBlockID = id;
    m_isInLoop = m_loopBlocks.get(id.index());
    bool didEmitCalls = std::exchange(m_context.emitsCalls, false);
#if CPU(X86_64)
    m_context.hasColdSites = false;
#endif
    auto& block = m_function.at(id);
    m_currentBlock = &block;
    m_refillsIntervalOnSlowPath = isCompact();
    for (ValueID parameter : block.parameters) {
        didDefineAccessRunReceiver(parameter);
        Operand operand = m_values[parameter.index()];
        if (!operand.value || (operand.rep != Rep::Double && operand.rep != Rep::OptionalDouble))
            continue;
        bool isComparedHere = std::ranges::any_of(block.values, [&](ValueID user) {
            auto& value = m_function.at(user);
            return (value.opcode == Opcode::StrictEq || value.opcode == Opcode::Eq) && (value.child(0) == parameter || value.child(1) == parameter);
        });
        if (isComparedHere)
            didDefineNumber(parameter, m_function.at(parameter), Number(operand.value));
    }
    materializeFinalStringBuilderStates(block);
    convertNumberArraysBeforeLoops(block.terminator, true);
    auto classForMap = [&](ValueID id) -> uintptr_t {
        if (!id)
            return 0;
        auto& type = typeOf(id);
        if (type.anyObject)
            return m_function.at(id).prediction() ? 2 : 1;
        bool mayBeNullish = type.mayBeUndefinedOrNull();
        if (type.hasObjects()) {
            if (type.primitives & ~Primitive::UndefinedOrNull)
                return 7;
            if (type.acceptsUntypedObjects(m_program))
                return (type.objectLayouts.size() + type.engineClasses.size() + type.anyFunction == 1 ? 10 : 12) + mayBeNullish;
            return (type.objectLayouts.size() + type.engineClasses.size() + type.anyFunction == 1 ? 3 : 5) + mayBeNullish;
        }
        return type.isNumber() || type.isString() || type.isBoolean() ? 8 : 9;
    };
    auto conditionForMap = [&](ValueID id) -> uintptr_t {
        while (id && m_function.at(id).opcode == Opcode::LogicalNot)
            id = m_function.at(id).child(0);
        if (!id)
            return 0;
        auto& condition = m_function.at(id);
        auto isOperand = [&](Opcode opcode) {
            return (condition.child(0) && m_function.at(condition.child(0)).opcode == opcode) || (condition.child(1) && m_function.at(condition.child(1)).opcode == opcode);
        };
        switch (condition.opcode) {
        case Opcode::StrictEq:
        case Opcode::Eq:
            return isOperand(Opcode::String) ? 1 : isOperand(Opcode::Integer) ? 2 : 3;
        case Opcode::ToBoolean:
            return 4;
        case Opcode::Less:
        case Opcode::LessEq:
            return 5;
        case Opcode::CallBuiltin:
            return 7;
        default:
            return 6;
        }
    };
    auto formForMap = [&](ValueID id, const Value& value) -> uintptr_t {
        if (value.isAtKnownSlot())
            return 13;
        const Field* field = fieldFor(typeOf(value.child(0)), value.string());
        if (!field)
            return 0;
        bool mayBeAbsent = field->stored && m_program.at(field->stored).primitives & Primitive::Absent;
        bool tests = false;
        if (value.opcode == Opcode::GetById && field->stored) {
            Type held = m_program.at(field->stored);
            if (held.primitives & Primitive::Absent)
                held.primitives = (held.primitives & ~Primitive::Absent) | Primitive::Undefined;
            tests = !held.isSubsetOf(typeOf(id), m_program);
        }
        return 1 + (field->isOutOfLine ? 2 : mayBeAbsent ? 1 : 0) + 3 * (field->isMissingInSomeSubtype || field->hasOtherOffsetInSomeSubtype) + 6 * tests;
    };
    for (unsigned i = 0; i < block.values.size(); ++i) {
        ValueID valueID = block.values[i];
        auto& value = m_function.at(valueID);
        m_currentIndexInBlock = i;
        if (block.terminator.kind == TerminatorKind::Invoke && i + 1 == block.values.size() && !m_handlersEnteredByBranch.get(block.terminator.targets[1].block.index()))
            m_activeHandler = &block.terminator.targets[1];
        m_unconditionalBlock = m_out.m_block;
        if (Options::aotMapFilePath()) [[unlikely]]
            m_out.setOrigin(std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(m_function, value)) | static_cast<uintptr_t>(valueID.index() + 1) << 32 | classForMap(value.child(0)) << 56 | ((value.opcode == Opcode::GetById || value.opcode == Opcode::PutById) && valueID.index() + 1 < 1u << 20 ? (valueID.index() + 1 < 1u << 17 ? formForMap(valueID, value) << 3 | (value.rep == Rep::Int32 && typeOf(valueID).isBoolean() ? 0 : static_cast<uintptr_t>(value.rep)) : 15 << 3) : 0) << 49 | static_cast<uintptr_t>(m_isInLoop)));
        double frequency = m_out.m_block->frequency();
        size_t numberOfAssumptions = m_assumptions.size();
        widenLiteralStringOperands(value);
        m_tailCall = m_tailCallCandidates.get(valueID.index()) ? &value : nullptr;
        m_tailCallReturnsArgument = m_tailCallsReturningArgument.get(valueID.index());
        m_dropsDroppableCallerFrame = value.dropsDroppableCallerFrame && tailCallDropsMachineFrame(block, value) && !passesPointerIntoFrame(value);
        m_leavesDroppableFrame = value.leavesDroppableFrame && tailCallDropsMachineFrame(block, value) && !passesPointerIntoFrame(value) && !m_activeHandler;
        Operand result = lower(valueID, value);
        m_assumptions.shrink(numberOfAssumptions);
        m_tailCall = nullptr;
        m_dropsDroppableCallerFrame = false;
        m_leavesDroppableFrame = false;
        didLowerOperationWithHandler();
        if (!m_failureReason.isNull())
            return;
        RELEASE_ASSERT_WITH_MESSAGE(m_out.m_block->frequency() >= frequency, "AOT: the lowering of an operation ends in a colder block than it started in");
        if (result.rep == Rep::Cell && value.rep == Rep::JSValue && Options::auditAOTTypedAssertions()) [[unlikely]]
            result = box(result.as<CellPointer>());
        if (result.rep != value.rep) [[unlikely]] {
            dataLogLn("AOT: f", m_id.index(), ": v", valueID.index(), " = ", nameOf(value.opcode), " has representation ", nameOf(value.rep), " but was lowered as ", nameOf(result.rep));
            CRASH();
        }
        if (result.value && m_substringsOnlyInspected.get(valueID.index())) {
            m_substringsOnlyInspected.clear(valueID.index());
            m_isLazilyFetched.clear(valueID.index());
        }
        if (result.value && m_splitsOnlyTakenApart.remove(valueID.index()))
            m_isLazilyFetched.clear(valueID.index());
        define(valueID, result);
        openStringsAgainAfter(valueID);
        RELEASE_ASSERT_WITH_MESSAGE(!m_environmentAwaitingBarrier || i + 1 < block.values.size(), "AOT: a block ends between a store to an environment and its barrier");
        if (m_isFillingReusedEnvironment && i + 1 == block.values.size()) [[unlikely]] {
            dataLogLn("AOT: f", m_id.index(), ": a block ends between the allocation of a reused environment and its barrier");
            CRASH();
        }
        if (isLazilyFetched(valueID))
            continue;
        didDefineAccessRunReceiver(valueID);
        if (result.rep == Rep::Double || result.rep == Rep::OptionalDouble)
            didDefineNumber(valueID, value, Number(result.value));
        if (value.assertedType() && value.opcode != Opcode::Cast) [[unlikely]]
            auditResult(result, value);
        if (Options::validateAOTTypedFacts()) [[unlikely]]
            validateFact(result, valueID, typeOf(valueID));
        bool isDefinedHere = result.rep == Rep::Cell && result.value->opcode() != B3::Get && !m_assumedCells.contains(result.value);
        value.forEachChild([&](ValueID child) {
            isDefinedHere &= m_values[child.index()].value != result.value;
        });
        if (isDefinedHere)
            m_cellTypes.add(result.value, value.type);
    }
    if (Options::aotMapFilePath()) [[unlikely]]
        m_out.setOrigin(std::bit_cast<DFG::Node*>(std::bit_cast<uintptr_t>(originForMap(firstTerminatorForMap + static_cast<unsigned>(block.terminator.kind), false)) | static_cast<uintptr_t>(0x800000 + id.index() + 1) << 32 | (id.index() + 1 < 1u << 16 ? conditionForMap(block.terminator.value) : 0) << 48 | classForMap(block.terminator.value) << 56));
    lowerTerminator(block.terminator);
#if CPU(X86_64)
    if (m_context.hasColdSites && LoweringContext::treatsColdSitesAsCallsForSplitting())
        m_context.blocksWithCalls.set(id.index());
#endif
    if (m_context.emitsCalls)
        m_context.blocksWithCalls.set(id.index());
    if (!m_blocks[id.index()]->frequency())
        m_context.unreachableBlocks.set(id.index());
    m_context.emitsCalls |= didEmitCalls;
}

void Lowering::findStructureDescriptorOfRun(AccessRun& run) const
{
    if (Options::auditAOTTypedAssertions())
        return;
    const Vector<uint32_t, 1>* fewest = nullptr;
    for (StringID name : run.names) {
        auto found = m_program.structureDescriptorsByName.find(name.index() + 1);
        if (found == m_program.structureDescriptorsByName.end())
            return;
        if (!fewest || found->value.size() < fewest->size())
            fewest = &found->value;
    }
    uint32_t only = Format::none;
    for (uint32_t number : *fewest) {
        auto& row = m_program.structureDescriptors[number];
        if (!std::ranges::all_of(run.names, [&](StringID name) { return row.names.contains(name); }))
            continue;
        if (only != Format::none)
            return;
        only = number;
    }
    if (only == Format::none || only >= m_program.firstThisStructureDescriptor || !m_program.structuresWithoutTransitions.get(only))
        return;
    auto& row = m_program.structureDescriptors[only];
    if (row.names.size() > row.inlineCapacity)
        return;
    run.findStructureDescriptor = only;
    for (StringID name : run.names)
        run.places.append(row.names.find(name));
}

void Lowering::findAccessRuns(const Vector<BlockID>& order)
{
    if (m_function.isTyped() || !m_program.isHotOutsideLoops(m_id) || m_program.runsOnce(m_id))
        return;
    struct Access {
        BlockID block;
        unsigned index;
        StringID name;
        bool isStore;
    };
    struct Receiver {
        ValueID value;
        Vector<Access, 4> accesses;
        unsigned numberDominated { 0 };
    };
    Vector<Receiver> receivers;
    UncheckedKeyHashMap<uint32_t, unsigned, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> receiverIndexInCall;
    for (BlockID id : order) {
        auto& block = m_function.at(id);
        for (unsigned i = 0; i < block.values.size(); ++i) {
            auto& value = m_function.at(block.values[i]);
            if ((value.opcode != Opcode::GetById && value.opcode != Opcode::PutById) || !value.receiverHasSingleStructure || value.isFirstStoreToThis() || isLazilyFetched(block.values[i]) || value.string() == m_length)
                continue;
            ValueID object = value.child(0);
            if (m_function.at(object).rep != Rep::JSValue || isLazilyFetched(object))
                continue;
            auto added = receiverIndexInCall.add(object.index(), receivers.size());
            if (added.isNewEntry)
                receivers.append(Receiver { object, { }, 0 });
            receivers[added.iterator->value].accesses.append(Access { id, i, value.string(), value.opcode == Opcode::PutById });
        }
    }
    auto isDominated = [&](const Receiver& receiver, const Access& access) {
        for (auto& other : receiver.accesses) {
            if (&other != &access && (other.block == access.block ? other.index < access.index : dominates(other.block, access.block)))
                return true;
        }
        return false;
    };
    for (auto& receiver : receivers) {
        for (auto& access : receiver.accesses)
            receiver.numberDominated += isDominated(receiver, access);
    }
    std::ranges::stable_sort(receivers, [](auto& a, auto& b) {
        return a.numberDominated > b.numberDominated;
    });
    auto callsOnFastPath = [](Opcode opcode) {
        switch (opcode) {
        case Opcode::Undefined:
        case Opcode::Integer:
        case Opcode::String:
        case Opcode::Boolean:
        case Opcode::GetModuleVariable:
        case Opcode::PutModuleVariable:
        case Opcode::GetById:
        case Opcode::PutById:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::TypeOf:
        case Opcode::IsUndefinedOrNull:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::Cast:
            return false;
        default:
            return true;
        }
    };
    constexpr unsigned maxNames = 24;
    for (auto& receiver : receivers) {
        if (!receiver.numberDominated || m_accessRuns.size() == 6)
            break;
        AccessRun run;
        for (auto& access : receiver.accesses) {
            size_t index = run.names.find(access.name);
            if (index == notFound) {
                if (run.names.size() == maxNames)
                    continue;
                index = run.names.size();
                run.names.append(access.name);
            }
            if (access.isStore)
                run.stored |= 1u << index;
        }
        findStructureDescriptorOfRun(run);
        unsigned slots = run.findStructureDescriptor != Format::none ? 0 : (run.names.size() + 7) / 8;
        if (m_context.sites.size() + slots > std::min<uint32_t>(SharedData::maxSlots, FunctionInfo::maxEncodedSlots))
            break;
        run.header = m_context.sites.size();
        for (unsigned i = 0; i < slots; ++i)
            m_context.sites.append(Site { });
        run.verdict = m_proc.addVariable(Int32);
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), run.verdict, m_out.int32Zero);
        for (unsigned k = 0; k < receiver.accesses.size(); ++k) {
            auto& access = receiver.accesses[k];
            if (isDominated(receiver, access))
                m_dominatedAccesses.add(accessRunKey(access.block, access.index));
            if (!k || receiver.accesses[k - 1].block != access.block)
                continue;
            auto& block = m_function.at(access.block);
            bool follows = true;
            for (unsigned i = receiver.accesses[k - 1].index + 1; i < access.index; ++i)
                follows &= !callsOnFastPath(m_function.at(block.values[i]).opcode);
            if (follows)
                m_followingAccesses.add(accessRunKey(access.block, access.index));
        }
        m_accessRuns.add(receiver.value.index(), WTF::move(run));
    }
}

void Lowering::didDefineAccessRunReceiver(ValueID id)
{
    if (m_accessRuns.isEmpty())
        return;
    if (auto found = m_accessRuns.find(id.index()); found != m_accessRuns.end())
        m_out.m_block->appendNew<VariableValue>(m_proc, B3::Set, m_out.origin(), found->value.verdict, m_out.int32Zero);
}

void Lowering::invalidateAccessRunVerdicts()
{
    if (m_accessRuns.isEmpty())
        return;
    HeapRange structureIDs = m_heaps.JSCell_structureID.range();
    InsertionSet insertions(m_proc);
    auto invalidateAt = [&](unsigned index, B3::Origin origin) {
        B3::Value* none = insertions.insertIntConstant(index, origin, Int32, 0);
        for (auto& run : m_accessRuns.values())
            insertions.insert<VariableValue>(index, B3::Set, origin, run.verdict, none);
    };
    UncheckedKeyHashSet<B3::BasicBlock*> afterTerminals;
    for (B3::BasicBlock* block : m_proc) {
        for (unsigned i = 0; i < block->size(); ++i) {
            B3::Value* value = block->at(i);
            HeapRange writes = value->effects().writes;
            if (!writes || (value->as<MemoryValue>() && (!writes.overlaps(structureIDs) || m_structurePreservingStores.contains(value))))
                continue;
            if (i + 1 == block->size()) {
                for (B3::BasicBlock* successor : block->successorBlocks())
                    afterTerminals.add(successor);
                continue;
            }
            invalidateAt(i + 1, value->origin());
        }
        insertions.execute(block);
    }
    for (B3::BasicBlock* block : m_proc) {
        if (!afterTerminals.contains(block))
            continue;
        invalidateAt(0, block->at(0)->origin());
        insertions.execute(block);
    }
}

bool Lowering::run()
{
    ImmutableFunctionScope immutableFunctionScope;
    if (m_function.blocks.isEmpty()) {
        fail("a function without a body"_s);
        return false;
    }
    static_assert(maxInlineFrames == PackedSite::maxInlineFrames);
    for (uint32_t position : m_function.suspensionPositions) {
        ++m_nextSite;
        m_context.sitePositions.append({ position, 0 });
    }
    for (auto& frame : m_function.inlineFrames) {
        bool isSelfFunction = m_context.inlineCallSites.isEmpty();
        m_context.inlineCallSites.append(isSelfFunction ? 0 : CallSiteIndex(BytecodeIndex(m_nextSite++)).bits());
        if (!isSelfFunction)
            m_context.sitePositions.append({ frame.callPosition, frame.parent });
    }
    findTailCallsEmittedAsJumps();
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            m_opcodes.add(value.opcode);
            if (!value.isInTailPosition || m_tailCallsEmittedAsJumps.get(id.index()) || m_function.isSharedOutlinedBody || frameRemainingAfterTailCall(value) || m_context.framesDroppedByTailCalls.contains(value.inlineFrame) || m_context.inlineCallSites.size() > maxInlineFrames)
                continue;
            if (m_context.inlineCallSites.isEmpty())
                m_context.inlineCallSites.append(0);
            m_context.framesDroppedByTailCalls.append(static_cast<uint32_t>(value.inlineFrame));
            m_context.inlineCallSites.append(CallSiteIndex(BytecodeIndex(m_nextSite++)).bits());
            m_context.sitePositions.append({ 0, value.inlineFrame });
        }
    }
    m_context.numberOfSitesWithoutCode = m_nextSite - 1;
    for (uint32_t index = 0; index < m_function.values.size() && !Options::validateAOTTypedFacts(); ++index) {
        auto& value = m_function.values[index];
        if (!value.hasInitialStructure() || !m_program.initialStructures[value.initialStructure()].isWhole || m_context.sites.size() + 2 > maxFunctionSlots())
            continue;
        m_completeAllocationSlots.add(index, m_context.sites.size());
        m_context.sites.append(Site { });
        m_context.sites.append(Site { });
    }
    unsigned numberOfBlocks = m_function.blocks.size();
    m_values.grow(m_function.values.size());
    m_blocks.fill(nullptr, numberOfBlocks);
    m_phis.grow(numberOfBlocks);
    m_out.initialize(m_heaps);
    m_out.setFrequency(1);
    LBasicBlock prologue = m_out.newBlock();
    Vector<BlockID> order = reversePostorder();
    {
        PredecessorLists predecessors = computePredecessors(order);
        computeLoopBlocks(order, predecessors);
        computeDominators(order, predecessors);
        computeLoops(order, predecessors);
    }
    if (!verifyDefinitions(order)) {
        m_usesUndefinedValue = true;
        fail("a value is used where it is not defined on every path: this is a bug in bun --aot: please report it"_s);
        return false;
    }
    findHandlersEnteredByBranch(order);
    for (BlockID block : order) {
        m_blocks[block.index()] = m_out.newBlock();
        if (m_function.at(block).terminator.kind == TerminatorKind::Unreachable || m_function.at(block).terminator.kind == TerminatorKind::Throw || m_handlersEnteredByBranch.get(block.index()))
            m_blocks[block.index()]->setFrequency(0);
    }
    for (BlockID block : order) {
        auto& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.targets[0].block == terminator.targets[1].block || !isReceiverGuard(m_function.at(terminator.value)))
            continue;
        auto& guard = m_function.at(terminator.value);
        auto* engineClass = typeOf(guard.arguments[0]).singleEngineClass();
        BlockID otherwise = terminator.targets[1].block;
        if (engineClass && engineClass->index == guard.engineClass() && m_blocks[otherwise.index()] && numberOfPredecessorsOf(otherwise) == 1)
            m_blocks[otherwise.index()]->setFrequency(0);
    }
    for (unsigned i = order.size(); i--;) {
        auto& block = m_function.at(order[i]);
        LBasicBlock lowered = m_blocks[order[i].index()];
        if (!lowered->frequency())
            continue;
        bool isUnreachable = m_opcodes.contains(Opcode::CallDirect) && block.values.containsIf([&](ValueID id) {
            return m_function.at(id).opcode == Opcode::CallDirect && m_program.neverReturns(m_function.at(id).function());
        });
        if (!isUnreachable && (block.terminator.kind == TerminatorKind::Jump || block.terminator.kind == TerminatorKind::Branch || block.terminator.kind == TerminatorKind::Switch)) {
            isUnreachable = !block.terminator.targets.isEmpty();
            for (auto& target : block.terminator.targets)
                isUnreachable &= m_blocks[target.block.index()] && !m_blocks[target.block.index()]->frequency();
        }
        if (isUnreachable && order[i].index())
            lowered->setFrequency(0);
    }
    {
        auto guardsFastPath = [&](const Terminator& terminator) {
            return terminator.kind == TerminatorKind::Branch && ((m_function.at(terminator.value).opcode == Opcode::HasShapeOf && m_function.at(terminator.value).immediates[1] == shapeTestGuardsFastPath) || m_function.at(terminator.value).guardsFastPath);
        };
        bool hasGuards = false;
        for (BlockID block : order)
            hasGuards |= guardsFastPath(m_function.at(block).terminator);
        if (hasGuards) {
            BitVector isOnFastPath(numberOfBlocks);
            Vector<BlockID> worklist { BlockID(0u) };
            isOnFastPath.set(0);
            while (!worklist.isEmpty()) {
                auto& terminator = m_function.at(worklist.takeLast()).terminator;
                unsigned numberOfTargets = guardsFastPath(terminator) ? 1 : terminator.targets.size();
                for (unsigned t = 0; t < numberOfTargets; ++t) {
                    if (!isOnFastPath.set(terminator.targets[t].block.index()))
                        worklist.append(terminator.targets[t].block);
                }
            }
            for (BlockID block : order) {
                if (!isOnFastPath.get(block.index()))
                    m_blocks[block.index()]->setFrequency(0);
            }
        }
    }
    for (unsigned i = order.size(); m_function.kind != FunctionKind::Module && i--;) {
        auto& block = m_function.at(order[i]);
        bool evaluatesModule = m_opcodes.contains(Opcode::CallDirect) && block.values.containsIf([&](ValueID id) {
            return m_function.at(id).opcode == Opcode::CallDirect && m_program.at(m_function.at(id).function()).kind == FunctionKind::Module;
        });
        if (!evaluatesModule && (block.terminator.kind == TerminatorKind::Jump || block.terminator.kind == TerminatorKind::Branch)) {
            bool onlyLeadsToColdBlocks = true;
            for (auto& target : block.terminator.targets) {
                evaluatesModule |= m_moduleEvaluatingBlocks.get(target.block.index());
                onlyLeadsToColdBlocks &= m_blocks[target.block.index()] && !m_blocks[target.block.index()]->frequency();
            }
            evaluatesModule &= onlyLeadsToColdBlocks;
        }
        if (evaluatesModule) {
            m_blocks[order[i].index()]->setFrequency(0);
            m_moduleEvaluatingBlocks.set(order[i].index());
        }
    }
    m_out.appendTo(prologue);
    m_out.initializeConstants(m_proc, prologue);
    findPinnedRegisters();
    if (!lowerEntryParameters())
        return false;
    static constexpr OpcodeSet lazilyFetchedLeaves { Opcode::String, Opcode::BuiltinFunction };
    static constexpr OpcodeSet concatenations { Opcode::Add, Opcode::Strcat };
    static constexpr OpcodeSet usersOfInt32 { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift, Opcode::BitNot, Opcode::CallBuiltin };
    bool callsBuiltins = m_opcodes.contains(Opcode::CallBuiltin);
    bool opensIterators = m_opcodes.contains(Opcode::IteratorOpen);
    if (m_opcodes.contains(Opcode::ToBoolean))
        findTruthinessPhis();
    findClosureTestsFusedWithBranches();
    if (m_opcodes.intersects(lazilyFetchedLeaves))
        findLazilyFetchedLeaves();
    if (callsBuiltins) {
        findSplitsOnlyTakenApart();
        findSubstringsOnlyInspected();
    }
    if (m_opcodes.intersects(concatenations))
        findConcatenationsWithNumbers();
    findStringsReadInLoops();
    findNumberArraysUsedInLoops();
    findTypedArraysAccessedInLoops();
    if (opensIterators)
        findStringIterations();
    if (callsBuiltins)
        findArrayEntries();
    if (opensIterators) {
        findElementsPerStep();
        findIterationsInFrame();
    }
    findSplitJoinParameters();
    findNumbersJoinedWithIntegers(order);
    if (m_opcodes.intersects(usersOfInt32))
        findNumbersJoinedWithInt32(order);
    findConstantJoinsUsedAsNumbers();
    findRepeatedShortStringComparisons(order);
    findRepeatedIntegerComparisons(order);
    findInstanceOfRuns(order);
    BitVector hasLocalThrowSource;
    for (BlockID block : order) {
        auto& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Throw || terminator.targets.isEmpty())
            continue;
        auto& handler = m_function.at(terminator.targets[0].block);
        if (handler.values.containsIf([&](ValueID id) { return m_function.at(id).opcode == Opcode::CatchValue; }))
            hasLocalThrowSource.set(terminator.targets[0].block.index());
    }
    findAccessRuns(order);
    for (BlockID block : order) {
        if (!block.index())
            continue;
        m_out.appendTo(m_blocks[block.index()]);
        if (hasLocalThrowSource.get(block.index()))
            m_caughtValues.add(block.index(), m_out.phi(Int64));
        for (ValueID id : m_function.at(block).parameters) {
            Rep rep = m_function.at(id).rep;
            LValue phi = m_out.phi(b3TypeOf(rep));
            m_phis[block.index()].append(phi);
            m_values[id.index()] = Operand(rep, phi);
            if (m_isSplitJoin.get(id.index())) {
                LValue cell = m_out.phi(Int64);
                m_cellPhis.add(id.index(), cell);
                m_values[id.index()] = fromParts(phi, cell, true);
            }
            if (m_hasTruthinessPhi.get(id.index()))
                m_truthinessPhis.add(id.index(), Truthiness { m_values[id.index()].value, m_out.phi(Int32) });
            if (auto minimum = m_minOfNumbersJoinedWithIntegers.find(id.index()); minimum != m_minOfNumbersJoinedWithIntegers.end()) {
                LValue integerPhi = m_out.phi(Int64);
                m_integerPhis.add(id.index(), integerPhi);
                m_numbersAsIntegers.add(id.index(), NumberAsInteger { phi, integerPhi, minimum->value });
            }
            if (m_hasInt32Phi.get(id.index()))
                m_int32Phis.add(id.index(), m_out.phi(Int32));
            if (m_isConstantJoinUsedAsNumber.get(id.index()))
                m_numberPhis.add(id.index(), m_out.phi(Double));
            if (rep == Rep::Cell)
                m_cellTypes.add(phi, m_function.at(id).type);
        }
    }
    m_out.appendTo(m_selfTailCallLoopHeader ? m_selfTailCallLoopHeader : prologue);
    m_out.jump(m_blocks[0]);
    for (BlockID block : order) {
        if (m_blocksTakenIntoBranchOnLiteral.get(block.index())) {
            m_out.appendTo(m_blocks[block.index()]);
            m_out.unreachable();
            continue;
        }
        lowerBlock(block);
        if (!m_failureReason.isNull())
            return false;
    }
    m_heaps.computeRangesAndDecorateInstructions();
    returnHeapsToThread();
    invalidateAccessRunVerdicts();
    m_proc.deleteOrphans();
    m_out.applyBlockOrder();
    return true;
}

bool Lowering::lowerEntryParameters()
{
    auto& parameters = m_function.blocks[0].parameters;
    unsigned nextGPR = 0;
    if (m_function.has(FunctionUsesArgumentBuffer)) {
        m_argumentCount = Word32(m_out.castToInt32(registerOnEntry(argumentGPR(0))));
        m_argumentBuffer = Pointer(registerOnEntry(argumentGPR(1)));
    }
    if (m_function.isGenericEntrypoint()) {
        for (unsigned i = 0; i < parameters.size(); ++i) {
            Rep rep = m_function.at(parameters[i]).rep;
            RELEASE_ASSERT(i ? rep == Rep::JSValue : holdsCells(rep));
            GPRReg reg = i ? thisGPR : calleeGPR;
            if (i >= 2) {
                ++m_genericEntrypointParameterCount;
                if (nextGPR >= numberOfArgumentGPRs) {
                    fail("more parameters than there are registers"_s);
                    return false;
                }
                reg = argumentGPR(nextGPR++);
            }
            LValue value = registerOnEntry(reg);
            m_values[parameters[i].index()] = Operand(rep, value);
            if (rep == Rep::Cell)
                m_cellTypes.add(value, m_function.at(parameters[i]).type);
        }
        return true;
    }

    unsigned nextFPR = 0;
    unsigned onStack = 0;
    Vector<LValue, 8> passed;
    for (unsigned i = 0; i < parameters.size(); ++i) {
        Rep rep = repOf(declaredArgumentType(m_function, i));
        bool isDouble = b3TypeOf(rep) == Double;
        LValue value;
        if (i < 2 && m_function.isValueEntry && !m_function.isValueEntryClone)
            value = registerOnEntry(i ? thisGPR : calleeGPR);
        else if (m_function.takesCapturesFromClosure && i < m_function.captureTypes.size())
            value = closureCapture(CellPointer(registerOnEntry(calleeGPR)), m_function, i).value;
        else if (isDouble && nextFPR < FPRInfo::numberOfArgumentRegisters)
            value = registerOnEntry(FPRInfo::toArgumentRegister(nextFPR++));
        else if (!isDouble && nextGPR < numberOfDirectArgumentGPRs) {
            value = registerOnEntry(directArgumentGPR(nextGPR++));
            if (rep == Rep::Int32)
                value = m_out.castToInt32(value);
        } else {
            m_context.readsCallerFrame = true;
            TypedPointer pointer = m_out.address(m_heaps.root, m_out.framePointer(), sizeof(CallerFrameAndPC) + onStack++ * sizeof(Register));
            value = isDouble ? m_out.loadDouble(pointer) : rep == Rep::Int32 ? m_out.load32(pointer) : m_out.load64(pointer);
        }
        passed.append(value);
    }
    if (m_hasSelfTailCall) {
        Vector<ValueFromBlock, 8> initialValues;
        for (LValue value : passed)
            initialValues.append(m_out.anchor(value));
        m_selfTailCallLoopHeader = m_out.newBlock();
        m_out.jump(m_selfTailCallLoopHeader);
        m_out.appendTo(m_selfTailCallLoopHeader);
        for (unsigned i = 0; i < passed.size(); ++i) {
            passed[i] = m_out.phi(passed[i]->type(), initialValues[i]);
            m_selfTailCallPhis.append(passed[i]);
        }
    }
    for (unsigned i = 0; i < parameters.size(); ++i) {
        auto& parameter = m_function.at(parameters[i]);
        auto& type = declaredArgumentType(m_function, i);
        Rep rep = repOf(type);
        LValue value = passed[i];
        m_values[parameters[i].index()] = convert(Operand(rep, value), type, parameter.rep);
        if (rep == Rep::Cell && parameter.rep == Rep::Cell)
            m_cellTypes.add(value, parameter.type);
    }
    return true;
}

TypedPointer Lowering::inStatics(const FTL::AbstractHeap& heap, ptrdiff_t offset)
{
    if (!isX86())
        return m_out.address(heap, fixedPointer(Instance::offsetOfTypedStatics()), offset);
    return m_out.address(heap, m_instance, Instance::typedStaticsDisplacement(m_program.sizeOfStatics) + offset);
}

TypedPointer Lowering::cardOf(uint32_t offsetInStatics)
{
    if (!isX86())
        return m_out.address(m_heaps.AOTTyped_cards, fixedPointer(Instance::offsetOfTypedCards()), offsetInStatics >> Instance::typedCardShift);
    return m_out.address(m_heaps.AOTTyped_cards, m_instance, Instance::typedCardsDisplacement(m_program.sizeOfStatics) + static_cast<ptrdiff_t>(offsetInStatics >> Instance::typedCardShift));
}

LValue Lowering::staticsBase()
{
    if (!isX86())
        return fixedPointer(Instance::offsetOfTypedStatics());
    return m_out.add(m_instance, m_out.constIntPtr(Instance::typedStaticsDisplacement(m_program.sizeOfStatics)));
}

LValue Lowering::cardsBase()
{
    if (!isX86())
        return fixedPointer(Instance::offsetOfTypedCards());
    return m_out.add(m_instance, m_out.constIntPtr(Instance::typedCardsDisplacement(m_program.sizeOfStatics)));
}

TypedPointer Lowering::addressOf(VariableID id)
{
    return inStatics(m_heaps.AOTTyped_variables[id.index()], m_program.variables[id.index()].offset);
}

void Lowering::checkInitialized(Operand raw, StringID name)
{
    requireOrThrowAt(logicalNot(isAbsent(raw)), ThrowKind::TDZ, std::nullopt, name.index());
}

void Lowering::validateIsInitialized(Operand raw)
{
    LBasicBlock doesNotHold = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isAbsent(raw).value(), rarely(doesNotHold), usually(continuation));
    m_out.appendTo(doesNotHold);
    {
        SetForScope isReporting(m_isReportingFactViolation, true);
        AOT_TYPED_OPERATION(FactViolated, Boxed(m_out.int64Zero), word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
    }
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

void Lowering::findStoresToVariables()
{
    if (std::exchange(m_didFindStoresToVariables, true))
        return;
    for (auto& block : m_function.blocks) {
        for (ValueID id : block.values) {
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::PutModuleVariable)
                m_storedVariables.set(value.variable().index());
            else if (value.opcode == Opcode::PutField)
                m_storedFields.add(static_cast<uint64_t>(value.layout().index()) << 32 | value.field());
        }
    }
}

bool Lowering::isKnownInitialized(VariableID variable) const
{
    auto found = m_blocksCheckingInitialization.find(variable.index());
    if (found == m_blocksCheckingInitialization.end())
        return false;
    return std::ranges::any_of(found->value, [&](BlockID block) {
        return dominates(block, m_currentBlockID);
    });
}

void Lowering::didCheckInitialized(VariableID variable)
{
    if (m_activeHandler || !m_currentBlock)
        return;
    m_blocksCheckingInitialization.add(variable.index(), Vector<BlockID, 2> { }).iterator->value.append(m_currentBlockID);
}

Operand Lowering::lowerGetModuleVariable(ValueID result, const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    unsigned flags = IsSafe;
    if (variable.isStoredOnce) {
        findStoresToVariables();
        if (!m_storedVariables.get(value.variable().index()))
            flags |= IsFixed;
    }
    Operand raw = load(variable.store, addressOf(value.variable()), flags);
    if (variable.mayBeReadUninitialized() && !value.readsInitializedVariable() && !isKnownInitialized(value.variable())) {
        checkInitialized(raw, variable.name);
        didCheckInitialized(value.variable());
    }
    if (Options::validateAOTTypedFacts() && variable.mayBeReadUninitialized() && value.readsInitializedVariable()) [[unlikely]]
        validateIsInitialized(raw);
    if (Options::validateAOTTypedFacts() && variable.singleFunctionIsProven) [[unlikely]]
        validateSingleFunction(raw, result, variable.singleFunction, false);
    return ensure(raw, m_program.at(variable.type ? variable.type : m_program.types.anything), typeOf(result));
}

LValue Lowering::globalPropertyCacheWord(uint32_t cacheNumber, size_t offset)
{
    return marked(m_out.load64(inStatics(m_heaps.AOTTyped_collectorState, m_program.globalPropertyCachesOffset + (cacheNumber - 1) * sizeof(GlobalPropertyCache) + offset)), IsSafe);
}

Boxed Lowering::globalPropertyValueOrElse(StringID name, uint32_t cacheNumber, LBasicBlock miss, bool isFreeName)
{
    Boxed value(globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value)));
    orElse(m_out.notZero64(value.value()), miss);
    if (Options::validateAOTTypedFacts() && isFreeName) [[unlikely]]
        AOT_TYPED_OPERATION(ValidateGlobalPermanently, word32(name.index()), value);
    return value;
}

TypedPointer Lowering::globalPropertyOrElse(uint32_t cacheNumber, LBasicBlock miss)
{
    auto inCache = [&](size_t offset) {
        return globalPropertyCacheWord(cacheNumber, offset);
    };
    static_assert(!OBJECT_OFFSETOF(GlobalPropertyCache, structureID) && OBJECT_OFFSETOF(GlobalPropertyCache, globalLexicalBindingEpoch) == sizeof(uint32_t));
    CellPointer globalObject(fixedPointer(Instance::offsetOfGlobalObject()));
    Integer structureID = zeroExtended(loadField<Word32>(globalObject, m_heaps.JSCell_structureID, IsSafe));
    Integer epoch = zeroExtended(loadField<Word32>(globalObject, m_heaps.JSGlobalObject_globalLexicalBindingEpoch, IsSafe));
    orElse(m_out.equal(m_out.bitOr(structureID.value(), m_out.shl(epoch.value(), m_out.constInt32(32))), inCache(0)), miss);
    LValue location = inCache(OBJECT_OFFSETOF(GlobalPropertyCache, location));
    LValue butterflyIfNegative = m_out.bitAnd(loadField<Pointer>(globalObject, m_heaps.JSObject_butterfly, IsSafe).value(), m_out.aShr(location, m_out.constInt32(63)));
    return TypedPointer(m_heaps.properties.atAnyNumber(), m_out.add(butterflyIfNegative, location));
}

Boxed Lowering::lowerGetGlobal(const Value& value)
{
    Word32 name = word32(value.string().index());
    auto& sourceFunction = value.inlineFrame ? m_program.at(m_function.inlineFrames[value.inlineFrame].callee) : m_function;
    bool readsGlobalObject = sourceFunction.has(Format::FunctionIsInBuiltinModule);
    uint32_t cacheNumber = readsGlobalObject ? m_program.globalThisPropertyCacheNumber(value.string()) : m_program.globalPropertyCacheNumber(value.string(), false);
    auto genericPath = [&] {
        if (readsGlobalObject)
            return AOT_TYPED_OPERATION(GetGlobalThisProperty, name, word32(cacheNumber));
        if (value.opcode == Opcode::GetGlobal)
            return AOT_TYPED_OPERATION(GetFreeVariable, name, word32(cacheNumber));
        return AOT_TYPED_OPERATION(GetFreeVariableForTypeOf, name, word32(cacheNumber));
    };
    if (!cacheNumber || !m_out.m_block->frequency())
        return genericPath();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    ValueFromBlock fast = m_out.anchor(globalPropertyValueOrElse(value.string(), cacheNumber, miss, !readsGlobalObject).value());
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(miss, [&] {
        slow = m_out.anchor(genericPath().value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow));
}

std::optional<Boxed> Lowering::getGlobalThisProperty(ValueID base, StringID name)
{
    uint32_t cacheNumber = m_program.globalThisPropertyCacheNumber(name);
    if (auto& value = m_function.at(base); !cacheNumber || value.opcode != Opcode::GetGlobal || !m_program.isGlobalThisName(value.string()))
        return std::nullopt;
    Boxed object = in<Boxed>(base);
    LBasicBlock isAnotherObject = newColdBlock();
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    orElse(m_out.equal(object.value(), marked(m_out.load64(m_out.address(m_heaps.AOTTyped_collectorState, fixedPointer(Instance::offsetOfGlobalObject()), JSGlobalObject::offsetOfGlobalThis())), IsSafe)), isAnotherObject);
    ValueFromBlock fast = m_out.anchor(globalPropertyValueOrElse(name, cacheNumber, miss, false).value());
    m_out.jump(continuation);
    ValueFromBlock slow;
    inColdBlock(miss, [&] {
        slow = m_out.anchor(AOT_TYPED_OPERATION(GetGlobalThisProperty, word32(name.index()), word32(cacheNumber)).value());
        m_out.jump(continuation);
    });
    ValueFromBlock general;
    inColdBlock(isAnotherObject, [&] {
        general = m_out.anchor(emitGetById(object, name).value());
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
    return Boxed(m_out.phi(Int64, fast, slow, general));
}

std::optional<Boxed> Lowering::getGlobalMember(ValueID base, StringID name)
{
    auto& global = m_function.at(base);
    if (global.opcode != Opcode::GetGlobal || !m_out.m_block->frequency())
        return std::nullopt;
    bool isReadFromCache = !m_function.readsHostObjectMember && m_program.isHostObjectFunction(global.string(), name);
    if (isReadFromCache ? m_out.m_block->frequency() < 1 || prefersGenericPathInColdCode() : !m_function.readsHostObjectMember)
        return std::nullopt;
    uint32_t cacheNumber = m_program.globalMemberCacheNumber(global.string(), name);
    if (!cacheNumber)
        return std::nullopt;
    Boxed holder = in<Boxed>(base);
    if (isReadFromCache) {
        LBasicBlock miss = newColdBlock();
        LBasicBlock looks = newColdBlock();
        LBasicBlock reads = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue cached = globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value));
        ValueFromBlock known = m_out.anchor(cached);
        m_out.branch(m_out.notZero64(cached), usually(continuation), rarely(miss));
        m_out.appendTo(miss);
        m_out.branch(m_out.isZero64(globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, location))), unsure(looks), unsure(reads));
        m_out.appendTo(looks);
        AOT_TYPED_OPERATION(LookUpGlobalMember, holder, word32(global.string().index()), word32(name.index()), word32(cacheNumber));
        m_out.jump(reads);
        m_out.appendTo(reads);
        ValueFromBlock found = m_out.anchor(emitGetById(holder, name).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return Boxed(m_out.phi(Int64, known, found));
    }
    LBasicBlock look = newColdBlock();
    LBasicBlock read = m_out.newBlock();
    m_out.branch(m_out.isZero64(globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, location))), rarely(look), usually(read));
    m_out.appendTo(look);
    AOT_TYPED_OPERATION(LookUpGlobalMember, holder, word32(global.string().index()), word32(name.index()), word32(cacheNumber));
    m_out.jump(read);
    m_out.appendTo(read);
    return emitGetById(holder, name);
}

void Lowering::lowerPutGlobal(const Value& value)
{
    Word32 name = word32(value.string().index());
    uint32_t cacheNumber = m_program.globalPropertyCacheNumber(value.string(), true);
    Boxed stored = in<Boxed>(value.child(0));
    auto genericPath = [&, isSloppy = isInSloppyCode()] {
        if (isSloppy)
            AOT_TYPED_OPERATION(SetFreeVariableSloppy, name, word32(cacheNumber), stored);
        else
            AOT_TYPED_OPERATION(SetFreeVariable, name, word32(cacheNumber), stored);
    };
    if (!cacheNumber) {
        genericPath();
        return;
    }
    LBasicBlock miss = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    TypedPointer property = globalPropertyOrElse(cacheNumber, miss);
    didEmitWrite(Writes::Heap);
    m_out.store64(stored.value(), property);
    if (typeOf(value.child(0)).hasCells())
        barrier(CellPointer(fixedPointer(Instance::offsetOfGlobalObject())));
    m_out.jump(continuation);
    inColdBlock(miss, [&] {
        genericPath();
        m_out.jump(continuation);
    });
    m_out.appendTo(continuation);
}

void Lowering::lowerPutModuleVariable(const Value& value)
{
    auto& variable = m_program.variables[value.variable().index()];
    didEmitWrite(Writes::Fields);
    Operand stored = in(value.child(0), repOf(variable.store));
    if (variable.storesUndefinedAsZero && typeOf(value.child(0)).primitives & Primitive::Undefined)
        stored = Boxed(m_out.select(m_out.equal(stored.value, undefined().value()), m_out.int64Zero, stored.value));
    if (Options::validateAOTTypedFacts() && (variable.singleFunctionIsProven || variable.singleFunctionOrNullish)) [[unlikely]]
        validateSingleFunction(stored, value.child(0), variable.singleFunction, true);
    store(variable.store, stored, addressOf(value.variable()));
    if (variable.mayBeReadUninitialized())
        didCheckInitialized(value.variable());
    if (variable.hasCard && holdsCells(variable.store) && typeOf(value.child(0)).hasCells())
        m_out.store32As8(m_out.constInt32(Instance::typedCardIsDirty), cardOf(variable.offset));
}

Operand Lowering::lowerGetEnvironmentField(ValueID result, const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        validateIsNotReleased(cell, value);
    unsigned flags = safeIfReaches(cell, field.offset + sizeOf(field.store));
    if (field.flags & fieldIsImmutable)
        flags |= IsFixed;
    else if (field.flags & fieldIsStoredOnce && field.flags & fieldIsNeverReadUninitialized) {
        findStoresToVariables();
        if (!m_storedFields.contains(static_cast<uint64_t>(value.layout().index()) << 32 | value.field()))
            flags |= IsFixed;
    }
    Operand raw = load(field.store, fieldAt(cell, field), flags, cell.value());
    auto& stored = m_program.at(field.stored);
    if (stored.primitives & Primitive::Absent)
        checkInitialized(raw, field.name);
    if (Options::validateAOTTypedFacts() && field.singleFunctionIsProven) [[unlikely]]
        validateSingleFunction(raw, result, field.singleFunction, false);
    if (Options::validateAOTTypedFacts() && field.otherFunction) [[unlikely]]
        validateSingleFunction(raw, result, field.singleFunction, true, field.otherFunction);
    return ensure(raw, stored.withoutPrimitives(Primitive::Absent), typeOf(result));
}

bool Lowering::isFollowedByStoreWithBarrier(ValueID environment) const
{
    if (Options::validateAOTTypedFacts()) [[unlikely]]
        return false;
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size(); ++i) {
        auto& next = m_function.at(m_currentBlock->values[i]);
        if (next.opcode == Opcode::Integer || next.opcode == Opcode::Unspecified)
            continue;
        if (next.opcode != Opcode::PutField || next.child(0) != environment || isObjectLayout(m_program.at(next.layout()).kind))
            return false;
        if (isAllocatedByFirstUse(next.child(1)) || isLazilyFetched(next.child(1)) || isInitializingStore(m_program, m_function, *m_currentBlock, i))
            return false;
        if (holdsCells(m_program.at(next.layout()).fields[next.field()].store) && typeOf(next.child(1)).hasCells() && m_function.at(next.child(1)).opcode != Opcode::Unspecified)
            return true;
    }
    return false;
}

bool Lowering::isResumeClosureCreatedAtSuspend(size_t index)
{
    auto& values = m_currentBlock->values;
    if (index + 3 > values.size() || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
        return false;
    ValueID closure = values[index];
    auto& created = m_function.at(closure);
    if (created.opcode != Opcode::NewClosure || created.isStackAllocatedClosure() || created.arguments.size() != 1 || !m_program.isResumeFunction(m_program.at(created.function())))
        return false;
    auto& resume = m_program.at(created.function());
    if (m_program.usesSlimClosures(resume) || closureStructureOf(resume.declaredAs) || resume.kind == FunctionKind::Constructor)
        return false;
    ValueID frame = created.arguments[0];
    auto& store = m_function.at(values[index + 1]);
    if (store.opcode != Opcode::PutField || store.child(0) != frame || store.child(1) != closure || store.field() != CoroutineFrame::resumeClosureField || store.layout() != m_program.at(resume.captureTypes[0]).singleLayout())
        return false;
    auto& field = m_program.at(store.layout()).fields[store.field()];
    if ((field.store != Store::Cell && field.store != Store::JSValue) || field.isOutOfLine || field.offset % sizeof(uint64_t) || field.offset / sizeof(uint64_t) > ResumeClosureRequest::maxFrameWord || m_program.at(store.layout()).isStackAllocated)
        return false;
    auto& suspension = m_function.at(values[index + 2]);
    if (!isInlineBuiltin(values[index + 2], InlineBuiltin::SuspendAsyncFunction) || suspension.arguments.size() != 2 || suspension.arguments[0] != closure || suspension.arguments[1] == closure)
        return false;
    return useCountOf(closure) == 2;
}

bool Lowering::defersBarrierToPark(ValueID frame) const
{
    if (Options::validateAOTTypedFacts())
        return false;
    auto belongsToFrame = [&](const Value& access) {
        return access.child(0) == frame && m_program.at(access.layout()).isCoroutineFrame;
    };
    auto isResumeClosure = [&](const Value& value) {
        return value.opcode == Opcode::GetField && belongsToFrame(value) && value.field() == CoroutineFrame::resumeClosureField;
    };
    auto isAlreadyAvailable = [&](ValueID id) {
        return !isAllocatedByFirstUse(id) && !isLazilyFetched(id);
    };
    for (size_t i = m_currentIndexInBlock + 1; i < m_currentBlock->values.size(); ++i) {
        auto& next = m_function.at(m_currentBlock->values[i]);
        if (next.opcode == Opcode::Integer || next.opcode == Opcode::Unspecified || isResumeClosure(next))
            continue;
        if (next.opcode == Opcode::PutField && belongsToFrame(next)) {
            Opcode stored = m_function.at(next.child(1)).opcode;
            if (stored != Opcode::Integer && stored != Opcode::Unspecified && !isAlreadyAvailable(next.child(1)))
                return false;
            continue;
        }
        if (next.opcode != Opcode::CallBuiltin)
            return false;
        auto& implementation = m_builtins.at(next.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline || implementation.inlined != InlineBuiltin::SuspendAsyncFunction)
            return false;
        if (!isResumeClosure(m_function.at(next.arguments[0])) || !isAlreadyAvailable(next.arguments[1]))
            return false;
        return true;
    }
    return false;
}

void Lowering::lowerPutEnvironmentField(const Value& value)
{
    auto& field = m_program.at(value.layout()).fields[value.field()];
    CellPointer cell = in<CellPointer>(value.child(0));
    ValueID stored = value.child(1);
    if (Options::validateAOTTypedFacts() && !isLoweringInitializingStore()) [[unlikely]]
        validateIsNotReleased(cell, value);
    didEmitWrite(isLocalAllocation(m_function, value.child(0)) ? Writes::RuntimeState : Writes::Fields);
    Operand operand = in(stored, repOf(field.store));
    if (Options::validateAOTTypedFacts() && (field.singleFunctionIsProven || field.singleFunctionOrNullish || field.otherFunction)) [[unlikely]]
        validateSingleFunction(operand, stored, field.singleFunction, true, field.otherFunction);
    store(field.store, operand, fieldAt(cell, field));
    if (isLoweringInitializingStore())
        didLowerInitializingStore();
    else if (holdsCells(field.store) && typeOf(stored).hasCells() && m_function.at(stored).opcode != Opcode::Unspecified && !m_program.at(value.layout()).isStackAllocated) {
        m_environmentAwaitingBarrier = isFollowedByStoreWithBarrier(value.child(0)) || defersBarrierToPark(value.child(0)) ? value.child(0) : ValueID();
        if (!m_environmentAwaitingBarrier)
            barrier(cell);
    }
    auto& layout = m_program.at(value.layout());
    if (!layout.reuseVariable || value.field() + 1 != layout.fields.size())
        return;
    auto& hasEscaped = layout.fields[value.field() - 1];
    auto& variable = m_program.variables[layout.reuseVariable.index()];
    RELEASE_ASSERT(hasEscaped.store == Store::Boolean);
    Operand released = in(value.child(0), repOf(variable.store));
    LBasicBlock release = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(load(hasEscaped.store, fieldAt(cell, hasEscaped), 0, cell.value()).value, unsure(continuation), unsure(release));
    m_out.appendTo(release);
    for (unsigned i = 0; i + 2 < layout.fields.size(); ++i) {
        auto& environmentVariableID = layout.fields[i];
        if (!holdsCells(environmentVariableID.store) || environmentVariableID.isOutOfLine || environmentVariableID.flags & fieldIsNeverStored)
            continue;
        Operand nothing;
        if (m_program.at(environmentVariableID.stored).primitives & Primitive::Undefined)
            nothing = convert(undefined(), Type::fromPrimitives(Primitive::Undefined), repOf(environmentVariableID.store));
        else if (repOf(environmentVariableID.store) == Rep::Cell)
            nothing = CellPointer(m_out.int64Zero);
        else if (repOf(environmentVariableID.store) == Rep::JSValue)
            nothing = Boxed(m_out.int64Zero);
        else
            continue;
        store(environmentVariableID.store, nothing, fieldAt(cell, environmentVariableID));
    }
    didEmitWrite(Writes::RuntimeState);
    if (!Options::validateAOTTypedFacts()) [[likely]] {
        TypedPointer place = addressOf(layout.reuseVariable);
        store(variable.store, released, TypedPointer(m_heaps.AOTTyped_runtimeState, place.base(), place.offset()));
    }
    if (variable.hasCard && holdsCells(variable.store) && !Options::validateAOTTypedFacts())
        m_out.store32As8(m_out.constInt32(Instance::typedCardIsDirty), cardOf(variable.offset));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
}

CellPointer Lowering::lowerNewPromise()
{
    static_assert(static_cast<unsigned>(Stub::TypedNewPromise) + 1 == static_cast<unsigned>(Stub::LeafTypedNewPromise));
    if (!m_isInLoop && !m_program.isHotOutsideLoops(m_id))
        return CellPointer(callAllocationStub(Stub::TypedNewPromise, { }, effectsForOperation<operationAOTTypedNewPromise>(), false));
    LBasicBlock slowPath = newColdBlock();
    LBasicBlock continuation = m_out.newBlock();
    LValue promise = newPromise(slowPath);
    applyEffects(promise->as<PatchpointValue>(), { Writes::RuntimeState, false });
    ValueFromBlock fast = m_out.anchor(promise);
    m_out.jump(continuation);
    m_out.appendTo(slowPath);
    ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(NewPromise).value());
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    return CellPointer(m_out.phi(Int64, fast, slow));
}

bool Lowering::awaitsBefore(const Block& block, size_t end) const
{
    for (size_t i = 0; i < end; ++i) {
        auto& value = m_function.at(block.values[i]);
        if (value.opcode != Opcode::CallBuiltin)
            continue;
        auto& implementation = m_builtins.at(value.builtin()).implementation;
        if (implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::AwaitBySavingFrame)
            return true;
    }
    return false;
}

void Lowering::findBlocksAroundAwaits()
{
    if (!m_blocksReachedWithoutAwaiting.isEmpty())
        return;
    m_blocksReachedWithoutAwaiting.ensureSize(m_function.blocks.size());
    m_blocksReachedAfterAwait.ensureSize(m_function.blocks.size());
    auto awaits = [&](const Block& block) {
        return awaitsBefore(block, block.values.size());
    };
    Vector<unsigned, 16> worklist { 0u };
    Vector<unsigned, 16> behind;
    m_blocksReachedWithoutAwaiting.set(0);
    while (!worklist.isEmpty()) {
        auto& block = m_function.blocks[worklist.takeLast()];
        bool leaves = awaits(block);
        for (unsigned i = 0; i < block.terminator.targets.size(); ++i) {
            unsigned next = block.terminator.targets[i].block.index();
            if (leaves)
                behind.append(next);
            if ((!leaves || i) && !m_blocksReachedWithoutAwaiting.set(next))
                worklist.append(next);
        }
    }
    while (!behind.isEmpty()) {
        unsigned b = behind.takeLast();
        if (m_blocksReachedAfterAwait.set(b))
            continue;
        for (auto& target : m_function.blocks[b].terminator.targets)
            behind.append(target.block.index());
    }
}

bool Lowering::isReachedWithoutAwaiting()
{
    findBlocksAroundAwaits();
    return m_blocksReachedWithoutAwaiting.get(m_currentBlockID.index()) && !awaitsBefore(*m_currentBlock, m_currentIndexInBlock);
}

bool Lowering::isPromiseOfUnsuspendedFunction(ValueID promise) const
{
    if (m_function.savesFrameOnSuspend && (m_blocksReachedWithoutAwaiting.isEmpty() || m_blocksReachedAfterAwait.get(m_currentBlockID.index()) || awaitsBefore(*m_currentBlock, m_currentIndexInBlock)))
        return false;
    if (m_function.mayRunWithoutPromise)
        return m_function.blocks[0].parameters.last() == promise;
    if (m_function.startFunction) {
        bool isReturned = false;
        for (auto& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Return)
                continue;
            ValueID returned = block.terminator.value;
            while (returned && m_function.at(returned).opcode == Opcode::Cast)
                returned = m_function.at(returned).child(0);
            if (returned != promise)
                return false;
            isReturned = true;
        }
        return isReturned;
    }
    if (!m_id.index() || m_function.blocks[0].parameters.isEmpty() || m_function.blocks[0].parameters.last() != promise)
        return false;
    auto& resume = m_program.at(FunctionID(m_id.index() - 1));
    if (!m_program.isResumeFunction(resume))
        return false;
    auto& frame = m_program.at(m_program.at(resume.captureTypes[0]).singleLayout());
    return m_program.at(frame.fields[CoroutineFrame::promiseField].type).singleEngineClass() == &m_builtins.promiseClass();
}

Boxed Lowering::lowerResolvePromise(const Value& value)
{
    CellPointer promise = in<CellPointer>(value.arguments[0]);
    Boxed resolution = in<Boxed>(value.arguments[1]);
    OperationEffects effects = builtinCallEffects(value);
    auto& source = m_function.at(value.arguments[0]);
    bool mayHaveNoPromise = m_function.mayRunWithoutPromise && m_function.blocks[0].parameters.last() == value.arguments[0];
    auto resolveByStub = [&] {
        Vector<std::pair<LValue, GPRReg>, 4> operands { { promise.value(), GPRInfo::argumentGPR1 }, { resolution.value(), GPRInfo::argumentGPR2 } };
        if (mayHaveNoPromise)
            return Boxed(callICStub(Stub::ResolvePromiseOrReturnResult, Int64, operands, std::nullopt, effects));
        callICStub(Stub::ResolvePromiseAfterAwait, Void, operands, std::nullopt, effects);
        return undefined();
    };
    if (source.opcode == Opcode::GetField && m_program.at(source.layout()).isCoroutineFrame && !source.prediction())
        return resolveByStub();
    if (m_function.savesFrameOnSuspend && (!isReachedWithoutAwaiting() || (prefersGenericPathInColdCode() && !isPromiseOfUnsuspendedFunction(value.arguments[0]))))
        return resolveByStub();
    auto& type = typeOf(value.arguments[1]);
    bool isFresh = isPromiseOfUnsuspendedFunction(value.arguments[0]);
    bool mayBePrimitiveCell = type.primitives & (Primitive::String | Primitive::Symbol | Primitive::BigInt);
    bool isNotThenableWithoutExtraProperties = this->isNotThenableWithoutExtraProperties(type);
    if ((isFresh && type.isObject() && !isNotThenableWithoutExtraProperties) || (mayHaveNoPromise && !isFresh))
        return resolveByStub();
    bool mayBecomeOutOfLine = !isFresh || type.hasObjects();
    LBasicBlock inlinePath = m_out.newBlock();
    LBasicBlock outOfLine = mayBecomeOutOfLine ? m_out.newBlock() : nullptr;
    LBasicBlock continuation = m_out.newBlock();
    if (!isFresh) {
        LBasicBlock isUntouched = m_out.newBlock();
        m_out.branch(isZero(loadField<Integer>(promise, m_heaps.JSPromise_packed)).value(), unsure(isUntouched), unsure(outOfLine));
        m_out.appendTo(isUntouched);
    }
    if (type.hasObjects()) {
        LBasicBlock isAnyCell = m_out.newBlock();
        m_out.branch(isCell(resolution, type).value(), unsure(isAnyCell), unsure(inlinePath));
        m_out.appendTo(isAnyCell);
        LBasicBlock isObjectCase = isNotThenableWithoutExtraProperties ? m_out.newBlock() : outOfLine;
        if (mayBePrimitiveCell)
            m_out.branch(below(loadCellType(asCell(resolution)), word32(ObjectType)).value(), unsure(inlinePath), unsure(isObjectCase));
        else
            m_out.jump(isObjectCase);
        if (isNotThenableWithoutExtraProperties) {
            m_out.appendTo(isObjectCase);
            m_out.branch(hasNoBits(orderWordOf(asCell(resolution)), orderWordHasExtraProperties).value(), usually(inlinePath), rarely(outOfLine));
        }
    } else
        m_out.jump(inlinePath);
    m_out.appendTo(inlinePath);
    Vector<ValueFromBlock, 3> returned;
    if (mayHaveNoPromise) {
        LBasicBlock hasPromise = m_out.newBlock();
        returned.append(m_out.anchor(resolution.value()));
        m_out.branch(m_out.notZero64(promise.value()), unsure(hasPromise), unsure(continuation));
        m_out.appendTo(hasPromise);
        returned.append(m_out.anchor(promise.value()));
    }
    didEmitWrite(effects.writes);
    static_assert(CompactPointerTuple<JSCell*, uint16_t>::maxNumberOfBitsInPointer == 48);
    storeField(promise, m_heaps.JSPromise_slot, resolution);
    storeField(promise, m_heaps.JSPromise_packed, integer(static_cast<int64_t>(static_cast<uint64_t>(JSPromise::Status::Fulfilled) | JSPromise::isFirstResolvingFunctionCalledFlag) << 48));
    if (mayBePrimitiveCell || isNotThenableWithoutExtraProperties)
        barrier(promise);
    m_out.jump(continuation);
    if (mayBecomeOutOfLine) {
        m_out.appendTo(outOfLine);
        Boxed result = resolveByStub();
        if (mayHaveNoPromise)
            returned.append(m_out.anchor(result.value()));
        m_out.jump(continuation);
    }
    m_out.appendTo(continuation);
    if (!mayHaveNoPromise)
        return undefined();
    LValue phi = m_out.phi(Int64);
    for (ValueFromBlock value : returned)
        m_out.addIncomingToPhi(phi, value);
    return Boxed(phi);
}

Operand Lowering::lowerLanguageOperation(ValueID id, const Value& value)
{
    auto& builtin = m_builtins.at(value.builtin());
    RELEASE_ASSERT(builtin.implementation.kind == BuiltinImplementation::Operation);
    if (builtin.implementation.operation == LanguageOperation::ConstructWith)
        return lowerConstructWith(id, value);
    if (builtin.implementation.operation == LanguageOperation::Apply || builtin.implementation.operation == LanguageOperation::CallWithSpread)
        return lowerApply(value.arguments[0], value.arguments[1], value.arguments[2], typeOf(id), builtin.implementation.operation);
    auto& type = typeOf(id);
    auto cellOf = [&](ValueID operand) {
        Operand lowered = get(operand);
        return lowered.rep == Rep::JSValue ? asCell(lowered.as<Boxed>()) : lowered.as<CellPointer>();
    };
    auto returnedValue = [&](Boxed result) {
        return guaranteesResultType(m_program, m_function, value) ? convert(result, type, value.rep) : ensure(result, type);
    };
    switch (builtin.implementation.operation) {
    case LanguageOperation::CreateArguments:
        m_context.emitsCalls = true;
        return returnedValue(Boxed(callAllocationStub(Stub::CreateArguments, { { cellOf(value.arguments[0]).value(), GPRInfo::toArgumentRegister(1) } }, builtinCallEffects(value), true)));
    case LanguageOperation::NewGenerator:
    case LanguageOperation::NewAsyncGenerator: {
        bool isAsync = builtin.implementation.operation == LanguageOperation::NewAsyncGenerator;
        auto& closure = m_function.at(value.arguments[0]);
        uint32_t index = !isAsync && value.arguments.size() == 1 && closure.opcode == Opcode::NewClosure ? m_program.at(closure.function()).generatorTemplate : UINT32_MAX;
        auto inOperation = [&] {
            return AOT_TYPED_OPERATION_WITH_EFFECTS(NewGenerator, builtinCallEffects(value), cellOf(value.arguments[0]), value.arguments.size() > 1 ? in<Boxed>(value.arguments[1]) : undefined(), word32(isAsync), word32(static_cast<int32_t>(index)), word32(index == UINT32_MAX ? 0 : static_cast<int32_t>(m_program.numberOfGeneratorTemplates)));
        };
        if (index == UINT32_MAX || Options::validateAOTTypedFacts() || Options::countAOTTypedAllocations())
            return returnedValue(box(inOperation()));
        auto at = [&](LValue base, ptrdiff_t offset) {
            return TypedPointer(m_heaps.root, m_out.add(base, m_out.constIntPtr(offset)));
        };
        CellPointer resumeClosure = cellOf(value.arguments[0]);
        LBasicBlock hasTemplates = m_out.newBlock();
        LBasicBlock hasTemplate = m_out.newBlock();
        LBasicBlock slowPath = newColdBlock();
        Join join(m_out, Rep::Cell, Int64);
        LValue templates = m_out.loadPtr(at(m_instance, Instance::offsetOfGeneratorTemplates()));
        m_out.branch(m_out.notNull(templates), usually(hasTemplates), rarely(slowPath));
        m_out.appendTo(hasTemplates);
        LValue header = m_out.load64(at(templates, index * sizeof(Generator::Template) + OBJECT_OFFSETOF(Generator::Template, header)));
        m_out.branch(m_out.notZero64(header), usually(hasTemplate), rarely(slowPath));
        m_out.appendTo(hasTemplate);
        LValue allocator = m_out.loadPtr(at(fixedPointer(Instance::offsetOfCellSpace()), CompleteSubspace::offsetOfAllocatorForSizeStep() + MarkedSpace::sizeClassToIndex(sizeof(Generator)) * sizeof(Allocator)));
        Emitter::orElse(m_out.notNull(allocator), slowPath);
        CellPointer created = allocateCell(allocator, sizeof(Generator), slowPath);
        m_out.store64(header, created.value(), m_heaps.JSCell_header);
        m_out.storePtr(m_out.intPtrZero, created.value(), m_heaps.JSObject_butterfly);
        m_out.storePtr(resumeClosure.value(), at(created.value(), Generator::offsetOfResumeClosure()));
        m_out.store64(m_out.load64(at(templates, index * sizeof(Generator::Template) + OBJECT_OFFSETOF(Generator::Template, offsetOfStateAndFlags))), at(created.value(), Generator::offsetOfOffsetOfState()));
        fenceAfterInitialization();
        join.add(created);
        m_out.appendTo(slowPath);
        join.add(inOperation());
        return returnedValue(box(join.finish().as<CellPointer>()));
    }
    case LanguageOperation::GetAsyncIterator:
        return returnedValue(AOT_TYPED_OPERATION_WITH_EFFECTS(GetAsyncIterator, builtinCallEffects(value), in<Boxed>(value.arguments[0])));
    case LanguageOperation::DefineField:
        if (auto* literal = spreadLiteralAt(m_currentIndexInBlock)) {
            if (m_currentIndexInBlock == literal->members.last())
                lowerSpreadLiteral(*literal);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (m_spreadGroup && m_currentIndexInBlock > m_spreadGroup->first && m_currentIndexInBlock < m_spreadGroup->cast)
            return placeholder(value.rep);
        if (m_cloneDefinitionsBlock == m_currentBlock && m_cloneDefinitions.contains(m_currentIndexInBlock))
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (m_currentIndexInBlock == literal->fields.last() && !tryDeferObjectLiteral(*literal))
                lowerObjectLiteral(*literal);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (auto* run = propertyRunAt(m_currentIndexInBlock)) {
            if (m_currentIndexInBlock == run->stores.last().index)
                lowerPropertyRun(*run);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (auto& name = m_function.at(value.arguments[1]); name.opcode == Opcode::String && emitDefineByName(in<Boxed>(value.arguments[0]), name.string(), in<Boxed>(value.arguments[2])))
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        if (value.arguments.size() == 3 && m_function.at(value.arguments[1]).opcode != Opcode::String) {
            callICStub(Stub::DefineByKeyCached, Void, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[2]).value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        break;
    case LanguageOperation::DefineGetter:
    case LanguageOperation::DefineSetter:
        if (auto* group = accessorGroupAt(m_currentIndexInBlock)) {
            lowerMemberOfAccessorGroup(*group, value);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (value.arguments.size() == 4 && m_function.at(value.arguments[0]).opcode == Opcode::NewObject && m_function.at(value.arguments[1]).opcode == Opcode::String && m_function.at(value.arguments[3]).opcode == Opcode::Boolean && m_function.at(value.arguments[3]).boolean()) {
            Stub stub = builtin.implementation.operation == LanguageOperation::DefineGetter ? Stub::DefineGetterCached : Stub::DefineSetterCached;
            callICStub(stub, Void, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[2]).value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        break;
    case LanguageOperation::SuperConstruct:
        if (value.arguments.size() == 3 || value.arguments.size() == 4)
            return returnedValue(AOT_TYPED_OPERATION_WITH_EFFECTS(ConstructSuper, builtinCallEffects(value), in<Boxed>(value.arguments[0]), in<Boxed>(value.arguments[1]), in<Boxed>(value.arguments[2]), value.arguments.size() == 4 ? in<Boxed>(value.arguments[3]) : Boxed(m_out.int64Zero)));
        break;
    case LanguageOperation::GetNewTarget:
        return ensure(lowerGetNewTarget(), type);
    case LanguageOperation::GetMappedParameter:
        if (auto& number = m_function.at(value.arguments[1]); number.opcode == Opcode::Integer)
            return ensure(Boxed(m_out.load64(asCell(in<Boxed>(value.arguments[0])).value(), m_heaps.DirectArguments_storage[number.integer()])), type);
        break;
    case LanguageOperation::PutMappedParameter:
        if (auto& number = m_function.at(value.arguments[1]); number.opcode == Opcode::Integer) {
            CellPointer object = asCell(in<Boxed>(value.arguments[0]));
            didEmitWrite(Writes::Heap);
            m_out.store64(in<Boxed>(value.arguments[2]).value(), object.value(), m_heaps.DirectArguments_storage[number.integer()]);
            barrier(object);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        break;
    case LanguageOperation::PrivateGet:
    case LanguageOperation::PrivateHas: {
        if (builtin.implementation.operation == LanguageOperation::PrivateGet) {
            if (auto read = lowerPrivateAccessAtSite(value, false))
                return ensure(*read, type);
        }
        Stub stub = builtin.implementation.operation == LanguageOperation::PrivateGet ? Stub::PrivateGet : Stub::PrivateHas;
        return ensure(Boxed(callICStub(stub, Int64, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 } }, std::nullopt, builtinCallEffects(value))), type);
    }
    case LanguageOperation::PrivateMethodsAdd:
        callICStub(Stub::PrivateMethodsAdd, Void, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 } }, std::nullopt, builtinCallEffects(value));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case LanguageOperation::PrivateSet:
    case LanguageOperation::PrivateFieldAdd: {
        if (auto* run = builtin.implementation.operation == LanguageOperation::PrivateFieldAdd ? propertyRunAt(m_currentIndexInBlock) : nullptr) {
            if (m_currentIndexInBlock == run->stores.last().index)
                lowerPropertyRun(*run);
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        }
        if (builtin.implementation.operation == LanguageOperation::PrivateSet && lowerPrivateAccessAtSite(value, true))
            return ensure(undefined(), m_program.at(m_program.types.undefined), type);
        Stub stub = builtin.implementation.operation == LanguageOperation::PrivateSet ? Stub::PrivateSet : Stub::PrivateFieldAdd;
        callICStub(stub, Void, { { in<Boxed>(value.arguments[0]).value(), firstStubOperandGPR }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[2]).value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    case LanguageOperation::CloneObjectAfter:
        if (value.arguments.size() == 1)
            return ensure(finishObjectLiteral(in<CellPointer>(value.arguments[0])), m_program.at(m_program.types.object), type);
        break;
    case LanguageOperation::CloneObject:
        if (!m_isLoweringMergeGroup) {
            if (auto group = findMergeGroup(m_currentIndexInBlock)) {
                m_mergeGroup = group;
                return placeholder(value.rep);
            }
        }
        if (auto group = findCloneGroup(m_currentIndexInBlock)) {
            m_cloneGroup = WTF::move(group);
            if (m_cloneGroup->isDecidedAtClone)
                decideCloneGroup(*m_cloneGroup);
            return placeholder(value.rep);
        }
        if (auto target = typedCloneTargetAt(m_currentIndexInBlock))
            return ensure(AOT_TYPED_OPERATION(CloneObjectAs, in<Boxed>(value.arguments[0]), integer(static_cast<int64_t>(*target))), m_program.at(m_program.types.object), type);
        {
            auto hasRecordLayouts = [](const Type& ofCopy) {
                return !ofCopy.anyObject && !ofCopy.objectLayouts.isEmpty();
            };
            auto expected = predictionOf(id);
            bool keepsLayout = hasRecordLayouts(type) || (expected && hasRecordLayouts(*expected));
            bool takesStores = !keepsLayout && !m_isLoweringMergeGroup;
            return ensure(countPlainAllocation(PlainAllocationKind::Clone, id, cloneObject(value.arguments[0], keepsLayout, takesStores ? storesDirectlyBehindClone(id) : CloneDefinitions { 0, { } }, cloneCapacityOf(id))), m_program.at(m_program.types.object), type);
        }
    case LanguageOperation::CopyDataProperties:
        if (auto* literal = spreadLiteralAt(m_currentIndexInBlock)) {
            if (m_currentIndexInBlock == literal->members.last())
                lowerSpreadLiteral(*literal);
            return placeholder(value.rep);
        }
        if (m_mergeGroup && m_mergeGroup->first + 1 == m_currentIndexInBlock)
            return placeholder(value.rep);
        if (!m_isLoweringMergeGroup && isAllocatedHere(value.arguments[0])) {
            if (auto group = findMergeGroup(m_currentIndexInBlock)) {
                m_mergeGroup = group;
                return placeholder(value.rep);
            }
            if (auto group = findSpreadGroup(m_currentIndexInBlock)) {
                m_spreadGroup = WTF::move(group);
                return placeholder(value.rep);
            }
        }
        if (isAllocatedHere(value.arguments[0])) {
            CellPointer object = countPlainAllocation(PlainAllocationKind::Clone, value.arguments[0], cloneObject(value.arguments[1], false, definitionsOnClone(value.arguments[0], true)));
            defineDeferredAllocation(value.arguments[0], object);
            return ensure(object, m_program.at(m_program.types.object), type);
        }
        AOT_TYPED_OPERATION_WITH_EFFECTS(CopyDataProperties, builtinCallEffects(value), cellOf(value.arguments[0]), in<Boxed>(value.arguments[1]));
        return returnedValue(in<Boxed>(value.arguments[0]));
    case LanguageOperation::NewPromise:
        return ensure(lowerNewPromise(), Type::engineClass(m_builtins.promiseClass()), type);
    case LanguageOperation::ResolvePromise:
        if (m_function.mayRunWithoutPromise && m_function.blocks[0].parameters.last() == value.arguments[0])
            return ensure(lowerResolvePromise(value), type);
        lowerResolvePromise(value);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    case LanguageOperation::Await: {
        auto& closure = m_function.at(value.arguments[0]);
        bool isAsyncFunction = closure.opcode == Opcode::GetField && m_program.at(closure.layout()).isCoroutineFrame
            && m_program.at(m_program.at(closure.layout()).fields[CoroutineFrame::promiseField].type).singleEngineClass() == &m_builtins.promiseClass();
        if (isAsyncFunction)
            AOT_TYPED_OPERATION_WITH_EFFECTS(Await, builtinCallEffects(value), in<CellPointer>(value.arguments[0]), in<Boxed>(value.arguments[1]), boolean(true));
        else
            callICStub(Stub::AwaitInAsyncGenerator, Void, { { in<CellPointer>(value.arguments[0]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    default:
        break;
    }
    if (builtin.implementation.operation == LanguageOperation::MakeModuleNamespace) {
        Vector<StringID, 32> names;
        unsigned end = 1;
        for (; end < value.arguments.size() && m_function.at(value.arguments[end]).opcode == Opcode::String && isLazilyFetched(value.arguments[end]); ++end)
            names.append(m_function.at(value.arguments[end]).string());
        bool restAreNotNames = true;
        Arguments others;
        others.append(value.arguments[0]);
        for (unsigned i = end; i < value.arguments.size(); ++i) {
            restAreNotNames &= m_function.at(value.arguments[i]).opcode != Opcode::String;
            others.append(value.arguments[i]);
        }
        if (!names.isEmpty() && restAreNotNames) {
            uint32_t row = rowOfNames(names.span());
            auto [count, list] = argumentBuffer(others);
            return returnedValue(AOT_TYPED_OPERATION_WITH_EFFECTS(MakeModuleNamespace, builtinCallEffects(value), word32(names.size()), word32(row), count, list));
        }
    }
    auto outOfLine = [&] {
        auto [count, list] = argumentBuffer(value.arguments);
        Boxed result = AOT_TYPED_OPERATION_WITH_EFFECTS(LanguageOperation, builtinCallEffects(value), word32(static_cast<int32_t>(builtin.implementation.operation)), count, list);
        return guaranteesResultType(m_program, m_function, value) ? convert(result, type, value.rep) : ensure(result, type);
    };
    if (builtin.implementation.operation == LanguageOperation::UnlistFields || (builtin.implementation.operation == LanguageOperation::ListFields && value.arguments.size() == 6)) {
        bool unlists = builtin.implementation.operation == LanguageOperation::UnlistFields;
        auto& receiver = typeOf(value.arguments[0]);
        uint32_t lastRequestedEnd = 0;
        auto initializedFieldCountBefore = [&](ValueID name, ValueID includesName) -> std::optional<uint32_t> {
            if ((m_function.at(name).opcode != Opcode::String && m_function.at(name).opcode != Opcode::ConstantSymbol) || (includesName && m_function.at(includesName).opcode != Opcode::Boolean))
                return std::nullopt;
            std::optional<uint32_t> agreed;
            for (LayoutID supertype : receiver.objectLayouts) {
                for (LayoutID subtype : m_program.at(supertype).subtypes) {
                    auto& layout = m_program.at(subtype);
                    auto named = layout.fieldNamed(m_function.at(name).string());
                    if (!isObjectLayout(layout.kind) || !named)
                        return std::nullopt;
                    uint32_t endInLayout = *named + (includesName && m_function.at(includesName).boolean());
                    uint32_t end = 0;
                    uint32_t count = 0;
                    for (uint32_t index = 0; index < endInLayout; ++index) {
                        end += !(layout.fields[index].flags & fieldIsNeverStored);
                        count += !(layout.fields[index].flags & (fieldIsAbsentAtCreation | fieldIsNeverStored));
                    }
                    if (count > maxFieldsInOrder || (agreed && (*agreed != count || lastRequestedEnd != end)))
                        return std::nullopt;
                    agreed = count;
                    lastRequestedEnd = end;
                }
            }
            return agreed;
        };
        bool hasSingleLayout = receiver.objectLayouts.size() == 1 && !receiver.anyObject && !receiver.anyFunction && receiver.engineClasses.isEmpty();
        bool mayBeUntypedObject = hasSingleLayout && m_program.at(receiver.objectLayouts[0]).acceptsUntypedObjects && !isAllocatedAsSubtypeOf(value.arguments[0], receiver.objectLayouts[0]);
        auto listed = hasSingleLayout && !(mayBeUntypedObject && m_program.at(receiver.objectLayouts[0]).test.neverPasses()) ? initializedFieldCountBefore(value.arguments[1], unlists ? ValueID() : value.arguments[2]) : std::nullopt;
        uint32_t end = lastRequestedEnd;
        auto listedBefore = listed && !unlists ? initializedFieldCountBefore(value.arguments[3], value.arguments[4]) : std::nullopt;
        if (listed && (unlists || listedBefore)) {
            static constexpr uint32_t count = maxFieldsInOrder << 16;
            LBasicBlock genericPath = newColdBlock();
            Join join(m_out, value.rep, b3TypeOf(value.rep));
            CellPointer cell = cellOf(value.arguments[0]);
            if (mayBeUntypedObject)
                orElse(passes(shapeWordOf(cell), m_program.at(receiver.objectLayouts[0]).test), genericPath);
            Word32 word = orderWordOf(cell);
            Word32 rest = bitAnd(unlists ? word : low32(in<Integer>(value.arguments[5])), word32(~count));
            if (unlists)
                orElse(hasNoBits(word, orderWordHasExtraProperties | orderWordIsNotExtensible), genericPath);
            else
                orElse(logicalAnd(hasNoBits(word, orderWordHasExtraProperties), equal(word, bitOr(rest, word32(*listedBefore << 16)))), genericPath);
            if (unlists || *listed != *listedBefore) {
                didEmitWrite(Writes::Fields);
                storeField(cell, m_heaps.AOTTypedObject_orderWord, bitOr(rest, word32(*listed << 16)));
            }
            join.add(unlists ? ensure(signExtended(word), m_program.at(m_program.types.int32), type) : ensure(undefined(), m_program.at(m_program.types.undefined), type));
            m_out.appendTo(genericPath);
            Word32 found = AOT_TYPED_OPERATION(ListFieldsBelow, cell, word32(end | (unlists ? 0 : listsFieldsWithoutUnlisting)));
            join.add(unlists ? ensure(signExtended(found), m_program.at(m_program.types.int32), type) : ensure(undefined(), m_program.at(m_program.types.undefined), type));
            return join.finish();
        }
    }
    if (builtin.implementation.operation == LanguageOperation::SuperGet && value.arguments.size() == 4) {
        auto& key = m_function.at(value.arguments[1]);
        auto& startsAtHomeObject = m_function.at(value.arguments[3]);
        auto prototype = m_program.find(asBytes("prototype"_s.span8()));
        auto number = key.opcode == Opcode::String && startsAtHomeObject.opcode == Opcode::Boolean && (startsAtHomeObject.boolean() || prototype) ? tableIndexOfName(key.string()) : std::nullopt;
        if (number) {
            LBasicBlock isNotInTable = newColdBlock();
            Join join(m_out, value.rep, b3TypeOf(value.rep));
            Boxed home = in<Boxed>(value.arguments[0]);
            if (!startsAtHomeObject.boolean())
                home = emitStandaloneCachedGetById(home, *prototype);
            Boxed found(callICStub(Stub::GetParentDataByName, Int64, { { home.value(), firstStubOperandGPR } }, std::pair { GPRInfo::argumentGPR1, *number }, { Writes::Nothing, false }));
            orElse(isNotEmpty(found), isNotInTable);
            join.add(returnedValue(found));
            m_out.appendTo(isNotInTable);
            join.add(returnedValue(AOT_TYPED_OPERATION_WITH_EFFECTS(GetSuperPropertyByName, builtinCallEffects(value), home, in<Boxed>(value.arguments[2]), word32(*number))));
            return join.finish();
        }
    }
    if (builtin.implementation.operation == LanguageOperation::RequireIteratorResultObject) {
        Boxed step = in<Boxed>(value.arguments[0]);
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isNotObject = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        m_out.branch(isCell(step).value(), usually(isCellCase), rarely(isNotObject));
        m_out.appendTo(isCellCase);
        m_out.branch(below(loadCellType(asCell(step)), word32(ObjectType)).value(), rarely(isNotObject), usually(continuation));
        m_out.appendTo(isNotObject);
        outOfLine();
        m_out.unreachable();
        m_out.appendTo(continuation);
        return ensure(undefined(), m_program.at(m_program.types.undefined), type);
    }
    if (builtin.implementation.operation == LanguageOperation::CopyDataPropertiesExcluding || builtin.implementation.operation == LanguageOperation::CloneObjectWith || builtin.implementation.operation == LanguageOperation::CloneObjectAfter) {
        if (builtin.implementation.operation == LanguageOperation::CloneObjectWith && m_isLoweringMergeGroup)
            return outOfLine();
        if (builtin.implementation.operation == LanguageOperation::CloneObjectWith) {
            if (auto group = findSpreadGroupAfterClone(m_currentIndexInBlock)) {
                m_spreadGroup = WTF::move(group);
                return placeholder(value.rep);
            }
        }
        auto expected = predictionOf(id);
        LayoutID supertype = expected ? expected->singleLayout() : LayoutID();
        if (supertype && m_program.at(supertype).kind == LayoutKind::Record && m_program.at(supertype).shape && !Options::auditAOTTypedAssertions()) {
            auto& inBlock = m_currentBlock->values;
            auto& ofSupertype = m_program.at(m_program.at(supertype).type);
            size_t castIndex = m_currentIndexInBlock + 1;
            bool fusesCast = !m_isLoweringMergeGroup && !handlerEnteredByBranch() && castIndex < inBlock.size() && useCountOf(id) == 1 && m_function.at(inBlock[castIndex]).opcode == Opcode::Cast
                && m_function.at(inBlock[castIndex]).castReason() == CastReason::As && m_function.at(inBlock[castIndex]).child(0) == id && m_program.castIsRedundant(ofSupertype, typeOf(inBlock[castIndex]), m_function.at(inBlock[castIndex]));
            Rep rep = fusesCast ? m_function.at(inBlock[castIndex]).rep : value.rep;
            LBasicBlock isNotCreated = newColdBlock();
            Join join(m_out, rep, b3TypeOf(rep));
            CellPointer created = copyRecordAs(value, supertype, builtin.implementation.operation == LanguageOperation::CopyDataPropertiesExcluding ? CopyNamesKind::Excluded : builtin.implementation.operation == LanguageOperation::CloneObjectWith ? CopyNamesKind::StoredNext : CopyNamesKind::StoredFirst, nullptr, false, isNotCreated);
            join.add(fusesCast ? convert(Operand(created), ofSupertype, rep) : ensure(created, m_program.at(m_program.types.object), type));
            m_out.appendTo(isNotCreated);
            auto finish = [&](Operand made) {
                if (!fusesCast) {
                    join.add(made);
                    return join.finish();
                }
                define(id, made);
                SetForScope indexInBlock(m_currentIndexInBlock, castIndex);
                SetForScope isOnConditionalPath(m_isOnConditionalPath, true);
                join.add(lower(inBlock[castIndex], m_function.at(inBlock[castIndex])));
                m_castOfCopiedRecord = { m_currentBlock, castIndex, join.finish() };
                return placeholder(value.rep);
            };
            Vector<StringID, 4> names;
            bool isLiteral = builtin.implementation.operation == LanguageOperation::CloneObjectAfter && value.arguments.size() % 2;
            for (size_t i = 1; i < value.arguments.size() && isLiteral; i += 2) {
                auto& name = m_function.at(value.arguments[i]);
                isLiteral = name.opcode == Opcode::String && !names.contains(name.string()) && !m_program.string(name.string()).empty() && !isASCIIDigit(m_program.string(name.string())[0]);
                if (isLiteral)
                    names.append(name.string());
            }
            if (!isLiteral)
                return finish(outOfLine());
            Vector<Boxed, 4> values;
            for (size_t i = 2; i < value.arguments.size(); i += 2)
                values.append(in<Boxed>(value.arguments[i]));
            LValue inBuffer[] = { box(countPlainAllocation(PlainAllocationKind::Object, id, newObjectLiteral(names.span(), values.span()))).value(), in<Boxed>(value.arguments[0]).value() };
            LValue buffer = transientBuffer(std::size(inBuffer) * sizeof(EncodedJSValue));
            for (unsigned i = 0; i < std::size(inBuffer); ++i)
                m_out.store64(inBuffer[i], m_out.address(m_heaps.AOTTyped_argumentBuffer, buffer, i * sizeof(EncodedJSValue)));
            return finish(ensure(AOT_TYPED_OPERATION(LanguageOperation, word32(static_cast<int32_t>(LanguageOperation::CopyDataProperties)), word32(std::size(inBuffer)), Pointer(buffer)), type));
        }
    }
    if (builtin.implementation.operation == LanguageOperation::AppendSpread) {
        if (LBasicBlock slowPath = appendSpread(value)) {
            bool isDeferredToStub = std::exchange(m_spreadUsesStub, false);
            Join join(m_out, value.rep, b3TypeOf(value.rep));
            join.add(ensure(get(value.arguments[0]), typeOf(value.arguments[0]), type));
            m_out.appendTo(slowPath);
            if (isDeferredToStub) {
                Boxed appended(callICStub(Stub::AppendSpread, Int64, { { in<Boxed>(value.arguments[0]).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(value.arguments[1]).value(), GPRInfo::argumentGPR2 } }, std::pair { GPRInfo::argumentGPR3, numberOfElementsPushedTo(id) }));
                join.add(ensure(convert(appended, typeOf(value.arguments[0]), get(value.arguments[0]).rep), typeOf(value.arguments[0]), type));
            } else
                join.add(outOfLine());
            return join.finish();
        }
    }
    if (builtin.implementation.operation != LanguageOperation::GetFromArguments || !mayBeIndex(value.arguments[1]))
        return outOfLine();
    LBasicBlock slowPath = newColdBlock();
    Join join(m_out, value.rep, b3TypeOf(value.rep));
    join.add(getArrayElement(asCell(in<Boxed>(value.arguments[0])), value.arguments[1], type, slowPath));
    m_out.appendTo(slowPath);
    join.add(outOfLine());
    return join.finish();
}

Operand Lowering::lower(ValueID id, const Value& value)
{
    auto& type = typeOf(id);
    if (auto character = iteratedCharacterCast(value))
        return *character;
    if (auto part = arrayEntriesPart(id, value))
        return *part;
    if (auto deferred = isCompact() ? std::nullopt : deferToBranchOnLiteral(id, value))
        return *deferred;
    if (auto concatenated = lowerAsConcatenationPart(id, value))
        return *concatenated;
    switch (value.opcode) {
    case Opcode::Parameter:
        RELEASE_ASSERT_NOT_REACHED();
    case Opcode::Undefined:
        return convert(undefined(), type, value.rep);
    case Opcode::Null:
        return convert(boxedConstant(jsNull()), type, value.rep);
    case Opcode::Boolean:
        return convert(boolean(value.boolean()), type, value.rep);
    case Opcode::Integer:
        return convert(integer(value.integer()), type, value.rep);
    case Opcode::Double:
        return convert(number(std::bit_cast<double>(value.bits())), type, value.rep);
    case Opcode::String:
        if (value.rep == Rep::LiteralString)
            return literalStringConstant(value.string());
        if (isLazilyFetched(id))
            return Operand(value.rep, nullptr);
        return convert(stringConstant(value.string()), type, value.rep);
    case Opcode::BigInt:
        return convert(AOT_TYPED_OPERATION(BigIntConstant, word32(value.string().index())), type, value.rep);
    case Opcode::ConstantSymbol:
        return convert(AOT_TYPED_OPERATION(ConstantSymbol, word32(value.string().index())), type, value.rep);
    case Opcode::FunctionReference:
        if (isOnlyUsedByShapeInstanceOf(id))
            return placeholder(value.rep);
        return convert(closureConstant(value.function()), type, value.rep);
    case Opcode::NewClosure:
        if (isStoredAsLazyClosure(id) || isLazilyCreated(id))
            return placeholder(value.rep);
        if (auto* store = m_currentIndexInBlock + 1 == m_currentBlock->values.size() ? lazyClosureStoreAfterJump(*m_currentBlock) : nullptr) {
            putLazyClosure(in<Boxed>(store->child(0)), store->string(), value.function());
            return placeholder(value.rep);
        }
        if (isResumeClosureCreatedAtSuspend(m_currentIndexInBlock) || isCreatedWithPropertyRun(id))
            return placeholder(value.rep);
        return convert(lowerNewClosure(value), type, value.rep);
    case Opcode::NewClosureOnce:
        return convert(lowerNewClosureOnce(value), type, value.rep);
    case Opcode::GetArgument: {
        uint32_t index = value.immediates[0];
        return ensure(either<Boxed>(below(word32(index), m_argumentCount), [&] {
            return Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_argumentBuffer, m_argumentBuffer.value(), index * sizeof(EncodedJSValue))));
        }, [&] {
            return undefined();
        }), type);
    }
    case Opcode::CreateRest:
        return ensure(createRest(value.numberOfArgumentsToSkip()), Type::engineClass(m_builtins.arrayClass()), type);
    case Opcode::GetRestLength:
        return convert(restLength(value.numberOfArgumentsToSkip()), type, value.rep);
    case Opcode::GetMyArgumentByVal:
        return ensure(lowerGetMyArgumentByVal(value), type);
    case Opcode::CallForwardVarargs:
        return ensure(lowerCallForwardVarargs(value), type);
    case Opcode::CreateThis:
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (!literal->fields.isEmpty())
                return placeholder(value.rep);
            lowerObjectLiteral(*literal);
            return get(id);
        }
        return convert(countPlainAllocation(PlainAllocationKind::This, id, lowerCreateThis(value)), type, value.rep);
    case Opcode::ConstructSuper:
    case Opcode::ConstructSuperForwardVarargs:
        return lowerConstructSuper(id, value);
    case Opcode::GetCapture: {
        Operand closure = get(value.child(0));
        if (isCalledWithoutAllocation(value.child(0))) {
            Rep rep = repOf(m_program.at(m_program.at(value.function()).captureTypes[value.captureIndex()]));
            RELEASE_ASSERT(closure.rep == Rep::JSValue && (rep == Rep::Cell || rep == Rep::JSValue));
            return eitherOperand(isUnmaterializedClosure(closure.as<Boxed>()), rep, [&]() -> Operand {
                Operand holder = get(m_function.at(value.child(0)).child(0));
                CellPointer cell = holder.rep == Rep::JSValue ? asCell(holder.as<Boxed>()) : holder.as<CellPointer>();
                if (rep == Rep::Cell)
                    return cell;
                return box(cell);
            }, [&]() -> Operand {
                return closureCapture(asCell(closure.as<Boxed>()), m_program.at(value.function()), value.captureIndex());
            });
        }
        return closureCapture(closure.rep == Rep::JSValue ? asCell(closure.as<Boxed>()) : closure.as<CellPointer>(), m_program.at(value.function()), value.captureIndex());
    }
    case Opcode::GetModuleVariable:
        if (isKeyReadOnPropertyRunMiss(id))
            return placeholder(value.rep);
        return lowerGetModuleVariable(id, value);
    case Opcode::PutModuleVariable:
        lowerPutModuleVariable(value);
        return undefined();
    case Opcode::GetGlobal:
    case Opcode::GetGlobalForTypeOf:
        if (BuiltinName name = m_program.builtinName(value.string()); m_builtins.isGlobal(name) && !m_program.isExtended(name)) {
            if (isUsedOnlyByInlineInstanceOf(id))
                return placeholder(value.rep);
            return convert(builtinCell(m_builtins.cellOf(name)), type, value.rep);
        }
        return ensure(lowerGetGlobal(value), type);
    case Opcode::PutGlobal:
        lowerPutGlobal(value);
        return undefined();
    case Opcode::NewObjectWithShape:
        if (tryDeferLiteralAllocation(id))
            return Operand(value.rep, m_out.constInt64(0));
        return lowerNewObject(id, value);
    case Opcode::MaterializeEnvironment:
        return lowerMaterializeEnvironment(value);
    case Opcode::GetField:
        if (!isObjectLayout(m_program.at(value.layout()).kind))
            return lowerGetEnvironmentField(id, value);
        return lowerGetField(id, value);
    case Opcode::PutField:
        if (isDeferredLiteralStore(m_currentIndexInBlock))
            return undefined();
        if (m_currentIndexInBlock && isResumeClosureCreatedAtSuspend(m_currentIndexInBlock - 1))
            return undefined();
        if (!isObjectLayout(m_program.at(value.layout()).kind))
            lowerPutEnvironmentField(value);
        else
            lowerPutField(id, value);
        return undefined();
    case Opcode::GetById:
        if (value.methodGetByIdBuiltin())
            return lowerMethodGetById(value);
        if (auto property = lowerPropertyOfMatch(id, value))
            return *property;
        return lowerGetById(id, value);
    case Opcode::PutById:
        lowerPutById(id, value);
        return undefined();
    case Opcode::InById:
        return lowerInById(value);
    case Opcode::DeleteById:
        if (auto deleted = fieldFor(typeOf(value.child(0)), value.string()) ? std::optional<Word32>() : emitDeleteByVal(in<Boxed>(value.child(0)), keyConstant(value.string())))
            return *deleted;
        if (isInSloppyCode())
            return AOT_TYPED_OPERATION(DeleteByValSloppy, in<Boxed>(value.child(0)), keyConstant(value.string()));
        return AOT_TYPED_OPERATION(DeleteByName, in<Boxed>(value.child(0)), word32(value.string().index()));
    case Opcode::InByVal:
        if (!predictedTypeOf(value.child(1)).isNumber())
            return emitInByVal(in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
        if (Operand operand = get(value.child(0)); holdsCells(operand.rep)) {
            LBasicBlock genericPath = newUnlikelyBlock();
            LBasicBlock absent = m_out.newBlock();
            LBasicBlock isDouble = m_out.newBlock();
            LBasicBlock isNotDouble = m_out.newBlock();
            Join join(m_out, Rep::Int32, Int32);
            if (operand.rep == Rep::JSValue)
                orElse(isCell(operand.as<Boxed>()), genericPath);
            CellPointer array = operand.rep == Rep::Cell ? operand.as<CellPointer>() : asCell(operand.as<Boxed>());
            Integer index = indexOrElse(value.child(1), genericPath);
            Word32 shape = anyArrayIndexingMode(array, IndexingShapeMask);
            m_out.branch(equal(shape, word32(DoubleShape)).value(), unsure(isDouble), unsure(isNotDouble));
            m_out.appendTo(isDouble);
            Pointer doubles = butterflyOf(array);
            orElse(below(index, publicLength(doubles)), absent);
            Number asDouble(m_out.loadDouble(m_out.baseIndex(m_heaps.indexedDoubleProperties, doubles.value(), index.value())));
            orElse(equal(asDouble, asDouble), absent);
            join.add(boolean(true));
            m_out.appendTo(isNotDouble);
            orElse(notEqual(shape, word32(UndecidedShape)), absent);
            orElse(logicalOr(equal(shape, word32(ContiguousShape)), equal(shape, word32(Int32Shape))), genericPath);
            Pointer values = butterflyOf(array);
            orElse(below(index, publicLength(values)), absent);
            orElse(isNotEmpty(Boxed(m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, values.value(), index.value())))), absent);
            join.add(boolean(true));
            m_out.appendTo(absent);
            orElse(isPlainArray(array), genericPath);
            orElse(belowOrEqual(index, integer(MAX_ARRAY_INDEX)), genericPath);
            join.add(boolean(false));
            m_out.appendTo(genericPath);
            join.add(AOT_TYPED_OPERATION(HasKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1))));
            return join.finish();
        }
        return AOT_TYPED_OPERATION(HasKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::DeleteByVal:
        if (auto deleted = predictedTypeOf(value.child(1)).isNumber() ? std::optional<Word32>() : emitDeleteByVal(in<Boxed>(value.child(0)), in<Boxed>(value.child(1))))
            return *deleted;
        if (isInSloppyCode())
            return AOT_TYPED_OPERATION(DeleteByValSloppy, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
        return AOT_TYPED_OPERATION(DeleteByKey, in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::GetByVal: {
        Operand element = lowerGetByVal(id, value);
        if (std::exchange(m_needsPoll, false))
            poll();
        return element;
    }
    case Opcode::PutByVal:
        lowerPutByVal(value);
        return undefined();
    case Opcode::NewArray:
        if (isPassedInMemory(id))
            return placeholder(value.rep);
        return ensure(countPlainAllocation(PlainAllocationKind::Array, id, newArrayLiteral(id)), Type::engineClass(m_builtins.arrayClass()), type);
    case Opcode::NewObject:
        if (isAllocatedByFirstUse(id) || spreadLiteralAt(m_currentIndexInBlock))
            return placeholder(value.rep);
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (!literal->fields.isEmpty())
                return placeholder(value.rep);
            lowerObjectLiteral(*literal);
            return get(id);
        }
        {
            Vector<StringID, 16> names;
            unsigned numberOfKeys = 0;
            collectAssignedProperties(m_program, m_function, id, 3, names, numberOfKeys, usersOfAllocation(id));
            unsigned room = names.size() + numberOfKeys;
            if (value.hasInitialStructure()) {
                auto& row = m_program.initialStructures[value.initialStructure()];
                room = std::max<unsigned>(room, row.firstSlot + row.names.size());
                if (row.isWhole && !Options::validateAOTTypedFacts())
                    fail("an allocation marked as fully initialized has no literal"_s);
            }
            room = std::max(room, inlineCapacityForLiteral(m_program, m_function, id, false, usersOfAllocation(id)));
            return ensure(countPlainAllocation(PlainAllocationKind::Object, id, newEmptyObject(room)), m_program.at(m_program.types.object), type);
        }
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
        if (auto fused = lowerFusedUse(m_currentIndexInBlock))
            return *fused;
        return lowerBinary(id, value);
    case Opcode::Negate:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::BitNot:
        return lowerUnary(id, value);
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        if (m_isFusedWithBranch.get(id.index()))
            return boolean(false);
        if (auto fused = lowerFusedUse(m_currentIndexInBlock))
            return *fused;
        return lowerRelational(value);
    case Opcode::StrictEq:
        return lowerStrictEq(value.child(0), value.child(1));
    case Opcode::Eq:
        return lowerLooseEq(value.child(0), value.child(1));
    case Opcode::LogicalNot:
        return logicalNot(get(value.child(0)).as<Word32>());
    case Opcode::ToBoolean:
        if (auto fused = lowerFusedUse(m_currentIndexInBlock))
            return *fused;
        if (m_isTruthinessOnlyBranchedOn.get(id.index()) && get(value.child(0)).rep == Rep::JSValue && toBooleanNeedsStub(typeOf(value.child(0)))) {
            m_truthinessDeferredToBranch = id;
            return boolean(false);
        }
        return lowerToBoolean(value.child(0));
    case Opcode::TypeOf:
        return convert(AOT_TYPED_OPERATION(TypeOf, in<Boxed>(value.child(0))), type, value.rep);
    case Opcode::InstanceOf:
        if (instanceOfUsesShapeTest(value))
            return lowerInstanceOfByShape(value);
        if (auto* engineClass = instanceOfEngineClass(value))
            return lowerInstanceOfEngineClass(id, value, *engineClass);
        return lowerGenericInstanceOf(in<Boxed>(value.child(0)), in<Boxed>(value.child(1)));
    case Opcode::ToString:
        return convert(toStringCell(value.child(0)), type, value.rep);
    case Opcode::ToPropertyKey:
        return ensure(Boxed(callICStub(Stub::ToPropertyKey, Int64, { { in<Boxed>(value.child(0)).value(), firstStubOperandGPR } })), type);
    case Opcode::IsUndefinedOrNull:
        return lowerIsUndefinedOrNull(value.child(0));
    case Opcode::IsInstanceOfClass:
        return lowerIsInstanceOfClass(value);
    case Opcode::HasShapeOf:
        return lowerHasShapeOf(value);
    case Opcode::IsClassObjectUnmodified:
        return lowerIsClassObjectUnmodified(value);
    case Opcode::GetStaticField:
        return lowerGetStaticField(id, value);
    case Opcode::PutStaticField:
        lowerPutStaticField(value);
        return undefined();
    case Opcode::IsClosureOf:
        if (m_isFusedWithBranch.get(id.index()))
            return boolean(false);
        return lowerIsClosureOf(value);
    case Opcode::IsBuiltinFunction:
        return lowerIsBuiltinFunction(value);
    case Opcode::BuiltinFunction:
        if (isLazilyFetched(id))
            return Operand(value.rep, nullptr);
        return convert(builtinCell(m_builtins.cellOf(value.builtin())), type, value.rep);
    case Opcode::TypeOfIs:
        return lowerTypeOfIs(value);
    case Opcode::Results:
    case Opcode::Unspecified:
        return placeholder(value.rep);
    case Opcode::ResultAt:
        return lowerResultAt(value);
    case Opcode::Strcat:
        return convert(lowerStrcat(value), type, value.rep);
    case Opcode::IteratorOpen:
        return lowerIteratorOpen(id, value);
    case Opcode::IteratorHasNext:
        return lowerIteratorHasNext(value);
    case Opcode::IteratorValue:
        return lowerIteratorValue(id, value);
    case Opcode::IteratorClose:
        lowerIteratorClose(value);
        return undefined();
    case Opcode::Call:
        return lowerCall(id, value);
    case Opcode::Construct:
        return lowerConstruct(id, value);
    case Opcode::CallMethod:
        return lowerCallMethod(id, value);
    case Opcode::CallDirect:
        return lowerCallDirect(id, value);
    case Opcode::CallBuiltin:
        if (m_builtins.at(value.builtin()).implementation.kind == BuiltinImplementation::Operation)
            return lowerLanguageOperation(id, value);
        if (auto match = lowerMatchOnlyTakenApart(id, value))
            return *match;
        if (auto length = lowerLengthOfSeparatedString(id, value))
            return *length;
        if (auto number = lowerNumberFromSubstring(id, value))
            return *number;
        if (auto* literal = objectLiteralAt(m_currentIndexInBlock)) {
            if (!literal->fields.isEmpty())
                return placeholder(value.rep);
            lowerObjectLiteral(*literal);
            return get(id);
        }
        return lowerCallBuiltin(id, value);
    case Opcode::GetExternalModule:
        return ensure(AOT_TYPED_OPERATION(GetExternalModule, word32(value.string().index())), type);
    case Opcode::ModuleEvaluationWord: {
        LValue data = fixedPointer(Instance::offsetOfTypedData());
        LValue table = m_out.add(data, m_out.zeroExtPtr(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, data, OBJECT_OFFSETOF(Data, moduleEvaluationsOffset))), IsFixed)));
        LValue address = m_out.add(table, m_out.shl(in<Integer>(value.child(0)).value(), m_out.constInt32(2)));
        return convert(Integer(m_out.signExt32To64(marked(m_out.load32(m_out.address(m_heaps.AOTTyped_runtimeState, address, value.immediates[0] * sizeof(uint32_t))), IsFixed))), type, value.rep);
    }
    case Opcode::OffsetIndex:
        return convert(Integer(m_out.add(in<Integer>(value.child(0)).value(), m_out.constInt64(value.immediates[0]))), type, value.rep);
    case Opcode::GetModuleVariableAt:
        if (value.immediates[0] != Format::none)
            return ensure(Boxed(m_out.zeroExtPtr(m_out.load8ZeroExt32(TypedPointer(m_heaps.AOTTyped_variables.atAnyNumber(), m_out.add(staticsBase(), in<Integer>(value.child(0)).value()))))), type);
        return ensure(Boxed(m_out.load64(TypedPointer(m_heaps.AOTTyped_variables.atAnyNumber(), m_out.add(staticsBase(), in<Integer>(value.child(0)).value())))), type);
    case Opcode::PutModuleVariableAt:
        didEmitWrite(Writes::Fields);
        if (value.immediates[0] != Format::none) {
            m_out.store32As8(m_out.castToInt32(in<Boxed>(value.child(1)).value()), TypedPointer(m_heaps.AOTTyped_variables.atAnyNumber(), m_out.add(staticsBase(), in<Integer>(value.child(0)).value())));
            return convert(undefined(), type, value.rep);
        }
        m_out.store64(in<Boxed>(value.child(1)).value(), TypedPointer(m_heaps.AOTTyped_variables.atAnyNumber(), m_out.add(staticsBase(), in<Integer>(value.child(0)).value())));
        m_out.store32As8(m_out.constInt32(Instance::typedCardIsDirty), TypedPointer(m_heaps.AOTTyped_cards, m_out.add(cardsBase(), m_out.lShr(in<Integer>(value.child(0)).value(), m_out.constInt32(Instance::typedCardShift)))));
        return convert(undefined(), type, value.rep);
    case Opcode::CallModuleFunction:
        return ensure(Boxed(callICStub(Stub::CallModuleFunction, Int64, { { in<Integer>(value.child(0)).value(), GPRInfo::argumentGPR1 } })), type);
    case Opcode::FailModuleCycle:
        AOT_TYPED_OPERATION(FailModuleCycle, in<Integer>(value.child(0)), word32(value.immediates[0]), in<Boxed>(value.child(1)));
        return convert(undefined(), type, value.rep);
    case Opcode::AddToModuleEvaluationDepth: {
        didEmitWrite(Writes::RuntimeState);
        TypedPointer depth = m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfModuleEvaluationDepth());
        m_out.store32(m_out.add(m_out.load32(depth), m_out.constInt32(static_cast<int32_t>(value.immediates[0]))), depth);
        return convert(undefined(), type, value.rep);
    }
    case Opcode::JumpToModuleEvaluator:
        jumpToFunction(calleeIndex(m_program.moduleEvaluatorForExternalCall()), { in<Integer>(value.child(0)).value() });
        return convert(undefined(), type, value.rep);
    case Opcode::Operation:
    case Opcode::Await:
    case Opcode::Yield:
    case Opcode::GetResumeMode:
    case Opcode::AssertNotAllocated:
        RELEASE_ASSERT_NOT_REACHED();
    case Opcode::Cast:
        return lowerCast(id, value);
    case Opcode::CatchValue: {
        if (m_handlersEnteredByBranch.get(m_currentBlockID.index()))
            return convert(undefined(), m_program.at(m_program.types.undefined), value.rep);
        if (ValueID exception = exceptionCaughtIn(m_currentBlockID); exception && exception != id)
            return ensure(AOT_TYPED_OPERATION(ExceptionValue, in<Boxed>(exception)), type);
        if (LValue caught = m_caughtValues.get(m_currentBlockID.index()))
            return ensure(Boxed(caught), type);
        return ensure(catchException(m_currentBlockID), type);
    }
    case Opcode::LoopHint:
        if (isFollowedByInBoundsElementRead(m_currentIndexInBlock))
            m_needsPoll = true;
        else
            poll();
        return undefined();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ImageKey keyOf(FunctionID function)
{
    ImageKey key { };
    key.module = 0;
    key.start = function.index();
    key.kind = ImageKey::typedFunction;
    return key;
}

ImageKey continuationKey(FunctionID function, unsigned index)
{
    ImageKey key = keyOf(function);
    RELEASE_ASSERT(index + 1 < ImageKey::typedBody);
    key.kind |= index + 1;
    return key;
}

void resequenceMovesBeforePatches(Air::Code& code)
{
    auto isSimpleMove = [](Air::Inst& inst) {
        if ((inst.kind.opcode != Air::Move && inst.kind.opcode != Air::MoveDouble) || inst.args().size() != 2 || !inst.args()[1].isReg())
            return false;
        if (inst.args()[0].isReg())
            return true;
        return inst.kind.opcode == Air::Move && (inst.args()[0].isSomeImm() || inst.args()[0].isZeroReg());
    };
    struct PendingMove {
        Air::Arg source;
        Reg destination;
        Air::Inst* original;
    };
    for (Air::BasicBlock* block : code) {
        bool didChange = false;
        for (unsigned end = 0; end < block->size(); ++end) {
            Air::Inst& patch = block->at(end);
            if (patch.kind.opcode != Air::Patch)
                continue;
            unsigned begin = end;
            while (begin && block->at(begin - 1) && isSimpleMove(block->at(begin - 1)))
                --begin;
            if (end - begin < 2)
                continue;
            Vector<PendingMove, 8> pending;
            bool forwardsWideImmediate = false;
            auto valueOf = [&](Air::Arg arg) {
                if (!arg.isReg())
                    return arg;
                for (auto& move : pending) {
                    if (move.destination == arg.reg()) {
                        forwardsWideImmediate |= move.source.isBigImm();
                        return move.source;
                    }
                }
                return arg;
            };
            for (unsigned i = begin; i < end; ++i) {
                Air::Inst& inst = block->at(i);
                Air::Arg source = valueOf(inst.args()[0]);
                Reg destination = inst.args()[1].reg();
                pending.removeAllMatching([&](auto& move) { return move.destination == destination; });
                pending.append({ source, destination, &inst });
            }
            if (forwardsWideImmediate)
                continue;
            RegisterSet used;
            RegisterSet overwritten = patch.extraEarlyClobberedRegs();
            overwritten.merge(patch.extraClobberedRegs());
            patch.forEachTmp([&](Air::Tmp& tmp, Air::Arg::Role role, Bank, Width) {
                if (!tmp.isReg())
                    return;
                if (Air::Arg::isAnyUse(role))
                    used.add(tmp.reg(), IgnoreVectors);
                if (Air::Arg::isAnyDef(role))
                    overwritten.add(tmp.reg(), IgnoreVectors);
            });
            pending.removeAllMatching([&](auto& move) {
                if (move.source.isReg() && move.source.reg() == move.destination)
                    return true;
                return overwritten.contains(move.destination, IgnoreVectors) && !used.contains(move.destination, IgnoreVectors);
            });
            if (pending.size() >= end - begin)
                continue;
            Vector<Air::Inst, 8> ordered;
            while (!pending.isEmpty()) {
                size_t next = pending.findIf([&](auto& candidate) {
                    return !pending.containsIf([&](auto& other) {
                        return &other != &candidate && other.source.isReg() && other.source.reg() == candidate.destination;
                    });
                });
                if (next == notFound)
                    break;
                auto& move = pending[next];
                ordered.append(Air::Inst(move.destination.isGPR() ? Air::Move : Air::MoveDouble, move.original->origin, move.source, Air::Tmp(move.destination)));
                pending.removeAt(next);
            }
            if (!pending.isEmpty())
                continue;
            for (unsigned i = begin; i < end; ++i)
                block->at(i) = i - begin < ordered.size() ? ordered[i - begin] : Air::Inst();
            didChange = true;
        }
        if (didChange)
            block->insts().removeAllMatching([](const Air::Inst& inst) { return !inst; });
    }
}

void mergeIdenticalBlocks(Air::Code& code)
{
    auto isSame = [](Air::BasicBlock* a, Air::BasicBlock* b) {
        if (a->size() != b->size() || a->numSuccessors() != b->numSuccessors() || (a->numSuccessors() && a->successorBlock(0) != b->successorBlock(0)))
            return false;
        for (unsigned i = 0; i < a->size(); ++i) {
            if (a->at(i).kind != b->at(i).kind || !std::ranges::equal(a->at(i).args(), b->at(i).args()))
                return false;
        }
        return true;
    };
    auto canBeInverted = [](Air::Inst& branch) {
        switch (branch.kind.opcode) {
        case Air::BranchOnFlags:
        case Air::Branch8:
#if CPU(X86_64)
        case Air::Branch16:
#endif
        case Air::Branch32:
        case Air::Branch64:
        case Air::BranchTest8:
        case Air::BranchTest32:
        case Air::BranchTest64:
        case Air::BranchFloat:
        case Air::BranchDouble:
            return branch.args()[0].isInvertible();
        default:
            return false;
        }
    };
    UncheckedKeyHashMap<unsigned, Vector<Air::BasicBlock*, 2>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> candidates;
    Vector<Air::BasicBlock*> fallsInto;
    bool hasChanged = true;
    for (bool absorbsFallthrough = false;;) {
        if (hasChanged) {
            candidates = { };
            fallsInto.fill(nullptr, code.size());
        }
        Air::BasicBlock* previous = nullptr;
        for (Air::BasicBlock* block : code) {
            if (!hasChanged)
                break;
            if (previous) {
                for (Air::BasicBlock* successor : previous->successorBlocks()) {
                    if (successor == block)
                        fallsInto[block->index()] = previous;
                }
            }
            previous = block;
            if (code.isEntrypoint(block) || block->numSuccessors() > 1 || (block->numSuccessors() && block->last().kind.opcode != Air::Jump))
                continue;
            unsigned hash = block->size() + (block->numSuccessors() ? block->successorBlock(0)->index() * 131 : 0);
            bool hasPatch = false;
            for (Air::Inst& inst : *block) {
                if (inst.kind.opcode == Air::Patch) {
                    hasPatch = true;
                    break;
                }
                hash = hash * 33 + inst.kind.opcode;
                for (Air::Arg& arg : inst.args())
                    hash = hash * 33 + arg.hash();
            }
            if (!hasPatch)
                candidates.add(hash, Vector<Air::BasicBlock*, 2>()).iterator->value.append(block);
        }
        UncheckedKeyHashMap<Air::BasicBlock*, Air::BasicBlock*> replacements;
        Vector<Air::BasicBlock*, 4> inverted;
        Vector<Air::BasicBlock*, 4> postponed;
        for (auto& entry : candidates) {
            if (entry.value.size() < 2)
                continue;
            auto blocks = entry.value;
            for (unsigned i = 0; i < blocks.size(); ++i) {
                if (!blocks[i])
                    continue;
                Vector<Air::BasicBlock*, 4> same { blocks[i] };
                for (unsigned k = i + 1; k < blocks.size(); ++k) {
                    if (blocks[k] && isSame(blocks[i], blocks[k])) {
                        same.append(blocks[k]);
                        blocks[k] = nullptr;
                    }
                }
                Air::BasicBlock* kept = same[0];
                for (Air::BasicBlock* block : same) {
                    if (block->numSuccessors() && block->successorBlock(0) == code.findNextBlock(block))
                        kept = block;
                }
                for (Air::BasicBlock* block : same) {
                    if (block == kept)
                        continue;
                    Air::BasicBlock* source = fallsInto[block->index()];
                    if (!source) {
                        if (!absorbsFallthrough)
                            replacements.add(block, kept);
                        continue;
                    }
                    if (!absorbsFallthrough || source->numSuccessors() != 2 || source->successorBlock(1) != block || source->successorBlock(0) == block)
                        continue;
                    if (source->successorBlock(0) != code.findNextBlock(block) || !canBeInverted(source->last()))
                        continue;
                    replacements.add(block, kept);
                    inverted.append(source);
                }
            }
        }
        inverted.removeAllMatching([&](Air::BasicBlock* source) {
            if (!replacements.contains(source->successorBlock(0)))
                return false;
            postponed.append(source->successorBlock(1));
            return true;
        });
        for (Air::BasicBlock* block : postponed)
            replacements.remove(block);
        if (replacements.isEmpty()) {
            if (absorbsFallthrough)
                return;
            absorbsFallthrough = true;
            hasChanged = false;
            continue;
        }
        for (Air::BasicBlock* block : code) {
            for (Air::BasicBlock*& successor : block->successorBlocks()) {
                if (auto found = replacements.find(successor); found != replacements.end())
                    successor = found->value;
            }
        }
        for (Air::BasicBlock* source : inverted) {
            std::swap(source->successor(0), source->successor(1));
            source->last().args()[0] = source->last().args()[0].inverted();
        }
        code.resetReachability();
        absorbsFallthrough = false;
        hasChanged = true;
    }
}

void duplicateReturnsIntoJumps(Air::Code& code)
{
    bool didChange = false;
    for (Air::BasicBlock* block : code) {
        if (block->last().kind.opcode != Air::Jump)
            continue;
        Air::BasicBlock* target = block->successorBlock(0);
        if (target == code.findNextBlock(block) || target->size() != 1 || !Air::isReturn(target->last().kind.opcode))
            continue;
        block->last() = target->last();
        block->successors().clear();
        didChange = true;
    }
    if (didChange)
        code.resetReachability();
}

static bool allVariablesAreSetBeforeRead(B3::Procedure& proc)
{
    if (!proc.variables().size())
        return true;
    auto indexOf = [](B3::Value* value) {
        return value->as<B3::VariableValue>()->variable()->index();
    };
    Vector<BitVector> setAtBlockStart(proc.size());
    BitVector isReached;
    isReached.set(proc.at(0)->index());
    Vector<B3::BasicBlock*, 16> worklist { proc.at(0) };
    while (!worklist.isEmpty()) {
        B3::BasicBlock* block = worklist.takeLast();
        BitVector isSet = setAtBlockStart[block->index()];
        for (B3::Value* value : *block) {
            if (value->opcode() == B3::Set)
                isSet.set(indexOf(value));
        }
        for (B3::BasicBlock* successor : block->successorBlocks()) {
            BitVector& atHead = setAtBlockStart[successor->index()];
            if (!isReached.get(successor->index())) {
                isReached.set(successor->index());
                atHead = isSet;
                worklist.append(successor);
                continue;
            }
            BitVector onAllPaths = atHead;
            onAllPaths.filter(isSet);
            if (onAllPaths == atHead)
                continue;
            atHead = WTF::move(onAllPaths);
            worklist.append(successor);
        }
    }
    for (B3::BasicBlock* block : proc) {
        if (!isReached.get(block->index()))
            continue;
        BitVector isSet = setAtBlockStart[block->index()];
        for (B3::Value* value : *block) {
            if (value->opcode() == B3::Set)
                isSet.set(indexOf(value));
            else if (value->opcode() == B3::Get && !isSet.get(indexOf(value)))
                return false;
        }
    }
    return true;
}

void placeInvokeResults(Air::Code& code)
{
    code.resetReachability();
    Air::BlockInsertionSet blockInsertionSet(code);
    Air::InsertionSet insertionSet(code);
    for (Air::BasicBlock* block : code) {
        unsigned terminalIndex = block->size() - 1;
        while (!block->at(terminalIndex).isTerminal())
            --terminalIndex;
        if (terminalIndex + 1 == block->size())
            continue;
        Vector<Air::Inst, 2> moves;
        for (unsigned i = terminalIndex + 1; i < block->size(); ++i)
            moves.append(block->at(i));
        Air::FrequentedBlock& continuation = block->successor(0);
        if (continuation.block()->numPredecessors() == 1) {
            insertionSet.insertInsts(0, moves);
            insertionSet.execute(continuation.block());
        } else {
            Air::BasicBlock* edge = blockInsertionSet.insertBefore(continuation.block(), continuation.block()->frequency());
            for (const Air::Inst& move : moves)
                edge->appendInst(move);
            edge->appendInst(Air::Inst(Air::Jump, moves.last().origin));
            edge->successors().append(continuation.block());
            continuation.block() = edge;
        }
        block->resize(terminalIndex + 1);
    }
    if (blockInsertionSet.execute())
        code.resetReachability();
}

static void sinkStackChecksBelowCalls(Procedure& proc)
{
    InsertionSet insertionSet(proc);
    for (B3::BasicBlock* block : proc) {
        for (unsigned index = 0; index < block->size(); ++index) {
            auto* base = block->at(index)->as<SlotBaseValue>();
            if (!base)
                continue;
            PatchpointValue* address = insertionSet.insert<PatchpointValue>(index, Int64, base->origin());
            address->effects = Effects::none();
            address->effects.reads = HeapRange::top();
            address->setGenerator([slot = base->slot()](CCallHelpers& jit, const StackmapGenerationParams& params) {
                int32_t offset = static_cast<int32_t>(slot->offsetFromFP());
                int32_t distance = std::abs(offset);
                GPRReg result = params[0].gpr();
                if (isARM64() && !isUInt12(distance) && ((distance & 0xfff) || !isUInt12(distance >> 12))) {
                    jit.move(CCallHelpers::TrustedImm64(offset), result);
                    jit.addPtr(GPRInfo::callFrameRegister, result);
                    return;
                }
                jit.addPtr(CCallHelpers::TrustedImm32(offset), GPRInfo::callFrameRegister, result);
            });
            base->replaceWithIdentity(address);
        }
        insertionSet.execute(block);
    }
}

#if CPU(X86_64)
static void makeCallsPreserveLiveRegisters(Procedure& proc, LoweringContext& context)
{
    if (context.tailCallsNeedFrame)
        return;
    Vector<PatchpointValue*, 16> calls;
    BitVector hasCall(proc.size());
    for (B3::BasicBlock* block : proc) {
        for (B3::Value* value : *block) {
            auto* patchpoint = value->as<PatchpointValue>();
            if (!patchpoint || !patchpoint->lateClobbered().contains(callMarkerGPR, IgnoreVectors))
                continue;
            if (patchpoint->effects.terminal)
                return;
            for (unsigned i = 0; i < patchpoint->numChildren(); ++i) {
                if (patchpoint->constrainedChild(i).rep().kind() == ValueRep::StackArgument)
                    return;
            }
            calls.append(patchpoint);
            hasCall.set(block->index());
        }
    }
    if (calls.isEmpty())
        return;
    BitVector allPathsCall(proc.size());
    for (B3::BasicBlock* block : proc)
        allPathsCall.set(block->index());
    for (bool changed = true; changed;) {
        changed = false;
        for (unsigned i = proc.size(); i--;) {
            B3::BasicBlock* block = proc[i];
            if (!block || hasCall.get(i) || !allPathsCall.get(i))
                continue;
            bool holds = block->numSuccessors() || block->last()->opcode() == B3::Oops;
            for (B3::BasicBlock* successor : block->successorBlocks())
                holds &= allPathsCall.get(successor->index());
            if (!holds) {
                allPathsCall.clear(i);
                changed = true;
            }
        }
    }
    if (allPathsCall.get(proc.at(0)->index()))
        return;
    double predictedCalls = 0;
    for (PatchpointValue* call : calls)
        predictedCalls += call->owner->frequency();
    if (2 * predictedCalls > proc.at(0)->frequency())
        return;
    context.emitsCalls = false;
    context.callsPreserveLiveRegisters = true;
    for (PatchpointValue* call : calls) {
        RegisterSet kept = call->lateClobbered();
        kept.exclude(call->earlyClobbered());
        kept.remove(callMarkerGPR);
        call->lateClobbered().filter(call->earlyClobbered());
        call->setGenerator([context = &context, generator = call->generator(), kept](CCallHelpers& jit, const StackmapGenerationParams& params) {
            AllowMacroScratchRegisterUsage allowScratch(jit);
            constexpr auto sp = CCallHelpers::stackPointerRegister;
            auto* patchpoint = params.value()->as<PatchpointValue>();
            RegisterSet live = params.usedRegisters();
            live.filter(kept);
            for (unsigned i = 0; i < params.proc().resultCount(patchpoint->type()); ++i) {
                if (params[i].isReg())
                    live.remove(params[i].reg());
            }
            for (unsigned i = 0; i < patchpoint->numGPScratchRegisters; ++i)
                live.remove(params.gpScratch(i));
            for (unsigned i = 0; i < patchpoint->numFPScratchRegisters; ++i)
                live.remove(params.fpScratch(i));
            Vector<GPRReg, 8> gprs;
            Vector<FPRReg, 4> fprs;
            live.forEach([&](Reg reg) {
                if (reg.isGPR())
                    gprs.append(reg.gpr());
                else
                    fprs.append(reg.fpr());
            });
            bool createsFrame = context->hasNoFrame(params.proc().code());
            unsigned wordsBelow = fprs.size() + ((gprs.size() + fprs.size()) & 1);
            context->stackUsedAtCalls = std::max<unsigned>(context->stackUsedAtCalls, (createsFrame + gprs.size() + wordsBelow) * sizeof(CPURegister));
            if (createsFrame)
                jit.emitFunctionPrologue();
            for (GPRReg reg : gprs)
                jit.push(reg);
            if (wordsBelow)
                jit.subPtr(CCallHelpers::TrustedImm32(wordsBelow * sizeof(CPURegister)), sp);
            for (unsigned i = 0; i < fprs.size(); ++i)
                jit.storeDouble(fprs[i], CCallHelpers::Address(sp, i * sizeof(CPURegister)));
            context->createsFrameRecord = createsFrame;
            generator->run(jit, params);
            context->createsFrameRecord = false;
            for (unsigned i = 0; i < fprs.size(); ++i)
                jit.loadDouble(CCallHelpers::Address(sp, i * sizeof(CPURegister)), fprs[i]);
            if (wordsBelow)
                jit.addPtr(CCallHelpers::TrustedImm32(wordsBelow * sizeof(CPURegister)), sp);
            for (unsigned i = gprs.size(); i--;)
                jit.pop(gprs[i]);
            if (createsFrame)
                jit.emitFunctionEpilogueWithEmptyFrame();
        });
    }
}
#endif

struct CompiledProcedureCache::Entry {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Entry);

    bool collectStackmaps(Procedure& compiled)
    {
        for (Air::BasicBlock* block : compiled.code()) {
            for (Air::Inst& inst : *block) {
                if (inst.kind.opcode != Air::Patch)
                    continue;
                auto* stackmap = inst.origin->as<StackmapValue>();
                if (!stackmap)
                    continue;
                size_t original = fingerprint.findGenerator(stackmap->generator().get());
                if (original == notFound)
                    return false;
                stackmaps.append({ stackmap, static_cast<unsigned>(original) });
            }
        }
        fingerprint.clearGenerators();
        return true;
    }

    void adoptGeneratorsOf(Procedure& other, const ProcedureFingerprint& otherFingerprint)
    {
        for (auto [stackmap, original] : stackmaps)
            stackmap->setGenerator(otherFingerprint.generators()[original]);
        procedure->code().copyFrameGeneratorsFrom(other.code());
        procedure->code().clearTablesInCode();
        PCToOriginMap discardedRanges(WTF::move(procedure->pcToOriginMap()));
    }

    ProcedureFingerprint fingerprint;
    std::unique_ptr<Procedure> procedure;
    Vector<std::pair<StackmapValue*, unsigned>, 8> stackmaps;
    uint64_t lastUse { 0 };
    bool isInUse { true };
};

static thread_local CompiledProcedureCache* currentCompiledProcedureCache;

CompiledProcedureCache::CompiledProcedureCache()
{
    RELEASE_ASSERT(!currentCompiledProcedureCache);
    currentCompiledProcedureCache = this;
}

CompiledProcedureCache::~CompiledProcedureCache()
{
    currentCompiledProcedureCache = nullptr;
}

CompiledProcedureCache* CompiledProcedureCache::current()
{
    CompiledProcedureCache* cache = currentCompiledProcedureCache;
    return cache && !cache->m_suspensionCount ? cache : nullptr;
}

CompiledProcedureCache::Entry* CompiledProcedureCache::find(const ProcedureFingerprint& wanted)
{
    for (auto& entry : m_entries) {
        if (entry->fingerprint == wanted)
            return entry.get();
    }
    return nullptr;
}

CompiledProcedureCache::Entry* CompiledProcedureCache::add(std::unique_ptr<Entry>&& entry)
{
    if (m_entries.size() == capacity) {
        size_t leastRecentlyUsed = notFound;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (!m_entries[i]->isInUse && (leastRecentlyUsed == notFound || m_entries[i]->lastUse < m_entries[leastRecentlyUsed]->lastUse))
                leastRecentlyUsed = i;
        }
        if (leastRecentlyUsed == notFound)
            return nullptr;
        m_entries.removeAt(leastRecentlyUsed);
    }
    m_entries.append(WTF::move(entry));
    return m_entries.last().get();
}

void CompiledProcedureCache::remove(Entry* entry)
{
    m_entries.removeFirstMatching([&](auto& other) {
        return other.get() == entry;
    });
}

void CompiledProcedureCache::didUse(Entry& entry)
{
    entry.lastUse = ++m_lastUse;
    entry.isInUse = false;
}

uint32_t shapeHashForCompilationOrder(const Function& function)
{
    uint32_t hash = function.flags;
    auto add = [&](uint32_t word) {
        hash = (hash ^ word) * 0x01000193u;
    };
    for (auto& block : function.blocks) {
        add(block.parameters.size() << 8 | static_cast<uint32_t>(block.terminator.kind));
        for (ValueID id : block.values) {
            auto& value = function.at(id);
            add(static_cast<uint32_t>(value.opcode) << 16 | static_cast<uint32_t>(value.rep) << 8 | value.arguments.size());
            if (value.opcode == Opcode::Cast)
                add(value.type.index());
        }
    }
    return hash;
}

static bool emitCode(const Program&, FunctionID, const Function&, Lowering&, LoweringContext&, Procedure&, CCallHelpers::JumpList& frameDoesNotFit, bool& fitsByProof, CCallHelpers::Label& afterFramelessStackCheck, bool& hasFrame, CompiledCode& result);

static UTF8CString sourceNameOf(const Module& module, std::span<const char8_t> path)
{
    if (module.isBuiltin || path.empty() || path.front() == '/')
        return UTF8CString(path);
    Vector<char8_t, 128> name;
    name.append(ProgramData::relativeSourceNameMarker);
    name.append(path);
    return UTF8CString(name.span());
}

static bool compilePart(const Program& program, FunctionID id, const Function& function, SplitFunction* split, bool& hasFrame, CompiledCode& result)
{
    CompilationCohort::yieldBeforePhase("lower"_s);
    LoweringContext context;
    auto ownedProcedure = makeUnique<Procedure>(/* usesSIMD = */ false);
    Procedure& proc = *ownedProcedure;
    if (Options::airDumpPhaseStats() || Options::airGreedyRegAllocDumpFunction()) [[unlikely]]
        proc.setName(makeString('#', id.index(), '#'));
    proc.setPositionIndependent();
    proc.setMaterializesConstantsByFrequency();
    proc.setRotatesLoops();
    proc.setNamesPinnedArgumentRegistersDirectly();
    pinRegisters(proc);
#if CPU(X86_64)
    proc.code().setUnsavedCalleeSaves(RegisterSet { X86Registers::r12 });
    if (function.savesFrameOnSuspend)
        proc.code().setCalleeSavesClobberedAtSuspensions(RegisterSet { X86Registers::r15 });
#elif CPU(ARM64)
    if (function.savesFrameOnSuspend) {
        RegisterSet clobberedAtSuspensions;
        for (GPRReg reg : { ARM64Registers::x19, ARM64Registers::x20, ARM64Registers::x21, ARM64Registers::x22, ARM64Registers::x23, ARM64Registers::x24 })
            clobberedAtSuspensions.add(reg, IgnoreVectors);
        proc.code().setCalleeSavesClobberedAtSuspensions(clobberedAtSuspensions);
        for (FPRReg reg : { ARM64Registers::q8, ARM64Registers::q9, ARM64Registers::q10, ARM64Registers::q11, ARM64Registers::q12, ARM64Registers::q13, ARM64Registers::q14, ARM64Registers::q15 })
            proc.pinRegister(reg);
    }
#endif
    proc.code().setChargesForCalleeSaves();
    static_assert(Instance::offsetOfNotUndefinedOrNullMask() >= std::numeric_limits<int8_t>::min() && Instance::offsetOfNotUndefinedOrNullMask() <= std::numeric_limits<int8_t>::max());
    proc.code().setMaskInMemory(~static_cast<int64_t>(JSValue::ValueUndefined), instanceGPR, Instance::offsetOfNotUndefinedOrNullMask());
    proc.code().setPlacesRareBlocksInGaps();
    proc.code().setOrdersBlocksByEdgeFrequency();
    proc.code().setRecordsRareCodeStart();
    if (Options::aotMapFilePath()) [[unlikely]]
        proc.setNeedsPCToOriginMap();
    context.nullishTestMarker = createSharedTask<StackmapGeneratorFunction>([](CCallHelpers&, const StackmapGenerationParams&) { });
    proc.setLastPhase(createSharedTask<void(Procedure&)>([&](Procedure& proc) {
        foldNumberBoxings(proc, context);
        loadDoubleConstantsFromInstance(proc);
        narrowSignExtendedComparisons(proc);
        narrowZeroExtendedComparisons(proc);
        storeAdjacentConstantsTogether(proc);
        useFaultsForNullishTests(proc, context);
        foldDecodingsIntoAccesses(proc, context);
        branchOverUndefinedForAbsentCells(proc);
        useMemoryOperandsAtLoopPolls(proc, context);
        readTagsFromInstance(proc);
#if AOT_CHECKS_ASAN_SHADOW_IN_COMPILED_CODE
        checkShadowBeforeAccesses(proc);
#endif
    }));
    Lowering lowering(proc, program, id, function, context);
    if (!lowering.run()) {
        result.whyDeclined = lowering.failureReason();
        if (lowering.usesUndefinedValue()) {
            Vector<String> findings;
            verify(program, id, function, findings, VerifierScope::DefinitionsOnly);
            if (!findings.isEmpty())
                result.whyDeclined = findings.first();
        }
        return false;
    }
    CompilationCohort::yieldBeforePhase("prepareForB3"_s);
    if (split && context.emitsCalls && !context.readsCallerFrame && splitFunction(program, id, function, context.blocksWithCalls, context.unreachableBlocks, *split))
        return true;
    splitBranchConditions(proc);
    estimateFrequencies(proc, !function.savesFrameOnSuspend && !(function.declaredAs & (functionIsAsync | functionIsGenerator)) && !program.isResumeFunction(function));
#if CPU(X86_64)
#if OS(LINUX)
    constexpr bool canCheckStackWithoutFrame = true;
#else
    constexpr bool canCheckStackWithoutFrame = false;
#endif
    if (context.emitsCalls && !context.readsCallerFrame && !context.savesFrameOnSuspend && !context.unwindsToLandingPads && function.isTyped() && !function.isGenericEntrypoint() && !function.declaredAs && program.isHotOutsideLoops(id) && (canCheckStackWithoutFrame || program.hasCheckedStackOnEntry(id))) {
        proc.resetValueOwners();
        makeCallsPreserveLiveRegisters(proc, context);
    }
#endif
    placeSlotBlockFetches(proc, context);
    if (context.savesFrameOnSuspend)
        sinkStackChecksBelowCalls(proc);
    if (Options::dumpAOTB3Graph() && result.mayBeDumped) [[unlikely]]
        dataLogLn("AOT B3:\n", proc);
    if (!allVariablesAreSetBeforeRead(proc)) {
        result.whyDeclined = "a back-end variable is read that is not set on every path to the read"_s;
        return false;
    }
    for (B3::Value* value : proc.values()) {
        if (!value->hasInt64())
            continue;
        uint64_t bits = value->asInt64();
        if (AddressLikeIntegers::isOne(bits) && !context.wideIntegerConstants.contains(static_cast<int64_t>(bits))) {
            result.whyDeclined = "an address in the code"_s;
            return false;
        }
    }

    CCallHelpers::JumpList frameDoesNotFit;
    bool fitsByProof = false;
    CCallHelpers::Label afterFramelessStackCheck;
    proc.code().setPrologueForEntrypoint(0, createSharedTask<Air::PrologueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code, Air::GenerationContext&) {
        if (context.hasNoFrame(code)) {
            if (context.callsPreserveLiveRegisters && !program.hasCheckedStackOnEntry(id)) {
                AllowMacroScratchRegisterUsage allowScratch(jit);
                frameDoesNotFit = emitStackCheck(jit, 0);
                afterFramelessStackCheck = jit.label();
            }
            return;
        }
        AllowMacroScratchRegisterUsage allowScratch(jit);
        if (context.dropsCollapsibleCallerFrames)
            dropCollapsibleCallerFrames(jit, stubTemporaryGPRs[4]);
        bool isDeferredToCallee = function.isGenericEntrypoint() && function.inlineFrames.size() <= 1 && program.genericEntrypointsWithCalleeStackCheck.get(id.index());
        static_assert(maxFrameSizeWithoutStackCheck <= 256);
        fitsByProof = (context.emitsCalls || context.callsPreserveLiveRegisters) && !isDeferredToCallee && program.hasCheckedStackOnEntry(id) && code.frameSize() <= maxFrameSizeWithoutStackCheck;
        isDeferredToCallee |= fitsByProof;
#if CPU(ARM64)
        if (code.placesSpillsAboveFrameRecord()) {
            jit.m_assembler.stp<64>(GPRInfo::callFrameRegister, CCallHelpers::linkRegister, CCallHelpers::stackPointerRegister, PairPreIndex(-static_cast<int>(code.bytesAboveFrameRecord() + sizeof(CallerFrameAndPC))));
            jit.move(CCallHelpers::stackPointerRegister, GPRInfo::callFrameRegister);
            if ((context.emitsCalls && !isDeferredToCallee) || context.needsStackCheckForTailCalls || code.frameSize() > 256)
                frameDoesNotFit = emitStackCheck(jit, 0);
            pushCalleeSaves(jit, code.calleeSaveRegisterAtOffsetList());
            return;
        }
#endif
        jit.emitFunctionPrologue();
        RegisterAtOffsetList saves = code.calleeSaveRegisterAtOffsetList();
        bool checksStack = ((context.emitsCalls || context.callsPreserveLiveRegisters) && !isDeferredToCallee) || context.needsStackCheckForTailCalls || code.frameSize() > 256;
#if CPU(X86_64)
        if (code.placesCalleeSavesAtFrameBottom() || !code.registersPushedInPrologue().isEmpty()) {
            unsigned above = code.frameSize() - saves.registerCount() * sizeof(CPURegister);
            for (Reg reg : code.registersPushedInPrologue())
                jit.push(reg.gpr());
            unsigned left = above - code.registersPushedInPrologue().size() * sizeof(CPURegister);
            if (left == sizeof(CPURegister))
                jit.push(GPRInfo::returnValueGPR);
            else if (left)
                jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(above), CCallHelpers::stackPointerRegister);
            for (size_t i = saves.registerCount(); i--;)
                jit.push(saves.at(i).reg().gpr());
            if (checksStack)
                frameDoesNotFit = emitStackCheck(jit, 0);
            return;
        }
#endif
        bool arePushed = isX86() && saves.registerCount();
        for (size_t i = 0; i < saves.registerCount(); ++i) {
            auto& entry = saves.at(saves.registerCount() - 1 - i);
            arePushed &= entry.reg().isGPR() && entry.offset() == -static_cast<ptrdiff_t>((i + 1) * sizeof(CPURegister));
        }
#if CPU(X86_64)
        for (size_t i = 0; arePushed && i < saves.registerCount(); ++i)
            jit.push(saves.at(saves.registerCount() - 1 - i).reg().gpr());
#endif
        if (checksStack)
            frameDoesNotFit = emitStackCheck(jit, code.frameSize());
        else if (code.frameSize())
            jit.subPtr(GPRInfo::callFrameRegister, CCallHelpers::TrustedImm32(code.frameSize()), CCallHelpers::stackPointerRegister);
        if (!arePushed)
            jit.emitSave(saves);
    }));
    proc.code().setEpilogueGenerator(createSharedTask<Air::EpilogueGeneratorFunction>([&](CCallHelpers& jit, Air::Code& code) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context.emitFrameTeardown(jit, code);
        jit.ret();
    }));

    if (!context.readsCallerFrame && !(function.declaredAs & functionIsAsync) && !function.savesFrameOnSuspend)
        proc.code().setMayPlaceSpillsAboveFrameRecord();
    if (!context.savesFrameOnSuspend && !function.savesFrameOnSuspend)
        proc.code().setMayPlaceCalleeSavesAtFrameBottom();

    constexpr unsigned maxValuesInCachedProcedure = 128;
    CompiledProcedureCache* cache = CompiledProcedureCache::current();
    CompiledProcedureCache::Entry* newEntry = nullptr;
    bool passesDependOnlyOnProcedure = context.nullishTests.isEmpty() && context.latePathCallAccesses.isEmpty() && context.loopPollSites.isEmpty() && context.accessesThroughPointer.isEmpty() && !context.keepsCellsAliveForWords && !context.numberBoxingGenerator && !proc.loopPollGenerator();
    if (cache && passesDependOnlyOnProcedure && proc.values().size() <= maxValuesInCachedProcedure && !Options::dumpAOTB3Graph()) {
        ProcedureFingerprint fingerprint;
        std::array<StackmapGenerator*, 3> distinguishedGenerators { context.nullishTestMarker.get(), context.emptyGenerator.get(), context.slotBlockFetchGenerator.get() };
        uint32_t contextBits = static_cast<uint32_t>(context.emitsCalls) | context.readsCallerFrame << 1 | context.createsFrameRecord << 2 | context.needsStackCheckForTailCalls << 3 | context.tailCallsNeedFrame << 4 | context.unwindsToLandingPads << 5
            | context.savesFrameOnSuspend << 6 | context.callsPreserveLiveRegisters << 7 | context.dropsCollapsibleCallerFrames << 8 | context.hasColdSites << 9;
        if (fingerprint.compute(proc, distinguishedGenerators, contextBits)) {
            CompiledProcedureCache::Entry* equal = cache->find(fingerprint);
            while (equal && equal->isInUse) {
                RELEASE_ASSERT(CompilationCohort::waitForOtherMembers());
                equal = cache->find(fingerprint);
            }
            if (equal) {
                equal->isInUse = true;
                equal->adoptGeneratorsOf(proc, fingerprint);
                bool didEmit = emitCode(program, id, function, lowering, context, *equal->procedure, frameDoesNotFit, fitsByProof, afterFramelessStackCheck, hasFrame, result);
                cache->didUse(*equal);
#if ASSERT_ENABLED
                CompiledCode expected;
                bool expectedHasFrame = false;
                cache->suspend();
                bool didCompile = compilePart(program, id, function, nullptr, expectedHasFrame, expected);
                cache->resume();
                ASSERT(didCompile == didEmit);
                ASSERT(!didEmit || (expectedHasFrame == hasFrame && expected.bytes == result.bytes && expected.rareCodeStart == result.rareCodeStart && expected.info.frameSizeInBytes == result.info.frameSizeInBytes));
#endif
                return didEmit;
            }
            auto entry = makeUnique<CompiledProcedureCache::Entry>();
            entry->fingerprint = WTF::move(fingerprint);
            newEntry = cache->add(WTF::move(entry));
        }
    }

    generateToAir(proc);
    CompilationCohort::yieldBeforePhase("prepareForAir"_s);
    if (context.unwindsToLandingPads)
        placeInvokeResults(proc.code());
    Air::prepareForGeneration(proc.code());
    CompilationCohort::yieldBeforePhase("generate"_s);
    resequenceMovesBeforePatches(proc.code());
    mergeIdenticalBlocks(proc.code());
    if (context.hasNoFrame(proc.code()) || (isX86() && !proc.code().calleeSaveRegisterAtOffsetList().registerCount()))
        duplicateReturnsIntoJumps(proc.code());
    bool didEmit = emitCode(program, id, function, lowering, context, proc, frameDoesNotFit, fitsByProof, afterFramelessStackCheck, hasFrame, result);
    if (newEntry) {
        if (didEmit && newEntry->collectStackmaps(proc)) {
            newEntry->procedure = WTF::move(ownedProcedure);
            cache->didUse(*newEntry);
        } else
            cache->remove(newEntry);
    }
    return didEmit;
}

static bool emitCode(const Program& program, FunctionID id, const Function& function, Lowering& lowering, LoweringContext& context, Procedure& proc, CCallHelpers::JumpList& frameDoesNotFit, bool& fitsByProof, CCallHelpers::Label& afterFramelessStackCheck, bool& hasFrame, CompiledCode& result)
{
    CCallHelpers jit;
    readTagsFromInstance(jit);
#if CPU(X86_64)
    jit.m_assembler.recordLinkedJumps();
#endif
    CCallHelpers::Label codeStart = jit.label();
    jit.setOopsEmitsBreakpointOnly();
    generate(proc, jit);
#if CPU(X86_64) && OS(LINUX)
    if (afterFramelessStackCheck.isSet()) {
        frameDoesNotFit.link(&jit);
        frameDoesNotFit.clear();
        jit.emitFunctionPrologue();
        jit.m_assembler.buffer().putByte(static_cast<int8_t>(trapInstruction));
        jit.emitFunctionEpilogueWithEmptyFrame();
        jit.jump().linkTo(afterFramelessStackCheck, &jit);
    }
#endif
    bool checksStackWithoutFrame = afterFramelessStackCheck.isSet();
    emitStackOverflow(jit, context.stubCalls, frameDoesNotFit, proc.code().frameSize());
    RELEASE_ASSERT(!CCallHelpers::differenceBetween(codeStart, proc.code().entrypointLabel(0)));
    jit.breakpoint();
    jit.padBeforePatch();
#if CPU(X86_64)
    jit.m_assembler.shortenLinkedJumps();
#endif
    Vector<uint32_t> storage(WTF::roundUpToMultipleOf<sizeof(uint32_t)>(jit.m_assembler.codeSize()) / sizeof(uint32_t));
    LinkBuffer linkBuffer(jit, CodePtr<LinkBufferPtrTag>::fromUntaggedPtr(storage.mutableSpan().data()), storage.sizeInBytes(), LinkBuffer::Profile::FTL);

    CompilationCohort::yieldBeforePhase("describeCode"_s);
    CompiledFunctionInfo info;
    info.stubCalls = context.stubCalls.link(linkBuffer);
    info.argumentMoves = context.stubCalls.takeArgumentMoves();
    info.codeSize = linkBuffer.size();
    void* start = linkBuffer.entrypoint<JSEntryPtrTag>().untaggedPtr();
    info.convention = Convention { Signature::Registers, 0, false };
    if (program.at(id).isGenericEntrypoint())
        info.convention = Convention { program.at(id).has(FunctionUsesArgumentBuffer) ? Signature::List : Signature::Registers, lowering.genericEntrypointParameterCount(), true };
    info.frameSizeInBytes = proc.frameSize();
    info.knownCallees = WTF::move(context.knownCallees);
    if (function.adapterResult != Format::none) {
        info.adapterResult = function.adapterResult;
        info.compatibleResultsMaskSize = program.compatibleResultsMaskSize;
        info.adapterArgumentCount = function.blocks[0].parameters.size() - 2;
    } else if (function.isValueEntry && !function.valueEntryTarget)
        info.signatureAdapter = program.signatureIndexOfAdapter(id);
    if (uint64_t results = function.compatibleResultsMask & program.resultsUsedByCalls; results && !program.compiledValueEntryOf(id) && info.convention.signature == Signature::Registers) {
        info.compatibleResultsMask = results;
        info.compatibleResultsMaskSize = program.compatibleResultsMaskSize;
    }
    if (FunctionID entry = program.compiledValueEntryOf(id)) {
        info.valueEntry = info.knownCallees.size();
        info.valueEntrySignatureIndex = program.signatureIndexOfAdapter(program.at(entry).valueEntryAdapter);
        info.knownCallees.append(keyOf(entry));
    }
    info.indexReferences = context.indexReferences.link(linkBuffer);
    info.numSlots = context.sites.size();
    info.numberOfInlineEmptyArrayLiterals = context.numberOfInlineEmptyArrayLiterals;
    info.numberOfStubEmptyArrayLiterals = context.numberOfStubEmptyArrayLiterals;
    info.sites = WTF::move(context.sites);
    info.siteConstants.fill(0, info.numSlots);
    info.plans = WTF::move(context.plans);
    for (auto [slot, row] : context.slotPlanOffsets)
        info.siteConstants[slot] = (row + 1) | CompiledFunctionInfo::siteConstantIsPlan;
    info.startsCold = info.numSlots;
    for (StringID name : context.identifiers) {
        info.typedIdentifierIndices.append(program.identifierNumbers[name.index()]);
        info.typedIdentifierNames.append(String::fromUTF8(program.string(name)));
    }
    info.calleeSaveRegisters = proc.calleeSaveRegisterAtOffsetList();
    constexpr unsigned maxFrameSizeInBytes = std::numeric_limits<uint16_t>::max() * stackAlignmentBytes();
    bool savedRegistersAreTooFar = info.calleeSaveRegisters.registerCount() && -info.calleeSaveRegisters.at(0).offset() / static_cast<ptrdiff_t>(sizeof(CPURegister)) > static_cast<ptrdiff_t>(ImageFrame::maxCalleeSavesStart);
    static const char* const limitForTesting = byteCast<char>(Options::aotImageLimitToExceedForTesting());
    bool isOverForTesting = limitForTesting && !strcmp(limitForTesting, program.entries.contains(id) ? "frameSize" : "functionFrameSize");
    if (info.frameSizeInBytes > maxFrameSizeInBytes || savedRegistersAreTooFar || isOverForTesting) {
        result.whyDeclined = makeString("it holds so many values at once that its frame on the stack would be "_s, info.frameSizeInBytes, " bytes, and the limit is "_s, maxFrameSizeInBytes, ": a very large object or array literal can cause this"_s);
        return false;
    }
    for (auto [site, landingPad] : context.landingPadsForCallSites)
        info.typedCatchEntrypointsForCallSites.append({ site, static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(*context.landingPads[landingPad]).untaggedPtr()) - static_cast<uint8_t*>(start)) });
    for (auto& access : context.faultingAccesses) {
        auto& test = context.nullishTests[access.nullishTest];
        uint32_t codeOffset = static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(access.instruction).untaggedPtr()) - static_cast<uint8_t*>(start);
        info.faultingAccesses.append({ codeOffset, test.callSite, throwSiteWords(test.kind, Rep::Cell, static_cast<unsigned>(access.base), test.a, test.b) });
    }
    for (auto& trap : context.throwSiteTraps)
        info.faultingAccesses.append({ static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(trap.instruction).untaggedPtr()) - static_cast<uint8_t*>(start)), trap.callSite, { } });
    for (auto& trap : context.loopPollTraps)
        info.faultingAccesses.append({ static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(trap.instruction).untaggedPtr()) - static_cast<uint8_t*>(start)), trap.callSite, { }, true });
    for (auto& site : context.callThrowSites)
        info.faultingAccesses.append({ static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(site.call).untaggedPtr()) - static_cast<uint8_t*>(start)), StubCall::noCallSite, site.words });
    std::ranges::sort(info.faultingAccesses, { }, &CompiledFunctionInfo::FaultingAccess::codeOffset);
    info.hasNoFrame = context.hasNoFrame(proc.code());
    info.isEnteredWithoutStackCheck = frameDoesNotFit.empty() && !checksStackWithoutFrame;
    info.isProvenToFitOnStack = fitsByProof || (context.callsPreserveLiveRegisters && info.hasNoFrame && !checksStackWithoutFrame);
    info.stackUsedAtCalls = context.stackUsedAtCalls;
    info.typedFunction = function.isValueEntryClone ? function.valueEntryTarget.index() : id.index();
    info.isGenericEntrypoint = function.isGenericEntrypoint();
    info.kindByte = kindByteOf(program, function);
    Vector<ModuleID, 2> sources;
    constexpr ModuleID builtinsModule { UINT32_MAX - 1 };
    auto builtinsSource = [&] {
        size_t source = sources.find(builtinsModule);
        if (source == notFound) {
            source = sources.size();
            sources.append(builtinsModule);
            info.typedSourceNames.append(UTF8CString(u8""));
        }
        return static_cast<uint32_t>(source);
    };
    auto positionIn = [&](const Function& owner, uint32_t position, uint32_t frame) {
        if (BuiltinID builtin = program.builtinImplementedBy(owner)) {
            bool isHostFunction = Builtins::singleton().isHostFunctionInEngine(builtin);
            return TypedSitePosition { isHostFunction ? 0u : 1u, isHostFunction ? 0u : 11u, builtinsSource(), frame };
        }
        if (program.isInBuiltinFunctionModule(owner)) {
            auto* around = program.enclosingBuiltinFunction(owner);
            if (!around || program.at(around->function).position == owner.position)
                return TypedSitePosition { 1, 11, builtinsSource(), frame };
            LineColumn start = program.lineColumnFor(owner.module, owner.position);
            return TypedSitePosition { start.line - program.lineColumnFor(owner.module, program.at(around->function).position).line + 1, start.column, builtinsSource(), frame };
        }
        if (!position && owner.kind != FunctionKind::Module)
            position = owner.position;
        size_t source = sources.find(owner.module);
        if (source == notFound) {
            source = sources.size();
            sources.append(owner.module);
            StringID path = owner.module ? program.modules[owner.module.index()].displayPath : StringID();
            if (!path && owner.module)
                path = program.modules[owner.module.index()].path;
            info.typedSourceNames.append(path ? sourceNameOf(program.modules[owner.module.index()], byteCast<char8_t>(program.string(path))) : UTF8CString(u8"unknown"));
        }
        LineColumn lineColumn = program.lineColumnFor(owner.module, position);
        return TypedSitePosition { lineColumn.line, lineColumn.column, static_cast<uint32_t>(source), frame };
    };
    size_t nonItemSiteCount;
    {
        uint32_t numberOfSitesWithoutCode = context.numberOfSitesWithoutCode;
        info.numberOfSitesWithoutCode = numberOfSitesWithoutCode;
        Vector<std::pair<uint32_t, uint32_t*>> returns;
        for (auto& call : info.stubCalls) {
            if (call.callSite != StubCall::noCallSite && !call.isTailCall)
                returns.append({ call.offset, &call.callSite });
        }
        for (auto& access : info.faultingAccesses) {
            if (access.callSite != StubCall::noCallSite)
                returns.append({ access.codeOffset, &access.callSite });
        }
        std::ranges::sort(returns, { }, [](auto& entry) {
            return entry.first;
        });
        constexpr uint32_t none = std::numeric_limits<uint32_t>::max();
        Vector<uint32_t> catchEntrypointOf;
        catchEntrypointOf.fill(none, context.sitePositions.size() + 1);
        BitVector handlerTakesException;
        BitVector handlerIgnoresValue;
        for (auto [site, entrypoint] : info.typedCatchEntrypointsForCallSites) {
            catchEntrypointOf[site & ~FunctionReportableSites::handlerFlags] = entrypoint;
            if (site & FunctionReportableSites::handlerTakesException)
                handlerTakesException.set(site & ~FunctionReportableSites::handlerFlags);
            if (site & FunctionReportableSites::handlerIgnoresValue)
                handlerIgnoresValue.set(site & ~FunctionReportableSites::handlerFlags);
        }
        decltype(context.sitePositions) positions;
        positions.append(context.sitePositions.span().first(numberOfSitesWithoutCode));
        decltype(info.typedCatchEntrypointsForCallSites) catchEntrypoints;
        for (auto [offset, site] : returns) {
            uint32_t old = CallSiteIndex(PackedSite::bits(*site)).bytecodeIndex().offset();
            RELEASE_ASSERT(old > numberOfSitesWithoutCode && old <= context.sitePositions.size());
            positions.append(context.sitePositions[old - 1]);
            uint32_t number = positions.size();
            if (catchEntrypointOf[old] != none)
                catchEntrypoints.append({ number | (handlerTakesException.get(old) ? FunctionReportableSites::handlerTakesException : 0) | (handlerIgnoresValue.get(old) ? FunctionReportableSites::handlerIgnoresValue : 0), catchEntrypointOf[old] });
            *site = PackedSite::pack(PackedSite::inlineFrame(*site), CallSiteIndex(BytecodeIndex(number)).bits());
            for (auto entry = std::ranges::lower_bound(context.itemSites, old, { }, &SpreadSite::callSite); entry != context.itemSites.end() && entry->callSite == old; ++entry)
                info.spreadSites.append({ *site, entry->item, entry->site });
        }
        nonItemSiteCount = positions.size();
        for (auto& entry : info.spreadSites) {
            positions.append(context.sitePositions[CallSiteIndex(PackedSite::bits(entry.site)).bytecodeIndex().offset() - 1]);
            entry.site = PackedSite::pack(PackedSite::inlineFrame(entry.site), CallSiteIndex(BytecodeIndex(positions.size())).bits());
        }
        context.sitePositions = WTF::move(positions);
        info.typedCatchEntrypointsForCallSites = WTF::move(catchEntrypoints);
        for (uint32_t i = numberOfSitesWithoutCode; i < context.sitePositions.size(); ++i) {
            if (context.sitePositions[i].constructs)
                info.constructingSites.append(i + 1);
        }
    }
    info.typedPositions.append(positionIn(function, function.position, 0));
    uint32_t tailCallFirstFrame = context.framesDroppedByTailCalls.isEmpty() ? UINT32_MAX : static_cast<uint32_t>(context.inlineCallSites.size() - context.framesDroppedByTailCalls.size());
    for (auto& site : context.sitePositions) {
        uint32_t frame = site.inlineFrame >= tailCallFirstFrame ? context.framesDroppedByTailCalls[site.inlineFrame - tailCallFirstFrame] : site.inlineFrame;
        info.typedPositions.append(positionIn(frame ? program.at(function.inlineFrames[frame].callee) : function, site.position, info.typedPositions.size() > nonItemSiteCount ? 0 : site.inlineFrame));
    }
    for (uint32_t i = 0; i < context.sitePositions.size(); ++i) {
        if (FunctionID callee = context.sitePositions[i].parameterCheckCallee)
            info.parameterCheckSites.append({ i + 1, callee.index(), positionIn(program.at(callee), program.at(callee).position, 0) });
    }
    for (unsigned frame = 0; frame < function.inlineFrames.size(); ++frame)
        info.inlineFrames.append({ function.inlineFrames[frame].parent, context.inlineCallSites[frame], function.inlineFrames[frame].callee.index(), function.inlineFrames[frame].isTailCall, function.inlineFrames[frame].isConstruction });
    if (function.inlineFrames.isEmpty() && !context.framesDroppedByTailCalls.isEmpty())
        info.inlineFrames.append({ 0, 0, FunctionID().index(), false, false });
    for (uint32_t left : context.framesDroppedByTailCalls) {
        auto entrypoint = program.genericEntrypoints.find((left ? function.inlineFrames[left].callee : id).index());
        info.inlineFrames.append({ left, context.inlineCallSites[info.inlineFrames.size()], (entrypoint == program.genericEntrypoints.end() ? program.entries[0] : entrypoint->value).index(), true, false });
    }
    MacroAssemblerCodeRef<JSEntryPtrTag> codeRef = FINALIZE_CODE_IF(Options::dumpAOTDisassembly() && result.mayBeDumped, linkBuffer, JSEntryPtrTag, nullptr, "AOT code");
    dropPaddingAtEnd(info, start, proc.code());
    if (Options::aotMapFilePath()) [[unlikely]]
        result.bytesByOperation = makeString(bytesByOperation(proc.pcToOriginMap(), linkBuffer, info.codeSize), context.literalsForMap.isEmpty() ? ""_s : "\nV"_s, context.literalsForMap.toString());
    result.bytes.append(std::span { static_cast<const uint8_t*>(start), static_cast<size_t>(info.codeSize) });
    auto offsetOf = [&](CCallHelpers::Label label) {
        return static_cast<uint32_t>(static_cast<uint8_t*>(linkBuffer.locationOf<JSEntryPtrTag>(label).untaggedPtr()) - static_cast<uint8_t*>(start));
    };
    result.rareCodeStart = std::min<uint32_t>(offsetOf(proc.code().rareCodeStart()), info.codeSize);
    for (auto& data : context.dataInCode)
        result.dataInCode.append({ offsetOf(data.start), data.numberOfWords, false });
    for (auto [table, numberOfEntries] : proc.code().tablesInCode())
        result.dataInCode.append({ offsetOf(table), numberOfEntries, true });
    std::ranges::sort(result.dataInCode, { }, &DataInCode::offset);
#if CPU(X86_64)
    result.instructions = instructionRecords(result.bytes.span(), result.dataInCode.span());
#endif
    result.hasCodeAnnotations = true;
    result.info = WTF::move(info);
    result.rareCodeMayBeOutlined = true;
    result.allCodeMayBeOutlined = program.isColdInCallGraph(id);
    hasFrame = !context.hasNoFrame(proc.code());
    return true;
}

bool compile(VM&, const Program& program, FunctionID id, CompiledCode& result)
{
    bool hasFrame = false;
    SplitFunction split;
    if (!compilePart(program, id, program.at(id), &split, hasFrame, result))
        return false;
    if (split.continuations.isEmpty())
        return true;
    bool mayBeDumped = result.mayBeDumped;
    auto compileParts = [&] {
        if (!compilePart(program, id, split.entry, nullptr, hasFrame, result) || hasFrame)
            return false;
        for (auto& continuation : split.continuations) {
            CompiledCode code;
            code.mayBeDumped = mayBeDumped;
            if (!compilePart(program, id, continuation, nullptr, hasFrame, code))
                return false;
            result.continuations.append(WTF::move(code));
        }
        return true;
    };
    if (compileParts())
        return true;
    result = CompiledCode { };
    result.mayBeDumped = mayBeDumped;
    return compilePart(program, id, program.at(id), nullptr, hasFrame, result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
