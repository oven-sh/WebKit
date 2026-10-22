/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRBuilder.h"

#if ENABLE(AOT)

#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static bool isUnterminated(const Block& block)
{
    const Terminator& terminator = block.terminator;
    return terminator.kind == TerminatorKind::Unreachable && terminator.targets.isEmpty();
}

template<typename List>
static void removeAtIndexes(List& list, const BitVector& indexes)
{
    size_t numberOfRemainingElements = 0;
    for (size_t index = 0; index < list.size(); ++index) {
        if (!indexes.get(index))
            list[numberOfRemainingElements++] = list[index];
    }
    RELEASE_ASSERT(list.size() - numberOfRemainingElements == indexes.bitCount());
    list.shrink(numberOfRemainingElements);
}

static void setOperands(Instruction& instruction, std::initializer_list<ValueID> operands)
{
    uint32_t operandIndex = 0;
    for (ValueID operand : operands) {
        if (operandIndex < instruction.numberOfNamedOperands())
            instruction.setOperand(operandIndex, operand);
        else
            instruction.appendVariadicOperand(operand);
        ++operandIndex;
    }
}

// Selection drops what is only expected of a value.
static bool keepsExpectedTypes(const Program& program)
{
    return program.form() < IRForm::Selected;
}

static bool canReceiveEdgeResults(BlockID id, const Block& block)
{
    return id != entryBlock && !block.isHandlerBlock() && !block.parameters.isEmpty();
}

static ValueID appendEdgeResult(IRBuilder& builder, Edge edge, uint32_t proof)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    RELEASE_ASSERT(!edge.isHandlerEdge());
    const Terminator& terminator = function.at(edge.from).terminator;
    RELEASE_ASSERT(targetResultsOf(terminator.kind, edge.targetIndex, terminator.targets.size()) == TargetResults::Proofs);

    Target& target = editTarget(function, edge, BlockEdit::Arguments);
    EdgeResultPosition position { edge.from, edge.targetIndex, static_cast<uint32_t>(target.proofs.size()) };
    target.proofs.append(proof);

    Value parameter(Opcode::Parameter);
    parameter.setOrigin(builder.origin());
    parameter.type = typeOnEdge(program, function, position, builder.types());
    if (keepsExpectedTypes(program))
        parameter.setExpected(expectedTypeOnEdge(program, function, position, parameter.type, builder.types()));
    function.add(WTF::move(parameter));
    ValueID id(function.values.size() - 1);
    function.edit(target.block, BlockEdit::Parameters).parameters.append(id);
    return id;
}

template<typename Key>
static Vector<Key> appendCaseTargets(Terminator& terminator, Vector<std::pair<Key, Target>>&& cases, Target&& otherwise)
{
    Vector<Key> keys;
    keys.reserveInitialCapacity(cases.size());
    terminator.targets.reserveCapacity(cases.size() + 1);
    for (auto& [key, target] : cases) {
        keys.append(key);
        terminator.targets.append(WTF::move(target));
    }
    terminator.targets.append(WTF::move(otherwise));
    return keys;
}

IRBuilder::IRBuilder(const AnalysisContext& context, Function& function, TypeArena& types, FunctionAnalyses* analyses)
    : m_context(context)
    , m_function(function)
    , m_types(types)
    , m_analyses(analyses)
{
    RELEASE_ASSERT(!analyses || &analyses->function() == &function);
}

IRBuilder::~IRBuilder()
{
    RELEASE_ASSERT(m_insertions.isEmpty());
}

void IRBuilder::setHandler(std::optional<Target> handler)
{
    m_handler = WTF::move(handler);
}

void IRBuilder::moveToEndOf(BlockID block)
{
    RELEASE_ASSERT(isUnterminated(m_function.at(block)));
    m_block = block;
}

