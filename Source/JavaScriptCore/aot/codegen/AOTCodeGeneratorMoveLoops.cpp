/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBarrierEmitter.h"
#include "AOTCompileUnit.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTOperationContext.h"
#include "AOTTypeTestEmitter.h"
#include <algorithm>
#include <ranges>
#include <wtf/Scope.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet namesOfMovedValues { Opcode::Narrowed, Opcode::Unbox, Opcode::Box, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::IntegerToNumber, Opcode::NumberToInteger };

struct MoveLoop {
    struct Key {
        ValueID base;
        std::optional<unsigned> read;
        bool hasCounter { false };
        int64_t offset { 0 };

        friend bool operator==(const Key&, const Key&) = default;
        bool isInvariant() const { return !read && !hasCounter; }
    };
    struct Array {
        ValueID invariant;
        std::optional<unsigned> read;
        unsigned group { 0 };
        bool holdsRows { false };
        bool isStoredInto { false };
        bool holdsNumbers { false };
        bool hasTestedNames { false };
    };
    struct Read {
        ValueID value;
        unsigned array { 0 };
        Key key;
        bool isRow { false };
        bool isKey { false };
        bool isMoved { false };
        bool isNumber { false };
        bool isOnlyAsked { false };
        bool isHoisted { false };
        bool isReadAgain { false };
    };
    struct Store {
        ValueID value;
        BlockID block;
        unsigned array { 0 };
        Key key;
        std::optional<unsigned> moved;
        ValueID invariant;
        ValueID number;
    };
    struct CarriedNumber {
        ValueID parameter;
        ValueID next;
        unsigned place { 0 };
    };
    struct Decision {
        ValueID condition;
        bool expected { false };
    };
    struct Comparison {
        Key left;
        Key right;
        std::optional<unsigned> lengthOf;
    };
    BlockID head;
    BlockID testing;
    ValueID counter;
    unsigned placeOfCounter { 0 };
    Vector<CarriedNumber, 2> carriedNumbers;
    ValueID bound;
    std::optional<unsigned> boundIsLengthOf;
    Opcode relation { Opcode::Less };
    ValueID poll;
    Vector<Array, 4> arrays;
    Vector<Read, 6> reads;
    Vector<Store, 2> stores;
    Vector<Decision, 4> decisions;
    Vector<Comparison, 2> comparisons;
    Vector<ValueID, 4> invariantNumbers;
    Vector<ValueID, 2> numbersAsked;
    Vector<ValueID, 2> integers;
    Vector<BlockID, 24> path;
    Vector<ValueID, 16> names;
    Vector<ValueID, 8> operators;
    Vector<std::pair<ValueID, ValueID>, 4> aliases;
    Vector<std::pair<ValueID, unsigned>, 8> readsOfValues;
    Vector<std::pair<unsigned, unsigned>, 4> distinct;

    bool isOnPath(const Code& code, ValueID value) const { return !code.isConstant(value) && path.contains(code.blockOf(value)); }
    ValueID resolve(const Code& code, ValueID value) const
    {
        while (isOnPath(code, value)) {
            if (names.contains(value))
                value = code.at(value).operand(0);
            else if (auto alias = std::ranges::find(aliases, value, &std::pair<ValueID, ValueID>::first); alias != aliases.end())
                value = alias->second;
            else
                break;
        }
        return value;
    }
    std::optional<unsigned> findRead(ValueID value) const
    {
        auto read = std::ranges::find(readsOfValues, value, &std::pair<ValueID, unsigned>::first);
        return read == readsOfValues.end() ? std::nullopt : std::optional(read->second);
    }
};

class MoveLoopFinder {
public:
    MoveLoopFinder(const CodeGenerator& generator, BlockID head)
        : m_generator(generator)
        , m_code(generator.code())
    {
        m_found.head = head;
    }

    std::optional<MoveLoop> find();

private:
    using Key = MoveLoop::Key;
    const Operation& at(ValueID value) const { return m_code.at(value); }
    bool isPlainBlock(BlockID id) const
    {
        const Block& block = m_code.at(id);
        return m_code.isReached(id) && block.frequency == m_code.at(m_found.head).frequency && !m_code.handlerOf(id) && !block.subject && !block.isEntry && !block.isHandlerBlock();
    }
    bool isOnPath(ValueID value) const { return m_found.isOnPath(m_code, value); }
    bool isAvailableBefore(ValueID value) const { return m_code.isConstant(value) || (!isOnPath(value) && m_generator.m_values[value.index()]); }
    ValueID resolve(ValueID value) const { return m_found.resolve(m_code, value); }
    std::optional<unsigned> findRead(ValueID value) const { return m_found.findRead(value); }
    std::optional<unsigned> addArray(ValueID);
    bool addTerm(Key&, ValueID, bool isSubtracted, unsigned depth = 0);
    std::optional<Key> keyOf(ValueID);
    bool addRead(ValueID, ValueID array, ValueID position);
    bool askElement(ValueID test);
    bool addStore(BlockID, ValueID, ValueID array, ValueID position, ValueID stored);
    bool addNumber(ValueID, unsigned depth = 0);
    bool isCarriedNumber(ValueID value) const { return std::ranges::contains(m_found.carriedNumbers, value, &MoveLoop::CarriedNumber::parameter); }
    bool isTestOfNumber(ValueID);
    bool isListed(ValueID value) const { return std::ranges::any_of(m_found.path, [&](BlockID block) { return std::ranges::contains(m_code.valuesOf(m_code.at(block)), value); }); }
    bool isTestOfNaN(ValueID test) { return at(test).opcode == Opcode::StrictEq && at(test).form() == OperatorForm::Numbers && at(test).operand(0) == at(test).operand(1) && addNumber(at(test).operand(0)); }
    bool addOperation(BlockID, ValueID);
    bool addComparison(ValueID);
    bool isTestOfArray(ValueID);
    bool testsForArray(const Operation& operation) const
    {
        if (operation.opcode != Opcode::TypeTest)
            return false;
        TypeArena& types = m_generator.inputs().types;
        const Type& tested = types.at(operation.testedType());
        const ClassPart* part = tested.isObject() ? singleBuiltinClassOf(types, tested) : nullptr;
        return part && part->builtinClass == BuiltinClasses::Array;
    }
    bool followHeader();
    bool followBody();
    unsigned groupOf(unsigned array) const;
    bool joinGroups(unsigned array, unsigned other);
    bool finish();

    static constexpr unsigned mostBlocks = 24;
    static constexpr size_t mostParameters = 3;
    static constexpr size_t mostReads = 8;
    static constexpr size_t mostStores = 4;
    const CodeGenerator& m_generator;
    const Code& m_code;
    Vector<ValueID, 2> m_operatorsOfAnyValues;
    Vector<ValueID, 4> m_presenceTests;
    MoveLoop m_found;
};

std::optional<unsigned> MoveLoopFinder::addArray(ValueID value)
{
    static constexpr OpcodeSet names { Opcode::Narrowed, Opcode::Unbox, Opcode::Box };
    value = resolve(value);
    std::optional<unsigned> read = isOnPath(value) ? findRead(value) : std::nullopt;
    if (isOnPath(value) ? !read : !isAvailableBefore(value) || m_code.isConstant(value))
        return std::nullopt;
    if (!read) {
        Representation representation = at(value).representation;
        if (representation != Representation::JSValue && representation != Representation::Cell)
            return std::nullopt;
        while (names.contains(at(value).opcode) && !at(value).operands().empty() && isAvailableBefore(at(value).operand(0)) && !m_code.isConstant(at(value).operand(0))) {
            Representation named = at(at(value).operand(0)).representation;
            if (named != Representation::JSValue && named != Representation::Cell)
                break;
            value = at(value).operand(0);
        }
    }
    for (unsigned place = 0; place < m_found.arrays.size(); ++place) {
        const MoveLoop::Array& array = m_found.arrays[place];
        if (read ? array.read == read : array.invariant == value)
            return place;
    }
    m_found.arrays.append(MoveLoop::Array { read ? ValueID() : value, read, static_cast<unsigned>(m_found.arrays.size()) });
    return m_found.arrays.size() - 1;
}

bool MoveLoopFinder::addTerm(Key& key, ValueID value, bool isSubtracted, unsigned depth)
{
    static constexpr unsigned mostTerms = 4;
    static constexpr int64_t mostOffset = 1 << 20;
    value = resolve(value);
    int64_t sign = isSubtracted ? -1 : 1;
    if (m_code.isConstant(value) && at(value).opcode == Opcode::Integer) {
        int64_t integer = at(value).integer();
        if (integer < -mostOffset || integer > mostOffset)
            return false;
        key.offset += sign * integer;
        return true;
    }
    if (value == m_found.counter) {
        if (isSubtracted || std::exchange(key.hasCounter, true))
            return false;
        return true;
    }
    if (isOnPath(value)) {
        if (m_found.operators.contains(value)) {
            const Operation& operation = at(value);
            if (depth == mostTerms)
                return false;
            bool countsInDoubles = at(m_found.counter).representation == Representation::Double && operation.representation == Representation::Double && operation.form() == OperatorForm::Numbers;
            bool isInteger = operation.representation == Representation::Int32 || operation.representation == Representation::Int64;
            switch (operation.opcode) {
            case Opcode::Increment: case Opcode::Decrement:
                if (operation.form() != OperatorForm::Integers && operation.form() != OperatorForm::CheckedIntegers && !countsInDoubles)
                    return false;
                key.offset += operation.opcode == Opcode::Increment ? sign : -sign;
                return addTerm(key, operation.operand(0), isSubtracted, depth + 1);
            case Opcode::Add: case Opcode::Sub:
                if ((!isInteger || operation.form() != OperatorForm::Integers) && !countsInDoubles)
                    return false;
                return addTerm(key, operation.operand(0), isSubtracted, depth + 1) && addTerm(key, operation.operand(1), isSubtracted != (operation.opcode == Opcode::Sub), depth + 1);
            default:
                return false;
            }
        }
        std::optional<unsigned> read = findRead(value);
        if (!read || isSubtracted || depth || key.read)
            return false;
        key.read = read;
        return true;
    }
    if (!isAvailableBefore(value) || isSubtracted || key.base)
        return false;
    Representation representation = at(value).representation;
    if (representation != Representation::Int32 && representation != Representation::Int64 && representation != Representation::JSValue && representation != Representation::Double)
        return false;
    key.base = value;
    return true;
}

