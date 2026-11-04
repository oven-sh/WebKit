/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCode.h"

#if ENABLE(AOT)

#include "AOTArrivals.h"
#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTRepresentation.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSchemaPayloadPositions.h"
#include "AOTTypeArena.h"
#include "AOTTypeRelations.h"
#include "AOTTypeText.h"
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

// Which object stands for what such an operation makes is the walk's input. No other operation is handed one.
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
    TypeID slot(TypeLevel level, LayoutID layout, uint32_t field) const final { return m_positionsOfWalk.slot(level, layout, field); }
    TypeID readOfSlot(TypeLevel level, LayoutID layout, uint32_t field) const final { return m_positionsOfWalk.readOfSlot(level, layout, field); }
    TypeID halfOfAccessorOfField(TypeLevel level, LayoutID layout, uint32_t field, bool isSetter) const final { return m_positionsOfWalk.halfOfAccessorOfField(level, layout, field, isSetter); }
    std::optional<uint32_t> appendedField(LayoutID layout, PropertyKeyID key) const final { return m_positionsOfWalk.appendedField(layout, key); }
    TypeID foundAboveObjectsOf(TypeLevel level, LayoutID layout, PropertyKeyID key) const final { return m_positionsOfWalk.foundAboveObjectsOf(level, layout, key); }
    TypeID readByNameOffObjectsOf(TypeLevel level, LayoutID layout, PropertyKeyID key) const final { return m_positionsOfWalk.readByNameOffObjectsOf(level, layout, key); }
    LayoutID classAbove(LayoutID classLayout) const final { return m_positionsOfWalk.classAbove(classLayout); }
    TypeID property(TypeLevel level, ObjectID object, PropertyKeyID key) const final { return m_positionsOfWalk.property(level, object, key); }
    TypeID ownProperty(TypeLevel level, ObjectID object, PropertyKeyID key) const final { return m_positionsOfWalk.ownProperty(level, object, key); }
    TypeID contents(TypeLevel level, ObjectID object, ContentsSlot slot) const final { return m_positionsOfWalk.contents(level, object, slot); }
    TypeID awaited(TypeLevel level, ObjectID object) const final { return m_positionsOfWalk.awaited(level, object); }
    TypeID result(TypeLevel level, FunctionID function, uint32_t resultIndex) const final { return m_positionsOfWalk.result(level, function, resultIndex); }
    TypeID resultOfBodyThatNeverSuspends(TypeLevel level, FunctionID function) const final { return m_positionsOfWalk.resultOfBodyThatNeverSuspends(level, function); }
    TypeID entryParameter(TypeLevel level, FunctionID function, uint32_t entryParameterIndex) const final { return m_positionsOfWalk.entryParameter(level, function, entryParameterIndex); }
    bool entryParameterIsLeftOutOfCalls(FunctionID function, uint32_t entryParameterIndex) const final { return m_positionsOfWalk.entryParameterIsLeftOutOfCalls(function, entryParameterIndex); }
    TypeID capture(TypeLevel level, FunctionID function, uint32_t captureIndex) const final { return m_positionsOfWalk.capture(level, function, captureIndex); }
    TypeID captureOfObject(TypeLevel level, ObjectID object, uint32_t captureIndex) const final { return m_positionsOfWalk.captureOfObject(level, object, captureIndex); }
    TypeID resultOfSelector(TypeLevel level, uint32_t selector) const final { return m_positionsOfWalk.resultOfSelector(level, selector); }
    TypeID askedOfEachObject(QuestionOfEachObject question, TypeLevel level, ObjectSetID objects, uint32_t word, NOESCAPE const ScopedLambda<TypeID()>& askEachObject) const final { return m_positionsOfWalk.askedOfEachObject(question, level, objects, word, askEachObject); }
    TypeID askedOfEachLayout(QuestionOfEachLayout question, TypeLevel level, LayoutSetID layouts, PropertyKeyID key, NOESCAPE const ScopedLambda<TypeID()>& askEachLayout) const final { return m_positionsOfWalk.askedOfEachLayout(question, level, layouts, key, askEachLayout); }

    // What Link has proven of the read stands among the facts that its record leans on.
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

    // What the walk got for the call stands among the facts that its record leans on. A call with several results is not typed by its rule.
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
    CodeTypeVerifier(const TypingContext&, const CallingConventions&, const Facts&, const Arrivals&, const Code&);

    String run();