BlockID IRBuilder::addBlock(BlockFrequency frequency)
{
    RELEASE_ASSERT(!m_handler || m_handler->block.index() < m_function.blocks.size());
    Block block;
    block.frequency = frequency;
    block.handler = m_handler;
    BlockID id = m_function.addBlock(WTF::move(block));
    didChangeControlFlow(std::span<const BlockID> { });
    return id;
}

BlockID IRBuilder::addHandlerBlock(BlockRole role, ValueID subject, BlockFrequency frequency)
{
    RELEASE_ASSERT(role != BlockRole::Ordinary);
    BlockID id = addBlock(frequency);
    Block& block = m_function.edit(id, BlockEdit::Rest);
    block.role = role;
    block.subject = subject;
    return id;
}

Vector<Edge, 8> IRBuilder::incomingEdgesOf(BlockID block)
{
    Vector<Edge, 8> edges;
    if (m_analyses) {
        edges.append(m_analyses->cfg().incomingEdges(block));
        for (Edge edge : m_analyses->edgesFromUnreachableBlocks()) {
            if (targetOf(m_function, edge).block == block)
                edges.append(edge);
        }
    } else {
        for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
            forEachOutgoingEdge(m_function, BlockID(index), [&](Edge edge, const Target& target) {
                if (target.block == block)
                    edges.append(edge);
            });
        }
    }
    return edges;
}

uint32_t IRBuilder::numberOfParametersForEdgeResults(BlockID block)
{
    const Block& contents = m_function.at(block);
    if (!canReceiveEdgeResults(block, contents))
        return 0;

    uint32_t largest = 0;
    for (Edge edge : incomingEdgesOf(block)) {
        if (!edge.isHandlerEdge())
            largest = std::max(largest, numberOfEdgeResults(m_function, edge.from, edge.targetIndex));
    }
    RELEASE_ASSERT(largest <= contents.parameters.size());
    return largest;
}

uint32_t IRBuilder::numberOfSharedExplicitParameters(BlockID block)
{
    return m_function.at(block).parameters.size() - numberOfParametersForEdgeResults(block);
}

ValueID IRBuilder::addParameter(BlockID block, TypeID type)
{
    return addParameter(block, type, numberOfParametersForEdgeResults(block));
}

ValueID IRBuilder::addParameter(BlockID block, TypeID type, uint32_t numberOfParametersForEdgeResults)
{
    Value parameter(Opcode::Parameter);
    parameter.type = type;
    parameter.setOrigin(m_origin);
    ValueID id = addValue(WTF::move(parameter));
    auto& parameters = m_function.edit(block, BlockEdit::Parameters).parameters;
    RELEASE_ASSERT(numberOfParametersForEdgeResults <= parameters.size());
    parameters.insert(parameters.size() - numberOfParametersForEdgeResults, id);
    didAddDetachedValue(id);
    didChangeBlock(block);
    return id;
}

void IRBuilder::appendArgument(Edge edge, ValueID value)
{
    appendArgument(edge, value, numberOfParametersForEdgeResults(targetOf(m_function, edge).block));
}

void IRBuilder::appendArgument(Edge edge, ValueID value, uint32_t numberOfParametersForEdgeResults)
{
    Target& target = editTarget(m_function, edge, BlockEdit::Arguments);
    size_t numberOfParameters = m_function.at(target.block).parameters.size();
    size_t numberOfResults = edge.isHandlerEdge() ? 0 : numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
    RELEASE_ASSERT(target.arguments.size() + numberOfResults < numberOfParameters);
    size_t missing = numberOfParameters - numberOfResults - target.arguments.size();
    size_t shared = numberOfParameters - numberOfParametersForEdgeResults;
    RELEASE_ASSERT(missing <= shared);
    target.arguments.insert(shared - missing, value);
    didChangeBlock(edge.from);
}

ValueID IRBuilder::addEdgeResult(Edge edge, uint32_t proof)
{
    ASSERT(incomingEdgesOf(targetOf(m_function, edge).block).size() == 1);

    ValueID id = appendEdgeResult(*this, edge, proof);
    didAddDetachedValue(id);
    didChangeBlock(edge.from);
    didChangeBlock(targetOf(m_function, edge).block);
    return id;
}