std::optional<MoveLoop::Key> MoveLoopFinder::keyOf(ValueID value)
{
    Key key;
    if (!addTerm(key, value, false) || (key.read && (key.base || key.hasCounter || key.offset)))
        return std::nullopt;
    return key;
}

bool MoveLoopFinder::addRead(ValueID value, ValueID array, ValueID position)
{
    std::optional<unsigned> from = addArray(array);
    std::optional<Key> key = from ? keyOf(position) : std::nullopt;
    if (!key)
        return false;
    for (unsigned place = 0; place < m_found.reads.size(); ++place) {
        if (m_found.reads[place].array == *from && m_found.reads[place].key == *key) {
            m_found.readsOfValues.append({ value, place });
            m_found.reads[place].isReadAgain |= !m_found.stores.isEmpty();
            return true;
        }
    }
    if (m_found.reads.size() == mostReads || !m_found.stores.isEmpty())
        return false;
    m_found.readsOfValues.append({ value, static_cast<unsigned>(m_found.reads.size()) });
    m_found.reads.append(MoveLoop::Read { value, *from, *key });
    return true;
}

bool MoveLoopFinder::askElement(ValueID test)
{
    const Operation& operation = at(test);
    std::optional<unsigned> from = addArray(operation.arrayOperand());
    std::optional<Key> key = from ? keyOf(operation.positionOperand()) : std::nullopt;
    if (!key)
        return false;
    if (std::ranges::any_of(m_found.reads, [&](const MoveLoop::Read& read) { return read.array == *from && read.key == *key; }))
        return true;
    ArrayStorage storage = operation.arrayStorage();
    if (m_found.reads.size() == mostReads || (storage != ArrayStorage::Double && storage != ArrayStorage::Contiguous))
        return false;
    MoveLoop::Read read { test, *from, *key };
    read.isOnlyAsked = true;
    read.isNumber = storage == ArrayStorage::Double;
    m_found.reads.append(read);
    return true;
}

bool MoveLoopFinder::addStore(BlockID block, ValueID value, ValueID array, ValueID position, ValueID stored)
{
    stored = resolve(stored);
    std::optional<unsigned> moved = isOnPath(stored) ? findRead(stored) : std::nullopt;
    bool isComputed = !moved && isOnPath(stored);
    if (isComputed ? !addNumber(stored) : !moved && (!isAvailableBefore(stored) || at(stored).representation != Representation::JSValue))
        return false;
    std::optional<unsigned> into = addArray(array);
    std::optional<Key> key = into ? keyOf(position) : std::nullopt;
    if (!key || m_found.stores.size() == mostStores)
        return false;
    if (moved)
        m_found.reads[*moved].isMoved = true;
    m_found.arrays[*into].isStoredInto = true;
    m_found.arrays[*into].holdsNumbers |= isComputed;
    m_found.stores.append(MoveLoop::Store { value, block, *into, *key, moved, moved || isComputed ? ValueID() : stored, isComputed ? stored : ValueID() });
    return true;
}

bool MoveLoopFinder::addNumber(ValueID value, unsigned depth)
{
    static constexpr unsigned mostDepth = 6;
    value = resolve(value);
    if (value == m_found.counter || isCarriedNumber(value))
        return true;
    if (!isOnPath(value)) {
        if (m_code.isConstant(value))
            return at(value).opcode == Opcode::Integer || at(value).opcode == Opcode::Double;
        Representation representation = at(value).representation;
        if (!isAvailableBefore(value) || (representation != Representation::Int32 && representation != Representation::Int64 && representation != Representation::Double && representation != Representation::JSValue))
            return false;
        if (!m_found.invariantNumbers.contains(value))
            m_found.invariantNumbers.append(value);
        return true;
    }
    if (std::optional<unsigned> read = findRead(value)) {
        m_found.reads[*read].isNumber = true;
        return true;
    }
    if (depth == mostDepth || !m_found.operators.contains(value))
        return false;
    m_operatorsOfAnyValues.removeFirst(value);
    const Operation& operation = at(value);
    switch (operation.opcode) {
    case Opcode::Increment: case Opcode::Decrement:
        return addNumber(operation.operand(0), depth + 1);
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div:
        return addNumber(operation.operand(0), depth + 1) && addNumber(operation.operand(1), depth + 1);
    default:
        return false;
    }
}

bool MoveLoopFinder::isTestOfNumber(ValueID test)
{
    const Operation& operation = at(test);
    ValueID tested = operation.opcode == Opcode::TypeTest && operation.testedType() == TypeID(TypeTable::number) ? resolve(operation.valueOperand()) : ValueID();
    if (tested && isCarriedNumber(tested))
        return true;
    std::optional<unsigned> read = tested && isOnPath(tested) ? findRead(tested) : std::nullopt;
    if (!read)
        return false;
    m_found.reads[*read].isNumber = true;
    return true;
}

bool MoveLoopFinder::addOperation(BlockID block, ValueID value)
{
    const Operation& operation = at(value);
    switch (operation.opcode) {
    case Opcode::Narrowed: case Opcode::Unbox: case Opcode::Box: case Opcode::WidenInteger: case Opcode::NarrowInteger: case Opcode::IntegerToNumber: case Opcode::NumberToInteger:
        m_found.names.append(value);
        return true;
    case Opcode::RequireObjectCoercible:
        m_found.names.append(value);
        return !!addArray(operation.valueOperand());
    case Opcode::ElemGet:
        return addRead(value, operation.objectOperand(), operation.propertyOperand());
    case Opcode::GetArrayElement:
        return addRead(value, operation.arrayOperand(), operation.positionOperand());
    case Opcode::ElemSet:
        return addStore(block, value, operation.objectOperand(), operation.propertyOperand(), operation.valueOperand());
    case Opcode::PutArrayElement:
        return addStore(block, value, operation.arrayOperand(), operation.positionOperand(), operation.valueOperand());
    case Opcode::PutContiguousIndex: case Opcode::PutDoubleIndex:
        return addStore(block, value, operation.arrayOperand(), operation.positionOperand(), operation.valueOperand());
    case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::Div: case Opcode::Increment: case Opcode::Decrement:
        m_found.operators.append(value);
        if (operation.form() != OperatorForm::Integers && operation.form() != OperatorForm::CheckedIntegers && operation.form() != OperatorForm::Numbers)
            m_operatorsOfAnyValues.append(value);
        return true;
    case Opcode::Less:
        return operation.form() == OperatorForm::Integers;
    case Opcode::TypeTest:
        return isTestOfArray(value) || isTestOfNumber(value) || m_generator.inputs().types.at(operation.testedType()).isObject();
    case Opcode::StrictEq:
        return isTestOfNaN(value);
    case Opcode::IsPristine: {
        OptionSet<PristineAspects> aspects = operation.aspects();
        aspects.remove(PristineAspects::Storage);
        if (aspects.isEmpty())
            return true;
        bool asksNames = operation.builtinClass() == BuiltinClassID(BuiltinClasses::Array) && aspects == OptionSet<PristineAspects> { PristineAspects::Names };
        std::optional<unsigned> array = asksNames ? addArray(operation.valueOperand()) : std::nullopt;
        if (!array)
            return false;
        m_found.arrays[*array].hasTestedNames = true;
        return true;
    }
    case Opcode::GetArrayStorage: case Opcode::GetArrayLength: case Opcode::HasArrayStorage: case Opcode::IsElementPresent:
        return true;
    default:
        return false;
    }
}

bool MoveLoopFinder::isTestOfArray(ValueID test)
{
    return testsForArray(at(test)) && addArray(at(test).valueOperand());
}

bool MoveLoopFinder::addComparison(ValueID test)
{
    const Operation& operation = at(test);
    if (operation.opcode != Opcode::Less || operation.form() != OperatorForm::Integers || !m_found.stores.isEmpty())
        return false;
    MoveLoop::Comparison comparison;
    std::optional<Key> left = keyOf(operation.leftOperand());
    if (!left || left->read)
        return false;
    comparison.left = *left;
    ValueID right = resolve(operation.rightOperand());
    if (isOnPath(right) && at(right).opcode == Opcode::GetArrayLength) {
        comparison.lengthOf = addArray(at(right).arrayOperand());
        if (!comparison.lengthOf)
            return false;
    } else {
        std::optional<Key> bound = keyOf(right);
        if (!bound || bound->read)
            return false;
        comparison.right = *bound;
    }
    m_found.comparisons.append(comparison);
    return true;
}

bool MoveLoopFinder::followHeader()
{
    const Block& head = m_code.at(m_found.head);
    if (!isPlainBlock(m_found.head) || !head.isLoopHead || head.parameters().size() > mostParameters)
        return false;
    m_found.path.append(m_found.head);
    BlockID testing = m_found.head;
    while (m_code.at(testing).terminator.kind == TerminatorKind::Jump) {
        const Edge& edge = m_code.at(m_code.at(testing).terminator.next());
        if (m_found.path.size() == mostBlocks || !isPlainBlock(edge.to) || m_code.at(edge.to).isLoopHead || !m_code.at(edge.to).parameters().empty())
            return false;
        testing = edge.to;
        m_found.path.append(testing);
    }
    if (m_code.at(testing).terminator.kind != TerminatorKind::Branch)
        return false;
    m_found.testing = testing;
    static constexpr OpcodeSet relations { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };
    ValueID test = m_code.at(testing).terminator.conditionOperand();
    if (!isOnPath(test) || !relations.contains(at(test).opcode))
        return false;
    m_found.relation = at(test).opcode;
    for (BlockID block : m_found.path) {
        for (ValueID value : m_code.valuesOf(m_code.at(block))) {
            if (namesOfMovedValues.contains(at(value).opcode))
                m_found.names.append(value);
        }
    }
    ValueID counter = resolve(at(test).operand(0));
    for (unsigned place = 0; place < head.parameters().size(); ++place) {
        ValueID parameter = head.parameters()[place];
        Representation representation = at(parameter).representation;
        bool isNumber = representation == Representation::Double && !m_generator.isCarriedBoxed(parameter);
        if (!m_generator.m_values[parameter.index()])
            return false;
        if (parameter == counter) {
            if (representation != Representation::Int32 && representation != Representation::Int64 && !isNumber)
                return false;
            m_found.counter = counter;
            m_found.placeOfCounter = place;
        } else if (isNumber || representation == Representation::JSValue)
            m_found.carriedNumbers.append(MoveLoop::CarriedNumber { parameter, { }, place });
        else
            return false;
    }
    if (!m_found.counter)
        return false;
    for (BlockID block : m_found.path) {
        for (ValueID value : m_code.valuesOf(m_code.at(block))) {
            const Operation& operation = at(value);
            if (value == test || namesOfMovedValues.contains(operation.opcode) || operation.opcode == Opcode::GetArrayLength)
                continue;
            if (operation.opcode != Opcode::LoopHint || operation.throwsOwedError() || m_found.poll)
                return false;
            m_found.poll = value;
        }
    }
    const Operation& comparison = at(test);
    if (resolve(comparison.leftOperand()) != counter)
        return false;
    ValueID bound = resolve(comparison.rightOperand());
    if (isOnPath(bound)) {
        if (at(bound).opcode != Opcode::GetArrayLength)
            return false;
        m_found.boundIsLengthOf = addArray(at(bound).arrayOperand());
        return !!m_found.boundIsLengthOf;
    }
    if (!isAvailableBefore(bound))
        return false;
    switch (at(bound).representation) {
    case Representation::Int32: case Representation::Int64: case Representation::Double: case Representation::OptionalDouble: case Representation::JSValue:
        break;
    default:
        return false;
    }
    m_found.bound = bound;
    return true;
}

