/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTElementAccessEmitter.h"
#include "AOTImageText.h"
#include "AOTOperationContext.h"
#include "AOTOperationEmitter.h"
#include "JSString.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

struct SearchLoop {
    ValueID counter;
    ValueID bound;
    ValueID array;
    ValueID sought;
    ValueID equality;
    bool isBoundedByLength { false };
    bool isKnownArray { false };
    bool isStrict { false };
    bool skipsOtherValues { false };
};

static constexpr int64_t fewestElementsSkipped = 4;
static constexpr int64_t mostElementsSkipped = 1 << 20;

class SearchLoopFinder {
public:
    SearchLoopFinder(const CodeGenerator& generator, BlockID header)
        : m_generator(generator)
        , m_code(generator.code())
        , m_header(header)
    {
    }

    std::optional<SearchLoop> find();

private:
    const Operation& at(ValueID value) const { return m_code.at(value); }
    bool isPlainBlock(BlockID id) const
    {
        const Block& block = m_code.at(id);
        return m_code.isReached(id) && block.frequency == BlockFrequency::Normal && !m_code.handlerOf(id) && !block.subject && !block.isEntry && !block.isHandlerBlock();
    }
    bool isAvailableBefore(ValueID value) const { return m_code.isConstant(value) || (!m_blocks.contains(m_code.blockOf(value)) && m_generator.m_values[value.index()]); }
    ValueID withoutNames(ValueID value) const
    {
        while (!m_code.isConstant(value)) {
            if (isName(at(value).opcode)) {
                value = at(value).operand(0);
                continue;
            }
            size_t place = m_passedArguments.findIf([&](const std::pair<ValueID, ValueID>& passed) { return passed.first == value; });
            if (place == notFound)
                break;
            value = m_passedArguments[place].second;
        }
        return value;
    }
    static bool isName(Opcode opcode)
    {
        static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Unbox, Opcode::Box, Opcode::WidenInteger, Opcode::NarrowInteger };
        return names.contains(opcode);
    }
    bool addOperation(ValueID);
    const Edge* findNextEdge(const Terminator&);
    bool passArguments(const Edge&);
    bool passesParametersOn(const Edge& backEdge) const;

    static constexpr unsigned mostBlocks = 10;
    const CodeGenerator& m_generator;
    const Code& m_code;
    BlockID m_header;
    Vector<BlockID, mostBlocks> m_blocks;
    Vector<std::pair<ValueID, ValueID>, 4> m_passedArguments;
    Vector<ValueID, 4> m_testedValues;
    ValueID m_length;
    ValueID m_test;
    ValueID m_element;
    ValueID m_increment;
    SearchLoop m_found;
};

bool SearchLoopFinder::addOperation(ValueID value)
{
    const Operation& operation = at(value);
    auto setOnce = [&](ValueID& place) {
        return !std::exchange(place, value);
    };
    switch (operation.opcode) {
    case Opcode::LoopHint:
        return !operation.throwsOwedError();
    case Opcode::CheckProof:
    case Opcode::TypeTest:
        return true;
    case Opcode::GetArrayLength:
        return setOnce(m_length);
    case Opcode::Less:
        return operation.form() == OperatorForm::Integers && setOnce(m_test);
    case Opcode::Increment:
        return operation.form() == OperatorForm::Integers && setOnce(m_increment);
    case Opcode::GetArrayElement:
        return (operation.arrayStorage() == ArrayStorage::Contiguous || operation.arrayStorage() == ArrayStorage::Other) && setOnce(m_element);
    case Opcode::Eq:
    case Opcode::StrictEq: {
        static constexpr uint16_t strings = Primitive::AnyString;
        if (!m_element || !setOnce(m_found.equality))
            return false;
        ValueID left = withoutNames(operation.leftOperand());
        ValueID right = withoutNames(operation.rightOperand());
        if (left != m_element && right != m_element)
            return false;
        m_found.sought = left == m_element ? right : left;
        m_found.isStrict = operation.opcode == Opcode::StrictEq;
        return m_generator.typeOf(left).primitives & m_generator.typeOf(right).primitives & strings;
    }
    default:
        return isName(operation.opcode);
    }
}