private:
    static constexpr uint32_t notReached = UINT32_MAX;

    bool hasFailed() const { return !m_failure.isNull(); }
    void printPlace(PrintStream&, BlockID, ValueID record) const;
    void printTypeInFailure(PrintStream&, TypeID) const;
    ASCIILiteral howRecordDiffersFrom(TypeID recorded, TypeID other) const;
    void fail(BlockID, ValueID record, ASCIILiteral rule, ValueID other = { });
    void failWithTypes(BlockID, ValueID record, ASCIILiteral whatGivesOtherType, TypeID other, BlockID sourceOfEdge = { });

    // The edges as the walk left them. The sweep takes handler edges away, and the walk has united what all of them bring.
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
    const SplitResult* fieldsHandedBackBy(const Operation& call) const;
    TypeID typeOfResultOfSplitCall(const SplitResult&, uint32_t resultIndex) const;
    TypeID typeOfResultAskedFor(const Operation& resultAt);
    TypeID typeByResultRule(ValueID, const Operation&);
    void verifyTypeReadFromPayload(BlockID, ValueID, const Operation&);
    void deriveOperation(BlockID, ValueID);
    void deriveConstants();
    void deriveOperations(BlockID);

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
        // The record of the cell above the index in it.
        uint64_t word;
        // Of its block in m_blocksInOrder.
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
    const Facts& m_facts;
    const Arrivals& m_arrivals;
    PositionsOfTypeVerifier m_positions;
    TypingContext m_typing;
    TypingContext m_typingWithoutFacts;
    String m_failure;

    // By record. A record without a result has the null type, so whether a type was derived is kept apart.
    Vector<TypeID> m_derivedTypes;
    BitVector m_isDerived;
    Vector<uint32_t> m_firstFactUse;
    Vector<FactUse> m_factUses;
    // By block.
    Vector<uint32_t> m_placesInOrder;
    Vector<uint32_t> m_firstIncomingEdge;

    // In reverse postorder from the entries: a block stands behind every block that dominates it.
    Vector<BlockID> m_blocksInOrder;
    Vector<EdgeID> m_incomingEdges;
    Vector<BlockID> m_blocksWhoseArgumentsComeLater;
    Vector<ValueID, 64> m_operationsOfBlock;
    Vector<TypeID, 16> m_operandTypes;
    Vector<TypeID, 16> m_operandTypesOfProducer;
    // For one parameter of a head at a time: what changeSinceHeadOfLoop() has answered. An empty range: nothing, or the value is being looked at.
    Vector<std::pair<ValueID, Range>, 16> m_changesSinceHeadOfLoop;
    // What mostTurnsOfLoop() has answered. The loops around a loop ask once for each of their parameters that goes through it.
    Vector<std::pair<BlockID, std::optional<int64_t>>, 4> m_mostTurnsOfLoops;
    // In the order in which the blocks are typed, and within a block as the operations stand.
    Vector<AccessToHeapWord> m_accessesToHeapWords;
    // By block: the newest search for stores that has gone on in front of the block.
    Vector<uint32_t> m_searchThatWentOnInFrontOfBlock;
    Vector<BlockID, 64> m_blocksToGoOnInFrontOf;
    uint32_t m_numberOfSearchesForStores { 0 };
};