bool MoveLoopFinder::followBody()
{
    const Edge* edge = &m_code.at(m_code.at(m_found.testing).terminator.taken());
    while (edge->to != m_found.head) {
        BlockID id = edge->to;
        const Block& block = m_code.at(id);
        if (m_found.path.size() == mostBlocks || !isPlainBlock(id) || block.isLoopHead || m_found.path.contains(id) || block.parameters().size() != edge->arguments().size())
            return false;
        m_found.path.append(id);
        for (unsigned place = 0; place < block.parameters().size(); ++place) {
            if (edge->arguments()[place])
                m_found.aliases.append({ block.parameters()[place], edge->arguments()[place] });
        }
        for (ValueID value : m_code.valuesOf(block)) {
            if (!addOperation(id, value))
                return false;
        }
        if (block.terminator.kind == TerminatorKind::Jump) {
            edge = &m_code.at(block.terminator.next());
            continue;
        }
        if (block.terminator.kind != TerminatorKind::Branch)
            return false;
        ValueID condition = block.terminator.conditionOperand();
        if (isOnPath(condition) && !isListed(condition))
            return false;
        const Edge& taken = m_code.at(block.terminator.taken());
        const Edge& notTaken = m_code.at(block.terminator.notTaken());
        if (isOnPath(condition) && (isTestOfArray(condition) || isTestOfNumber(condition))) {
            edge = &taken;
            continue;
        }
        bool isForObject = at(condition).opcode == Opcode::TypeTest && m_generator.inputs().types.at(at(condition).testedType()).isObject();
        if (isOnPath(condition) && isForObject) {
            ValueID key = resolve(at(condition).operand(0));
            if (isOnPath(key) || !isAvailableBefore(key) || at(key).representation != Representation::JSValue || !isPlainBlock(notTaken.to))
                return false;
            m_found.integers.append(key);
            edge = &notTaken;
            continue;
        }
        if (isAvailableBefore(condition)) {
            bool isForArray = testsForArray(at(condition));
            bool goesTaken = isForArray || (isPlainBlock(taken.to) && !(isForObject && isPlainBlock(notTaken.to)));
            if (at(condition).representation != Representation::Boolean || (!isForArray && !isForObject && goesTaken == isPlainBlock(notTaken.to)))
                return false;
            m_found.decisions.append(MoveLoop::Decision { condition, goesTaken });
            edge = goesTaken ? &taken : &notTaken;
            continue;
        }
        if (isOnPath(condition) && at(condition).opcode == Opcode::HasArrayStorage && isPlainBlock(taken.to) && isPlainBlock(notTaken.to)) {
            ArrayStorage storage = at(condition).arrayStorage();
            std::optional<unsigned> array = storage == ArrayStorage::Int32 || storage == ArrayStorage::Double ? addArray(at(condition).arrayOperand()) : std::nullopt;
            if (!array)
                return false;
            m_found.arrays[*array].holdsNumbers = true;
            edge = storage == ArrayStorage::Double ? &taken : &notTaken;
            continue;
        }
        if (isOnPath(condition) && isTestOfNaN(condition) && isPlainBlock(taken.to) && !isPlainBlock(notTaken.to)) {
            m_found.numbersAsked.append(at(condition).operand(0));
            edge = &taken;
            continue;
        }
        static constexpr OpcodeSet testsOfArrays { Opcode::HasArrayStorage, Opcode::IsPristine, Opcode::IsElementPresent };
        if (!isOnPath(condition) || !isPlainBlock(taken.to) || isPlainBlock(notTaken.to) || (!testsOfArrays.contains(at(condition).opcode) && !addComparison(condition)))
            return false;
        if (at(condition).opcode == Opcode::IsElementPresent)
            m_presenceTests.append(condition);
        edge = &taken;
    }
    if (edge->arguments().size() != m_code.at(m_found.head).parameters().size())
        return false;
    if (!std::ranges::all_of(edge->arguments(), [](ValueID argument) { return !!argument; }))
        return false;
    for (MoveLoop::CarriedNumber& carried : m_found.carriedNumbers) {
        carried.next = resolve(edge->arguments()[carried.place]);
        if (!addNumber(carried.next))
            return false;
    }
    for (ValueID test : m_presenceTests) {
        if (!askElement(test))
            return false;
    }
    std::optional<Key> next = keyOf(edge->arguments()[m_found.placeOfCounter]);
    bool goesUp = m_found.relation == Opcode::Less || m_found.relation == Opcode::LessEq;
    return next && next->hasCounter && next->offset == (goesUp ? 1 : -1) && !next->base && !next->read;
}

unsigned MoveLoopFinder::groupOf(unsigned array) const
{
    while (m_found.arrays[array].group != array)
        array = m_found.arrays[array].group;
    return array;
}

bool MoveLoopFinder::joinGroups(unsigned array, unsigned other)
{
    m_found.arrays[groupOf(array)].group = groupOf(other);
    return true;
}

bool MoveLoopFinder::finish()
{
    if (!m_operatorsOfAnyValues.isEmpty())
        return false;
    for (const MoveLoop::Array& array : m_found.arrays) {
        if (array.read)
            m_found.reads[*array.read].isRow = true;
    }
    for (MoveLoop::Read& read : m_found.reads) {
        m_found.arrays[read.array].holdsRows |= read.isRow;
        m_found.arrays[read.array].holdsNumbers |= read.isNumber;
        if (read.key.read)
            m_found.reads[*read.key.read].isKey = true;
    }
    for (const MoveLoop::Store& store : m_found.stores) {
        if (store.key.read)
            m_found.reads[*store.key.read].isKey = true;
        if (store.moved)
            joinGroups(m_found.reads[*store.moved].array, store.array);
    }
    static constexpr OpcodeSet copies { Opcode::CopyElements, Opcode::CopyElementsDown, Opcode::FillElements };
    bool knowsKinds = std::ranges::all_of(m_found.stores, [&](const MoveLoop::Store& store) {
        Opcode stores = at(store.value).opcode;
        if (stores != Opcode::PutContiguousIndex && stores != Opcode::PutDoubleIndex)
            return false;
        if (!store.moved)
            return true;
        const Operation& read = at(m_found.reads[*store.moved].value);
        return read.opcode == Opcode::GetArrayElement && hasArrayStorageForm(read.arrayStorage());
    });
    bool someRowChanges = std::ranges::any_of(m_found.arrays, [&](const MoveLoop::Array& array) { return array.read && m_found.reads[*array.read].key.hasCounter; });
    if (m_found.stores.isEmpty()) {
        knowsKinds = std::ranges::all_of(m_found.reads, [&](const MoveLoop::Read& read) {
            const Operation& operation = at(read.value);
            return read.isRow || read.isOnlyAsked || (operation.opcode == Opcode::GetArrayElement && hasArrayStorageForm(operation.arrayStorage()));
        });
    }
    bool countsRows = m_generator.m_rows && std::ranges::any_of(m_generator.m_rows->guards, [&](const CodeGenerator::RowGuard& guard) { return m_found.path.contains(guard.presenceBlock); });
    if (knowsKinds && (countsRows || !someRowChanges))
        return false;
    bool isPlainCopy = m_found.carriedNumbers.isEmpty() && m_found.reads.size() <= 1 && m_found.stores.size() == 1 && m_found.arrays.size() <= 2;
    if (isPlainCopy && m_code.opcodesOfValues().intersects(copies))
        return false;
    bool addsUp = std::ranges::any_of(m_found.reads, &MoveLoop::Read::isNumber) && !m_found.carriedNumbers.isEmpty();
    if ((m_found.stores.isEmpty() && !addsUp) || !std::ranges::all_of(m_found.reads, [](const MoveLoop::Read& read) { return read.isRow || read.isKey || read.isMoved || read.isNumber || read.isOnlyAsked; }))
        return false;
    for (const MoveLoop::Array& array : m_found.arrays) {
        if (array.holdsRows && array.isStoredInto)
            return false;
    }
    for (MoveLoop::Read& read : m_found.reads) {
        if ((read.isReadAgain && (read.isMoved || read.isKey || read.isNumber)) || (read.isRow && read.isNumber))
            return false;
        if (!read.isRow)
            continue;
        const MoveLoop::Array& holder = m_found.arrays[read.array];
        read.isHoisted = !read.isKey && !read.isMoved && read.key.isInvariant() && (!holder.read || m_found.reads[*holder.read].isHoisted);
        if (!read.isHoisted && !read.isReadAgain)
            continue;
        for (const MoveLoop::Store& store : m_found.stores) {
            std::pair<unsigned, unsigned> pair { read.array, store.array };
            if (pair.first == pair.second)
                return false;
            if (!m_found.distinct.contains(pair))
                m_found.distinct.append(pair);
        }
    }
    for (unsigned place = 0; place < m_found.arrays.size(); ++place) {
        unsigned group = groupOf(place);
        m_found.arrays[group].holdsNumbers |= m_found.arrays[place].holdsNumbers;
    }
    for (unsigned place = 0; place < m_found.arrays.size(); ++place)
        m_found.arrays[place].group = groupOf(place);
    return true;
}

std::optional<MoveLoop> MoveLoopFinder::find()
{
    if (!followHeader() || !followBody() || !finish())
        return std::nullopt;
    return m_found;
}