const Edge* SearchLoopFinder::findNextEdge(const Terminator& terminator)
{
    if (terminator.kind == TerminatorKind::Jump)
        return &m_code.at(terminator.next());
    if (terminator.kind != TerminatorKind::Branch)
        return nullptr;
    ValueID condition = withoutNames(terminator.conditionOperand());
    if (condition == m_test)
        return &m_code.at(terminator.taken());
    if (condition == m_found.equality)
        return &m_code.at(terminator.notTaken());
    if (m_code.isConstant(condition) || at(condition).opcode != Opcode::TypeTest || !m_blocks.contains(m_code.blockOf(condition)))
        return nullptr;
    uint16_t testedStrings = m_generator.m_inputs.types.at(at(condition).testedType()).primitives & Primitive::AnyString;
    if (testedStrings && testedStrings != Primitive::AnyString)
        return nullptr;
    m_testedValues.append(withoutNames(at(condition).operand(0)));
    return &m_code.at(testedStrings ? terminator.taken() : terminator.notTaken());
}

bool SearchLoopFinder::passArguments(const Edge& edge)
{
    std::span<const ValueID> parameters = m_code.at(edge.to).parameters();
    if (edge.arguments().size() != parameters.size())
        return false;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_passedArguments.append({ parameters[index], edge.arguments()[index] });
    return true;
}

bool SearchLoopFinder::passesParametersOn(const Edge& backEdge) const
{
    std::span<const ValueID> parameters = m_code.at(m_header).parameters();
    if (backEdge.arguments().size() != parameters.size())
        return false;
    for (size_t index = 0; index < parameters.size(); ++index) {
        ValueID expected = parameters[index] == m_found.counter ? m_increment : parameters[index];
        if (withoutNames(backEdge.arguments()[index]) != expected || m_generator.isCarriedBoxed(parameters[index]))
            return false;
    }
    return true;
}

std::optional<SearchLoop> SearchLoopFinder::find()
{
    const Edge* edge = nullptr;
    for (BlockID id = m_header; !edge || id != m_header; id = edge->to) {
        const Block& block = m_code.at(id);
        if (m_blocks.size() == mostBlocks || !isPlainBlock(id) || m_blocks.contains(id) || (edge && (block.isLoopHead || !passArguments(*edge))))
            return std::nullopt;
        m_blocks.append(id);
        for (ValueID value : m_code.valuesOf(block)) {
            if (!addOperation(value))
                return std::nullopt;
        }
        edge = findNextEdge(block.terminator);
        if (!edge)
            return std::nullopt;
    }
    bool testsOtherValue = std::ranges::any_of(m_testedValues, [&](ValueID tested) { return tested != m_element && tested != m_found.sought; });
    if (!m_test || !m_found.equality || !m_increment || testsOtherValue)
        return std::nullopt;
    m_found.skipsOtherValues = m_found.isStrict && m_testedValues.isEmpty();
    const Operation& test = at(m_test);
    const Operation& element = at(m_element);
    m_found.counter = withoutNames(test.leftOperand());
    m_found.bound = withoutNames(test.rightOperand());
    m_found.array = element.arrayOperand();
    ValueID array = withoutNames(m_found.array);
    m_found.isBoundedByLength = m_length && m_found.bound == m_length && withoutNames(at(m_length).arrayOperand()) == array;
    bool hasCounter = std::ranges::contains(m_code.at(m_header).parameters(), m_found.counter) && m_generator.m_values[m_found.counter.index()] && withoutNames(element.positionOperand()) == m_found.counter
        && withoutNames(at(m_increment).operand(0)) == m_found.counter;
    if (!hasCounter || !passesParametersOn(*edge))
        return std::nullopt;
    auto isInteger = [&](ValueID value) {
        Representation representation = at(value).representation;
        return representation == Representation::Int32 || representation == Representation::Int64;
    };
    auto isBoxedOrCell = [&](ValueID value) {
        Representation representation = at(value).representation;
        return representation == Representation::JSValue || representation == Representation::Cell;
    };
    if (!isInteger(m_found.counter) || (!m_found.isBoundedByLength && (!isInteger(m_found.bound) || !isAvailableBefore(m_found.bound))))
        return std::nullopt;
    if (!isAvailableBefore(m_found.array) || !isBoxedOrCell(m_found.array))
        return std::nullopt;
    m_found.isKnownArray = element.level() == KnowledgeLevel::Proven && m_generator.typeOf(m_found.array).isObject();
    if (!isAvailableBefore(m_found.sought) || !isBoxedOrCell(m_found.sought))
        return std::nullopt;
    return m_found;
}