CodeTypeVerifier::CodeTypeVerifier(const TypingContext& context, const CallingConventions& conventions, const Facts& facts, const Arrivals& arrivals, const Code& code)
    : m_program(context.program)
    , m_types(context.types)
    , m_code(code)
    , m_conventions(conventions)
    , m_facts(facts)
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

// A record that claims more than was derived is what wrong code is made of. One that claims less costs speed.
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
    // The sources that stand for the copies of records are named beside it.
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

// The rule of an allocation reads the place of its prototype as new.target. The prototype object of a class is what a construction reads off that class.
// Any object says that nothing is known of new.target, also of a prototype that is itself a function.
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

// Null: the call gives what the function returns. One field that is handed back in place of the object is read by a ResultAt like any other.
const SplitResult* CodeTypeVerifier::fieldsHandedBackBy(const Operation& call) const
{
    if (call.opcode != Opcode::CallDirect)
        return nullptr;
    return m_conventions.splitResultOf(m_conventions.unitEnteredBy(call));
}

// Null: the call has no such result.
TypeID CodeTypeVerifier::typeOfResultOfSplitCall(const SplitResult& split, uint32_t resultIndex) const
{
    if (resultIndex < split.numberOfParts())
        return split.types[resultIndex];
    if (split.whole && resultIndex == split.resultOfWhole())
        return split.whole;
    if (split.hasMark() && resultIndex == split.resultOfMark())
        return split.markIsTag() ? m_types.integer(split.rangeOfTag()) : TypeTable::boolean;
    return resultIndex < split.numberOfResults() ? split.typeOfResult(resultIndex) : TypeID();
}

TypeID CodeTypeVerifier::typeOfResultAskedFor(const Operation& resultAt)
{
    if (!resultAt.resultsOperand())
        return { };
    const Operation& producer = m_code.at(resultAt.resultsOperand());
    if (!isTypedByResultRuleForTypeVerifier(producer.opcode))
        return { };
    // PositionTypes::result() speaks of what the function returns, which is the object.
    if (const SplitResult* split = fieldsHandedBackBy(producer))
        return typeOfResultOfSplitCall(*split, resultAt.index());
    std::optional<uint32_t> numberOfResults = numberOfResultsOf(m_program, producer);
    if (numberOfResults && resultAt.index() >= *numberOfResults)
        return { };

    m_operandTypesOfProducer.shrink(0);
    for (ValueID operand : producer.operands())
        m_operandTypesOfProducer.append(operand ? m_derivedTypes[operand.index()] : TypeID());
    return resultTypeOf(m_typing, producer, OperandTypes { m_operandTypesOfProducer.span() }, ObjectID(), resultAt.index());
}

// `m_operandTypes` holds the types of the operation's operands.
TypeID CodeTypeVerifier::typeByResultRule(ValueID id, const Operation& operation)
{
    // The record holds none of the results: each ResultAt asks the producer's rule for its own.
    if (opcodesThatMayHaveSeveralResults.contains(operation.opcode) && (numberOfResultsOf(m_program, operation) || fieldsHandedBackBy(operation)))
        return TypeTable::undefined;

    m_positions.setFactsOfRecordBeingTyped(factsUsedBy(id));
    if (operation.opcode == Opcode::ResultAt)
        m_operandTypes[OperandIndex::ResultAt::results] = typeOfResultAskedFor(operation);
    else if (operation.opcode == Opcode::NewObject && operation.prototypeOperand())
        m_operandTypes[OperandIndex::NewObject::prototype] = typeOfNewTargetBehind(operation.prototypeOperand());
    return resultTypeOf(m_typing, operation, OperandTypes { m_operandTypes.span() }, objectMadeBy(operation));
}