class MoveLoopEmitter {
public:
    MoveLoopEmitter(CodeGenerator& generator, const MoveLoop& loop)
        : m_generator(generator)
        , m_code(generator.code())
        , m_out(generator.m_out)
        , m_elements(generator.emitter<ElementAccessEmitter>())
        , m_loop(loop)
        , m_arrays(loop.arrays.size())
        , m_groups(loop.arrays.size())
        , m_words(loop.reads.size())
        , m_storedWords(loop.stores.size())
    {
    }

    void emit(std::span<const LValue> firsts, LBasicBlock written, std::span<const LValue> phisOfWritten);

private:
    struct FoundArray {
        LValue cell { nullptr };
        LValue storage { nullptr };
        LValue length { nullptr };
        bool isFoundBefore { false };
    };
    struct Group {
        LValue kind { nullptr };
        LValue hole { nullptr };
        LValue isBoxed { nullptr };
    };
    static constexpr uint32_t maskOfKind = 0xff00 | IsArray | IndexingShapeMask;
    static constexpr int32_t kindWith(IndexingType type) { return ArrayType << 8 | type; }
    static_assert(ArrayWithInt32 == 5 && ArrayWithDouble == 7 && ArrayWithContiguous == 9 && ArrayWithArrayStorage > 9);

    LValue valueOf(ValueID value)
    {
        LValue lowered = m_generator.get(value).value;
        m_out.clearDeclaredAccesses();
        return lowered;
    }
    void leaveUnless(LValue condition)
    {
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(condition, normalEdge(holds), unlikelyEdge(m_exit));
        ScopeFork fork = m_out.forkScope();
        m_out.appendToSuccessor(holds, fork);
    }
    LValue kindOf(LValue cell, uint32_t mask) { return m_elements.cellHeaderBytes(CellPointer(cell), mask).value(); }
    bool goesUp() const { return m_loop.relation == Opcode::Less || m_loop.relation == Opcode::LessEq; }
    LValue isBeforeEnd(LValue counter, LValue limit) { return goesUp() ? m_out.lessThan(counter, limit) : m_out.greaterThanOrEqual(counter, limit); }
    LValue counterInForm(LValue counter)
    {
        Representation representation = m_code.at(m_loop.counter).representation;
        return representation == Representation::Int32 ? m_out.castToInt32(counter) : representation == Representation::Double ? m_out.intToDouble(counter) : counter;
    }
    LValue firstAsInteger(LValue);
    LValue integerOf(ValueID);
    LValue limitOf(LValue first);
    LValue loadWord(unsigned read, LValue counter);
    LValue positionOf(const MoveLoop::Key&, LValue counter);
    bool isBelowLimit(unsigned array, const MoveLoop::Key& key) const { return m_arrays[array].isFoundBefore && key.hasCounter && !key.base; }
    void requireInside(unsigned array, const MoveLoop::Key&, LValue position);
    void findStorage(unsigned array, LValue cell);
    void requireKind(unsigned array, bool asksGroup);
    void requireNames(unsigned array);
    void requirePlainKind(LValue kind);
    void requireCell(LValue word);
    void findRow(unsigned read, LValue word);
    void requireDistinct(bool areFoundBefore);
    LValue wordFor(const Group&, LValue boxed);
    LValue asNumber(LValue boxed);
    LValue numberInFront(ValueID);
    LValue numberOf(ValueID, LValue counter);
    void emitPoll(LValue counter);
    void emitIteration(LValue counter);

    CodeGenerator& m_generator;
    const Code& m_code;
    Output& m_out;
    ElementAccessEmitter& m_elements;
    const MoveLoop& m_loop;
    Vector<FoundArray, 4> m_arrays;
    Vector<Group, 4> m_groups;
    Vector<LValue, 8> m_words;
    Vector<LValue, 4> m_storedWords;
    Vector<std::pair<ValueID, LValue>, 4> m_numbers;
    std::optional<unsigned> m_carriedRead;
    LValue m_carriedNumber { nullptr };
    LValue m_storedNumber { nullptr };
    Vector<LValue, 2> m_nextNumbers;
    LBasicBlock m_exit { nullptr };
    Vector<std::pair<ValueID, LValue>, 4> m_integers;
};

LValue MoveLoopEmitter::integerOf(ValueID value)
{
    if (auto known = std::ranges::find(m_integers, value, &std::pair<ValueID, LValue>::first); known != m_integers.end())
        return known->second;
    LValue lowered = valueOf(value);
    LValue integer = lowered;
    switch (m_code.at(value).representation) {
    case Representation::Int32:
        integer = m_out.signExt32To64(lowered);
        break;
    case Representation::JSValue:
        leaveUnless(m_elements.areInt32Elements(lowered).value());
        integer = m_out.signExt32To64(m_out.castToInt32(lowered));
        break;
    case Representation::Double: {
        integer = m_out.signExt32To64(m_generator.emitter<ConversionEmitter>().int32OrElse(Number(lowered), m_exit).value());
        break;
    }
    default:
        break;
    }
    m_integers.append({ value, integer });
    return integer;
}

LValue MoveLoopEmitter::firstAsInteger(LValue first)
{
    switch (m_code.at(m_loop.counter).representation) {
    case Representation::Int32:
        return m_out.signExt32To64(first);
    case Representation::Double:
        return m_out.signExt32To64(m_generator.emitter<ConversionEmitter>().int32OrElse(Number(first), m_exit).value());
    default:
        return first;
    }
}

LValue MoveLoopEmitter::limitOf(LValue first)
{
    bool takesNext = m_loop.relation == Opcode::LessEq || m_loop.relation == Opcode::Greater;
    auto next = [&](LValue integer) {
        return takesNext ? m_out.add(integer, m_out.constInt64(1)) : integer;
    };
    if (m_loop.boundIsLengthOf)
        return next(m_arrays[*m_loop.boundIsLengthOf].length);
    Representation representation = m_code.at(m_loop.bound).representation;
    if (representation != Representation::Double && representation != Representation::OptionalDouble)
        return next(integerOf(m_loop.bound));
    static constexpr double most = std::numeric_limits<int32_t>::max() - 1;
    LValue bound = valueOf(m_loop.bound);
    leaveUnless(goesUp() ? m_out.doubleLessThanOrEqual(m_out.intToDouble(first), bound) : m_out.doubleLessThanOrEqual(bound, m_out.intToDouble(first)));
    LValue clamped = m_out.select(m_out.doubleLessThan(bound, m_out.constDouble(most)), bound, m_out.constDouble(most));
    clamped = m_out.select(m_out.doubleLessThan(m_out.constDouble(-most), clamped), clamped, m_out.constDouble(-most));
    LValue truncated = m_generator.emitter<ConversionEmitter>().truncateToWord(Number(clamped)).value();
    LValue wide = m_out.signExt32To64(truncated);
    LValue whole = m_out.intToDouble(truncated);
    if (takesNext)
        return next(m_out.sub(wide, m_out.zeroExt(m_out.doubleLessThan(clamped, whole), Machine::Int64)));
    return m_out.add(wide, m_out.zeroExt(m_out.doubleLessThan(whole, clamped), Machine::Int64));
}

LValue MoveLoopEmitter::positionOf(const MoveLoop::Key& key, LValue counter)
{
    LValue position = key.read ? nullptr : key.base ? integerOf(key.base) : nullptr;
    if (key.read) {
        LValue word = m_words[*key.read];
        const Group& group = m_groups[m_loop.arrays[m_loop.reads[*key.read].array].group];
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock boxes = m_out.newBlock();
        LBasicBlock found = m_out.newBlock();
        LValue index = m_out.phiIn(found, Machine::Int64);
        m_out.branch(m_out.equal(group.kind, m_out.constInt32(kindWith(ArrayWithDouble))), normalEdge(doubles), normalEdge(boxes));
        m_out.appendTo(doubles);
        m_out.upsilon(m_out.signExt32To64(m_generator.emitter<ConversionEmitter>().int32OrElse(Number(m_out.bitCast(word, Machine::Double)), m_exit).value()), index);
        m_out.jump(normalEdge(found));
        m_out.appendTo(boxes);
        leaveUnless(m_elements.areInt32Elements(word).value());
        m_out.upsilon(m_out.signExt32To64(m_out.castToInt32(word)), index);
        m_out.jump(normalEdge(found));
        m_out.appendTo(found);
        return index;
    }
    if (key.hasCounter)
        position = position ? m_out.add(position, counter) : counter;
    if (!position)
        return m_out.constInt64(key.offset);
    return key.offset ? m_out.add(position, m_out.constInt64(key.offset)) : position;
}

void MoveLoopEmitter::requireInside(unsigned array, const MoveLoop::Key& key, LValue position)
{
    if (!isBelowLimit(array, key))
        leaveUnless(m_out.below(position, m_arrays[array].length));
}

void MoveLoopEmitter::findStorage(unsigned array, LValue cell)
{
    ArrayElements elements = m_elements.elementsOf(CellPointer(cell));
    m_arrays[array].cell = cell;
    m_arrays[array].storage = elements.butterfly.value();
    m_arrays[array].length = m_elements.publicLength(elements).value();
}

void MoveLoopEmitter::requireCell(LValue word)
{
    leaveUnless(m_out.isZero64(m_out.lShr(m_out.sub(word, m_out.constInt64(16)), 48)));
}

void MoveLoopEmitter::requireKind(unsigned array, bool asksGroup)
{
    const MoveLoop::Array& found = m_loop.arrays[array];
    LValue kindOfGroup = asksGroup ? m_groups[found.group].kind : nullptr;
    if (found.holdsRows)
        leaveUnless(m_out.equal(kindOf(m_arrays[array].cell, maskOfKind), m_out.constInt32(kindWith(ArrayWithContiguous))));
    if (kindOfGroup)
        leaveUnless(m_out.equal(found.isStoredInto ? m_elements.cellTypeAndIndexing(CellPointer(m_arrays[array].cell)).value() : kindOf(m_arrays[array].cell, maskOfKind), kindOfGroup));
    else if (!found.holdsRows)
        requirePlainKind(kindOf(m_arrays[array].cell, maskOfKind));
}

void MoveLoopEmitter::requireNames(unsigned array)
{
    if (!m_loop.arrays[array].hasTestedNames)
        return;
    LValue hasNamesAsMade = m_generator.emitter<TypeTestEmitter>().isPristine(CellPointer(m_arrays[array].cell), BuiltinClasses::Array, PristineAspects::Names).value();
    m_out.clearDeclaredAccesses();
    leaveUnless(hasNamesAsMade);
}