void IRBuilder::removeParameters(BlockID block, const BitVector& parameterIndexes)
{
    if (parameterIndexes.isEmpty())
        return;

    auto& parameters = m_function.edit(block, BlockEdit::Parameters).parameters;
    Vector<BlockID, 8> changedBlocks { block };
    for (Edge edge : incomingEdgesOf(block)) {
        Target& target = editTarget(m_function, edge, BlockEdit::Arguments);
        changedBlocks.append(edge.from);
        size_t numberOfArguments = target.arguments.size();
        size_t numberOfResults = edge.isHandlerEdge() ? 0 : numberOfEdgeResults(m_function, edge.from, edge.targetIndex);
        RELEASE_ASSERT(numberOfArguments + numberOfResults == parameters.size());
        if (!numberOfResults) {
            removeAtIndexes(target.arguments, parameterIndexes);
            continue;
        }

        BitVector argumentIndexes(numberOfArguments);
        BitVector proofIndexes(numberOfResults);
        for (size_t index : parameterIndexes) {
            if (index < numberOfArguments)
                argumentIndexes.set(index);
            else
                proofIndexes.set(index - numberOfArguments);
        }
        removeAtIndexes(target.arguments, argumentIndexes);
        if (proofIndexes.isEmpty())
            continue;

        const Terminator& terminator = m_function.at(edge.from).terminator;
        RELEASE_ASSERT(targetResultsOf(terminator.kind, edge.targetIndex, terminator.targets.size()) == TargetResults::Proofs);
        removeAtIndexes(target.proofs, proofIndexes);
    }

    for (size_t index : parameterIndexes)
        becomeUndefined(m_function, parameters[index]);
    removeAtIndexes(parameters, parameterIndexes);
    for (BlockID changedBlock : changedBlocks)
        didChangeBlock(changedBlock);
}

Value IRBuilder::make(Opcode opcode, std::initializer_list<ValueID> operands)
{
    Value value(opcode);
    RELEASE_ASSERT(operands.size() <= value.numberOfNamedOperands() || hasVariadicOperand(opcode));
    setOperands(value, operands);
    return value;
}

Terminator IRBuilder::makeTerminator(TerminatorKind kind, std::initializer_list<ValueID> operands)
{
    Terminator terminator(kind);
    RELEASE_ASSERT(operands.size() <= terminator.numberOfNamedOperands() || hasVariadicOperand(kind));
    setOperands(terminator, operands);
    return terminator;
}

ValueID IRBuilder::addValue(Value&& value)
{
    m_function.add(WTF::move(value));
    return ValueID(m_function.values.size() - 1);
}

void IRBuilder::prepare(Value& value)
{
    RELEASE_ASSERT(value.opcode != Opcode::Parameter);
    value.setOrigin(m_origin);
    TypingMemo* memo = m_analyses ? m_analyses->typingMemo() : nullptr;
    value.type = resultTypeOf(m_context.program, m_function, value, m_types, memo);
    if (keepsExpectedTypes(m_context.program) && !value.expected())
        value.setExpected(expectedTypeOf(m_context.program, m_function, value, m_types, memo));
}

ValueID IRBuilder::addDetached(Value&& value)
{
    prepare(value);
    ValueID id = addValue(WTF::move(value));
    didAddDetachedValue(id);
    return id;
}

ValueID IRBuilder::append(Value&& value)
{
    prepare(value);
    return appendPrepared(WTF::move(value));
}

ValueID IRBuilder::appendWithType(Value&& value, TypeID type)
{
    RELEASE_ASSERT(resultRuleOf(value.opcode) == ResultRule::Declared);
    value.setOrigin(m_origin);
    value.type = type;
    return appendPrepared(WTF::move(value));
}