// No operation of Code states a type. The rule of each of these reads one out of the payload, so what stands there is held against the thing that it speaks of.
// Whoever adds one says in verifyTypeReadFromPayload() what its payload is held against.
static constexpr OpcodeSet opcodesWhosePayloadIsHeldByTypeVerifier {
    Opcode::Unspecified, Opcode::GetHeapFrameWord, Opcode::GetCachedValue, Opcode::GetStaticFieldOrAbsent, Opcode::GetAccessorOfMember, Opcode::GetBoundCapture, Opcode::EmptyArgumentList, Opcode::TypedArrayRange, Opcode::NewFormatterWithPattern
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
    // The word says nothing, so no type is wrong of it.
    case Opcode::Unspecified:
    // verifyReadsOfHeapWords().
    case Opcode::GetHeapFrameWord:
        return;
    case Opcode::GetCachedValue:
    case Opcode::GetStaticFieldOrAbsent:
        if (!m_types.isSubset(m_types.withPrimitives(TypeTable::anything, Primitive::Absent), stated))
            fail(block, id, "the payload says more of what a lookup finds than that it is a value or absent"_s);
        return;
    case Opcode::GetAccessorOfMember: {
        bool isFact = std::ranges::any_of(factsUsedBy(id), [&](const FactUse& use) {
            return use.type == stated;
        });
        if (!isFact)
            fail(block, id, "the payload names an accessor, and the record leans on no fact that names the same type"_s);
        return;
    }
    // Plan laid the closure out. What it holds for an entry parameter of the target is what holds of every caller of the target: Facts is asked once more.
    case Opcode::GetBoundCapture: {
        const BoundFunctionPlan* bound = m_conventions.boundFunction(operation.function(), operation.numberOfBoundArguments());
        if (!bound || operation.index() >= bound->captures.size())
            return fail(block, id, "the plan of the bound function has no such capture"_s);
        const Function& target = m_program.at(bound->target);
        std::optional<unsigned> parameter = targetEntryParameterOf(target, bound->captures[operation.index()]);
        TypeID held;
        if (!parameter)
            held = m_types.forFunction(bound->target);
        else if (TypeID proven = m_facts.typeOfEntryParameter(bound->target, *parameter).proven)
            held = proven;
        else if (const DeclaredType* declared = target.declaredEntryParameter(*parameter))
            held = m_program.provenTypeOf(*declared);
        else if (m_types.isSubset(TypeTable::anything, stated))
            return;
        else
            held = TypeTable::anything;
        if (stated != held)
            failWithTypes(block, id, "the closure holds"_s, held);
        return;
    }
    case Opcode::EmptyArgumentList:
        if (!m_types.isSubset(stated, m_types.builtinClass(BuiltinClasses::Array)))
            fail(block, id, "the payload says that the empty list of arguments is no list"_s);
        return;
    case Opcode::TypedArrayRange:
    case Opcode::NewFormatterWithPattern:
        if (!m_types.at(stated).isObject())
            fail(block, id, "the payload says that an allocation may give something other than an object"_s);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void CodeTypeVerifier::deriveOperation(BlockID block, ValueID id)
{
    const Operation& operation = m_code.at(id);
    if (!collectTypesOfOperands(block, id, operation, m_operandTypes))
        return;

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

// An operand of a constant is an older constant.
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
    case CodeRole::BoundFunction:
    case CodeRole::GenericEntryOfBoundFunction:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
    case CodeRole::Serializer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What holds of an entry parameter without a fact. Whoever enters the fast version has come through the guard, or has proven as much himself: requiredTypeOfOperand() holds him to it.
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

// The call that Walker::takeEntryParameters() and takeArrivals() make.
TypeID CodeTypeVerifier::entryParameterTypeFromArrivals(unsigned entryParameterIndex) const
{
    TypeID laidDown = typeLaidDownAtEntry(entryParameterIndex);
    if (!laidDown)
        laidDown = TypeTable::anything;
    TypeID known = m_arrivals.provenAt(m_types, m_code.function(), entryParameterIndex);
    if (!m_types.at(laidDown).isJavaScriptValue())
        return { };
    // Where nothing is proven, whatever is laid down arrives.
    return known ? m_types.intersectionOf(laidDown, known) : laidDown;
}

// Nothing in Code derives what arrives from outside. A parameter that leans on a fact has the type that the fact says. Any other holds whatever may arrive.
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
            // Behind the function's own: the driver of a coroutine body, which the ramp has made.
            if (index >= numberOfEntryParameters)
                continue;
            arriving = typeLaidDownAtEntry(index);
        } else {
            // The convention of the unit says what arrives, and Code does not hold it.
            continue;
        }
        if (!m_types.isSubset(arriving, record.type))
            return failWithTypes(id, parameter, "what may arrive from outside is"_s, arriving);
    }
}