void MoveLoopEmitter::requirePlainKind(LValue kind)
{
    leaveUnless(m_out.belowOrEqual(m_out.sub(kind, m_out.constInt32(kindWith(ArrayWithInt32))), m_out.constInt32(ArrayWithContiguous - ArrayWithInt32)));
    leaveUnless(m_out.notZero32(m_out.bitAnd(kind, m_out.constInt32(IsArray))));
}

LValue MoveLoopEmitter::loadWord(unsigned read, LValue counter)
{
    const MoveLoop::Read& found = m_loop.reads[read];
    LValue position = positionOf(found.key, counter);
    requireInside(found.array, found.key, position);
    ArrayElements elements { CellPointer(m_arrays[found.array].cell), Pointer(m_arrays[found.array].storage) };
    return m_out.load64(m_elements.elementOfUnknownForm(elements, Integer(position)));
}

void MoveLoopEmitter::findRow(unsigned read, LValue word)
{
    requireCell(word);
    for (unsigned place = 0; place < m_loop.arrays.size(); ++place) {
        if (m_loop.arrays[place].read != read)
            continue;
        m_arrays[place].cell = word;
        requireKind(place, true);
        requireNames(place);
        findStorage(place, word);
    }
}

void MoveLoopEmitter::requireDistinct(bool areFoundBefore)
{
    for (auto [holder, stored] : m_loop.distinct) {
        if ((m_arrays[holder].isFoundBefore && m_arrays[stored].isFoundBefore) == areFoundBefore)
            leaveUnless(m_out.notEqual(m_arrays[holder].cell, m_arrays[stored].cell));
    }
}

LValue MoveLoopEmitter::wordFor(const Group& group, LValue boxed)
{
    LBasicBlock doubles = m_out.newBlock();
    LBasicBlock others = m_out.newBlock();
    LBasicBlock int32s = m_out.newBlock();
    LBasicBlock integers = m_out.newBlock();
    LBasicBlock fractions = m_out.newBlock();
    LBasicBlock found = m_out.newBlock();
    LValue word = m_out.phiIn(found, Machine::Int64);
    auto finish = [&](LValue result) {
        m_out.upsilon(result, word);
        m_out.jump(normalEdge(found));
    };
    LValue isInt32 = m_elements.areInt32Elements(boxed).value();
    m_out.branch(m_out.equal(group.kind, m_out.constInt32(kindWith(ArrayWithDouble))), normalEdge(doubles), normalEdge(others));
    m_out.appendTo(others);
    LBasicBlock boxes = m_out.newBlock();
    m_out.branch(group.isBoxed, normalEdge(boxes), normalEdge(int32s));
    m_out.appendTo(boxes);
    finish(boxed);
    m_out.appendTo(int32s);
    leaveUnless(isInt32);
    finish(boxed);
    m_out.appendTo(doubles);
    m_out.branch(isInt32, normalEdge(integers), normalEdge(fractions));
    m_out.appendTo(integers);
    finish(m_out.bitCast(m_out.intToDouble(m_out.castToInt32(boxed)), Machine::Int64));
    m_out.appendTo(fractions);
    leaveUnless(m_out.testNonZero64(boxed, m_generator.pinned().numberTag));
    LValue bits = m_out.add(boxed, m_generator.pinned().numberTag);
    LValue number = m_out.bitCast(bits, Machine::Double);
    leaveUnless(m_out.doubleEqual(number, number));
    finish(bits);
    m_out.appendTo(found);
    return word;
}

LValue MoveLoopEmitter::asNumber(LValue boxed)
{
    Group doubles { m_out.constInt32(kindWith(ArrayWithDouble)), nullptr, m_out.int32Zero };
    return m_out.bitCast(wordFor(doubles, boxed), Machine::Double);
}

LValue MoveLoopEmitter::numberInFront(ValueID value)
{
    LValue lowered = valueOf(value);
    switch (m_code.at(value).representation) {
    case Representation::Int32: case Representation::Int64:
        return m_out.intToDouble(lowered);
    case Representation::JSValue:
        return asNumber(lowered);
    default:
        return lowered;
    }
}