ValueID IRBuilder::appendPrepared(Value&& value)
{
    RELEASE_ASSERT(m_block);
    ValueID id = addValue(WTF::move(value));
    m_function.edit(m_block, BlockEdit::Values).values.append(id);
    didAddDetachedValue(id);
    didListValues(m_block, singleElementSpan(id));
    return id;
}

ValueID IRBuilder::appendUpcast(ValueID operand, TypeID type)
{
    if (m_function.at(operand).type == type)
        return operand;
    Value upcast = make(Opcode::Upcast, { operand });
    upcast.setCastType(type);
    return append(WTF::move(upcast));
}

ValueID IRBuilder::appendTypeTest(ValueID value, TypeID testedType)
{
    Value test = make(Opcode::TypeTest, { value });
    test.setTestedType(testedType);
    return append(WTF::move(test));
}

ValueID IRBuilder::appendTypeTest(ValueID value, const Type& tested)
{
    return appendTypeTest(value, m_types.intern(tested));
}

ValueID IRBuilder::appendAllGuards(std::span<const ValueID> guards)
{
    if (guards.size() == 1)
        return guards.front();
    Value value(Opcode::AllGuards);
    value.setGuardsOperands(guards);
    return append(WTF::move(value));
}

ValueID IRBuilder::appendUndefined()
{
    return append(make(Opcode::Undefined));
}

ValueID IRBuilder::appendBoolean(bool boolean)
{
    Value value = make(Opcode::Boolean);
    value.setBoolean(boolean);
    return append(WTF::move(value));
}

ValueID IRBuilder::appendInteger(int64_t integer)
{
    Value value = make(Opcode::Integer);
    value.setInteger(integer);
    return append(WTF::move(value));
}

ValueID IRBuilder::appendDouble(double number)
{
    Value value = make(Opcode::Double);
    value.setNumber(number);
    return append(WTF::move(value));
}

ValueID IRBuilder::insertBefore(ProgramPoint point, Value&& value)
{
    const Block& block = m_function.at(point.block);
    RELEASE_ASSERT(point.index <= block.values.size());
    prepare(value);
    ValueID id = addValue(WTF::move(value));
    m_insertions.insert(point, id);
    didAddDetachedValue(id);
    return id;
}

void IRBuilder::applyInsertions()
{
    if (m_insertions.isEmpty()) {
        didChangeValues();
        return;
    }
    m_insertions.execute(m_function, [&](BlockID block, std::span<const ValueID> values) {
        didListValues(block, values);
    });
}

void IRBuilder::eraseValues(BlockID id, const BitVector& indexes)
{
    RELEASE_ASSERT(m_insertions.isEmpty());
    Block& block = m_function.edit(id, BlockEdit::Values);
    Vector<ValueID, 8> erased;
    for (size_t index : indexes)
        erased.append(block.values[index]);
    removeAtIndexes(block.values, indexes);
    didEraseValues(id, erased.span());
}