// Null: the edge brings nothing for the parameter, or what it brings has no type yet.
TypeID CodeTypeVerifier::typeArrivingOn(EdgeID id, BlockID target, ValueID parameter, uint32_t parameterIndex)
{
    const Edge& edge = m_code.at(id);
    std::span<const ValueID> arguments = edge.arguments();
    if (parameterIndex < arguments.size()) {
        ValueID argument = arguments[parameterIndex];
        return m_isDerived.quickGet(argument.index()) ? m_derivedTypes[argument.index()] : TypeID();
    }

    // Behind the arguments: what the terminator itself hands to the target.
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

// The value less the parameter, on whichever way the turn has gone. Nothing: on some way it is more than the parameter plus integers.
// What is added is whatever its derived type holds, which rests on the types of the heads as every other type in the loop does: they hold in the turns before the one that is counted.
// Whatever a loop inside adds up goes through a parameter of that loop's head, and is counted by the turns of that loop.
std::optional<Range> CodeTypeVerifier::changeSinceHeadOfLoop(ValueID parameterOfHead, ValueID value)
{
    auto behindConversions = [&](ValueID name) {
        // An operand is older than its user. Parsed text that says otherwise must not make this loop forever.
        while (opcodesThatGiveSameValue.contains(m_code.at(name).opcode) && m_code.at(name).operand(0).index() < name.index())
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

    // Two exact integers overflow nothing.
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

// How often a back edge of the head is taken at the most once the loop is entered. An integer that every turn moves the same way takes each value of its type at most once.
// Not 2 ** 53: one more is no double, so a number that has come there stays there.
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

// `if (..) hits++` in a counted loop. What a turn has added to may stand one turn outside the type of the parameter, and comes round only as often as the loop has turns.
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

// A parameter of a join holds the union of what arrives: the walk's join widens several layouts to their family. The type of a parameter of a loop's head is a guess that the walk has checked, so it holds whatever arrives, which makes the types a solution.
// A tally is the exception: holdsWhatArrivesByCounting().
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

// ---- Heap words

// The rule of GetHeapFrameWord repeats the type that the read names, which proves nothing. A word is the compiler's own memory: one cell is made in each run of the unit, and nothing but the stores of this Code
// puts anything there. So a read finds what the stores that reach it have put, and the type that it names has to hold all of that.
void CodeTypeVerifier::noteAccessToHeapWord(BlockID block, ValueID id, const Operation& operation)
{
    uint64_t word = (static_cast<uint64_t>(operation.frameOperand().index()) << 32) | operation.index();
    m_accessesToHeapWords.append(AccessToHeapWord { word, m_placesInOrder[block.index()], id });
}

// Null: some way leads to the read past no store at all.
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

// Backward from all the reads that name the type at once: each way ends at the first store that it meets. An edge is followed at most once.
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
            // What throws may do so in front of any store of its block, and in front of the first.
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
    std::ranges::stable_sort(m_accessesToHeapWords, { }, &AccessToHeapWord::word);
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

String verifyTypesOfCode(const TypingContext& context, const CallingConventions& conventions, const Facts& facts, const Arrivals& arrivals, const Code& code)
{
    return CodeTypeVerifier(context, conventions, facts, arrivals, code).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