LValue MoveLoopEmitter::numberOf(ValueID value, LValue counter)
{
    value = m_loop.resolve(m_code, value);
    const Operation& operation = m_code.at(value);
    if (m_code.isConstant(value))
        return m_out.constDouble(operation.opcode == Opcode::Integer ? static_cast<double>(operation.integer()) : operation.number());
    if (value == m_loop.counter)
        return m_out.intToDouble(counter);
    if (auto known = std::ranges::find(m_numbers, value, &std::pair<ValueID, LValue>::first); known != m_numbers.end())
        return known->second;
    if (std::optional<unsigned> read = m_loop.findRead(value))
        return read == m_carriedRead ? m_carriedNumber : m_out.bitCast(m_words[*read], Machine::Double);
    LValue left = numberOf(operation.operand(0), counter);
    switch (operation.opcode) {
    case Opcode::Increment:
        return m_out.doubleAdd(left, m_out.constDouble(1));
    case Opcode::Decrement:
        return m_out.doubleSub(left, m_out.constDouble(1));
    default:
        break;
    }
    LValue right = numberOf(operation.operand(1), counter);
    switch (operation.opcode) {
    case Opcode::Add:
        return m_out.doubleAdd(left, right);
    case Opcode::Sub:
        return m_out.doubleSub(left, right);
    case Opcode::Mul:
        return m_out.doubleMul(left, right);
    case Opcode::Div:
        return m_out.doubleDiv(left, right);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void MoveLoopEmitter::emitPoll(LValue counter)
{
    if (!m_loop.poll)
        return;
    m_generator.m_values[m_loop.counter.index()] = counterInForm(counter);
    m_generator.m_values[m_loop.poll.index()] = nullptr;
    m_generator.generateValue(m_loop.head, m_loop.poll, m_code.at(m_loop.poll), false, { });
    m_out.clearDeclaredAccesses();
}

void MoveLoopEmitter::emitIteration(LValue counter)
{
    for (unsigned place = 0; place < m_loop.reads.size(); ++place) {
        const MoveLoop::Read& read = m_loop.reads[place];
        if (read.isHoisted || m_carriedRead == place)
            continue;
        LValue word = loadWord(place, counter);
        m_words[place] = word;
        if (read.isRow)
            findRow(place, word);
        else
            leaveUnless(m_out.notEqual(word, m_groups[m_loop.arrays[read.array].group].hole));
    }
    for (const MoveLoop::Comparison& comparison : m_loop.comparisons) {
        LValue left = positionOf(comparison.left, counter);
        leaveUnless(m_out.lessThan(left, comparison.lengthOf ? m_arrays[*comparison.lengthOf].length : positionOf(comparison.right, counter)));
    }
    Vector<LValue, 4> positions;
    for (const MoveLoop::Store& store : m_loop.stores) {
        LValue position = positionOf(store.key, counter);
        requireInside(store.array, store.key, position);
        positions.append(position);
    }
    requireDistinct(false);
    for (const MoveLoop::CarriedNumber& carried : m_loop.carriedNumbers) {
        LValue number = numberOf(carried.next, counter);
        leaveUnless(m_out.doubleEqual(number, number));
        m_nextNumbers.append(number);
    }
    for (ValueID asked : m_loop.numbersAsked) {
        if (std::ranges::any_of(m_loop.stores, [&](const MoveLoop::Store& store) { return store.number == m_loop.resolve(m_code, asked); }))
            continue;
        LValue number = numberOf(asked, counter);
        leaveUnless(m_out.doubleEqual(number, number));
    }
    for (unsigned place = 0; place < m_loop.stores.size(); ++place) {
        if (!m_loop.stores[place].number)
            continue;
        LValue number = numberOf(m_loop.stores[place].number, counter);
        leaveUnless(m_out.doubleEqual(number, number));
        m_storedWords[place] = m_out.bitCast(number, Machine::Int64);
        m_storedNumber = number;
    }
    for (unsigned place = 0; place < m_loop.stores.size(); ++place) {
        const MoveLoop::Store& store = m_loop.stores[place];
        ArrayElements elements { CellPointer(m_arrays[store.array].cell), Pointer(m_arrays[store.array].storage) };
        if (store.number) {
            m_out.storeDouble(m_out.bitCast(m_storedWords[place], Machine::Double), m_elements.elementOfUnknownForm(elements, Integer(positions[place])));
            continue;
        }
        m_out.store64(store.moved ? m_words[*store.moved] : m_storedWords[place], m_elements.elementOfUnknownForm(elements, Integer(positions[place])));
        LBasicBlock remembers = m_out.newBlock();
        LBasicBlock behind = m_out.newBlock();
        ScopeFork fork = m_out.forkScope();
        m_out.branch(m_groups[m_loop.arrays[store.array].group].isBoxed, normalEdge(remembers), normalEdge(behind));
        m_out.appendToArm(remembers, fork);
        ValueContext context = m_generator.contextOf(store.block, store.value);
        SetForScope<const OperationContext*> whileStoring(m_generator.m_operation, &context);
        m_generator.emitter<BarrierEmitter>().barrier(context, CellPointer(m_arrays[store.array].cell));
        m_out.clearDeclaredAccesses();
        m_out.jump(normalEdge(behind));
        m_out.appendToJoin(behind, fork);
    }
}

void MoveLoopEmitter::emit(std::span<const LValue> firsts, LBasicBlock written, std::span<const LValue> phisOfWritten)
{
    LValue firstInForm = firsts[m_loop.placeOfCounter];
    m_generator.build().statistics().count(StatisticID::ElementMoveLoops);
    m_out.clearDeclaredAccesses();
    m_out.setOrigin(Machine::Origin());
    SetForScope<BlockID> whileEmitting(m_generator.m_headOfTurnsLaidOut, m_loop.head);
    LValue savedCounter = m_generator.m_values[m_loop.counter.index()];
    LValue savedPoll = m_loop.poll ? m_generator.m_values[m_loop.poll.index()] : nullptr;
    auto restore = makeScopeExit([&] {
        m_generator.m_values[m_loop.counter.index()] = savedCounter;
        if (m_loop.poll)
            m_generator.m_values[m_loop.poll.index()] = savedPoll;
    });
    auto leaveUnchanged = [&] {
        for (size_t place = 0; place < firsts.size(); ++place)
            m_out.upsilon(firsts[place], phisOfWritten[place]);
        m_out.jump(normalEdge(written));
    };
    LBasicBlock unchanged = m_out.newBlock();
    LBasicBlock left = m_out.newBlock();
    m_exit = unchanged;
    LValue first = firstAsInteger(firstInForm);
    for (const MoveLoop::Decision& decision : m_loop.decisions) {
        LValue condition = valueOf(decision.condition);
        leaveUnless(decision.expected ? condition : m_out.isZero32(condition));
    }
    auto findBefore = [&](unsigned array, LValue cell) {
        m_arrays[array].cell = cell;
        requireKind(array, false);
        requireNames(array);
        findStorage(array, cell);
        m_arrays[array].isFoundBefore = true;
    };
    for (unsigned array = 0; array < m_loop.arrays.size(); ++array) {
        const MoveLoop::Array& found = m_loop.arrays[array];
        if (!found.invariant)
            continue;
        LValue cell = valueOf(found.invariant);
        if (m_code.at(found.invariant).representation == Representation::JSValue)
            requireCell(cell);
        findBefore(array, cell);
    }
    LValue limit = limitOf(first);
    leaveUnless(isBeforeEnd(first, limit));
    for (ValueID integer : m_loop.integers)
        integerOf(integer);
    for (const MoveLoop::Read& read : m_loop.reads) {
        if (read.key.base)
            integerOf(read.key.base);
    }
    for (const MoveLoop::Store& store : m_loop.stores) {
        if (store.key.base)
            integerOf(store.key.base);
    }
    for (const MoveLoop::Comparison& comparison : m_loop.comparisons) {
        if (comparison.left.base)
            integerOf(comparison.left.base);
        if (comparison.right.base)
            integerOf(comparison.right.base);
    }
    for (unsigned place = 0; place < m_loop.reads.size(); ++place) {
        if (!m_loop.reads[place].isHoisted)
            continue;
        LValue word = loadWord(place, first);
        requireCell(word);
        for (unsigned array = 0; array < m_loop.arrays.size(); ++array) {
            if (m_loop.arrays[array].read != place)
                continue;
            findBefore(array, word);
        }
    }
    auto limitByLength = [&](unsigned array, const MoveLoop::Key& key) {
        if (!isBelowLimit(array, key))
            return;
        if (!goesUp()) {
            leaveUnless(m_out.below(key.offset ? m_out.add(first, m_out.constInt64(key.offset)) : first, m_arrays[array].length));
            LValue least = m_out.constInt64(-key.offset);
            limit = m_out.select(m_out.lessThan(limit, least), least, limit);
            return;
        }
        leaveUnless(m_out.greaterThanOrEqual(first, m_out.constInt64(-key.offset)));
        LValue most = key.offset ? m_out.sub(m_arrays[array].length, m_out.constInt64(key.offset)) : m_arrays[array].length;
        limit = m_out.select(m_out.lessThan(most, limit), most, limit);
    };
    for (const MoveLoop::Read& read : m_loop.reads)
        limitByLength(read.array, read.key);
    for (const MoveLoop::Store& store : m_loop.stores)
        limitByLength(store.array, store.key);
    leaveUnless(isBeforeEnd(first, limit));

    auto isOfElements = [&](unsigned group) {
        return std::ranges::any_of(m_loop.reads, [&](const MoveLoop::Read& read) { return m_loop.arrays[read.array].group == group && !read.isRow; }) || std::ranges::any_of(m_loop.stores, [&](const MoveLoop::Store& store) { return m_loop.arrays[store.array].group == group; });
    };
    for (unsigned place = 0; place < m_loop.reads.size(); ++place) {
        const MoveLoop::Read& read = m_loop.reads[place];
        if (!read.isRow || read.isHoisted || !read.key.hasCounter || !m_arrays[read.array].isFoundBefore)
            continue;
        for (unsigned array = 0; array < m_loop.arrays.size(); ++array) {
            unsigned group = m_loop.arrays[array].group;
            bool isNeeded = m_loop.arrays[array].read == place && isOfElements(group) && !std::ranges::any_of(m_loop.arrays, [&](const MoveLoop::Array& other) { return other.group == group && m_arrays[&other - m_loop.arrays.begin()].isFoundBefore; });
            if (!isNeeded || m_groups[group].kind)
                continue;
            LValue word = loadWord(place, first);
            requireCell(word);
            m_groups[group].kind = kindOf(word, maskOfKind);
        }
    }
    for (unsigned array = 0; array < m_loop.arrays.size(); ++array) {
        unsigned group = m_loop.arrays[array].group;
        if (!isOfElements(group) || !m_arrays[array].isFoundBefore)
            continue;
        if (!m_groups[group].kind)
            m_groups[group].kind = kindOf(m_arrays[array].cell, maskOfKind);
    }
    for (unsigned group = 0; group < m_loop.arrays.size(); ++group) {
        if (m_loop.arrays[group].group != group || !isOfElements(group))
            continue;
        Group& found = m_groups[group];
        if (!found.kind) {
            leaveUnchanged();
            m_out.appendTo(unchanged);
            leaveUnchanged();
            return;
        }
        if (m_loop.arrays[group].holdsNumbers)
            leaveUnless(m_out.equal(found.kind, m_out.constInt32(kindWith(ArrayWithDouble))));
        else
            requirePlainKind(found.kind);
        found.hole = m_out.select(m_out.equal(found.kind, m_out.constInt32(kindWith(ArrayWithDouble))), m_out.constInt64(std::bit_cast<int64_t>(PNaN)), m_out.int64Zero);
        found.isBoxed = m_out.equal(found.kind, m_out.constInt32(kindWith(ArrayWithContiguous)));
    }
    for (unsigned array = 0; array < m_loop.arrays.size(); ++array) {
        if (m_arrays[array].isFoundBefore && isOfElements(m_loop.arrays[array].group))
            requireKind(array, true);
    }
    for (ValueID number : m_loop.invariantNumbers)
        m_numbers.append({ number, numberInFront(number) });
    for (unsigned place = 0; place < m_loop.stores.size(); ++place) {
        const MoveLoop::Store& store = m_loop.stores[place];
        if (store.invariant)
            m_storedWords[place] = wordFor(m_groups[m_loop.arrays[store.array].group], valueOf(store.invariant));
    }
    const MoveLoop::Store* onlyStore = m_loop.stores.size() == 1 ? &m_loop.stores[0] : nullptr;
    for (unsigned place = 0; place < m_loop.reads.size() && onlyStore && onlyStore->number && m_arrays[onlyStore->array].isFoundBefore; ++place) {
        const MoveLoop::Read& read = m_loop.reads[place];
        if (read.array == onlyStore->array && read.key == onlyStore->key && read.key.isInvariant() && read.isNumber && !read.isRow && !read.isKey && !read.isMoved)
            m_carriedRead = place;
    }
    LValue firstCarried = nullptr;
    if (m_carriedRead) {
        firstCarried = loadWord(*m_carriedRead, first);
        leaveUnless(m_out.notEqual(firstCarried, m_groups[m_loop.arrays[onlyStore->array].group].hole));
    }
    Vector<LValue, 2> carriedNumbers;
    for (const MoveLoop::CarriedNumber& number : m_loop.carriedNumbers) {
        LValue entering = firsts[number.place];
        if (entering->type() == Machine::Double)
            leaveUnless(m_out.doubleEqual(entering, entering));
        else
            entering = asNumber(entering);
        carriedNumbers.append(entering);
    }
    requireDistinct(true);
    if (m_generator.m_rows)
        m_generator.zeroRowCounts();

    LBasicBlock iteration = m_out.newBlock();
    LBasicBlock again = m_out.newBlock();
    LBasicBlock done = m_out.newBlock();
    LValue counter = m_out.phiIn(iteration, Machine::Int64);
    m_out.upsilon(first, counter);
    for (size_t place = 0; place < carriedNumbers.size(); ++place) {
        LValue number = m_out.phiIn(iteration, Machine::Double);
        m_out.upsilon(carriedNumbers[place], number);
        carriedNumbers[place] = number;
        m_numbers.append({ m_loop.carriedNumbers[place].parameter, number });
    }
    LValue carried = nullptr;
    if (m_carriedRead) {
        carried = m_out.phiIn(iteration, Machine::Double);
        m_out.upsilon(m_out.bitCast(firstCarried, Machine::Double), carried);
        m_carriedNumber = carried;
    }
    m_out.jump(normalEdge(iteration));
    m_exit = left;
    m_out.appendTo(iteration);
    emitPoll(counter);
    emitIteration(counter);
    LValue next = m_out.add(counter, m_out.constInt64(goesUp() ? 1 : -1));
    m_out.branch(isBeforeEnd(next, limit), normalEdge(again), normalEdge(done));
    m_out.appendTo(again);
    if (carried)
        m_out.upsilon(m_storedNumber, carried);
    for (size_t place = 0; place < carriedNumbers.size(); ++place)
        m_out.upsilon(m_nextNumbers[place], carriedNumbers[place]);
    m_out.upsilon(next, counter);
    m_out.jump(normalEdge(iteration));

    auto leave = [&](LBasicBlock block, LValue counterOfLeave, std::span<const LValue> numbers) {
        m_out.appendTo(block);
        m_out.upsilon(counterInForm(counterOfLeave), phisOfWritten[m_loop.placeOfCounter]);
        for (size_t place = 0; place < numbers.size(); ++place) {
            LValue phi = phisOfWritten[m_loop.carriedNumbers[place].place];
            m_out.upsilon(phi->type() == Machine::Double ? numbers[place] : m_generator.emitter<ConversionEmitter>().boxNumberByStub(Number(numbers[place])).value(), phi);
        }
        m_out.jump(normalEdge(written));
    };
    leave(done, next, m_nextNumbers.span());
    leave(left, counter, carriedNumbers.span());
    m_out.appendTo(unchanged);
    leaveUnchanged();
}

bool CodeGenerator::mayMoveElements(BlockID id)
{
    return code().at(id).frequency != BlockFrequency::Rare && runsRepeatedly(id) && !m_keepsValuesInHeapFrame && !m_interleaving && !m_headOfTurnsLaidOut && !m_checksEffects && !m_rows;
}

std::optional<CodeGenerator::ElementMoves> CodeGenerator::findElementMoves(std::span<const ValueID> values, size_t first)
{
    static constexpr OpcodeSet namesOfCells { Opcode::Narrowed, Opcode::Unbox, Opcode::Box, Opcode::RequireObjectCoercible };
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement };
    static constexpr OpcodeSet sums = steps | OpcodeSet { Opcode::Add, Opcode::Sub };
    static constexpr int64_t largestWrittenNumber = 1 << 20;
    static constexpr size_t mostOperations = 24;
    static constexpr size_t mostReads = 3;
    auto holdsCell = [&](ValueID value) {
        Representation representation = code().at(value).representation;
        return representation == Representation::JSValue || representation == Representation::Cell;
    };
    auto nameOfArray = [&](ValueID value) {
        while (!code().isConstant(value) && namesOfCells.contains(opcodeOf(value)) && !code().at(value).operands().empty() && holdsCell(code().at(value).operand(0)))
            value = code().at(value).operand(0);
        return value;
    };
    ElementMoves found;
    ValueID array;
    Vector<ValueID, 16> written;
    auto isWritten = [&](ValueID value) {
        return written.contains(value);
    };
    auto addAccess = [&](ValueID holder, ValueID position) {
        static constexpr auto takesIndex = [](Representation representation) {
            return representation == Representation::Int32 || representation == Representation::Int64 || representation == Representation::Double || representation == Representation::OptionalDouble || representation == Representation::JSValue;
        };
        if (!holdsCell(holder) || isWritten(position) || !takesIndex(code().at(position).representation) || !(inputs().types.at(code().at(position).type).primitives & Primitive::Integer))
            return false;
        ValueID name = nameOfArray(holder);
        if (code().isConstant(name) || isWritten(name) || (array && array != name))
            return false;
        const Operation& definition = code().at(name);
        if (Operation::opcodesWithStorage.contains(definition.opcode) && definition.storage() == AllocationStorage::Frame)
            return false;
        if (!array) {
            array = name;
            found.array = isWritten(holder) ? name : holder;
        }
        return true;
    };
    size_t numberOfWrittenUpToLastStore = 0;
    size_t numberMadeAheadUpToLastStore = 0;
    for (size_t index = first; index < std::min(values.size(), first + mostOperations); ++index) {
        ValueID value = values[index];
        const Operation& operation = code().at(value);
        if (isNeverType(operation.type))
            break;
        bool isRead = operation.opcode == Opcode::ElemGet || (operation.opcode == Opcode::GetArrayElement && !hasArrayStorageForm(operation.arrayStorage()));
        bool isStore = operation.opcode == Opcode::ElemSet || (operation.opcode == Opcode::PutArrayElement && !hasArrayStorageForm(operation.arrayStorage()) && !operation.fillsPlaceOfListMadeHere());
        if (isRead) {
            bool isOfArray = operation.opcode == Opcode::ElemGet ? addAccess(operation.objectOperand(), operation.propertyOperand()) : addAccess(operation.arrayOperand(), operation.positionOperand());
            if (!isOfArray || !found.stores.isEmpty() || found.reads.size() == mostReads)
                break;
            ValueID position = operation.opcode == Opcode::ElemGet ? operation.propertyOperand() : operation.positionOperand();
            found.reads.append({ value, position, operation.opcode == Opcode::GetArrayElement && operation.keyIsIntegral() });
            written.append(value);
            continue;
        }
        if (isStore) {
            bool isOfArray = operation.opcode == Opcode::ElemSet ? addAccess(operation.objectOperand(), operation.propertyOperand()) : addAccess(operation.arrayOperand(), operation.positionOperand());
            if (!isOfArray || found.stores.size() == mostReads)
                break;
            auto withoutNames = [&](ValueID named) {
                while (isWritten(named) && namesOfMovedValues.contains(opcodeOf(named)))
                    named = code().at(named).operand(0);
                return named;
            };
            ElementMoves::Store store;
            store.value = value;
            ValueID stored = withoutNames(operation.valueOperand());
            if (isWritten(stored) && sums.contains(opcodeOf(stored))) {
                const Operation& sum = code().at(stored);
                bool isStep = steps.contains(sum.opcode);
                bool readsLeft = isStep || isWritten(withoutNames(sum.operand(0)));
                ValueID addend = isStep ? ValueID() : sum.operand(readsLeft ? 1 : 0);
                if (!isStep && (isWritten(addend) || (!readsLeft && sum.opcode == Opcode::Sub)))
                    break;
                store.addsNumber = true;
                store.isSubtracted = sum.opcode == Opcode::Sub || sum.opcode == Opcode::Decrement;
                store.written = 1;
                if (!isStep && code().isConstant(addend) && opcodeOf(addend) == Opcode::Integer) {
                    store.written = code().at(addend).integer();
                    if (!store.written || std::abs(store.written) > largestWrittenNumber)
                        break;
                } else if (!isStep) {
                    if (opcodeOf(addend) == Opcode::IntegerToNumber && code().at(code().at(addend).valueOperand()).representation == Representation::Int32)
                        addend = code().at(addend).valueOperand();
                    if (!code().isConstant(addend) && opcodeOf(addend) == Opcode::IntegerToNumber && code().at(code().at(addend).operand(0)).representation == Representation::Int32)
                        addend = code().at(addend).operand(0);
                    Representation representation = code().at(addend).representation;
                    if (code().isConstant(addend) || (representation != Representation::Int32 && representation != Representation::Double))
                        break;
                    store.addend = addend;
                }
                stored = withoutNames(sum.operand(readsLeft ? 0 : 1));
                while (isWritten(stored) && opcodeOf(stored) == Opcode::ToNumeric)
                    stored = withoutNames(code().at(stored).operand(0));
            }
            auto moved = std::ranges::find(found.reads, stored, &ElementMoves::Read::value);
            if (moved == found.reads.end())
                break;
            store.position = operation.opcode == Opcode::ElemSet ? operation.propertyOperand() : operation.positionOperand();
            store.moved = static_cast<unsigned>(moved - found.reads.begin());
            store.keyIsIntegral = operation.opcode == Opcode::PutArrayElement && operation.keyIsIntegral();
            found.stores.append(store);
            written.append(value);
            found.end = index + 1;
            numberOfWrittenUpToLastStore = written.size();
            numberMadeAheadUpToLastStore = found.madeAhead.size();
            continue;
        }
        bool isIntegerArithmetic = sums.contains(operation.opcode) && (operation.form() == OperatorForm::Integers || operation.form() == OperatorForm::Numbers);
        bool readsWritten = std::ranges::any_of(operation.operands(), isWritten);
        if (operation.opcode == Opcode::CheckProof || operation.opcode == Opcode::RequireObjectCoercible || (readsWritten && (namesOfMovedValues.contains(operation.opcode) || operation.opcode == Opcode::ToNumeric || sums.contains(operation.opcode))))
            written.append(value);
        else if (!readsWritten && (namesOfMovedValues.contains(operation.opcode) || isIntegerArithmetic))
            found.madeAhead.append(value);
        else
            break;
    }
    if (found.stores.isEmpty())
        return std::nullopt;
    written.shrink(numberOfWrittenUpToLastStore);
    found.madeAhead.shrink(numberMadeAheadUpToLastStore);
    for (unsigned place = 0; place < found.reads.size(); ++place) {
        if (!std::ranges::contains(found.stores, place, &ElementMoves::Store::moved))
            return std::nullopt;
    }
    for (ValueID value : written) {
        unsigned uses = 0;
        for (ValueID user : written)
            uses += std::ranges::count(code().at(user).operands(), value);
        if (uses != code().useCountOf(value))
            return std::nullopt;
    }
    return found;
}

void CodeGenerator::beginElementMoves(BlockID block, ElementMoves& moves)
{
    for (ValueID value : moves.madeAhead) {
        generateValue(block, value, code().at(value), false, { });
        if (m_hasFailed)
            return;
    }
    for (const ElementMoves::Store& store : moves.stores) {
        if (!store.addend || !waitsForBoxOfSum(store.addend, code().at(store.addend)))
            continue;
        generate(ValueContext(block, store.addend, code().at(store.addend), handlerOf(block)));
        if (m_hasFailed)
            return;
    }
    auto hasValue = [&](ValueID value) {
        return code().isConstant(value) || m_values[value.index()];
    };
    bool areReady = hasValue(moves.array) && std::ranges::all_of(moves.reads, hasValue, &ElementMoves::Read::position)
        && std::ranges::all_of(moves.stores, [&](const ElementMoves::Store& store) { return hasValue(store.position) && (!store.addend || (hasValue(store.addend) && !isCarriedBoxed(store.addend) && !m_valuesDefinedBoxed.get(store.addend.index()))); });
    if (!areReady)
        return;
    bool addsNumbers = std::ranges::any_of(moves.stores, &ElementMoves::Store::addsNumber);
    build().statistics().count(addsNumbers ? StatisticID::ElementSumsInline : StatisticID::ElementMovesInline);
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    ValueContext context(block, moves.stores.last().value, code().at(moves.stores.last().value), handlerOf(block));
    SetForScope<const OperationContext*> whileMoving(m_operation, &context);
    m_out.setOrigin(machineOriginOf(moves.reads[0].value));
    LBasicBlock exit = m_out.newBlock();
    moves.continuation = m_out.newBlock();
    auto leaveUnless = [&](LValue condition) {
        LBasicBlock holds = m_out.newBlock();
        m_out.branch(condition, normalEdge(holds), unlikelyEdge(exit));
        ScopeFork fork = m_out.forkScope();
        m_out.appendToSuccessor(holds, fork);
    };
    static_assert(ArrayWithInt32 == 5 && ArrayWithDouble == 7 && ArrayWithContiguous == 9 && ArrayWithArrayStorage > 9);
    auto kindWith = [&](IndexingType type) {
        return m_out.constInt32(ArrayType << 8 | type);
    };
    LValue cell = get(moves.array).value;
    m_out.clearDeclaredAccesses();
    if (code().at(moves.array).representation == Representation::JSValue)
        leaveUnless(m_out.isZero64(m_out.lShr(m_out.sub(cell, m_out.constInt64(16)), 48)));
    LValue kind = elements.cellTypeAndIndexing(CellPointer(cell)).value();
    leaveUnless(m_out.belowOrEqual(m_out.sub(kind, kindWith(ArrayWithInt32)), m_out.constInt32(ArrayWithContiguous - ArrayWithInt32)));
    leaveUnless(m_out.notZero32(m_out.bitAnd(kind, m_out.constInt32(IsArray))));
    ArrayElements storage = elements.elementsOf(CellPointer(cell));
    LValue length = elements.publicLength(storage).value();
    Vector<std::pair<ValueID, LValue>, 4> indices;
    auto indexOf = [&](ValueID position, bool keyIsIntegral) {
        if (auto known = std::ranges::find(indices, position, &std::pair<ValueID, LValue>::first); known != indices.end())
            return known->second;
        LValue index;
        if (code().at(position).representation == Representation::JSValue) {
            LValue word = get(position).value;
            leaveUnless(elements.areInt32Elements(word).value());
            index = m_out.signExt32To64(m_out.castToInt32(word));
        } else
            index = elements.indexOrElse(position, keyIsIntegral, exit).value();
        m_out.clearDeclaredAccesses();
        leaveUnless(m_out.below(index, length));
        indices.append({ position, index });
        return index;
    };
    for (const ElementMoves::Read& read : moves.reads)
        indexOf(read.position, read.keyIsIntegral);
    for (const ElementMoves::Store& store : moves.stores)
        indexOf(store.position, store.keyIsIntegral);
    LValue hole = m_out.select(m_out.equal(kind, kindWith(ArrayWithDouble)), m_out.constInt64(std::bit_cast<int64_t>(PNaN)), m_out.int64Zero);
    Vector<LValue, 3> words;
    for (const ElementMoves::Read& read : moves.reads) {
        words.append(m_out.load64(elements.elementOfUnknownForm(storage, Integer(indexOf(read.position, read.keyIsIntegral)))));
        leaveUnless(m_out.notEqual(words.last(), hole));
    }
    for (const ElementMoves::Store& store : moves.stores) {
        if (!std::ranges::contains(moves.reads, store.position, &ElementMoves::Read::position))
            leaveUnless(m_out.notEqual(m_out.load64(elements.elementOfUnknownForm(storage, Integer(indexOf(store.position, store.keyIsIntegral)))), hole));
    }
    auto sumOf = [&](const ElementMoves::Store& store) {
        LValue word = words[store.moved];
        LBasicBlock doubles = m_out.newBlock();
        LBasicBlock integers = m_out.newBlock();
        LBasicBlock found = m_out.newBlock();
        LValue sum = m_out.phiIn(found, Machine::Int64);
        LValue addend = store.addend ? get(store.addend).value : nullptr;
        m_out.clearDeclaredAccesses();
        bool addsDouble = addend && addend->type() == Machine::Double;
        m_out.branch(m_out.equal(kind, kindWith(ArrayWithDouble)), normalEdge(doubles), normalEdge(integers));
        m_out.appendTo(doubles);
        LValue number = m_out.bitCast(word, Machine::Double);
        LValue added = !addend ? m_out.constDouble(static_cast<double>(store.written)) : addsDouble ? addend : m_out.intToDouble(addend);
        LValue numberOfSum = store.isSubtracted ? m_out.doubleSub(number, added) : m_out.doubleAdd(number, added);
        leaveUnless(m_out.doubleEqual(numberOfSum, numberOfSum));
        m_out.upsilon(m_out.bitCast(numberOfSum, Machine::Int64), sum);
        m_out.jump(normalEdge(found));
        m_out.appendTo(integers);
        if (addsDouble)
            m_out.jump(normalEdge(exit));
        else {
            leaveUnless(elements.areInt32Elements(word).value());
            LValue integer = m_out.signExt32To64(m_out.castToInt32(word));
            LValue wide = addend ? m_out.signExt32To64(addend) : m_out.constInt64(store.written);
            LValue wideSum = store.isSubtracted ? m_out.sub(integer, wide) : m_out.add(integer, wide);
            LValue narrow = m_out.castToInt32(wideSum);
            leaveUnless(m_out.equal(m_out.signExt32To64(narrow), wideSum));
            m_out.upsilon(m_out.add(m_out.zeroExt(narrow, Machine::Int64), m_pinned.numberTag), sum);
            m_out.jump(normalEdge(found));
        }
        m_out.appendTo(found);
        return sum;
    };
    Vector<LValue, 3> stored;
    for (const ElementMoves::Store& store : moves.stores)
        stored.append(store.addsNumber ? sumOf(store) : words[store.moved]);
    for (size_t place = 0; place < moves.stores.size(); ++place)
        m_out.store64(stored[place], elements.elementOfUnknownForm(storage, Integer(indexOf(moves.stores[place].position, moves.stores[place].keyIsIntegral))));
    if (std::ranges::all_of(moves.stores, &ElementMoves::Store::addsNumber)) {
        m_out.jump(normalEdge(moves.continuation));
        m_out.appendTo(exit);
        return;
    }
    LBasicBlock remembers = m_out.newBlock();
    ScopeFork fork = m_out.forkScope();
    m_out.branch(m_out.equal(kind, kindWith(ArrayWithContiguous)), normalEdge(remembers), normalEdge(moves.continuation));
    m_out.appendToArm(remembers, fork);
    emitter<BarrierEmitter>().barrier(context, CellPointer(cell));
    m_out.clearDeclaredAccesses();
    m_out.jump(normalEdge(moves.continuation));
    m_out.appendTo(exit);
}

void CodeGenerator::endElementMoves(std::optional<ElementMoves>& moves)
{
    if (moves->continuation) {
        m_out.jump(normalEdge(moves->continuation));
        m_out.appendTo(moves->continuation);
    }
    moves = std::nullopt;
}

static bool startsLikeMoveLoop(const Code& code, BlockID head)
{
    static constexpr OpcodeSet inMoveLoops = namesOfMovedValues | OpcodeSet { Opcode::LoopHint, Opcode::RequireObjectCoercible, Opcode::ElemGet, Opcode::GetArrayElement, Opcode::ElemSet, Opcode::PutArrayElement, Opcode::PutContiguousIndex, Opcode::PutDoubleIndex, Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Increment, Opcode::Decrement, Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq, Opcode::TypeTest, Opcode::StrictEq, Opcode::GetArrayStorage, Opcode::GetArrayLength, Opcode::HasArrayStorage, Opcode::IsPristine, Opcode::IsElementPresent };
    static constexpr unsigned mostBlocksBeforeTest = 4;
    auto holdsOthers = [&](const Block& block) {
        return std::ranges::any_of(code.valuesOf(block), [&](ValueID value) { return !inMoveLoops.contains(code.at(value).opcode); });
    };
    const Block* block = &code.at(head);
    for (unsigned place = 0; !holdsOthers(*block); ++place) {
        if (block->terminator.kind == TerminatorKind::Branch)
            return !holdsOthers(code.at(code.at(block->terminator.taken()).to));
        if (block->terminator.kind != TerminatorKind::Jump || place == mostBlocksBeforeTest)
            return false;
        block = &code.at(code.at(block->terminator.next()).to);
    }
    return false;
}

static const Operation* findLoopHint(const Code& code, const Block& head)
{
    for (ValueID value : code.valuesOf(head)) {
        if (code.at(value).opcode == Opcode::LoopHint)
            return &code.at(value);
    }
    return nullptr;
}

static bool hasInt32Version(const Code& code, BlockID head)
{
    static constexpr OpcodeSet accessesOfUnknownKind { Opcode::ElemGet, Opcode::ElemSet, Opcode::PutArrayElement };
    const Operation* hint = findLoopHint(code, code.at(head));
    for (uint32_t index = 0; hint && index < code.numberOfBlocks(); ++index) {
        BlockID other(index);
        const Block& block = code.at(other);
        if (other == head || !block.isLoopHead || !code.isReached(other) || block.frequency != BlockFrequency::Normal || !std::ranges::any_of(block.parameters(), [&](ValueID parameter) { return code.at(parameter).representation == Representation::Int32; }))
            continue;
        const Operation* otherHint = findLoopHint(code, block);
        if (!otherHint || otherHint->position != hint->position || otherHint->inlineFrame != hint->inlineFrame || block.terminator.kind != TerminatorKind::Branch || !startsLikeMoveLoop(code, other))
            continue;
        if (std::ranges::any_of(code.valuesOf(code.at(code.at(block.terminator.taken()).to)), [&](ValueID value) { return accessesOfUnknownKind.contains(code.at(value).opcode) || (code.at(value).opcode == Opcode::GetArrayElement && code.at(value).level() == KnowledgeLevel::Expected); }))
            return true;
    }
    return false;
}

void CodeGenerator::tryEmitMoveLoop(BlockID head)
{
    if (carriesBesideParameters(head))
        return;
    static constexpr OpcodeSet reads { Opcode::ElemGet, Opcode::GetArrayElement };
    if (m_keepsValuesInHeapFrame || m_interleaving || m_headOfTurnsLaidOut || m_checksEffects)
        return;
    if (!code().opcodesOfValues().intersects(reads) || !code().at(head).parameters().size() || !startsLikeMoveLoop(code(), head))
        return;
    std::optional<MoveLoop> loop = MoveLoopFinder(*this, head).find();
    if (!loop || (code().at(loop->counter).representation != Representation::Int32 && hasInt32Version(code(), head)))
        return;
    bool hasRowsOrKeys = std::ranges::any_of(loop->reads, [](const MoveLoop::Read& read) { return read.isRow || read.isKey; });
    if (!hasRowsOrKeys && (loop->reads.isEmpty() || loop->stores.isEmpty() || code().at(head).frequency != BlockFrequency::Normal))
        return;
    std::span<const ValueID> parameters = code().at(head).parameters();
    Vector<LValue, 4> firsts;
    for (ValueID parameter : parameters) {
        firsts.append(m_values[parameter.index()]);
        if ((code().at(parameter).representation == Representation::Double) != (firsts.last()->type() == Machine::Double))
            return;
    }
    LBasicBlock writtenHead = m_out.newBlock();
    LBasicBlock written = m_out.newBlock();
    Vector<LValue, 4> phisOfWrittenHead;
    Vector<LValue, 4> phisOfWritten;
    for (LValue first : firsts) {
        phisOfWrittenHead.append(m_out.phiIn(writtenHead, first->type()));
        phisOfWritten.append(m_out.phiIn(written, first->type()));
    }
    m_out.appendTo(blockFor(head));
    {
        TerminatorContext context(head, code().at(head).terminator, nullptr);
        SetForScope<const OperationContext*> whileEmitting(m_operation, &context);
        MoveLoopEmitter(*this, *loop).emit(firsts.span(), written, phisOfWritten.span());
    }
    if (m_hasFailed)
        return;
    m_out.appendTo(written);
    for (size_t place = 0; place < parameters.size(); ++place)
        m_out.upsilon(phisOfWritten[place], phisOfWrittenHead[place]);
    m_out.jump(normalEdge(writtenHead));
    m_blocks[head.index()] = writtenHead;
    for (size_t place = 0; place < parameters.size(); ++place)
        m_values[parameters[place].index()] = phisOfWrittenHead[place];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
