/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTBitVector.h"
#include "AOTCallingConventions.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTRepresentation.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSchemaPayloadPositions.h"
#include "AOTSort.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTTypeText.h"
#include "MathCommon.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesHandedTheirObjectByTypeVerifier = []() consteval {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (flowOf(opcode) == ValueFlowEdgeClass::Allocates)
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr bool isTypedByResultRuleForTypeVerifier(Opcode opcode)
{
    return resultRuleOf(opcode) != ResultRule::None && opcode != Opcode::Parameter && opcode != Opcode::Narrowed && !opcodesThatStateTypes.contains(opcode);
}

static constexpr OpcodeSet operatorsTriedOnNumbersByTypeVerifier {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::Negate, Opcode::Increment, Opcode::Decrement,
    Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift
};

static constexpr OpcodeSet accessesByKeyOfAnyKindForTypeVerifier { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete };

static double numberThatOperatorMakesForTypeVerifier(Opcode opcode, double left, double right)
{
    switch (opcode) {
    case Opcode::Add:
        return left + right;
    case Opcode::Sub:
        return left - right;
    case Opcode::Mul:
        return left * right;
    case Opcode::Div:
        return left / right;
    case Opcode::Mod:
        return std::fmod(left, right);
    case Opcode::Negate:
        return -left;
    case Opcode::Increment:
        return left + 1;
    case Opcode::Decrement:
        return left - 1;
    case Opcode::BitAnd:
        return toInt32(left) & toInt32(right);
    case Opcode::BitOr:
        return toInt32(left) | toInt32(right);
    case Opcode::BitXor:
        return toInt32(left) ^ toInt32(right);
    case Opcode::BitNot:
        return ~toInt32(left);
    case Opcode::LShift:
        return static_cast<int32_t>(toUInt32(left) << (toUInt32(right) & 31));
    case Opcode::RShift:
        return toInt32(left) >> (toUInt32(right) & 31);
    case Opcode::URShift:
        return toUInt32(left) >> (toUInt32(right) & 31);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Vector<double, 5> integersTriedByTypeVerifier(const Type& operand)
{
    Vector<double, 5> tried;
    if (!(operand.primitives & Primitive::Integer))
        return tried;
    Range written = operand.integers.intersectWith(Range::exact());
    for (int64_t integer : { written.minimum, written.maximum, int64_t { -1 }, int64_t { 0 }, int64_t { 1 } }) {
        if (written.contains(integer))
            tried.append(static_cast<double>(integer));
    }
    return tried;
}

namespace {

class PositionsOfTypeVerifier final : public PositionTypes {
    WTF_MAKE_NONCOPYABLE(PositionsOfTypeVerifier);
public:
    explicit PositionsOfTypeVerifier(const PositionTypes& positionsOfWalk)
        : m_positionsOfWalk(positionsOfWalk)
    {
    }

    void setFactsOfRecordBeingTyped(std::span<const FactUse> facts) { m_factsOfRecordBeingTyped = facts; }

    TypeID variable(TypeLevel level, VariableID variable) const final { return m_positionsOfWalk.variable(level, variable); }
    TypeID environmentField(TypeLevel level, LayoutID layout, uint32_t field) const final { return m_positionsOfWalk.environmentField(level, layout, field); }
    bool environmentLiesAmongVariables(LayoutID layout) const final { return m_positionsOfWalk.environmentLiesAmongVariables(layout); }
    TypeID slot(TypeLevel level, LayoutID layout, uint32_t field) const final { return m_positionsOfWalk.slot(level, layout, field); }
    TypeID readOfSlot(TypeLevel level, LayoutID layout, uint32_t field) const final { return m_positionsOfWalk.readOfSlot(level, layout, field); }
    TypeID halfOfAccessorOfField(TypeLevel level, LayoutID layout, uint32_t field, bool isSetter) const final { return m_positionsOfWalk.halfOfAccessorOfField(level, layout, field, isSetter); }
    std::optional<uint32_t> appendedField(LayoutID layout, PropertyKeyID key) const final { return m_positionsOfWalk.appendedField(layout, key); }
    bool collectQueriedLayouts(PropertyKeyID key, LayoutSetID layouts, bool includesInheritedWrites, Vector<LayoutID, 16>& asked) const final { return m_positionsOfWalk.collectQueriedLayouts(key, layouts, includesInheritedWrites, asked); }
    TypeID foundAboveObjectsOf(TypeLevel level, LayoutID layout, PropertyKeyID key) const final { return m_positionsOfWalk.foundAboveObjectsOf(level, layout, key); }
    TypeID readByNameOffObjectsOf(TypeLevel level, LayoutID layout, PropertyKeyID key) const final { return m_positionsOfWalk.readByNameOffObjectsOf(level, layout, key); }
    LayoutID classAbove(LayoutID classLayout) const final { return m_positionsOfWalk.classAbove(classLayout); }
    TypeID property(TypeLevel level, ObjectID object, PropertyKeyID key) const final { return m_positionsOfWalk.property(level, object, key); }
    TypeID result(TypeLevel level, FunctionID function, uint32_t resultIndex) const final { return m_positionsOfWalk.result(level, function, resultIndex); }
    TypeID resultOfBodyThatNeverSuspends(TypeLevel level, FunctionID function) const final { return m_positionsOfWalk.resultOfBodyThatNeverSuspends(level, function); }
    TypeID entryParameter(TypeLevel level, FunctionID function, uint32_t entryParameterIndex) const final { return m_positionsOfWalk.entryParameter(level, function, entryParameterIndex); }
    bool entryParameterIsLeftOutOfCalls(FunctionID function, uint32_t entryParameterIndex) const final { return m_positionsOfWalk.entryParameterIsLeftOutOfCalls(function, entryParameterIndex); }
    TypeID capture(TypeLevel level, FunctionID function, uint32_t captureIndex) const final { return m_positionsOfWalk.capture(level, function, captureIndex); }
    TypeID captureOfObject(TypeLevel level, ObjectID object, uint32_t captureIndex) const final { return m_positionsOfWalk.captureOfObject(level, object, captureIndex); }
    TypeID scalarOfResult(TypeLevel level, FunctionID function, uint32_t index) const final { return m_positionsOfWalk.scalarOfResult(level, function, index); }
    TypeID scalarOfField(TypeLevel level, LayoutID layout, uint32_t field, Representation form) const final { return m_positionsOfWalk.scalarOfField(level, layout, field, form); }
    TypeID resultOfSelector(TypeLevel level, uint32_t selector) const final { return m_positionsOfWalk.resultOfSelector(level, selector); }
    TypeID resultOfTableRow(TypeLevel level, uint32_t selector, LayoutID receiver, bool& hasRow) const final { return m_positionsOfWalk.resultOfTableRow(level, selector, receiver, hasRow); }
    TypeID askedOfEachObject(QuestionOfEachObject question, TypeLevel level, ObjectSetID objects, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const final { return m_positionsOfWalk.askedOfEachObject(question, level, objects, word, askEachObject); }
    TypeID askedOfEachLayout(QuestionOfEachLayout question, TypeLevel level, LayoutSetID layouts, PropertyKeyID key, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const final { return m_positionsOfWalk.askedOfEachLayout(question, level, layouts, key, askEachLayout); }

    TypeID resultOfReadBeingTyped(TypeLevel level) const final
    {
        if (level != TypeLevel::Proven)
            return { };
        for (const FactUse& use : m_factsOfRecordBeingTyped) {
            if (use.kind == FactKind::ProvenContents && use.subjectKind == FactSubject::Kind::Operation && use.type)
                return use.type;
        }
        return { };
    }

    TypeID contentsBeingTyped(TypeLevel level) const final { return resultOfReadBeingTyped(level); }

    TypeID resultOfCallBeingTyped(TypeLevel level, uint32_t resultIndex) const final
    {
        if (level != TypeLevel::Proven || resultIndex)
            return { };
        for (const FactUse& use : m_factsOfRecordBeingTyped) {
            if (use.kind == FactKind::OperandSummaries && use.subjectKind == FactSubject::Kind::Operation && use.type)
                return use.type;
        }
        return { };
    }

private:
    const PositionTypes& m_positionsOfWalk;
    std::span<const FactUse> m_factsOfRecordBeingTyped;
};

class CodeTypeVerifier {
    WTF_MAKE_NONCOPYABLE(CodeTypeVerifier);
public:
    CodeTypeVerifier(const TypingContext&, const CallingConventions&, const Arrivals&, const Code&);

    String run();

private:
    static constexpr uint32_t notReached = UINT32_MAX;

    bool hasFailed() const { return !m_failure.isNull(); }
    void printPlace(PrintStream&, BlockID, ValueID record) const;
    void printTypeInFailure(PrintStream&, TypeID) const;
    ASCIILiteral howRecordDiffersFrom(TypeID recorded, TypeID other) const;
    void fail(BlockID, ValueID record, ASCIILiteral rule, ValueID other = { });
    void failWithTypes(BlockID, ValueID record, ASCIILiteral whatGivesOtherType, TypeID other, BlockID sourceOfEdge = { });

    static uint32_t numberOfEdgesLeaving(const Block& block) { return block.terminator.numberOfEdges + (block.handler ? 1 : 0); }
    static EdgeID edgeLeaving(const Block& block, uint32_t index) { return index < block.terminator.numberOfEdges ? block.terminator.edge(index) : block.handler; }
    template<typename Functor> void forEachClosedEdgeOfReachedBlocks(const Functor&) const;
    std::span<const EdgeID> incomingEdgesOf(BlockID) const;
    std::span<const FactUse> factsUsedBy(ValueID) const;
    bool someArgumentComesLater(BlockID) const;
    bool takesEntryParametersOfFunction() const;

    void groupFactUsesByRecord();
    void orderBlocks();
    void listIncomingEdges();

    void setDerivedType(ValueID, TypeID);
    bool collectTypesOfOperands(BlockID, ValueID user, const Instruction&, Vector<TypeID, 16>&);
    ObjectID objectMadeBy(const Operation&) const;
    TypeID typeOfNewTargetBehind(ValueID prototype) const;
    TypeID typeOfResultAskedFor(const Operation& resultAt);
    TypeID typeByResultRule(ValueID, const Operation&);
    void verifyTypeReadFromPayload(BlockID, ValueID, const Operation&);
    void verifyThatTypeHoldsWhatOperatorMakes(BlockID, ValueID, const Operation&);
    void deriveOperation(BlockID, ValueID);
    void deriveConstants();
    void deriveOperations(BlockID);
    void verifyEndBehindCheck(BlockID);

    TypeID typeLaidDownAtEntry(unsigned entryParameterIndex) const;
    TypeID entryParameterTypeFromArrivals(unsigned entryParameterIndex) const;
    void verifyEntryParameters(BlockID, const Block&);
    TypeID typeArrivingOn(EdgeID, BlockID target, ValueID parameter, uint32_t parameterIndex);
    bool isBackEdgeOf(BlockID head, EdgeID edge) const { return m_placesInOrder[m_code.at(edge).from.index()] >= m_placesInOrder[head.index()]; }
    std::optional<Range> changeSinceHeadOfLoop(ValueID parameterOfHead, ValueID);
    std::optional<Range> changeInTurnOfLoop(BlockID head, uint32_t parameterIndex);
    std::optional<int64_t> mostTurnsOfLoop(BlockID head);
    bool holdsWhatArrivesByCounting(BlockID head, uint32_t parameterIndex);
    void compareParametersWithArguments(BlockID);
    void deriveParameters(BlockID);

    struct AccessToHeapWord {
        uint64_t word;
        uint32_t place;
        ValueID record;
    };
    bool isStoreIntoHeapWord(const AccessToHeapWord& access) const { return m_code.at(access.record).opcode == Opcode::PutHeapFrameWord; }
    void noteAccessToHeapWord(BlockID, ValueID, const Operation&);
    void failAtReadOfHeapWord(const AccessToHeapWord& read, const AccessToHeapWord* store);
    void verifyStoresThatReachReadsOfHeapWord(std::span<const AccessToHeapWord> accessesToWord, TypeID typeThatReadsName);
    void verifyReadsOfHeapWords();

    const Program& m_program;
    TypeArena& m_types;
    const Code& m_code;
    const CallingConventions& m_conventions;
    const Arrivals& m_arrivals;
    PositionsOfTypeVerifier m_positions;
    TypingContext m_typing;
    TypingContext m_typingWithoutFacts;
    String m_failure;

    Vector<TypeID> m_derivedTypes;
    BitVector m_isDerived;
    Vector<uint32_t> m_firstFactUse;
    Vector<FactUse> m_factUses;
    Vector<uint32_t> m_placesInOrder;
    Vector<uint32_t> m_firstIncomingEdge;

    Vector<BlockID> m_blocksInOrder;
    Vector<EdgeID> m_incomingEdges;
    Vector<BlockID> m_blocksWhoseArgumentsComeLater;
    Vector<ValueID, 64> m_operationsOfBlock;
    Vector<TypeID, 16> m_operandTypes;
    Vector<TypeID, 16> m_operandTypesOfProducer;
    Vector<std::pair<ValueID, Range>, 16> m_changesSinceHeadOfLoop;
    Vector<std::pair<BlockID, std::optional<int64_t>>, 4> m_mostTurnsOfLoops;
    Vector<AccessToHeapWord> m_accessesToHeapWords;
    Vector<uint32_t> m_searchThatWentOnInFrontOfBlock;
    Vector<BlockID, 64> m_blocksToGoOnInFrontOf;
    uint32_t m_numberOfSearchesForStores { 0 };
};

CodeTypeVerifier::CodeTypeVerifier(const TypingContext& context, const CallingConventions& conventions, const Arrivals& arrivals, const Code& code)
    : m_program(context.program)
    , m_types(context.types)
    , m_code(code)
    , m_conventions(conventions)
    , m_arrivals(arrivals)
    , m_positions(context.positions)
    , m_typing { context.program, context.types, m_positions, TypeLevel::Proven }
    , m_typingWithoutFacts { context.program, context.types, PositionTypes::knowingNothing(), TypeLevel::Proven }
{
}

String CodeTypeVerifier::run()
{
    m_derivedTypes.fill(TypeID(), m_code.numberOfValues());
    m_isDerived.ensureSize(m_code.numberOfValues());
    groupFactUsesByRecord();
    orderBlocks();
    listIncomingEdges();

    deriveConstants();
    for (BlockID block : m_blocksInOrder) {
        if (hasFailed())
            break;
        deriveParameters(block);
        deriveOperations(block);
    }
    for (BlockID block : m_blocksWhoseArgumentsComeLater) {
        if (hasFailed())
            break;
        compareParametersWithArguments(block);
    }
    verifyReadsOfHeapWords();
    return m_failure;
}

void CodeTypeVerifier::printPlace(PrintStream& out, BlockID block, ValueID record) const
{
    out.print("f"_s, m_code.function().index(), " "_s, nameOf(m_code.role()));
    if (block)
        out.print(" b"_s, block.index());
    if (record)
        out.print(" v"_s, record.index(), " "_s, nameOf(m_code.at(record).opcode));
}

void CodeTypeVerifier::printTypeInFailure(PrintStream& out, TypeID type) const
{
    if (!type) {
        out.print("no type"_s);
        return;
    }
    TypeTextOptions options;
    options.mostMembersPrinted = 32;
    printType(out, TypeReader(m_types), type, options);
}

ASCIILiteral CodeTypeVerifier::howRecordDiffersFrom(TypeID recorded, TypeID other) const
{
    if (!recorded || !other)
        return "one of the two is missing"_s;
    bool recordLiesInside = m_types.isSubset(recorded, other);
    bool otherLiesInside = m_types.isSubset(other, recorded);
    if (recordLiesInside && otherLiesInside)
        return "both hold the same values and differ in their seeds or in what is written of contents"_s;
    if (recordLiesInside)
        return "the record claims more"_s;
    if (otherLiesInside)
        return "the record claims less"_s;
    return "neither holds the other"_s;
}

void CodeTypeVerifier::fail(BlockID block, ValueID record, ASCIILiteral rule, ValueID other)
{
    if (hasFailed())
        return;
    StringPrintStream out;
    printPlace(out, block, record);
    out.print(": "_s, rule);
    if (other)
        out.print(": v"_s, other.index());
    m_failure = out.toString();
}

void CodeTypeVerifier::failWithTypes(BlockID block, ValueID record, ASCIILiteral whatGivesOtherType, TypeID other, BlockID sourceOfEdge)
{
    if (hasFailed())
        return;
    TypeID recorded = m_code.at(record).type;
    StringPrintStream out;
    printPlace(out, block, record);
    out.print(": the record says "_s);
    printTypeInFailure(out, recorded);
    out.print(", "_s, whatGivesOtherType, " "_s);
    printTypeInFailure(out, other);
    if (sourceOfEdge)
        out.print(" on the edge from b"_s, sourceOfEdge.index());
    out.print(": "_s, howRecordDiffersFrom(recorded, other));
    m_failure = out.toString();
}

void CodeTypeVerifier::groupFactUsesByRecord()
{
    std::span<const FactUse> uses = m_code.factUses();
    uint32_t numberOfValues = m_code.numberOfValues();
    m_firstFactUse.fill(0, numberOfValues + 1);
    for (const FactUse& use : uses)
        ++m_firstFactUse[use.record.index() + 1];
    for (uint32_t index = 0; index < numberOfValues; ++index)
        m_firstFactUse[index + 1] += m_firstFactUse[index];

    m_factUses.fill(FactUse { }, uses.size());
    Vector<uint32_t> next(m_firstFactUse);
    for (const FactUse& use : uses)
        m_factUses[next[use.record.index()]++] = use;
}

std::span<const FactUse> CodeTypeVerifier::factsUsedBy(ValueID record) const
{
    uint32_t first = m_firstFactUse[record.index()];
    return m_factUses.span().subspan(first, m_firstFactUse[record.index() + 1] - first);
}

void CodeTypeVerifier::orderBlocks()
{
    struct Visit {
        BlockID block;
        uint32_t nextEdge;
    };

    uint32_t numberOfBlocks = m_code.numberOfBlocks();
    BitVector isSeen(numberOfBlocks);
    Vector<Visit, 64> stack;
    auto visitWhatIsReachedFrom = [&](BlockID first) {
        if (isSeen.quickSet(first.index()))
            return;
        stack.append(Visit { first, 0 });
        while (!stack.isEmpty()) {
            Visit& visit = stack.last();
            const Block& block = m_code.at(visit.block);
            if (visit.nextEdge == numberOfEdgesLeaving(block)) {
                m_blocksInOrder.append(visit.block);
                stack.removeLast();
                continue;
            }
            BlockID target = m_code.at(edgeLeaving(block, visit.nextEdge++)).to;
            if (target && !isSeen.quickSet(target.index()))
                stack.append(Visit { target, 0 });
        }
    };
    for (const EntryOfCode& entry : m_code.entries())
        visitWhatIsReachedFrom(entry.block);
    m_blocksInOrder.reverse();

    m_placesInOrder.fill(notReached, numberOfBlocks);
    for (uint32_t place = 0; place < m_blocksInOrder.size(); ++place)
        m_placesInOrder[m_blocksInOrder[place].index()] = place;
}

template<typename Functor>
void CodeTypeVerifier::forEachClosedEdgeOfReachedBlocks(const Functor& functor) const
{
    for (BlockID from : m_blocksInOrder) {
        const Block& block = m_code.at(from);
        uint32_t numberOfEdges = numberOfEdgesLeaving(block);
        for (uint32_t index = 0; index < numberOfEdges; ++index) {
            EdgeID edge = edgeLeaving(block, index);
            if (!m_code.at(edge).isOpen())
                functor(edge, m_code.at(edge));
        }
    }
}

void CodeTypeVerifier::listIncomingEdges()
{
    uint32_t numberOfBlocks = m_code.numberOfBlocks();
    m_firstIncomingEdge.fill(0, numberOfBlocks + 1);
    forEachClosedEdgeOfReachedBlocks([&](EdgeID, const Edge& edge) {
        ++m_firstIncomingEdge[edge.to.index() + 1];
    });
    for (uint32_t index = 0; index < numberOfBlocks; ++index)
        m_firstIncomingEdge[index + 1] += m_firstIncomingEdge[index];

    m_incomingEdges.fill(EdgeID(), m_firstIncomingEdge.last());
    Vector<uint32_t> next(m_firstIncomingEdge);
    forEachClosedEdgeOfReachedBlocks([&](EdgeID id, const Edge& edge) {
        m_incomingEdges[next[edge.to.index()]++] = id;
    });
}

std::span<const EdgeID> CodeTypeVerifier::incomingEdgesOf(BlockID block) const
{
    uint32_t first = m_firstIncomingEdge[block.index()];
    return m_incomingEdges.span().subspan(first, m_firstIncomingEdge[block.index() + 1] - first);
}

bool CodeTypeVerifier::someArgumentComesLater(BlockID block) const
{
    uint32_t place = m_placesInOrder[block.index()];
    for (EdgeID edge : incomingEdgesOf(block)) {
        if (m_placesInOrder[m_code.at(edge).from.index()] >= place)
            return true;
    }
    return false;
}

void CodeTypeVerifier::setDerivedType(ValueID record, TypeID type)
{
    m_derivedTypes[record.index()] = type;
    m_isDerived.quickSet(record.index());
}

bool CodeTypeVerifier::collectTypesOfOperands(BlockID block, ValueID user, const Instruction& instruction, Vector<TypeID, 16>& types)
{
    types.shrink(0);
    for (ValueID operand : instruction.operands()) {
        if (operand && !m_isDerived.quickGet(operand.index())) {
            fail(block, user, "an operand is used where its definition has not been typed"_s, operand);
            return false;
        }
        types.append(operand ? m_derivedTypes[operand.index()] : TypeID());
    }
    return true;
}

ObjectID CodeTypeVerifier::objectMadeBy(const Operation& operation) const
{
    if (!opcodesHandedTheirObjectByTypeVerifier.contains(operation.opcode) || !operation.type)
        return { };
    bool copiesObject = operation.opcode == Opcode::CopyObject || operation.opcode == Opcode::CopyObjectAndDefine || operation.opcode == Opcode::CopyTwoObjects;
    ObjectID made = copiesObject ? ordinaryObjectMadeByCopy(m_types, m_program, m_types.at(operation.type)) : singleObjectOf(m_types.at(operation.type));
    if (!made)
        return { };
    const ObjectTable& objects = m_types.objects();
    if (operation.opcode == Opcode::NewObject && objects.layoutOf(made) != operation.layout())
        return { };
    if (operation.opcode == Opcode::NewClosure && objects.closureFunction(made) != operation.function())
        return { };
    return made;
}

TypeID CodeTypeVerifier::typeOfNewTargetBehind(ValueID prototype) const
{
    ValueID root = prototype;
    while (opcodesThatNameTheirOperand.contains(m_code.at(root).opcode))
        root = m_code.at(root).operand(0);
    const Operation& read = m_code.at(root);
    if (read.opcode == Opcode::GetClassPrototype)
        return m_derivedTypes[read.classObjectOperand().index()];
    if (read.opcode != Opcode::GetConstructPrototype)
        return TypeTable::object;
    return m_derivedTypes[read.newTargetOperand().index()];
}

TypeID CodeTypeVerifier::typeOfResultAskedFor(const Operation& resultAt)
{
    if (!resultAt.resultsOperand())
        return { };
    const Operation& producer = m_code.at(resultAt.resultsOperand());
    if (!isTypedByResultRuleForTypeVerifier(producer.opcode))
        return { };
    if (producer.opcode == Opcode::CallDirect && returnsResultOfFunction(producer.role())) {
        if (TypeID scalar = m_positions.scalarOfResult(m_typing.level, producer.function(), resultAt.index()))
            return scalar;
    }
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, producer);
    if (numberOfResults && resultAt.index() >= *numberOfResults)
        return { };

    m_operandTypesOfProducer.shrink(0);
    for (ValueID operand : producer.operands())
        m_operandTypesOfProducer.append(operand ? m_derivedTypes[operand.index()] : TypeID());
    return resultTypeOf(m_typing, producer, OperandTypes { m_operandTypesOfProducer.span() }, ObjectID(), resultAt.index());
}

TypeID CodeTypeVerifier::typeByResultRule(ValueID id, const Operation& operation)
{
    if (opcodesThatMayHaveSeveralResults.contains(operation.opcode) && numberOfResultsOf(m_program, operation))
        return TypeTable::undefined;

    m_positions.setFactsOfRecordBeingTyped(factsUsedBy(id));
    if (operation.opcode == Opcode::ResultAt)
        m_operandTypes[OperandIndex::ResultAt::results] = typeOfResultAskedFor(operation);
    else if (operation.opcode == Opcode::NewObject && operation.prototypeOperand())
        m_operandTypes[OperandIndex::NewObject::prototype] = typeOfNewTargetBehind(operation.prototypeOperand());
    return resultTypeOf(m_typing, operation, OperandTypes { m_operandTypes.span() }, objectMadeBy(operation));
}

static constexpr OpcodeSet opcodesWhosePayloadIsHeldByTypeVerifier {
    Opcode::Unspecified, Opcode::GetHeapFrameWord, Opcode::GetStaticFieldOrAbsent, Opcode::GetValueAtSiteOrAbsent, Opcode::HasPropertyOrAbsent, Opcode::GetOwnValueUnderKeyOrAbsent, Opcode::GetKeyedValueOrAbsent, Opcode::GetValueUnderKeyOrAbsent, Opcode::GetElementInStorageOrAbsent, Opcode::GetWatchedBuiltinValueOrAbsent, Opcode::EmptyArgumentList, Opcode::NewFormatterWithPattern
};
static_assert([]() consteval {
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (!!PayloadWord::valueType(opcode) != opcodesWhosePayloadIsHeldByTypeVerifier.contains(opcode))
            return false;
    }
    return true;
}());

void CodeTypeVerifier::verifyTypeReadFromPayload(BlockID block, ValueID id, const Operation& operation)
{
    TypeID stated = operation.valueType();
    switch (operation.opcode) {
    case Opcode::Unspecified:
    case Opcode::GetHeapFrameWord:
        return;
    case Opcode::GetStaticFieldOrAbsent: {
        const LinkedStaticField* linked = operation.findsProperty() ? m_arrivals.facts().linked().findStaticField(operation.variable()) : nullptr;
        TypeID held = linked ? linked->type : TypeID(TypeTable::anything);
        if (operation.isKnownInitialized() && !operation.findsProperty()) {
            TypeID holder = m_types.withoutNullish(m_code.at(operation.classObjectOperand()).type);
            const LinkedNameOnFunction* name = m_arrivals.facts().linked().findNameOnFunction(singleFunctionOf(m_types, m_types.at(holder)), operation.key());
            held = name && name->isLaidByListedStores ? name->proven : held;
        }
        if (!operation.isKnownInitialized() && !operation.findsProperty()) {
            if (uint16_t primitives = m_arrivals.facts().linked().primitivesOfStaticField(operation.variable()))
                held = TypeArena::fromPrimitives(primitives);
            held = m_types.withPrimitives(held, Primitive::Absent);
        }
        if (!m_types.isSubset(held, stated))
            fail(block, id, "the payload says more of a static field than what is stored into it"_s);
        return;
    }
    case Opcode::GetValueAtSiteOrAbsent:
    case Opcode::HasPropertyOrAbsent:
    case Opcode::GetOwnValueUnderKeyOrAbsent:
    case Opcode::GetKeyedValueOrAbsent:
    case Opcode::GetValueUnderKeyOrAbsent:
    case Opcode::GetElementInStorageOrAbsent:
    case Opcode::GetWatchedBuiltinValueOrAbsent:
        if (!m_types.isSubset(m_types.withPrimitives(TypeTable::anything, Primitive::Absent), stated))
            fail(block, id, "the payload says more of what a lookup finds than that it is a value or absent"_s);
        return;
    case Opcode::EmptyArgumentList:
        if (!m_types.isSubset(stated, m_types.builtinClass(BuiltinClasses::Array)))
            fail(block, id, "the payload says that the empty list of arguments is no list"_s);
        return;
    case Opcode::NewFormatterWithPattern:
        if (!m_types.at(stated).isObject())
            fail(block, id, "the payload says that an allocation may give something other than an object"_s);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void CodeTypeVerifier::verifyThatTypeHoldsWhatOperatorMakes(BlockID block, ValueID id, const Operation& operation)
{
    bool isBinary = numberOfNamedOperandsOf(operation.opcode) == 2;
    if (!m_operandTypes[0] || (isBinary && !m_operandTypes[1]))
        return;
    Vector<double, 5> onLeft = integersTriedByTypeVerifier(m_types.at(m_operandTypes[0]));
    Vector<double, 5> onRight = isBinary ? integersTriedByTypeVerifier(m_types.at(m_operandTypes[1])) : Vector<double, 5> { 0 };
    for (double left : onLeft) {
        for (double right : onRight) {
            double made = numberThatOperatorMakesForTypeVerifier(operation.opcode, left, right);
            if (m_types.isSubset(m_types.number(made), operation.type))
                continue;
            StringPrintStream out;
            printPlace(out, block, id);
            out.print(": the record says "_s);
            printTypeInFailure(out, operation.type);
            out.print(isBinary ? ", and its operands may hold "_s : ", and its operand may hold "_s, static_cast<int64_t>(left));
            if (isBinary)
                out.print(" and "_s, static_cast<int64_t>(right));
            out.print(", of which the operator makes "_s);
            if (std::isnan(made))
                out.print("NaN"_s);
            else if (isTrackedInteger(made))
                out.print(static_cast<int64_t>(made));
            else
                out.print(made);
            m_failure = out.toString();
            return;
        }
    }
}

void CodeTypeVerifier::deriveOperation(BlockID block, ValueID id)
{
    const Operation& operation = m_code.at(id);
    if (!collectTypesOfOperands(block, id, operation, m_operandTypes))
        return;
    if (Operation::opcodesWithKeyIsIntegral.contains(operation.opcode) && operation.keyIsIntegral()) {
        ValueID key = accessesByKeyOfAnyKindForTypeVerifier.contains(operation.opcode) ? operation.propertyOperand() : operation.positionOperand();
        if (!m_types.at(m_derivedTypes[key.index()]).isNumberWithoutFraction())
            return fail(block, id, "a key is said to be integral, and its type holds more than numbers without a fraction"_s, key);
    }
    if (operatorsTriedOnNumbersByTypeVerifier.contains(operation.opcode) && operation.type) {
        verifyThatTypeHoldsWhatOperatorMakes(block, id, operation);
        if (hasFailed())
            return;
    }

    TypeID derived;
    if (operation.opcode == Opcode::Narrowed)
        derived = typeOfNarrowed(m_typing, m_code, id);
    else if (isTypedByResultRuleForTypeVerifier(operation.opcode))
        derived = typeByResultRule(id, operation);
    else if (resultRuleOf(operation.opcode) != ResultRule::None)
        return fail(block, id, "no rule types such an operation where it stands"_s);

    if (derived != operation.type)
        return failWithTypes(block, id, "its rule gives"_s, derived);
    setDerivedType(id, derived);
    if (PayloadWord::valueType(operation.opcode))
        verifyTypeReadFromPayload(block, id, operation);
    if (operation.opcode == Opcode::GetHeapFrameWord || operation.opcode == Opcode::PutHeapFrameWord)
        noteAccessToHeapWord(block, id, operation);
}

void CodeTypeVerifier::verifyEndBehindCheck(BlockID id)
{
    const Block& block = m_code.at(id);
    ValueID last = m_code.lastOperationOf(id);
    if (!last || !block.hasEnded() || block.terminator.kind != TerminatorKind::Unreachable)
        return;
    const Operation& check = m_code.at(last);
    if (check.opcode != Opcode::CheckProof || isNeverType(check.checkedType()))
        return;
    ValueID asked = check.valueOperand();
    if (m_code.at(asked).opcode == Opcode::Box)
        asked = m_code.at(asked).valueOperand();
    const Operation& value = m_code.at(asked);
    if (!m_code.isConstant(asked) && (value.representation == Representation::JSValue || holdsCells(value.representation)))
        return;
    TypeID said = m_code.isConstant(asked) || value.representation == Representation::Void ? value.type : widestTypeOf(value.representation);
    if (!said || !isNeverType(m_types.intersectionOf(said, check.checkedType())))
        fail(id, last, "a block ends behind a check that what comes might pass"_s, asked);
}

void CodeTypeVerifier::deriveConstants()
{
    for (uint32_t index = 0; index < m_code.numberOfValues() && !hasFailed(); ++index) {
        if (m_code.isConstant(ValueID(index)))
            deriveOperation({ }, ValueID(index));
    }
}

void CodeTypeVerifier::deriveOperations(BlockID block)
{
    m_operationsOfBlock.shrink(0);
    for (ValueID id = m_code.lastOperationOf(block); id; id = m_code.at(id).previous)
        m_operationsOfBlock.append(id);
    for (size_t index = m_operationsOfBlock.size(); index-- && !hasFailed();)
        deriveOperation(block, m_operationsOfBlock[index]);
    if (!hasFailed())
        verifyEndBehindCheck(block);
}

bool CodeTypeVerifier::takesEntryParametersOfFunction() const
{
    switch (m_code.role()) {
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::ModuleStart:
    case CodeRole::SlowBody:
        return true;
    case CodeRole::ParameterGuard:
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID CodeTypeVerifier::typeLaidDownAtEntry(unsigned entryParameterIndex) const
{
    FunctionID function = m_code.function();
    TypeID type;
    if (const DeclaredType* declared = m_program.at(function).declaredEntryParameter(entryParameterIndex))
        type = m_program.provenTypeOf(*declared);
    else
        type = declaredEntryTypeOf(m_typingWithoutFacts, function, entryParameterIndex);
    if (!type)
        type = TypeTable::anything;
    if (TypeID tested = m_code.role() == CodeRole::Body ? m_conventions.typeBehindGuard(function, entryParameterIndex) : TypeID()) {
        TypeID passed = m_types.intersectionOf(type, tested);
        type = isNeverType(passed) ? tested : passed;
    }
    return type;
}

TypeID CodeTypeVerifier::entryParameterTypeFromArrivals(unsigned entryParameterIndex) const
{
    FunctionID function = m_code.function();
    TypeID believed = m_arrivals.facts().typeBelievedOfEntryParameter(m_types, function, entryParameterIndex, m_arrivals.provenAt(m_types, function, entryParameterIndex));
    if (!m_types.at(believed).isJavaScriptValue())
        return { };
    if (TypeID tested = m_code.role() == CodeRole::Body ? m_conventions.typeBehindGuard(function, entryParameterIndex) : TypeID()) {
        TypeID passed = m_types.intersectionOf(believed, tested);
        believed = isNeverType(passed) ? tested : passed;
    }
    return believed;
}

void CodeTypeVerifier::verifyEntryParameters(BlockID id, const Block& block)
{
    bool takesParametersOfFunction = takesEntryParametersOfFunction();
    unsigned numberOfEntryParameters = m_program.at(m_code.function()).numberOfEntryParameters();
    std::span<const ValueID> parameters = block.parameters();

    for (uint32_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        const Operation& record = m_code.at(parameter);

        TypeID saidByFact;
        for (const FactUse& use : factsUsedBy(parameter)) {
            if (use.subjectKind == FactSubject::Kind::EntryParameter && use.type) {
                saidByFact = use.type;
                break;
            }
        }
        if (saidByFact) {
            if (saidByFact != record.type)
                return failWithTypes(id, parameter, "the fact that it leans on says"_s, saidByFact);
            if (takesParametersOfFunction && index < numberOfEntryParameters) {
                TypeID askedAgain = entryParameterTypeFromArrivals(index);
                if (askedAgain && !m_types.isSubset(askedAgain, record.type))
                    return failWithTypes(id, parameter, "Arrivals, asked once more, lets arrive"_s, askedAgain);
            }
            continue;
        }

        TypeID arriving;
        if (takesParametersOfFunction) {
            if (index >= numberOfEntryParameters)
                continue;
            arriving = typeLaidDownAtEntry(index);
        } else {
            continue;
        }
        if (!m_types.isSubset(arriving, record.type))
            return failWithTypes(id, parameter, "what may arrive from outside is"_s, arriving);
    }
}

TypeID CodeTypeVerifier::typeArrivingOn(EdgeID id, BlockID target, ValueID parameter, uint32_t parameterIndex)
{
    const Edge& edge = m_code.at(id);
    std::span<const ValueID> arguments = edge.arguments();
    if (parameterIndex < arguments.size()) {
        ValueID argument = arguments[parameterIndex];
        return m_isDerived.quickGet(argument.index()) ? m_derivedTypes[argument.index()] : TypeID();
    }

    const Block& from = m_code.at(edge.from);
    if (from.handler == id)
        return { };
    const Terminator& terminator = from.terminator;
    uint32_t targetIndex = id.index() - terminator.firstEdge.index();
    uint32_t resultIndex = parameterIndex - static_cast<uint32_t>(arguments.size());
    const TargetInfo* info = infoOfTarget(terminator.kind, targetIndex, terminator.numberOfEdges);
    if (!info || info->takesProofs || resultIndex >= info->numberOfResults)
        return { };
    if (!collectTypesOfOperands(target, parameter, terminator, m_operandTypes))
        return { };
    return edgeResultTypeOf(m_typing, terminator, OperandTypes { m_operandTypes.span() }, targetIndex, resultIndex);
}

std::optional<Range> CodeTypeVerifier::changeSinceHeadOfLoop(ValueID parameterOfHead, ValueID value)
{
    auto behindConversions = [&](ValueID name) {
        while (valuePreservingOpcodes.contains(m_code.at(name).opcode) && m_code.at(name).operand(0).index() < name.index())
            name = m_code.at(name).operand(0);
        return name;
    };
    value = behindConversions(value);
    if (value == parameterOfHead)
        return Range::one(0);
    for (const auto& [known, change] : m_changesSinceHeadOfLoop) {
        if (known == value)
            return change.isEmpty() ? std::nullopt : std::optional(change);
    }
    size_t place = m_changesSinceHeadOfLoop.size();
    m_changesSinceHeadOfLoop.append(std::pair { value, Range::empty() });

    const Operation& operation = m_code.at(value);
    if (!operation.type || !m_types.at(operation.type).isInteger())
        return std::nullopt;
    auto plus = [&](ValueID term, ValueID addend, bool subtracts) -> std::optional<Range> {
        TypeID typeOfAddend = m_isDerived.quickGet(addend.index()) ? m_derivedTypes[addend.index()] : TypeID();
        if (!typeOfAddend || !m_types.at(typeOfAddend).isInteger() || !m_types.at(typeOfAddend).integers.isSubsetOf(Range::exact()))
            return std::nullopt;
        Range added = m_types.at(typeOfAddend).integers;
        std::optional<Range> ofTerm = changeSinceHeadOfLoop(parameterOfHead, term);
        if (!ofTerm)
            return std::nullopt;
        if (subtracts)
            return Range { ofTerm->minimum - added.maximum, ofTerm->maximum - added.minimum };
        return Range { ofTerm->minimum + added.minimum, ofTerm->maximum + added.maximum };
    };

    std::optional<Range> change;
    if (operation.opcode == Opcode::Parameter) {
        const BlockID blockOfJoin = operation.block();
        const Block& join = m_code.at(blockOfJoin);
        if (join.isEntry || join.isHandlerBlock())
            return std::nullopt;
        std::span<const ValueID> parametersOfJoin = join.parameters();
        size_t index = std::ranges::find(parametersOfJoin, value) - parametersOfJoin.begin();
        Range united = Range::empty();
        for (EdgeID edge : incomingEdgesOf(blockOfJoin)) {
            if (join.isLoopHead && isBackEdgeOf(blockOfJoin, edge))
                continue;
            std::span<const ValueID> arguments = m_code.at(edge).arguments();
            std::optional<Range> onEdge = index < arguments.size() ? changeSinceHeadOfLoop(parameterOfHead, arguments[index]) : std::nullopt;
            if (!onEdge)
                return std::nullopt;
            united = united.unionWith(*onEdge);
        }
        if (join.isLoopHead && !united.isEmpty()) {
            auto changesOfLoopAround = std::exchange(m_changesSinceHeadOfLoop, { });
            std::optional<int64_t> turns = mostTurnsOfLoop(blockOfJoin);
            std::optional<Range> inTurn = turns ? changeInTurnOfLoop(blockOfJoin, static_cast<uint32_t>(index)) : std::nullopt;
            m_changesSinceHeadOfLoop = WTF::move(changesOfLoopAround);
            if (!inTurn)
                return std::nullopt;
            CheckedInt64 least = CheckedInt64(std::min<int64_t>(inTurn->minimum, 0)) * *turns + united.minimum;
            CheckedInt64 most = CheckedInt64(std::max<int64_t>(inTurn->maximum, 0)) * *turns + united.maximum;
            if (least.hasOverflowed() || most.hasOverflowed())
                return std::nullopt;
            united = { least.value(), most.value() };
        }
        if (!united.isEmpty())
            change = united;
    } else if (operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement) {
        if (std::optional<Range> ofTerm = changeSinceHeadOfLoop(parameterOfHead, operation.valueOperand())) {
            int64_t step = operation.opcode == Opcode::Increment ? 1 : -1;
            change = Range { ofTerm->minimum + step, ofTerm->maximum + step };
        }
    } else if (operation.opcode == Opcode::Add) {
        change = plus(operation.leftOperand(), operation.rightOperand(), false);
        if (!change)
            change = plus(operation.rightOperand(), operation.leftOperand(), false);
    } else if (operation.opcode == Opcode::Sub)
        change = plus(operation.leftOperand(), operation.rightOperand(), true);

    if (!change || !change->isSubsetOf(Range::exact()))
        return std::nullopt;
    m_changesSinceHeadOfLoop[place].second = *change;
    return change;
}

std::optional<Range> CodeTypeVerifier::changeInTurnOfLoop(BlockID head, uint32_t parameterIndex)
{
    m_changesSinceHeadOfLoop.shrink(0);
    Range united = Range::empty();
    for (EdgeID edge : incomingEdgesOf(head)) {
        if (!isBackEdgeOf(head, edge))
            continue;
        std::span<const ValueID> arguments = m_code.at(edge).arguments();
        std::optional<Range> onEdge = parameterIndex < arguments.size() ? changeSinceHeadOfLoop(m_code.at(head).parameters()[parameterIndex], arguments[parameterIndex]) : std::nullopt;
        if (!onEdge)
            return std::nullopt;
        united = united.unionWith(*onEdge);
    }
    return united.isEmpty() ? std::nullopt : std::optional(united);
}

std::optional<int64_t> CodeTypeVerifier::mostTurnsOfLoop(BlockID head)
{
    constexpr Range changedByEveryStep { -maxExactInteger + 1, maxExactInteger - 1 };
    for (const auto& [known, turns] : m_mostTurnsOfLoops) {
        if (known == head)
            return turns;
    }
    std::span<const ValueID> parameters = m_code.at(head).parameters();
    std::optional<int64_t> fewest;
    for (uint32_t index = 0; index < parameters.size(); ++index) {
        TypeID recorded = m_code.at(parameters[index]).type;
        Type type = m_types.at(recorded);
        if (!type.isInteger() || !type.integers.isSubsetOf(changedByEveryStep))
            continue;
        bool holdsWhatArrives = std::ranges::all_of(incomingEdgesOf(head), [&](EdgeID edge) {
            TypeID arriving = typeArrivingOn(edge, head, parameters[index], index);
            return arriving && m_types.isSubset(arriving, recorded);
        });
        std::optional<Range> change = holdsWhatArrives ? changeInTurnOfLoop(head, index) : std::nullopt;
        if (!change || change->contains(0))
            continue;
        int64_t turns = (type.integers.maximum - type.integers.minimum) / std::min(std::abs(change->minimum), std::abs(change->maximum));
        fewest = fewest ? std::min(*fewest, turns) : turns;
    }
    m_mostTurnsOfLoops.append(std::pair { head, fewest });
    return fewest;
}

bool CodeTypeVerifier::holdsWhatArrivesByCounting(BlockID head, uint32_t parameterIndex)
{
    ValueID parameter = m_code.at(head).parameters()[parameterIndex];
    Type recorded = m_types.at(m_code.at(parameter).type);
    if (!recorded.isInteger())
        return false;
    Range entering = Range::empty();
    for (EdgeID edge : incomingEdgesOf(head)) {
        if (isBackEdgeOf(head, edge))
            continue;
        TypeID arriving = typeArrivingOn(edge, head, parameter, parameterIndex);
        if (!arriving || !m_types.at(arriving).isInteger())
            return false;
        entering = entering.unionWith(m_types.at(arriving).integers);
    }
    std::optional<int64_t> turns = mostTurnsOfLoop(head);
    std::optional<Range> change = turns ? changeInTurnOfLoop(head, parameterIndex) : std::nullopt;
    if (!change || entering.isEmpty())
        return false;
    CheckedInt64 least = CheckedInt64(std::min<int64_t>(change->minimum, 0)) * *turns + entering.minimum;
    CheckedInt64 most = CheckedInt64(std::max<int64_t>(change->maximum, 0)) * *turns + entering.maximum;
    return !least.hasOverflowed() && !most.hasOverflowed() && Range { least.value(), most.value() }.isSubsetOf(recorded.integers);
}

void CodeTypeVerifier::compareParametersWithArguments(BlockID id)
{
    const Block& block = m_code.at(id);
    bool holdsWhatArrives = block.isLoopHead || block.isEntry;
    std::span<const ValueID> parameters = block.parameters();

    for (uint32_t index = 0; index < parameters.size(); ++index) {
        ValueID parameter = parameters[index];
        TypeID recorded = m_code.at(parameter).type;
        m_positions.setFactsOfRecordBeingTyped(factsUsedBy(parameter));

        if (!holdsWhatArrives) {
            TypeID united = typeOfJoinParameter(m_types, incomingEdgesOf(id), [&](EdgeID edge) {
                return typeArrivingOn(edge, id, parameter, index);
            });
            if (hasFailed())
                return;
            if (!united)
                return fail(id, parameter, "an edge brings no typed value for the parameter"_s);
            if (!recorded || !m_types.isSubset(united, recorded))
                return failWithTypes(id, parameter, "what arrives unites to"_s, united);
            setDerivedType(parameter, recorded);
            continue;
        }

        std::optional<bool> holdsByCounting;
        for (EdgeID edge : incomingEdgesOf(id)) {
            TypeID arriving = typeArrivingOn(edge, id, parameter, index);
            if (hasFailed())
                return;
            if (!arriving)
                return fail(id, parameter, "an edge brings no typed value for the parameter"_s);
            if (!m_types.isSubset(arriving, recorded)) {
                if (!holdsByCounting)
                    holdsByCounting = block.isLoopHead && !block.isEntry && holdsWhatArrivesByCounting(id, index);
                if (!*holdsByCounting)
                    return failWithTypes(id, parameter, "there arrives"_s, arriving, m_code.at(edge).from);
            }
        }
        setDerivedType(parameter, recorded);
    }
}

void CodeTypeVerifier::deriveParameters(BlockID id)
{
    const Block& block = m_code.at(id);
    if (block.parameters().empty())
        return;

    if (block.isEntry) {
        verifyEntryParameters(id, block);
        if (hasFailed())
            return;
    }
    if (!block.isEntry && !block.isLoopHead && !someArgumentComesLater(id)) {
        compareParametersWithArguments(id);
        return;
    }
    for (ValueID parameter : block.parameters())
        setDerivedType(parameter, m_code.at(parameter).type);
    m_blocksWhoseArgumentsComeLater.append(id);
}

void CodeTypeVerifier::noteAccessToHeapWord(BlockID block, ValueID id, const Operation& operation)
{
    uint64_t word = (static_cast<uint64_t>(operation.frameOperand().index()) << 32) | operation.index();
    m_accessesToHeapWords.append(AccessToHeapWord { word, m_placesInOrder[block.index()], id });
}

void CodeTypeVerifier::failAtReadOfHeapWord(const AccessToHeapWord& read, const AccessToHeapWord* store)
{
    if (hasFailed())
        return;
    StringPrintStream out;
    printPlace(out, m_blocksInOrder[read.place], read.record);
    out.print(": the read names "_s);
    printTypeInFailure(out, m_code.at(read.record).valueType());
    if (!store)
        out.print(", and on some way to a read of the word under that type nothing was stored into the word"_s);
    else {
        out.print(", and a store that reaches a read of the word under that type puts "_s);
        printTypeInFailure(out, m_derivedTypes[m_code.at(store->record).valueOperand().index()]);
        out.print(" there: v"_s, store->record.index(), " in b"_s, m_blocksInOrder[store->place].index());
    }
    m_failure = out.toString();
}

void CodeTypeVerifier::verifyStoresThatReachReadsOfHeapWord(std::span<const AccessToHeapWord> accessesToWord, TypeID typeThatReadsName)
{
    ++m_numberOfSearchesForStores;
    m_blocksToGoOnInFrontOf.shrink(0);
    const AccessToHeapWord* firstRead = nullptr;

    auto goOnInFrontOf = [&](uint32_t place) {
        BlockID block = m_blocksInOrder[place];
        if (std::exchange(m_searchThatWentOnInFrontOfBlock[block.index()], m_numberOfSearchesForStores) != m_numberOfSearchesForStores)
            m_blocksToGoOnInFrontOf.append(block);
    };
    auto meetStore = [&](const AccessToHeapWord& store) {
        TypeID stored = m_derivedTypes[m_code.at(store.record).valueOperand().index()];
        if (!stored || !m_types.isSubset(stored, typeThatReadsName))
            failAtReadOfHeapWord(*firstRead, &store);
    };

    const AccessToHeapWord* newestStore = nullptr;
    for (const AccessToHeapWord& access : accessesToWord) {
        if (isStoreIntoHeapWord(access)) {
            newestStore = &access;
            continue;
        }
        if (m_code.at(access.record).valueType() != typeThatReadsName)
            continue;
        if (!firstRead)
            firstRead = &access;
        if (newestStore && newestStore->place == access.place)
            meetStore(*newestStore);
        else
            goOnInFrontOf(access.place);
    }

    while (!m_blocksToGoOnInFrontOf.isEmpty() && !hasFailed()) {
        BlockID block = m_blocksToGoOnInFrontOf.takeLast();
        std::span<const EdgeID> incoming = incomingEdgesOf(block);
        if (incoming.empty() || m_code.at(block).isEntry)
            return failAtReadOfHeapWord(*firstRead, nullptr);
        for (EdgeID edge : incoming) {
            BlockID from = m_code.at(edge).from;
            uint32_t place = m_placesInOrder[from.index()];
            auto accessesInBlock = std::ranges::equal_range(accessesToWord, place, { }, &AccessToHeapWord::place);
            bool leavesFromAnywhereInBlock = m_code.at(from).handler == edge;
            bool hasMetNewestStore = false;
            for (auto access = accessesInBlock.end(); access != accessesInBlock.begin() && !hasMetNewestStore;) {
                --access;
                if (!isStoreIntoHeapWord(*access))
                    continue;
                meetStore(*access);
                hasMetNewestStore = !leavesFromAnywhereInBlock;
            }
            if (!hasMetNewestStore)
                goOnInFrontOf(place);
        }
    }
}

void CodeTypeVerifier::verifyReadsOfHeapWords()
{
    if (hasFailed() || m_accessesToHeapWords.isEmpty())
        return;
    AOT::stableSort(m_accessesToHeapWords, { }, &AccessToHeapWord::word);
    m_searchThatWentOnInFrontOfBlock.fill(0, m_code.numberOfBlocks());

    Vector<TypeID, 4> typesLookedFor;
    std::span<const AccessToHeapWord> rest = m_accessesToHeapWords.span();
    while (!rest.empty()) {
        size_t numberOfAccesses = std::ranges::find_if(rest, [&](const AccessToHeapWord& access) { return access.word != rest.front().word; }) - rest.begin();
        std::span<const AccessToHeapWord> accessesToWord = rest.first(numberOfAccesses);
        rest = rest.subspan(numberOfAccesses);

        typesLookedFor.shrink(0);
        for (const AccessToHeapWord& access : accessesToWord) {
            if (isStoreIntoHeapWord(access))
                continue;
            TypeID type = m_code.at(access.record).valueType();
            if (typesLookedFor.contains(type))
                continue;
            typesLookedFor.append(type);
            verifyStoresThatReachReadsOfHeapWord(accessesToWord, type);
            if (hasFailed())
                return;
        }
    }
}

} // anonymous namespace

TypeID typeOfJoinParameter(TypeArena& types, std::span<const EdgeID> edgesThatReachJoin, NOESCAPE const ScopedLambda<TypeID(EdgeID)>& typeArrivingOn)
{
    TypeID united = TypeTable::never;
    for (EdgeID edge : edgesThatReachJoin) {
        TypeID arriving = typeArrivingOn(edge);
        if (!arriving)
            return { };
        united = types.unionOf(united, arriving);
    }
    return united;
}

String verifyTypesOfCode(const TypingContext& context, const CallingConventions& conventions, const Arrivals& arrivals, const Code& code)
{
    return CodeTypeVerifier(context, conventions, arrivals, code).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