Terminator& IRBuilder::openTerminator()
{
    RELEASE_ASSERT(m_block);
    Block& block = m_function.edit(m_block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
    RELEASE_ASSERT(isUnterminated(block));
    return block.terminator;
}

void IRBuilder::closeBlock()
{
    Terminator& terminator = m_function.edit(m_block, BlockEdit::Terminator).terminator;
    for (const Target& target : terminator.targets)
        RELEASE_ASSERT(target.block != entryBlock && target.block.index() < m_function.blocks.size());
    terminator.setOrigin(m_origin);
    BlockID closed = std::exchange(m_block, BlockID());
    didChangeControlFlow(singleElementSpan(closed));
}

void IRBuilder::emitJump(BlockID block, Arguments&& arguments)
{
    Terminator& jump = openTerminator();
    jump.become(TerminatorKind::Jump);
    jump.targets.append(Target { block, WTF::move(arguments), { } });
    closeBlock();
}

void IRBuilder::emitBranch(ValueID condition, Target&& taken, Target&& notTaken)
{
    Terminator& branch = openTerminator();
    branch.become(TerminatorKind::Branch);
    branch.setConditionOperand(condition);
    branch.targets.append(WTF::move(taken));
    branch.targets.append(WTF::move(notTaken));
    closeBlock();
}

IRBuilder::BranchBlocks IRBuilder::emitBranch(ValueID condition, BlockFrequency taken, BlockFrequency notTaken)
{
    RELEASE_ASSERT(m_block);
    BlockFrequency frequency = m_function.at(m_block).frequency;
    BranchBlocks blocks;
    blocks.taken = addBlock(rarerOf(taken, frequency));
    blocks.notTaken = addBlock(rarerOf(notTaken, frequency));
    emitBranch(condition, Target { blocks.taken, { }, { } }, Target { blocks.notTaken, { }, { } });
    moveToEndOf(blocks.taken);
    return blocks;
}

// What stands behind a test need not name what the test found: the body of the one function that the callee may be needs nothing of the callee. Passes move, merge and remove tests and the jumps
// that lead past them. So in a validating build the code behind a test of a value begins with one operation that asks the value again. No pass counts or weighs that operation.
void IRBuilder::appendValidationOfTestedValue(ValueID test)
{
    if (!context().build.validates())
        return;
    const Value& tested = m_function.at(test);
    TypeID type;
    if (tested.opcode == Opcode::TypeTest)
        type = tested.testedType();
    else if (tested.opcode == Opcode::IsClosureOf)
        type = types().intern(Type::forFunction(tested.function()));
    else
        return;
    Value validation = make(Opcode::ValidateType, { tested.valueOperand() });
    validation.setValidatedType(type);
    append(WTF::move(validation));
}

IRBuilder::ProvenBranch IRBuilder::emitBranchWithProof(ValueID test, uint32_t provenOperand, BlockFrequency taken, BlockFrequency notTaken)
{
    Proof proof = provesOf(m_function.at(test).opcode);
    RELEASE_ASSERT(proof != Proof::None);
    RELEASE_ASSERT(!provesForStretch(proof) || !provenOperand);

    BlockID from = m_block;
    BranchBlocks blocks = emitBranch(test, taken, notTaken);
    ProvenBranch branch { blocks.taken, blocks.notTaken, appendEdgeResult(*this, Edge { from, 0 }, provenOperand), ValueID() };
    didAddDetachedValue(branch.provenOnTaken);
    didChangeBlock(blocks.taken);
    if (!provesForStretch(proof)) {
        branch.provenOnNotTaken = appendEdgeResult(*this, Edge { from, 1 }, provenOperand);
        didAddDetachedValue(branch.provenOnNotTaken);
        didChangeBlock(blocks.notTaken);
    }
    didChangeBlock(from);
    appendValidationOfTestedValue(test);
    return branch;
}

IRBuilder::ProvenBranch IRBuilder::emitBranchWithProof(ValueID test, uint32_t provenOperand, BlockFrequency taken, BlockFrequency notTaken, ProvenBranchTargets&& targets)
{
    Proof proof = provesOf(m_function.at(test).opcode);
    RELEASE_ASSERT(proof != Proof::None);
    RELEASE_ASSERT(!provesForStretch(proof) || !provenOperand);
    RELEASE_ASSERT(!targets.whenNotTaken || provesForStretch(proof));

    BlockID from = m_block;
    BlockFrequency frequency = m_function.at(from).frequency;
    BlockID takenBlock = targets.enteredOnlyWhenTaken ? targets.enteredOnlyWhenTaken : addBlock(rarerOf(taken, frequency));
    Target whenNotTaken = targets.whenNotTaken ? WTF::move(*targets.whenNotTaken) : Target { addBlock(rarerOf(notTaken, frequency)), { }, { } };
    BlockID notTakenBlock = whenNotTaken.block;
    emitBranch(test, Target { takenBlock, { }, { } }, WTF::move(whenNotTaken));
    ProvenBranch branch { takenBlock, notTakenBlock, appendEdgeResult(*this, Edge { from, 0 }, provenOperand), ValueID() };
    if (!provesForStretch(proof))
        branch.provenOnNotTaken = appendEdgeResult(*this, Edge { from, 1 }, provenOperand);
    didChangeValues();
    return branch;
}

void IRBuilder::emitSwitch(ValueID subject, Vector<std::pair<int64_t, Target>>&& cases, Target&& otherwise)
{
    Terminator& terminator = openTerminator();
    terminator.become(TerminatorKind::Switch);
    terminator.setSubjectOperand(subject);
    Vector<int64_t> keys = appendCaseTargets(terminator, WTF::move(cases), WTF::move(otherwise));
    terminator.setCases(keys.span());
    closeBlock();
}

void IRBuilder::emitSwitchString(ValueID subject, Vector<std::pair<StringID, Target>>&& cases, Target&& otherwise)
{
    Terminator& terminator = openTerminator();
    terminator.become(TerminatorKind::SwitchString);
    terminator.setSubjectOperand(subject);
    Vector<StringID> keys = appendCaseTargets(terminator, WTF::move(cases), WTF::move(otherwise));
    terminator.setStrings(keys.span());
    closeBlock();
}

void IRBuilder::emitSwitchOnShape(ValueID subject, Vector<std::pair<LayoutSetID, Target>>&& cases, Target&& otherwise)
{
    Terminator& terminator = openTerminator();
    terminator.become(TerminatorKind::SwitchOnShape);
    terminator.setSubjectOperand(subject);
    Vector<LayoutSetID> keys = appendCaseTargets(terminator, WTF::move(cases), WTF::move(otherwise));
    terminator.setLayoutSets(keys.span());
    closeBlock();
}

void IRBuilder::emitReturn(std::optional<ValueID> value)
{
    Terminator& terminator = openTerminator();
    terminator.become(TerminatorKind::Return);
    if (value)
        terminator.setValueOperand(*value);
    closeBlock();
}

void IRBuilder::emitThrow(ValueID value)
{
    Terminator& terminator = openTerminator();
    terminator.become(TerminatorKind::Throw);
    terminator.setValueOperand(value);
    closeBlock();
}

void IRBuilder::emitRethrow(ValueID exception)
{
    Terminator& terminator = openTerminator();
    terminator.become(TerminatorKind::Rethrow);
    terminator.setExceptionOperand(exception);
    closeBlock();
}

void IRBuilder::emitUnreachable()
{
    openTerminator();
    closeBlock();
}

void IRBuilder::emitTerminator(Terminator&& terminator)
{
    Terminator& current = openTerminator();
    std::optional<uint32_t> numberOfFixedTargets = numberOfFixedTargetsOf(terminator.kind);
    RELEASE_ASSERT(terminator.targets.size() == (numberOfFixedTargets ? *numberOfFixedTargets : terminator.numberOfCases() + 1));
    current = WTF::move(terminator);
    closeBlock();
}

void IRBuilder::didChangeValues()
{
    if (m_analyses)
        m_analyses->didChangeValues();
}

void IRBuilder::didChangeControlFlow()
{
    if (m_analyses)
        m_analyses->didChangeControlFlow();
}

void IRBuilder::didAddDetachedValue(ValueID value)
{
    if (m_analyses)
        m_analyses->didAddDetachedValue(value);
}

void IRBuilder::didListValues(BlockID block, std::span<const ValueID> values)
{
    if (m_analyses)
        m_analyses->didListValues(block, values);
}

void IRBuilder::didEraseValues(BlockID block, std::span<const ValueID> erased)
{
    if (m_analyses)
        m_analyses->didEraseValues(block, erased);
}

void IRBuilder::didChangeBlock(BlockID block)
{
    if (m_analyses)
        m_analyses->didChangeBlock(block);
}

void IRBuilder::didChangeControlFlow(std::span<const BlockID> changedBlocks)
{
    if (m_analyses)
        m_analyses->didChangeControlFlow(changedBlocks);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