class SearchLoopEmitter final : public OperationEmitter {
public:
    SearchLoopEmitter(CodeGenerator& generator, const SearchLoop& loop)
        : OperationEmitter(generator)
        , m_loop(loop)
    {
    }

    void emit(LValue first, LBasicBlock written, LValue counterOfWritten);

private:
    LValue cellOf(ValueID value)
    {
        LValue lowered = get(value).value;
        m_out.clearDeclaredAccesses();
        return lowered;
    }
    LValue widen(LValue integer) { return integer->type() == Machine::Int32 ? m_out.signExt32To64(integer) : integer; }
    void branchOrLeave(Condition condition, LBasicBlock left)
    {
        LBasicBlock holds = newBlock();
        m_out.branch(condition.value(), normalEdge(holds), unlikelyEdge(left));
        m_out.appendTo(holds);
    }
    Word32 loadFlags(Pointer text) { return loadField<Word32>(text, m_heaps.StringImpl_hashAndFlags); }
    Word32 loadLength(Pointer text) { return loadField<Word32>(text, m_heaps.StringImpl_length); }

    const SearchLoop& m_loop;
};

void SearchLoopEmitter::emit(LValue first, LBasicBlock written, LValue counterOfWritten)
{
    build().statistics().count(StatisticID::SearchLoops);
    m_out.clearDeclaredAccesses();
    m_out.setOrigin(Machine::Origin());
    LBasicBlock unchanged = newBlock();
    LBasicBlock left = newBlock();
    LBasicBlock done = newBlock();
    LBasicBlock iteration = newBlock();
    LBasicBlock next = newBlock();
    LBasicBlock again = newBlock();
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();

    Boxed list(cellOf(m_loop.array));
    CellPointer array = unboxCell(list);
    if (!m_loop.isKnownArray) {
        branchOrLeave(isCellAndNotEmpty(list), unchanged);
        branchOrLeave(hasCellType(array, ArrayType), unchanged);
    }
    ArrayElements storage = elements.elementsOf(array);
    LValue limit = m_out.zeroExt(elements.publicLengthAsWord(storage).value(), Machine::Int64);
    if (!m_loop.isBoundedByLength) {
        LValue bound = widen(cellOf(m_loop.bound));
        limit = m_out.select(m_out.lessThan(bound, limit), bound, limit);
    }
    LValue firstIndex = widen(first);
    branchOrLeave(Condition(m_out.greaterThanOrEqual(m_out.sub(limit, firstIndex), m_out.constInt64(fewestElementsSkipped))), unchanged);
    if (typeOf(m_loop.counter).integers.minimum < 0)
        branchOrLeave(Condition(m_out.greaterThanOrEqual(firstIndex, m_out.constInt64(0))), unchanged);
    branchOrLeave(elements.hasArrayStorage(array, ArrayStorage::Contiguous), unchanged);
    Boxed sought(cellOf(m_loop.sought));
    branchOrLeave(isCellAndNotEmpty(sought), unchanged);
    branchOrLeave(hasCellType(unboxCell(sought), StringType), unchanged);
    Pointer soughtText = loadField<Pointer>(unboxCell(sought), m_heaps.JSString_value);
    branchOrLeave(hasNoBits(soughtText, JSString::isRopeInPointer), unchanged);
    Word32 soughtLength = loadLength(soughtText);
    Word32 soughtFlags = bitAnd(loadFlags(soughtText), word32(flagsOfUniqueTexts));
    LValue farthest = m_out.add(firstIndex, m_out.constInt64(mostElementsSkipped));
    limit = m_out.select(m_out.lessThan(limit, farthest), limit, farthest);
    LValue index = m_out.phiIn(iteration, Machine::Int64);
    m_out.upsilon(firstIndex, index);
    m_out.jump(normalEdge(iteration));

    m_out.appendTo(iteration);
    Boxed element(m_out.load64(elements.elementOfStorage(ArrayStorage::Contiguous, storage.butterfly.value(), index)));
    LBasicBlock isPresentCell = newBlock();
    LBasicBlock isString = newBlock();
    LBasicBlock isNoCell = m_loop.skipsOtherValues ? newBlock() : left;
    LBasicBlock mayShareText = newBlock();
    m_out.branch(isCellAndNotEmpty(element).value(), normalEdge(isPresentCell), unlikelyEdge(isNoCell));
    if (m_loop.skipsOtherValues) {
        m_out.appendTo(isNoCell);
        m_out.branch(m_out.isZero64(element.value()), unlikelyEdge(left), normalEdge(next));
    }
    m_out.appendTo(isPresentCell);
    m_out.branch(hasCellType(unboxCell(element), StringType).value(), normalEdge(isString), unlikelyEdge(m_loop.skipsOtherValues ? next : left));
    m_out.appendTo(isString);
    Pointer text = loadField<Pointer>(unboxCell(element), m_heaps.JSString_value);
    branchOrLeave(hasNoBits(text, JSString::isRopeInPointer), left);
    branchOrLeave(notEqual(text, soughtText), left);
    m_out.branch(m_out.testIsZero32(loadFlags(text).value(), soughtFlags.value()), normalEdge(mayShareText), normalEdge(next));
    m_out.appendTo(mayShareText);
    m_out.branch(equal(loadLength(text), soughtLength).value(), unlikelyEdge(left), normalEdge(next));

    m_out.appendTo(next);
    LValue nextIndex = m_out.add(index, 1);
    m_out.branch(m_out.lessThan(nextIndex, limit), normalEdge(again), unlikelyEdge(done));
    m_out.appendTo(again);
    m_out.upsilon(nextIndex, index);
    m_out.jump(normalEdge(iteration));

    auto leave = [&](LBasicBlock from, LValue counter) {
        m_out.appendTo(from);
        m_out.upsilon(first->type() == counter->type() ? counter : m_out.castToInt32(counter), counterOfWritten);
        m_out.jump(normalEdge(written));
    };
    leave(done, nextIndex);
    leave(left, index);
    leave(unchanged, first);
}

bool CodeGenerator::carriesBesideParameters(BlockID head) const
{
    auto isAtHead = [&](const auto& carried) { return carried.block == head; };
    return std::ranges::any_of(m_listStorages, [&](const ListStorage& storage) { return !storage.isBehindCall && isAtHead(storage); })
        || std::ranges::any_of(m_loopCarriedStrings, [&](const LoopCarriedString& string) { return string.head == head; })
        || std::ranges::any_of(m_loopCarriedTables, isAtHead) || std::ranges::any_of(m_carriedSubstrings, isAtHead)
        || m_firstLoopCarriedNumbers.contains(head.index());
}

void CodeGenerator::tryEmitSearchLoop(BlockID header)
{
    if (m_keepsValuesInHeapFrame || (m_followsNumbersOutOfBytes && !m_followsMarkers) || m_interleaving || m_headOfTurnsLaidOut || m_checksEffects)
        return;
    if (carriesBesideParameters(header))
        return;
    std::optional<SearchLoop> loop = SearchLoopFinder(*this, header).find();
    if (!loop)
        return;
    LValue first = m_values[loop->counter.index()];
    LBasicBlock writtenHeader = m_out.newBlock();
    LBasicBlock written = m_out.newBlock();
    LValue counterOfWritten = m_out.phiIn(written, first->type());
    std::span<const ValueID> parameters = code().at(header).parameters();
    Vector<LValue, 4> phis;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phis.append(phi ? m_out.phiIn(writtenHeader, phi->type()) : nullptr);
    }
    m_out.appendTo(blockFor(header));
    {
        ValueContext equality = contextOf(code().blockOf(loop->equality), loop->equality);
        SetForScope<const OperationContext*> whileSearching(m_operation, &equality);
        SearchLoopEmitter(*this, *loop).emit(first, written, counterOfWritten);
    }
    if (m_hasFailed)
        return;
    m_out.appendTo(written);
    for (size_t index = 0; index < parameters.size(); ++index) {
        LValue& value = m_values[parameters[index].index()];
        if (!phis[index])
            continue;
        m_out.upsilon(parameters[index] == loop->counter ? counterOfWritten : value, phis[index]);
        value = phis[index];
    }
    m_out.jump(normalEdge(writtenHeader));
    m_blocks[header.index()] = writtenHeader;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
