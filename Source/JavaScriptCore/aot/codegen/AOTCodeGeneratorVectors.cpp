/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCodeGenerator.h"

#if ENABLE(AOT)

#include "AOTCompileUnit.h"
#include "AOTControlEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTElementAccessEmitter.h"
#include "AOTVectorOutput.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr int32_t iterationsBetweenPolls = 1 << 20;
static constexpr int32_t iterationsOfSums = 1 << 14;
static constexpr int64_t largestNarrowTerm = std::numeric_limits<uint16_t>::max();
static constexpr int fewestInstructionsSaved = 6;
static constexpr int instructionsOfTwoConversions = 3;

template<typename Switches>
static bool checksLengthsOfViews(Switches testing)
{
    return checksBoundsUnder(testing) || AOT::hasFlag(testing, Wire::TestingSwitch::CheckBoundsInEmittersOnly);
}

class LaneFinder {
public:
    using LanePair = CodeGenerator::LanePair;
    using LanePlan = CodeGenerator::LanePlan;
    using LaneSource = CodeGenerator::LaneSource;

    LaneFinder(const CodeGenerator& generator, const CodeGenerator::CountedLoop& loop)
        : m_generator(generator)
        , m_code(generator.code())
        , m_loop(loop)
        , m_head(generator.m_order[loop.positionOfHead])
        , m_parameters(m_code.at(m_head).parameters())
        , m_passedOn(m_code.at(m_code.at(loop.turn.last()).terminator.next()).arguments())
    {
    }

    std::optional<LanePlan> find();

private:
    struct PassedOn {
        ValueID first;
        ValueID second;
    };
    struct Element {
        ValueID base;
        int64_t offset;
    };
    const Operation& at(ValueID value) const { return m_code.at(value); }
    Representation representationOf(ValueID value) const { return m_generator.representationOf(value); }
    ValueID withoutNarrowing(ValueID value) const { return m_generator.withoutNarrowing(value); }
    bool isInvariant(ValueID value) const
    {
        BlockID home = m_code.blockOf(value);
        return !home || (home != m_head && !m_loop.turn.contains(home));
    }
    std::optional<size_t> indexOfParameter(ValueID value) const
    {
        size_t index = std::ranges::find(m_parameters, value) - m_parameters.begin();
        return index < m_parameters.size() && m_generator.m_values[value.index()] ? std::optional(index) : std::nullopt;
    }
    size_t positionOf(ValueID value) const { return std::ranges::find(m_values, value) - m_values.begin(); }
    bool isComputedInIteration(ValueID value) const { return positionOf(value) < m_values.size(); }
    bool isPaired(ValueID value) const { return m_plan.pairWith(value); }

    void append(LaneSource source, ValueID first, ValueID second)
    {
        bool isCarried = source == LaneSource::Parameter || source == LaneSource::IntegerParameter;
        ValueID later = positionOf(first) > positionOf(second) ? first : second;
        m_plan.pairs.append({ .source = source, .first = first, .second = second, .later = later, .position = isCarried ? 0 : positionOf(later) });
    }
    void join(ValueID first, ValueID second);
    bool joinParameters(ValueID first, ValueID second);
    bool joinConversions(ValueID first, ValueID second);
    bool joinLoads(ValueID first, ValueID second);
    bool joinArithmetic(ValueID first, ValueID second);
    bool convertsIntegers(ValueID first, ValueID second) const;
    void joinPassedOn();
    void joinStores();
    bool tryJoin(ValueID first, ValueID second);
    bool canCoexistWithVectors(ValueID) const;
    bool noteReadsOfLanes();

    Element elementAt(ValueID position) const;
    bool areAdjacentElements(const Operation& first, const Operation& second) const;
    bool onlyStoresNearby(size_t begin, size_t end, const Operation& first, const Operation& second) const;

    const CodeGenerator& m_generator;
    const Code& m_code;
    const CodeGenerator::CountedLoop& m_loop;
    BlockID m_head;
    std::span<const ValueID> m_parameters;
    std::span<const ValueID> m_passedOn;
    Vector<ValueID, 64> m_values;
    Vector<PassedOn, 8> m_pending;
    Vector<PassedOn, 8> m_pendingIntegers;
    LanePlan m_plan;
    int m_saved { 0 };
};

LaneFinder::Element LaneFinder::elementAt(ValueID position) const
{
    static constexpr OpcodeSet passingIntegers { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    Element element { position, 0 };
    while (true) {
        const Operation& operation = at(element.base);
        if (passingIntegers.contains(operation.opcode)) {
            element.base = operation.valueOperand();
            continue;
        }
        bool isSum = operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub;
        if (!isSum || operation.form() != OperatorForm::Integers || !m_code.isConstant(operation.rightOperand()) || at(operation.rightOperand()).opcode != Opcode::Integer)
            return element;
        int64_t added = at(operation.rightOperand()).integer();
        element.offset += operation.opcode == Opcode::Add ? added : -added;
        element.base = operation.leftOperand();
    }
}

bool LaneFinder::areAdjacentElements(const Operation& first, const Operation& second) const
{
    if (!first.isInsideView() || !second.isInsideView() || first.typedArrayType() != second.typedArrayType() || (first.typedArrayType() != TypeFloat32 && first.typedArrayType() != TypeFloat64))
        return false;
    if (withoutNarrowing(first.viewOperand()) != withoutNarrowing(second.viewOperand()) || first.vectorOperand() != second.vectorOperand() || !isInvariant(first.viewOperand()))
        return false;
    Element elementOfFirst = elementAt(first.positionOperand());
    Element elementOfSecond = elementAt(second.positionOperand());
    return elementOfFirst.base == elementOfSecond.base && elementOfSecond.offset == elementOfFirst.offset + 1;
}

bool LaneFinder::onlyStoresNearby(size_t begin, size_t end, const Operation& first, const Operation& second) const
{
    Element elementOfFirst = elementAt(first.positionOperand());
    Element elementOfSecond = elementAt(second.positionOperand());
    for (size_t position = begin; position < end; ++position) {
        const Operation& between = at(m_values[position]);
        EffectTraits traits = between.traits();
        if (traits.isPure())
            continue;
        if (between.opcode != Opcode::PutTypedArrayElement || !between.isInsideView() || between.viewOperand() != first.viewOperand() || between.vectorOperand() != first.vectorOperand())
            return false;
        Element element = elementAt(between.positionOperand());
        if (element.base != elementOfFirst.base || element.offset == elementOfFirst.offset || element.offset == elementOfSecond.offset)
            return false;
    }
    return true;
}

bool LaneFinder::joinParameters(ValueID first, ValueID second)
{
    std::optional<size_t> indexOfFirst = indexOfParameter(first);
    std::optional<size_t> indexOfSecond = indexOfParameter(second);
    if (!indexOfFirst || !indexOfSecond)
        return false;
    append(LaneSource::Parameter, first, second);
    m_pending.append({ m_passedOn[*indexOfFirst], m_passedOn[*indexOfSecond] });
    return true;
}

bool LaneFinder::joinConversions(ValueID first, ValueID second)
{
    const Operation& conversionOfFirst = at(first);
    const Operation& conversionOfSecond = at(second);
    if (conversionOfFirst.opcode != Opcode::IntegerToNumber || conversionOfSecond.opcode != Opcode::IntegerToNumber || !isComputedInIteration(first) || !isComputedInIteration(second))
        return false;
    ValueID integerOfFirst = withoutNarrowing(conversionOfFirst.valueOperand());
    ValueID integerOfSecond = withoutNarrowing(conversionOfSecond.valueOperand());
    if (representationOf(integerOfFirst) != Representation::Int32 || representationOf(integerOfSecond) != Representation::Int32)
        return false;
    if (m_generator.findBytesOfWord(integerOfFirst, integerOfSecond)) {
        append(LaneSource::Bytes, first, second);
        m_saved += instructionsOfTwoConversions;
        return true;
    }
    std::optional<size_t> indexOfFirst = indexOfParameter(integerOfFirst);
    std::optional<size_t> indexOfSecond = indexOfParameter(integerOfSecond);
    if (!indexOfFirst || !indexOfSecond || integerOfFirst == m_loop.counter || integerOfSecond == m_loop.counter)
        return false;
    const LanePair* known = m_plan.pairWith(integerOfFirst);
    if (known ? known->first != integerOfFirst || known->second != integerOfSecond : isPaired(integerOfSecond))
        return false;
    if (!known) {
        append(LaneSource::IntegerParameter, integerOfFirst, integerOfSecond);
        m_pendingIntegers.append({ withoutNarrowing(m_passedOn[*indexOfFirst]), withoutNarrowing(m_passedOn[*indexOfSecond]) });
    }
    append(LaneSource::Conversion, first, second);
    m_saved += instructionsOfTwoConversions;
    return true;
}

bool LaneFinder::joinLoads(ValueID first, ValueID second)
{
    const Operation& readOfFirst = at(first);
    const Operation& readOfSecond = at(second);
    if (readOfFirst.opcode != Opcode::GetTypedArrayElement || readOfSecond.opcode != Opcode::GetTypedArrayElement || !isComputedInIteration(first) || !isComputedInIteration(second) || !areAdjacentElements(readOfFirst, readOfSecond))
        return false;
    size_t positionOfFirst = positionOf(first);
    size_t positionOfSecond = positionOf(second);
    for (size_t position = std::min(positionOfFirst, positionOfSecond); position < std::max(positionOfFirst, positionOfSecond); ++position) {
        if (at(m_values[position]).traits().mayWriteObservableState())
            return false;
    }
    append(LaneSource::Load, first, second);
    m_saved += readOfFirst.typedArrayType() == TypeFloat32 ? 2 : 1;
    return true;
}

bool LaneFinder::joinArithmetic(ValueID first, ValueID second)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    const Operation& operationOfFirst = at(first);
    const Operation& operationOfSecond = at(second);
    if (!arithmetic.contains(operationOfFirst.opcode) || operationOfFirst.opcode != operationOfSecond.opcode || !isComputedInIteration(first) || !isComputedInIteration(second))
        return false;
    if (operationOfFirst.form() != OperatorForm::Numbers || operationOfSecond.form() != OperatorForm::Numbers)
        return false;
    for (ValueID operand : { operationOfFirst.leftOperand(), operationOfFirst.rightOperand(), operationOfSecond.leftOperand(), operationOfSecond.rightOperand() }) {
        if (representationOf(operand) != Representation::Double)
            return false;
    }
    join(operationOfFirst.leftOperand(), operationOfSecond.leftOperand());
    join(operationOfFirst.rightOperand(), operationOfSecond.rightOperand());
    if (isPaired(first) || isPaired(second))
        return false;
    append(LaneSource::Arithmetic, first, second);
    ++m_saved;
    return true;
}

void LaneFinder::join(ValueID first, ValueID second)
{
    first = withoutNarrowing(first);
    second = withoutNarrowing(second);
    if (isInvariant(first) && isInvariant(second)) {
        if (!std::ranges::any_of(m_plan.invariants, [&](const LanePair& known) { return known.first == first && known.second == second; }))
            m_plan.invariants.append({ .source = LaneSource::Parameter, .first = first, .second = second });
        return;
    }
    if (const LanePair* known = m_plan.pairWith(first); known && known->first == first && known->second == second)
        return;
    if (first == second || isPaired(first) || isPaired(second)) {
        m_saved -= 2;
        return;
    }
    if (joinParameters(first, second) || joinConversions(first, second) || joinLoads(first, second) || joinArithmetic(first, second))
        return;
    --m_saved;
}

bool LaneFinder::convertsIntegers(ValueID first, ValueID second) const
{
    return m_generator.findBytesOfWord(first, second) || std::ranges::any_of(m_plan.pairs, [&](const LanePair& pair) {
        return m_generator.convertsIntegers(pair, first, second);
    });
}

void LaneFinder::joinPassedOn()
{
    while (!m_pending.isEmpty() || !m_pendingIntegers.isEmpty()) {
        if (!m_pending.isEmpty()) {
            PassedOn passed = m_pending.takeLast();
            join(passed.first, passed.second);
            continue;
        }
        PassedOn passed = m_pendingIntegers.takeLast();
        if (!convertsIntegers(passed.first, passed.second))
            m_saved -= 1 + instructionsOfTwoConversions;
    }
}

void LaneFinder::joinStores()
{
    for (size_t pairIndex = 0; pairIndex < m_plan.pairs.size(); ++pairIndex) {
        LanePair pair = m_plan.pairs[pairIndex];
        if (pair.source == LaneSource::IntegerParameter || pair.source == LaneSource::Store)
            continue;
        auto storesValue = [&](ValueID store, ValueID value) {
            const Operation& operation = at(store);
            return operation.opcode == Opcode::PutTypedArrayElement && operation.valueOperand() == value && !isPaired(store);
        };
        for (size_t position = 0; position < m_values.size(); ++position) {
            if (!storesValue(m_values[position], pair.first))
                continue;
            const Operation& first = at(m_values[position]);
            for (size_t other = 0; other < m_values.size(); ++other) {
                if (!storesValue(m_values[other], pair.second) || !areAdjacentElements(first, at(m_values[other])) || !onlyStoresNearby(std::min(position, other) + 1, std::max(position, other), first, at(m_values[other])))
                    continue;
                append(LaneSource::Store, m_values[position], m_values[other]);
                m_saved += first.typedArrayType() == TypeFloat32 ? 2 : 1;
                break;
            }
        }
    }
}

bool LaneFinder::tryJoin(ValueID first, ValueID second)
{
    LanePlan planBefore = m_plan;
    int savedBefore = m_saved;
    join(first, second);
    joinPassedOn();
    if (m_saved >= savedBefore + fewestInstructionsSaved)
        return true;
    m_plan = WTF::move(planBefore);
    m_saved = savedBefore;
    return false;
}

bool LaneFinder::noteReadsOfLanes()
{
    bool isInOrder = true;
    auto noteRead = [&](ValueID read, size_t position) {
        read = withoutNarrowing(read);
        LanePair* pair = m_plan.pairWith(read);
        if (!pair)
            return;
        if (pair->source == LaneSource::IntegerParameter) {
            isInOrder = false;
            return;
        }
        isInOrder &= pair->position < position;
        (pair->first == read ? pair->firstIsRead : pair->secondIsRead) = true;
    };
    auto noteLanesRead = [&](ValueID first, ValueID second, size_t position) {
        first = withoutNarrowing(first);
        second = withoutNarrowing(second);
        if (isInvariant(first) && isInvariant(second))
            return;
        if (const LanePair* pair = m_plan.pairWith(first); pair && pair->first == first && pair->second == second) {
            isInOrder &= pair->position < position || pair->source == LaneSource::Parameter;
            return;
        }
        noteRead(first, position);
        noteRead(second, position);
    };
    for (size_t position = 0; position < m_values.size(); ++position) {
        ValueID value = m_values[position];
        const Operation& operation = at(value);
        if (const LanePair* pair = m_plan.pairWith(value)) {
            size_t positionOfPair = pair->position;
            if (positionOfPair != position)
                continue;
            if (pair->source == LaneSource::Arithmetic) {
                const Operation& first = at(pair->first);
                const Operation& second = at(pair->second);
                noteLanesRead(first.leftOperand(), second.leftOperand(), position);
                noteLanesRead(first.rightOperand(), second.rightOperand(), position);
            } else if (pair->source == LaneSource::Store)
                noteLanesRead(at(pair->first).valueOperand(), at(pair->second).valueOperand(), position);
            continue;
        }
        if (operation.opcode == Opcode::Narrowed)
            continue;
        operation.forEachChild([&](ValueID read) {
            noteRead(read, position);
        });
    }
    for (BlockID block : m_loop.turn) {
        m_code.at(block).terminator.forEachChild([&](ValueID read) {
            noteRead(read, m_values.size());
        });
    }
    for (size_t index = 0; index < m_parameters.size(); ++index) {
        if (!m_generator.m_values[m_parameters[index].index()])
            continue;
        if (!isPaired(m_parameters[index]))
            noteRead(m_passedOn[index], m_values.size());
    }
    for (const LanePair& pair : m_plan.pairs) {
        if (pair.source != LaneSource::Parameter && pair.source != LaneSource::IntegerParameter)
            continue;
        ValueID first = withoutNarrowing(m_passedOn[*indexOfParameter(pair.first)]);
        ValueID second = withoutNarrowing(m_passedOn[*indexOfParameter(pair.second)]);
        if (pair.source == LaneSource::Parameter)
            noteLanesRead(first, second, m_values.size());
        else if (!convertsIntegers(first, second)) {
            noteRead(first, m_values.size());
            noteRead(second, m_values.size());
        }
    }
    return isInOrder;
}

bool LaneFinder::canCoexistWithVectors(ValueID value) const
{
    static constexpr OpcodeSet computations {
        Opcode::LoopHint, Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::IntegerToNumber, Opcode::NumberToInteger, Opcode::ToInt32,
        Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Increment, Opcode::Decrement, Opcode::Negate,
        Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift,
        Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq,
    };
    static constexpr OpcodeSet truncations { Opcode::ToInt32, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::BitNot, Opcode::LShift, Opcode::RShift, Opcode::URShift };
    const Operation& operation = at(value);
    bool readsNumber = false;
    operation.forEachChild([&](ValueID operand) {
        readsNumber |= representationOf(operand) == Representation::Double || representationOf(operand) == Representation::OptionalDouble;
    });
    if (operation.opcode == Opcode::GetTypedArrayElement)
        return operation.isInsideView();
    if (operation.opcode == Opcode::PutTypedArrayElement) {
        Representation stored = representationOf(operation.valueOperand());
        bool isFloat = operation.typedArrayType() == TypeFloat32 || operation.typedArrayType() == TypeFloat64;
        if (stored != Representation::Double && stored != Representation::Int32 && stored != Representation::Int64)
            return false;
        return !isARM64() || (operation.isInsideView() && (stored != Representation::Double || isFloat));
    }
    if (!computations.contains(operation.opcode) || operation.traits().mayRunOtherCode())
        return false;
    bool isRaw = operation.representation == Representation::Int32 || operation.representation == Representation::Int64 || operation.representation == Representation::Double || operation.representation == Representation::Boolean || operation.representation == Representation::Void;
    return isRaw && (!isARM64() || !readsNumber || !truncations.contains(operation.opcode));
}

std::optional<CodeGenerator::LanePlan> LaneFinder::find()
{
    for (size_t place = 0; place < m_loop.turn.size(); ++place) {
        BlockID block = m_loop.turn[place];
        const Terminator& terminator = m_code.at(block).terminator;
        if (!m_code.at(block).parameters().empty() || (terminator.kind != TerminatorKind::Jump && terminator.kind != TerminatorKind::Branch))
            return std::nullopt;
        for (uint32_t edge = 0; edge < terminator.numberOfEdges; ++edge) {
            BlockID target = m_code.at(terminator.edge(edge)).to;
            if (m_loop.turn.contains(target) && (place + 1 == m_loop.turn.size() || m_loop.turn[place + 1] != target))
                return std::nullopt;
        }
        for (ValueID value : m_code.valuesOf(block)) {
            if (!canCoexistWithVectors(value))
                return std::nullopt;
            m_values.append(value);
        }
    }
    auto isCarriedByArithmetic = [&](size_t index) {
        static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
        ValueID parameter = m_parameters[index];
        ValueID passed = withoutNarrowing(m_passedOn[index]);
        return m_generator.m_values[parameter.index()] && representationOf(parameter) == Representation::Double && !isInvariant(passed) && arithmetic.contains(at(passed).opcode) && representationOf(passed) == Representation::Double && !isPaired(parameter);
    };
    for (size_t first = 0; first < m_parameters.size(); ++first) {
        if (!isCarriedByArithmetic(first))
            continue;
        for (size_t second = first + 1; second < m_parameters.size(); ++second) {
            if (isCarriedByArithmetic(second) && tryJoin(m_parameters[first], m_parameters[second]))
                break;
        }
    }
    if (m_plan.pairs.isEmpty())
        return std::nullopt;
    joinStores();
    if (!noteReadsOfLanes())
        return std::nullopt;
    return WTF::move(m_plan);
}

bool CodeGenerator::callsEndVectors() const
{
    return isARM64() && m_checksEffects;
}

std::optional<CodeGenerator::LanePlan> CodeGenerator::findLanes(const CountedLoop& loop) const
{
    if (loop.testsAtEnd || m_keepsValuesInHeapFrame || !loop.loopCarriedValueStores.isEmpty() || loop.storesIntoSlots || loop.storesIntoLists || !loop.flatStrings.isEmpty())
        return std::nullopt;
    if (!loop.listsAskedForKind.isEmpty() || !loop.listsWhoseKindIsRead.isEmpty() || !loop.viewsAskedForKind.isEmpty())
        return std::nullopt;
    auto accessesView = [&](ValueID value) {
        return opcodeOf(value) == Opcode::GetTypedArrayElement || opcodeOf(value) == Opcode::PutTypedArrayElement;
    };
    if (checksLengthsOfViews(m_inputs.build.options().testing) && std::ranges::any_of(loop.turn, [&](BlockID block) { return std::ranges::any_of(code().valuesOf(block), accessesView); }))
        return std::nullopt;
    if (callsEndVectors())
        return std::nullopt;
    std::optional<LanePlan> plan = LaneFinder(*this, loop).find();
    if (!plan)
        return std::nullopt;
    if (!m_followsNumbersOutOfBytes && std::ranges::any_of(plan->pairs, [](const LanePair& pair) { return pair.source == LaneSource::Load; }))
        return std::nullopt;
    return plan;
}

std::optional<CodeGenerator::BytesOfWord> CodeGenerator::findBytesOfWord(ValueID first, ValueID second) const
{
    auto constantOf = [&](ValueID operand) -> std::optional<int64_t> {
        return code().isConstant(operand) && opcodeOf(operand) == Opcode::Integer ? std::optional<int64_t>(code().at(operand).integer()) : std::nullopt;
    };
    auto byteOfWord = [&](ValueID value) -> std::optional<std::pair<ValueID, int64_t>> {
        const Operation& cut = code().at(value);
        if (cut.opcode != Opcode::BitAnd || constantOf(cut.rightOperand()) != 0xff)
            return std::nullopt;
        ValueID word = cut.leftOperand();
        int64_t amount = 0;
        if (const Operation& shift = code().at(word); shift.opcode == Opcode::RShift || shift.opcode == Opcode::URShift) {
            amount = constantOf(shift.rightOperand()).value_or(-1);
            word = shift.leftOperand();
        }
        Representation representation = representationOf(word);
        if (amount < 0 || amount > 24 || amount % 8 || (representation != Representation::Int32 && representation != Representation::Int64))
            return std::nullopt;
        return std::pair(word, amount / 8);
    };
    std::optional<std::pair<ValueID, int64_t>> byteOfFirst = byteOfWord(first);
    std::optional<std::pair<ValueID, int64_t>> byteOfSecond = byteOfWord(second);
    if (!byteOfFirst || !byteOfSecond || byteOfFirst->first != byteOfSecond->first || byteOfFirst->second % 2 || byteOfSecond->second != byteOfFirst->second + 1)
        return std::nullopt;
    return BytesOfWord { byteOfFirst->first, static_cast<unsigned>(byteOfFirst->second / 2) };
}

LValue CodeGenerator::numberForLane(ValueID id)
{
    Operand number = holdsAnyNaN(id) ? getWithAnyNaN(id) : get(id);
    return number.as<Number>().value();
}

ValueID CodeGenerator::withoutNarrowing(ValueID value) const
{
    while (opcodeOf(value) == Opcode::Narrowed)
        value = code().at(value).valueOperand();
    return value;
}

bool CodeGenerator::convertsIntegers(const LanePair& pair, ValueID first, ValueID second) const
{
    if (pair.source == LaneSource::IntegerParameter)
        return pair.first == first && pair.second == second;
    return pair.source == LaneSource::Bytes && withoutNarrowing(code().at(pair.first).valueOperand()) == first && withoutNarrowing(code().at(pair.second).valueOperand()) == second;
}

LValue CodeGenerator::vectorOfLanes(ValueID first, ValueID second)
{
    first = withoutNarrowing(first);
    second = withoutNarrowing(second);
    if (const LanePair* pair = m_lanes->pairWith(first); pair && pair->first == first && pair->second == second)
        return pair->vector;
    for (const LanePair& known : m_lanes->invariants) {
        if (known.first == first && known.second == second)
            return known.vector;
    }
    return VectorOutput(m_out).insert(numberForLane(first), numberForLane(second));
}

LValue CodeGenerator::vectorOfConvertedLanes(ValueID first, ValueID second)
{
    first = withoutNarrowing(first);
    second = withoutNarrowing(second);
    for (const LanePair& pair : m_lanes->pairs) {
        if (convertsIntegers(pair, first, second))
            return pair.vector;
    }
    if (std::optional<BytesOfWord> bytes = findBytesOfWord(first, second))
        return VectorOutput(m_out).toNumbers(spreadBytesOfWord(bytes->word), bytes->half);
    return VectorOutput(m_out).insert(m_out.intToDouble(get(first).value), m_out.intToDouble(get(second).value));
}

LValue CodeGenerator::spreadBytesOfWord(ValueID word)
{
    for (auto& [known, spread] : m_lanes->spreadWords) {
        if (known == word)
            return spread;
    }
    LValue bits = get(word).value;
    if (bits->type() != Machine::Int32)
        bits = m_out.castToInt32(bits);
    m_lanes->spreadWords.append({ word, VectorOutput(m_out).spreadBytes(bits) });
    return m_lanes->spreadWords.last().second;
}

bool CodeGenerator::emitLanes(BlockID block, ValueID id)
{
    LanePair* pair = m_lanes->pairWith(id);
    if (!pair)
        return false;
    m_out.setOrigin(machineOriginOf(id));
    const Operation& first = code().at(pair->first);
    const Operation& second = code().at(pair->second);
    if (id != pair->later)
        return true;
    ValueContext contextOfFirst(block, pair->first, first, nullptr);
    ValueContext contextOfSecond(block, pair->second, second, nullptr);
    SetForScope<const OperationContext*> whileEmitting(m_operation, &contextOfFirst);
    declareAccessesOf(contextOfFirst);
    VectorOutput vectors(m_out);
    switch (pair->source) {
    case LaneSource::Parameter:
    case LaneSource::IntegerParameter:
        RELEASE_ASSERT_NOT_REACHED();
    case LaneSource::Conversion:
        pair->vector = vectorOfConvertedLanes(first.valueOperand(), second.valueOperand());
        break;
    case LaneSource::Bytes: {
        std::optional<BytesOfWord> bytes = findBytesOfWord(withoutNarrowing(first.valueOperand()), withoutNarrowing(second.valueOperand()));
        RELEASE_ASSERT(bytes);
        pair->vector = vectors.toNumbers(spreadBytesOfWord(bytes->word), bytes->half);
        break;
    }
    case LaneSource::Arithmetic: {
        LValue left = vectorOfLanes(first.leftOperand(), second.leftOperand());
        LValue right = vectorOfLanes(first.rightOperand(), second.rightOperand());
        VectorOutput::Arithmetic operation = first.opcode == Opcode::Add ? VectorOutput::Arithmetic::Add : first.opcode == Opcode::Sub ? VectorOutput::Arithmetic::Sub : first.opcode == Opcode::Mul ? VectorOutput::Arithmetic::Mul : VectorOutput::Arithmetic::Div;
        pair->vector = vectors.compute(operation, left, right);
        break;
    }
    case LaneSource::Load: {
        emitter<ElementAccessEmitter>().elementInsideView(contextOfSecond);
        TypedPointer element = emitter<ElementAccessEmitter>().elementInsideView(contextOfFirst);
        pair->vector = first.typedArrayType() == TypeFloat32 ? vectors.singlesToNumbers(m_out.loadDouble(element)) : m_out.load(element, Machine::Vector128);
        break;
    }
    case LaneSource::Store: {
        LValue stored = vectorOfLanes(first.valueOperand(), second.valueOperand());
        emitter<ElementAccessEmitter>().elementInsideView(contextOfSecond);
        TypedPointer element = emitter<ElementAccessEmitter>().elementInsideView(contextOfFirst);
        if (first.typedArrayType() == TypeFloat32)
            m_out.storeDouble(vectors.numbersToSingles(stored), element);
        else
            m_out.store(stored, element);
        return true;
    }
    }
    if (m_followsNumbersOutOfBytes) {
        m_valuesHoldingAnyNaN.quickSet(pair->first.index());
        m_valuesHoldingAnyNaN.quickSet(pair->second.index());
    }
    if (pair->firstIsRead)
        m_values[pair->first.index()] = vectors.lane(pair->vector, 0);
    if (pair->secondIsRead)
        m_values[pair->second.index()] = vectors.lane(pair->vector, 1);
    build().statistics().count(StatisticID::LanePairsEmitted);
    return true;
}

void CodeGenerator::layOutLanes(const CountedLoop& loop, LanePlan& plan)
{
    build().statistics().count(StatisticID::LoopsWithLanes);
    m_proc.noteVectors();
    BlockID head = m_order[loop.positionOfHead];
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    std::span<const ValueID> passedOn = code().at(code().at(loop.turn.last()).terminator.next()).arguments();
    std::span<const BlockID> turn = loop.turn.span();
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    using Values = Vector<LValue, 16>;

    Vector<LBasicBlock, 4> blocksOfWrittenTurn;
    for (BlockID block : turn)
        blocksOfWrittenTurn.append(m_blocks[block.index()]);
    LBasicBlock writtenHead = m_out.newBlock();
    LBasicBlock stretch = m_out.newBlock();
    LBasicBlock turns = m_out.newBlock();
    Values phisOfHead;
    Values phisOfWrittenHead;
    Values phisOfStretch;
    Values phisOfTurns;
    for (ValueID parameter : parameters) {
        LValue phi = m_values[parameter.index()];
        phisOfHead.append(phi);
        phisOfWrittenHead.append(phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
        phisOfStretch.append(phi ? m_out.phiIn(stretch, phi->type()) : nullptr);
        phisOfTurns.append(phi && !plan.pairWith(parameter) ? m_out.phiIn(turns, phi->type()) : nullptr);
    }
    auto isCarried = [](const LanePair& pair) {
        return pair.source == LaneSource::Parameter || pair.source == LaneSource::IntegerParameter;
    };
    for (LanePair& pair : plan.pairs) {
        if (isCarried(pair))
            pair.vector = m_out.phiIn(turns, Machine::Vector128);
    }
    auto indexOfParameter = [&](ValueID parameter) {
        return static_cast<size_t>(std::ranges::find(parameters, parameter) - parameters.begin());
    };
    auto pass = [&](const Values& phis, LBasicBlock target, auto&& valueAt) {
        Values values;
        for (size_t index = 0; index < phis.size(); ++index)
            values.append(phis[index] ? valueAt(index) : nullptr);
        for (size_t index = 0; index < phis.size(); ++index) {
            if (phis[index])
                m_out.upsilon(values[index], phis[index]);
        }
        m_out.jump(normalEdge(target));
    };
    auto blockPassing = [&](const Values& phis, LBasicBlock target, auto&& valueAt) {
        LBasicBlock here = m_out.m_block;
        LBasicBlock blockOnEdge = m_out.newBlock();
        m_out.appendTo(blockOnEdge);
        pass(phis, target, valueAt);
        m_out.appendTo(here);
        return blockOnEdge;
    };
    bool countsUp = loop.step > 0;
    bool pollsBetweenStretches = !!loop.poll;
    LValue endOfStretch = nullptr;

    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    TerminatorContext context(head, blockOfHead.terminator, nullptr);
    LValue bound = nullptr;
    {
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LValue counter = phisOfHead[indexOfCounter];
        bound = loop.bound ? get(loop.bound).value : m_out.constantOfType(counter->type(), 0);
        RELEASE_ASSERT(bound && counter && bound->type() == counter->type());
        LBasicBlock written = blockPassing(phisOfWrittenHead, writtenHead, [&](size_t index) {
            return phisOfHead[index];
        });
        bool shouldSkipEntryChecks = AOT::hasFlag(build().options().testing, Wire::TestingSwitch::SkipUnrollEntryChecks);
        auto require = [&](LValue condition) {
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(shouldSkipEntryChecks ? m_out.opaque(m_out.booleanTrue) : condition, normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        };
        LBasicBlock someAreLeft = m_out.newBlock();
        m_out.branch(askCounter(loop.relation, counter, bound), normalEdge(someAreLeft), normalEdge(written));
        m_out.appendTo(someAreLeft);
        auto wide = [&](LValue value) { return value->type() == Machine::Int32 ? m_out.signExt32To64(value) : value; };
        for (const BoundAskedInFront& asked : loop.boundsAskedInFront) {
            LValue length = get(asked.length).value;
            require(m_out.lessThan(m_out.constantOfType(length->type(), asked.most), length));
        }
        for (ValueID relation : loop.relationsAskedInFront)
            require(emitter<ControlEmitter>().hoistedResult(relation).value());
        for (ValueID length : loop.lengthsHeldAgainstBound)
            require(m_out.greaterThanOrEqual(m_out.zeroExt(get(length).value, Machine::Int64), wide(bound)));
        for (const SteppedIndex& held : loop.steppedIndexes) {
            LValue greatest = wide(phisOfHead[held.indexOfParameter]);
            if (held.step > 0)
                greatest = m_out.add(greatest, m_out.mul(m_out.sub(wide(bound), wide(counter)), m_out.constInt64(held.step)));
            int64_t added = held.offset - (held.step > 0 && loop.relation == Opcode::Less ? held.step : 0);
            require(m_out.lessThan(m_out.add(greatest, m_out.constInt64(added)), m_out.zeroExt(get(held.length).value, Machine::Int64)));
        }
        pass(phisOfStretch, stretch, [&](size_t index) {
            return phisOfHead[index];
        });
    }

    m_lanes = &plan;
    VectorOutput vectors(m_out);
    m_out.appendTo(stretch);
    {
        SetForScope<const OperationContext*> whileBeginning(m_operation, &context);
        for (LanePair& invariant : plan.invariants)
            invariant.vector = invariant.first == invariant.second ? vectors.splat(numberForLane(invariant.first)) : vectors.insert(numberForLane(invariant.first), numberForLane(invariant.second));
        Vector<LValue, 8> carried;
        for (const LanePair& pair : plan.pairs) {
            if (!isCarried(pair))
                continue;
            LValue first = phisOfStretch[indexOfParameter(pair.first)];
            LValue second = phisOfStretch[indexOfParameter(pair.second)];
            carried.append(pair.source == LaneSource::Parameter ? vectors.insert(first, second) : vectors.insert(m_out.intToDouble(first), m_out.intToDouble(second)));
        }
        LValue counter = phisOfStretch[indexOfCounter];
        if (pollsBetweenStretches) {
            LValue behindStretch = m_out.add(counter, m_out.constantOfType(counter->type(), iterationsBetweenPolls * loop.step));
            endOfStretch = m_out.select(countsUp ? m_out.lessThan(behindStretch, bound) : m_out.greaterThan(behindStretch, bound), behindStretch, bound);
        } else
            endOfStretch = bound;
        size_t next = 0;
        for (const LanePair& pair : plan.pairs) {
            if (isCarried(pair))
                m_out.upsilon(carried[next++], pair.vector);
        }
        pass(phisOfTurns, turns, [&](size_t index) {
            return phisOfStretch[index];
        });
    }

    auto purifiedIfNeeded = [&](LValue number) {
        return m_followsNumbersOutOfBytes ? emitter<ConversionEmitter>().purified(Number(number)).value() : number;
    };
    auto takeApart = [&](const LanePair& pair, LValue vector, ValueID parameter) -> LValue {
        LValue number = vectors.lane(vector, pair.first == parameter ? 0 : 1);
        if (pair.source == LaneSource::Parameter)
            return purifiedIfNeeded(number);
        return m_out.castToInt32(emitter<ConversionEmitter>().toInteger(Number(number)).value());
    };
    Values valuesOfTurn = phisOfTurns;
    Vector<LValue, 8> vectorsOfTurn;
    for (const LanePair& pair : plan.pairs)
        vectorsOfTurn.append(isCarried(pair) ? pair.vector : nullptr);
    Vector<LValue, 8> phisOfVectors = vectorsOfTurn;
    auto valueInTurn = [&](size_t index) -> LValue {
        if (valuesOfTurn[index])
            return valuesOfTurn[index];
        size_t place = plan.pairWith(parameters[index]) - plan.pairs.begin();
        return takeApart(plan.pairs[place], vectorsOfTurn[place], parameters[index]);
    };
    static_assert(!(iterationsBetweenPolls % 2));
    unsigned iterationsInRow = std::ranges::any_of(plan.pairs, [&](const LanePair& pair) {
        return pair.source == LaneSource::Parameter && std::ranges::contains(parameters, withoutNarrowing(passedOn[indexOfParameter(pair.first)]));
    }) ? 2 : 1;

    m_out.appendTo(turns);
    m_headOfTurnsLaidOut = head;
    m_turnLaidOut = turn;
    m_valuesLeftOutOfTurnsLaidOut.append(loop.poll);
    if (m_followsNumbersOutOfBytes) {
        m_blocksBegun.quickSet(head.index());
        for (const LanePair& pair : plan.pairs) {
            if (pair.source != LaneSource::Parameter)
                continue;
            for (ValueID parameter : { pair.first, pair.second }) {
                if (!m_valuesHoldingAnyNaN.quickSet(parameter.index()))
                    plan.parametersHoldingAnyNaN.append(parameter);
            }
        }
    }
    m_boundsAskedInFrontOfTurnsLaidOut = loop.boundsAskedInFront.span();
    m_relationsAskedInFrontOfTurnsLaidOut = loop.relationsAskedInFront;
    m_relationsAskedInFrontOfTurnsLaidOut.appendVector(loop.relationsOfCounterBelowBound);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = loop.accessesUnderCounterBelowBound.span();
    Values valuesAfterIteration;
    Vector<LValue, 8> vectorsAfterIteration;
    auto valueAfterIteration = [&](size_t index) -> LValue {
        if (valuesAfterIteration[index])
            return valuesAfterIteration[index];
        size_t place = plan.pairWith(parameters[index]) - plan.pairs.begin();
        return takeApart(plan.pairs[place], vectorsAfterIteration[place], parameters[index]);
    };
    for (unsigned iteration = 0; iteration < iterationsInRow; ++iteration) {
        if (iteration) {
            m_nextBlockGoesOnHere = false;
            m_out.setOrigin(Machine::Origin());
            SetForScope<const OperationContext*> whileTesting(m_operation, &context);
            LBasicBlock again = m_out.newBlock();
            LBasicBlock ended = blockPassing(phisOfWrittenHead, writtenHead, valueAfterIteration);
            m_out.branch(askCounter(loop.relation, valuesAfterIteration[indexOfCounter], endOfStretch), normalEdge(again), unlikelyEdge(ended));
            m_out.appendTo(again);
            valuesOfTurn = std::exchange(valuesAfterIteration, { });
            vectorsOfTurn = std::exchange(vectorsAfterIteration, { });
            plan.spreadWords.shrink(0);
            for (BlockID block : turn) {
                for (ValueID value : code().valuesOf(block))
                    m_values[value.index()] = nullptr;
            }
        }
        for (size_t index = 0; index < parameters.size(); ++index)
            m_values[parameters[index].index()] = valuesOfTurn[index];
        for (size_t place = 0; place < plan.pairs.size(); ++place) {
            LanePair& pair = plan.pairs[place];
            pair.vector = vectorsOfTurn[place];
            if (pair.source != LaneSource::Parameter)
                continue;
            if (pair.firstIsRead)
                m_values[pair.first.index()] = vectors.lane(pair.vector, 0);
            if (pair.secondIsRead)
                m_values[pair.second.index()] = vectors.lane(pair.vector, 1);
        }
        LBasicBlock blockLeavingTurn = nullptr;
        m_blockLeavingTurnLaidOut = [&] {
            if (!blockLeavingTurn)
                blockLeavingTurn = blockPassing(phisOfWrittenHead, writtenHead, valueInTurn);
            return blockLeavingTurn;
        };
        for (size_t index = 0; index < turn.size(); ++index) {
            LBasicBlock& block = m_blocks[turn[index].index()];
            if (!index) {
                block = m_out.m_block;
                m_nextBlockGoesOnHere = true;
            } else
                block = goesOnBehindJump(loop.positionsOfTurn[index]) ? nullptr : m_out.newBlock();
        }
        for (BlockID block : turn) {
            generateBlock(block);
            if (m_hasFailed)
                return;
            m_scopeAtEndOfBlock[block.index()] = m_out.forkScope();
        }
        RELEASE_ASSERT(m_nextBlockGoesOnHere && currentBlockIsOpen() && m_argumentsForNextTurn.size() == parameters.size());
        for (size_t index = 0; index < parameters.size(); ++index)
            valuesAfterIteration.append(!valuesOfTurn[index] ? nullptr : index == indexOfCounter ? m_out.add(valuesOfTurn[index], m_out.constantOfType(valuesOfTurn[index]->type(), loop.step)) : m_argumentsForNextTurn[index].value);
        TerminatorContext contextOfEnd(turn.back(), code().at(turn.back()).terminator, nullptr);
        SetForScope<const OperationContext*> whileEnding(m_operation, &contextOfEnd);
        for (const LanePair& pair : plan.pairs) {
            if (!isCarried(pair)) {
                vectorsAfterIteration.append(nullptr);
                continue;
            }
            ValueID first = passedOn[indexOfParameter(pair.first)];
            ValueID second = passedOn[indexOfParameter(pair.second)];
            vectorsAfterIteration.append(pair.source == LaneSource::Parameter ? vectorOfLanes(first, second) : vectorOfConvertedLanes(first, second));
        }
    }
    m_nextBlockGoesOnHere = false;
    m_headOfTurnsLaidOut = BlockID();
    m_turnLaidOut = { };
    m_boundsAskedInFrontOfTurnsLaidOut = { };
    m_relationsAskedInFrontOfTurnsLaidOut.shrink(0);
    m_accessesUnderCounterBelowBoundOfTurnsLaidOut = { };
    m_blockLeavingTurnLaidOut = nullptr;
    m_valuesLeftOutOfTurnsLaidOut.shrink(0);
    m_lanes = nullptr;
    m_out.setOrigin(Machine::Origin());

    LValue counterAfterIteration = valuesAfterIteration[indexOfCounter];
    {
        SetForScope<const OperationContext*> whileEnding(m_operation, &context);
        LBasicBlock again = m_out.newBlock();
        LBasicBlock stretchHasEnded = m_out.newBlock();
        m_out.branch(askCounter(loop.relation, counterAfterIteration, endOfStretch), normalEdge(again), unlikelyEdge(stretchHasEnded));
        m_out.appendTo(again);
        for (size_t place = 0; place < plan.pairs.size(); ++place) {
            if (vectorsAfterIteration[place])
                m_out.upsilon(vectorsAfterIteration[place], phisOfVectors[place]);
        }
        pass(phisOfTurns, turns, [&](size_t index) {
            return valuesAfterIteration[index];
        });

        m_out.appendTo(stretchHasEnded);
        Values values;
        for (size_t index = 0; index < parameters.size(); ++index)
            values.append(phisOfHead[index] ? valueAfterIteration(index) : nullptr);
        if (pollsBetweenStretches) {
            LBasicBlock polls = m_out.newBlock();
            LBasicBlock ended = blockPassing(phisOfWrittenHead, writtenHead, [&](size_t index) {
                return values[index];
            });
            m_out.branch(askCounter(loop.relation, counterAfterIteration, bound), normalEdge(polls), normalEdge(ended));
            m_out.appendTo(polls);
            generateValue(code().blockOf(loop.poll), loop.poll, code().at(loop.poll), false, { });
            m_out.setOrigin(Machine::Origin());
            pass(phisOfStretch, stretch, [&](size_t index) {
                return values[index];
            });
        } else {
            pass(phisOfWrittenHead, writtenHead, [&](size_t index) {
                return values[index];
            });
        }
    }

    for (ValueID parameter : plan.parametersHoldingAnyNaN)
        m_valuesHoldingAnyNaN.quickClear(parameter.index());
    for (BlockID block : turn) {
        for (ValueID value : code().valuesOf(block)) {
            m_values[value.index()] = nullptr;
            if (m_followsNumbersOutOfBytes && plan.pairWith(value))
                m_valuesHoldingAnyNaN.quickClear(value.index());
        }
    }
    for (size_t index = 0; index < turn.size(); ++index)
        m_blocks[turn[index].index()] = blocksOfWrittenTurn[index];
    m_blocks[head.index()] = writtenHead;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfWrittenHead[index];
}

static constexpr int64_t bytesOfVector = 16;

static bool isAccessOfView(const Operation& operation)
{
    return operation.opcode == Opcode::GetTypedArrayElement || operation.opcode == Opcode::PutTypedArrayElement;
}

static bool isAccessOfList(const Operation& operation)
{
    return operation.opcode == Opcode::GetArrayElement || operation.opcode == Opcode::PutDoubleIndex || operation.opcode == Opcode::PutContiguousIndex;
}

static bool storesElement(const Operation& operation)
{
    return operation.opcode == Opcode::PutTypedArrayElement || operation.opcode == Opcode::PutDoubleIndex || operation.opcode == Opcode::PutContiguousIndex;
}

static int64_t bytesOfElement(const Operation& access)
{
    return isAccessOfView(access) ? static_cast<int64_t>(elementSize(access.typedArrayType())) : static_cast<int64_t>(sizeof(double));
}

class WideLoopFinder {
public:
    using WideLoop = CodeGenerator::WideLoop;

    WideLoopFinder(const CodeGenerator& generator, const CodeGenerator::CountedLoop& loop)
        : m_generator(generator)
        , m_code(generator.code())
        , m_loop(loop)
        , m_head(generator.m_order[loop.positionOfHead])
        , m_parameters(m_code.at(m_head).parameters())
        , m_passedOn(m_code.at(m_code.at(loop.turn.last()).terminator.next()).arguments())
    {
    }

    std::optional<WideLoop> find();

private:
    const Operation& at(ValueID value) const { return m_code.at(value); }
    Representation representationOf(ValueID value) const { return m_generator.representationOf(value); }
    bool isInvariant(ValueID value) const
    {
        BlockID home = m_code.blockOf(value);
        return !home || (home != m_head && !m_loop.turn.contains(home));
    }
    bool isData(ValueID value) const { return m_found.computed.contains(value); }
    bool isWord(ValueID value) const { return m_found.words.contains(value); }
    static bool isInteger(Representation representation) { return representation == Representation::Int32 || representation == Representation::Int64; }

    bool findParameters();
    static std::optional<CodeGenerator::WideLanes> lanesOfView(TypedArrayType);
    void findLanes();
    bool findTests();
    std::optional<int64_t> stepOf(ValueID) const;
    std::optional<int64_t> stepOfResult(const Operation&) const;
    bool isConsecutive(ValueID position) const { return stepOf(position) == 1; }
    bool classifyAccess(ValueID);
    bool classifyListAccess(ValueID);
    bool isNumberOperand(ValueID);
    bool classifyInteger(ValueID);
    bool classifyNumber(ValueID);
    bool isOperand(ValueID);
    bool isIntegerOperand(ValueID value)
    {
        Representation representation = representationOf(value);
        bool isOptional = representation == Representation::OptionalInt64;
        return (isInteger(representation) || (isOptional && isData(value))) && (isOperand(value) || isSteppedWord(value));
    }
    bool isSteppedWord(ValueID value)
    {
        if (m_found.lanes == CodeGenerator::WideLanes::Numbers || isData(value) || isInvariant(value) || !isInteger(representationOf(value)))
            return false;
        std::optional<int64_t> step = stepOf(value);
        if (!step)
            return false;
        if (!m_found.steppedWords.containsIf([&](const auto& known) { return known.first == value; }))
            m_found.steppedWords.append({ value, *step });
        return true;
    }
    bool findReduction(size_t indexOfParameter);
    bool isOfSum(ValueID value) const
    {
        return std::ranges::any_of(m_found.sums, [&](const CodeGenerator::WideSum& sum) { return value == sum.sum || (value == sum.termAsNumber && value != sum.term); });
    }
    bool isBoxedTerm(ValueID value) const
    {
        return std::ranges::any_of(m_found.sums, [&](const CodeGenerator::WideSum& sum) { return value == sum.term && sum.terms == CodeGenerator::WideSum::Terms::BoxedWords; });
    }
    bool findTermsOfSums(bool hasStored);

    const CodeGenerator& m_generator;
    const Code& m_code;
    const CodeGenerator::CountedLoop& m_loop;
    BlockID m_head;
    std::span<const ValueID> m_parameters;
    std::span<const ValueID> m_passedOn;
    Vector<ValueID, 8> m_indexParameters;
    Vector<size_t, 4> m_reduced;
    Vector<ValueID, 32> m_values;
    Vector<ValueID, 4> m_tests;
    Vector<ValueID, 2> m_presences;
    WideLoop m_found;
};

static ValueID passedInteger(const Code& code, ValueID value)
{
    static constexpr OpcodeSet passing { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger, Opcode::ToInt32 };
    while (passing.contains(code.at(value).opcode))
        value = code.at(value).valueOperand();
    return value;
}

bool WideLoopFinder::findParameters()
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub };
    for (size_t index = 0; index < m_parameters.size(); ++index) {
        ValueID parameter = m_parameters[index];
        if (!m_generator.m_values[parameter.index()])
            continue;
        if (!isInteger(representationOf(parameter))) {
            auto sum = std::ranges::find(m_loop.sumsOfIntegers, index, &CodeGenerator::SumOfIntegers::indexOfParameter);
            if (sum == m_loop.sumsOfIntegers.end())
                return false;
            using Terms = CodeGenerator::WideSum::Terms;
            Terms terms = Terms::BoxedWords;
            if (!sum->termIsElementOfList) {
                Range range = m_generator.typeOf(sum->term).integers;
                terms = std::max(-range.minimum, range.maximum) <= largestNarrowTerm ? Terms::NarrowWords : Terms::Words;
            }
            m_found.sums.append({ index, sum->sum, sum->termAsNumber, sum->term, terms, nullptr });
            continue;
        }
        ValueID value = m_passedOn[index];
        int64_t step = 0;
        while (value != parameter) {
            const Operation& operation = at(value);
            if (operation.opcode == Opcode::Narrowed || operation.opcode == Opcode::WidenInteger || operation.opcode == Opcode::NarrowInteger) {
                value = operation.valueOperand();
                continue;
            }
            if (!steps.contains(operation.opcode) || operation.form() != OperatorForm::Integers || isInvariant(value))
                break;
            if (operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement) {
                step += operation.opcode == Opcode::Increment ? 1 : -1;
                value = operation.valueOperand();
                continue;
            }
            if (!m_code.isConstant(operation.rightOperand()) || at(operation.rightOperand()).opcode != Opcode::Integer)
                break;
            step += operation.opcode == Opcode::Add ? at(operation.rightOperand()).integer() : -at(operation.rightOperand()).integer();
            value = operation.leftOperand();
        }
        if (value == parameter && step && std::abs(step) <= 16) {
            m_indexParameters.append(parameter);
            m_found.indexParameters.append({ parameter, step });
            m_found.steps.append({ index, step });
        } else
            m_reduced.append(index);
    }
    return m_indexParameters.contains(m_loop.counter);
}

std::optional<int64_t> WideLoopFinder::stepOf(ValueID value) const
{
    if (isInvariant(value))
        return isInteger(representationOf(value)) ? std::optional<int64_t>(0) : std::nullopt;
    for (auto [index, step] : m_found.steps) {
        if (m_parameters[index] == value)
            return step;
    }
    return m_found.scalars.contains(value) ? m_found.stepsOfScalars[m_found.scalars.find(value)] : std::optional<int64_t>();
}

std::optional<CodeGenerator::WideLanes> WideLoopFinder::lanesOfView(TypedArrayType type)
{
    switch (type) {
    case TypeFloat32:
    case TypeFloat64:
        return CodeGenerator::WideLanes::Numbers;
    case TypeInt8:
    case TypeUint8:
    case TypeUint8Clamped:
        return CodeGenerator::WideLanes::Bytes;
    case TypeInt16:
    case TypeUint16:
        return CodeGenerator::WideLanes::Halfwords;
    case TypeInt32:
    case TypeUint32:
        return CodeGenerator::WideLanes::Words;
    default:
        return std::nullopt;
    }
}

void WideLoopFinder::findLanes()
{
    std::optional<CodeGenerator::WideLanes> widest;
    for (ValueID value : m_values) {
        const Operation& operation = at(value);
        if (isOfSum(value))
            continue;
        if (isAccessOfList(operation) || operation.representation == Representation::Double || operation.representation == Representation::OptionalDouble) {
            m_found.lanes = CodeGenerator::WideLanes::Numbers;
            return;
        }
        if (isAccessOfView(operation)) {
            CodeGenerator::WideLanes lanes = lanesOfView(operation.typedArrayType()).value_or(CodeGenerator::WideLanes::Numbers);
            widest = widest ? std::max(*widest, lanes) : lanes;
        }
    }
    if (widest)
        m_found.lanes = m_reduced.isEmpty() && m_found.sums.isEmpty() ? *widest : std::max(*widest, CodeGenerator::WideLanes::Words);
}

bool WideLoopFinder::findTests()
{
    for (size_t place = 0; place < m_loop.turn.size(); ++place) {
        const Block& block = m_code.at(m_loop.turn[place]);
        const Terminator& terminator = block.terminator;
        if (!block.parameters().empty())
            return false;
        BlockID next = place + 1 < m_loop.turn.size() ? m_loop.turn[place + 1] : m_head;
        if (terminator.kind == TerminatorKind::Jump) {
            if (m_code.at(terminator.next()).to != next)
                return false;
            continue;
        }
        if (terminator.kind != TerminatorKind::Branch || m_code.at(terminator.taken()).to != next || next == m_head)
            return false;
        ValueID condition = terminator.conditionOperand();
        if (m_loop.relationsOfCounterBelowBound.contains(condition)) {
            m_tests.append(condition);
            continue;
        }
        if (m_loop.relationsAskedInFront.contains(condition)) {
            if (!isInvariant(condition))
                m_tests.append(condition);
            continue;
        }
        const Operation& test = at(condition);
        if (!isInvariant(condition) && test.opcode == Opcode::IsElementPresent && test.arrayStorage() == ArrayStorage::Double) {
            m_tests.append(condition);
            m_presences.append(condition);
            continue;
        }
        if (!isInvariant(condition) && test.opcode == Opcode::Less && test.form() == OperatorForm::Integers && isInvariant(test.rightOperand()) && isInteger(representationOf(test.rightOperand()))) {
            m_tests.append(condition);
            m_found.relationsHeldBeforeLoop.append({ condition, 0 });
            continue;
        }
        if (isInvariant(condition) || test.opcode != Opcode::StrictEq || test.form() != OperatorForm::Numbers || test.leftOperand() != test.rightOperand() || representationOf(test.leftOperand()) != Representation::Double)
            return false;
        m_tests.append(condition);
        m_found.numbersTested.append(test.leftOperand());
    }
    return true;
}

bool WideLoopFinder::classifyListAccess(ValueID value)
{
    const Operation& access = at(value);
    ValueID storage = access.storageOperand();
    if (m_found.lanes != CodeGenerator::WideLanes::Numbers || !storage || !isInvariant(storage) || !isInvariant(access.arrayOperand()) || !isConsecutive(access.positionOperand()))
        return false;
    m_found.accesses.append(value);
    if (isBoxedTerm(value))
        return access.isInBounds() || access.isPresent();
    if (storesElement(access)) {
        if (!access.isInBounds())
            return false;
        bool goesByKind = access.opcode == Opcode::PutContiguousIndex;
        if (goesByKind && (!access.goesByKindOfStorage() || !std::ranges::any_of(m_loop.listsAskedForKind, [&](const auto& asked) { return asked.list == access.arrayOperand() && asked.isStoredInto; })))
            return false;
        ValueID stored = access.valueOperand();
        if (!isNumberOperand(stored))
            return false;
        if (isData(stored) && !isWord(stored)) {
            if (!m_found.numbersTested.contains(stored)) {
                if (!goesByKind)
                    return false;
                m_found.numbersTested.append(stored);
            }
        } else if (goesByKind && representationOf(stored) == Representation::Double && !m_code.isConstant(stored))
            m_found.invariantsTested.append(stored);
        return true;
    }
    if (access.arrayStorage() != ArrayStorage::Double || representationOf(value) != Representation::Double || (!access.readsHoleAsNaN() && !access.isPresent() && !access.listHasNoHoles()))
        return false;
    for (size_t place = 0; place < m_presences.size(); ++place) {
        const Operation& presence = at(m_presences[place]);
        if (presence.arrayOperand() != access.arrayOperand() || presence.positionOperand() != access.positionOperand() || presence.storageOperand() != storage)
            continue;
        bool isBelowLength = presence.isInBounds() || m_loop.accessesUnderCounterBelowBound.contains(m_presences[place]);
        if (!isBelowLength && (access.lengthOperands().empty() || presence.lengthOperands().empty() || access.lengthOperands()[0] != presence.lengthOperands()[0] || !isInvariant(access.lengthOperands()[0]) || !isInteger(representationOf(access.lengthOperands()[0]))))
            return false;
        if (!isBelowLength)
            m_found.accessesComparedWithLength.append(value);
        m_found.numbersTested.append(value);
        m_presences.removeAt(place);
        return true;
    }
    if (access.isInBounds() || access.isPresent() || m_loop.accessesUnderCounterBelowBound.contains(value))
        return true;
    if (access.lengthOperands().empty() || !isInvariant(access.lengthOperands()[0]) || !isInteger(representationOf(access.lengthOperands()[0])))
        return false;
    m_found.accessesComparedWithLength.append(value);
    return true;
}

bool WideLoopFinder::isNumberOperand(ValueID value)
{
    if (isData(value))
        return true;
    if (isInvariant(value)) {
        Representation representation = representationOf(value);
        bool isInteger32 = m_code.isConstant(value) && at(value).opcode == Opcode::Integer;
        if (representation != Representation::Double && !isInteger(representation) && !isInteger32)
            return false;
        if (!m_found.invariants.contains(value))
            m_found.invariants.append(value);
        return true;
    }
    return !!stepOf(value);
}

bool WideLoopFinder::classifyAccess(ValueID value)
{
    const Operation& access = at(value);
    bool isStore = access.opcode == Opcode::PutTypedArrayElement;
    TypedArrayType type = access.typedArrayType();
    std::optional<CodeGenerator::WideLanes> lanes = lanesOfView(type);
    if (!lanes || !isInvariant(access.viewOperand()) || (access.vectorOperand() && !isInvariant(access.vectorOperand())) || !isConsecutive(access.positionOperand()))
        return false;
    if (!access.isInsideView()) {
        if (access.lengthOperands().empty() || !access.vectorOperand() || !isInvariant(access.lengthOperands()[0]) || !isInteger(representationOf(access.lengthOperands()[0])))
            return false;
        m_found.accessesComparedWithLength.append(value);
    }
    bool isConverted = m_found.lanes == CodeGenerator::WideLanes::Numbers && *lanes != CodeGenerator::WideLanes::Numbers;
    bool isWidened = m_found.lanes == CodeGenerator::WideLanes::Words && *lanes < CodeGenerator::WideLanes::Words;
    if (m_found.lanes != *lanes && ((!isConverted && !isWidened) || isStore || type == TypeUint32))
        return false;
    if (!isStore) {
        Representation representation = representationOf(value);
        bool isNumber = representation == Representation::Double || (representation == Representation::OptionalDouble && !access.isInsideView());
        bool isWholeNumber = isInteger(representation) || (!access.isInsideView() && representation == Representation::OptionalInt64);
        if (*lanes == CodeGenerator::WideLanes::Numbers ? !isNumber : !isWholeNumber)
            return false;
        if (isConverted)
            m_found.words.append(value);
        if (isWidened)
            m_found.widenedElements.append(value);
    } else {
        ValueID stored = access.valueOperand();
        if (m_found.lanes == CodeGenerator::WideLanes::Numbers ? !isNumberOperand(stored) : !isIntegerOperand(stored))
            return false;
        Type typeOfStored = m_generator.typeOf(stored);
        if (type == TypeUint8Clamped && (!typeOfStored.isInteger() || !typeOfStored.integers.isSubsetOf(Range { 0, UINT8_MAX })))
            return false;
    }
    m_found.accesses.append(value);
    return true;
}

std::optional<int64_t> WideLoopFinder::stepOfResult(const Operation& operation) const
{
    switch (operation.opcode) {
    case Opcode::Narrowed:
    case Opcode::WidenInteger:
    case Opcode::NarrowInteger:
        return stepOf(operation.valueOperand());
    case Opcode::Increment:
    case Opcode::Decrement:
        return operation.form() == OperatorForm::Integers ? stepOf(operation.valueOperand()) : std::nullopt;
    default:
        break;
    }
    if (operation.form() != OperatorForm::Integers)
        return std::nullopt;
    std::optional<int64_t> left = stepOf(operation.leftOperand());
    std::optional<int64_t> right = stepOf(operation.rightOperand());
    if (!left || !right)
        return std::nullopt;
    return operation.opcode == Opcode::Add ? *left + *right : *left - *right;
}

bool WideLoopFinder::isOperand(ValueID value)
{
    if (isData(value))
        return !isWord(value);
    if (!isInvariant(value))
        return false;
    Representation representation = representationOf(value);
    if (m_found.lanes == CodeGenerator::WideLanes::Numbers ? representation != Representation::Double : !isInteger(representation))
        return false;
    if (!m_found.invariants.contains(value))
        m_found.invariants.append(value);
    return true;
}

bool WideLoopFinder::classifyInteger(ValueID value)
{
    const Operation& operation = at(value);
    if (!isInteger(operation.representation))
        return false;
    bool isWord = m_found.lanes == CodeGenerator::WideLanes::Words;
    auto hasIntegerOperands = [&] {
        return isIntegerOperand(operation.leftOperand()) && isIntegerOperand(operation.rightOperand());
    };
    switch (operation.opcode) {
    case Opcode::Narrowed:
    case Opcode::WidenInteger:
    case Opcode::NarrowInteger:
    case Opcode::ToInt32:
        return isIntegerOperand(operation.valueOperand());
    case Opcode::Add:
    case Opcode::Sub:
        return operation.form() == OperatorForm::Integers && hasIntegerOperands();
    case Opcode::Mul:
        return isWord && operation.form() == OperatorForm::Integers && hasIntegerOperands();
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
        return hasIntegerOperands();
    case Opcode::Mod:
        return !!m_generator.maskOfRemainder(operation) && isIntegerOperand(operation.leftOperand());
    case Opcode::LShift:
    case Opcode::URShift:
        return isX86_64() && isWord && isInteger(representationOf(operation.leftOperand())) && isOperand(operation.leftOperand()) && m_code.isConstant(operation.rightOperand()) && at(operation.rightOperand()).opcode == Opcode::Integer;
    default:
        return false;
    }
}

bool WideLoopFinder::classifyNumber(ValueID value)
{
    static constexpr OpcodeSet arithmetic { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
    const Operation& operation = at(value);
    if (operation.opcode == Opcode::Narrowed && isWord(operation.valueOperand()) && isInteger(operation.representation)) {
        m_found.words.append(value);
        return true;
    }
    if (operation.representation != Representation::Double)
        return false;
    if (operation.opcode == Opcode::Narrowed)
        return isNumberOperand(operation.valueOperand());
    if (operation.opcode == Opcode::IntegerToNumber)
        return isNumberOperand(operation.valueOperand());
    if (operation.opcode == Opcode::Negate)
        return operation.form() == OperatorForm::Numbers && isData(operation.valueOperand());
    if (operation.opcode == Opcode::CallInline) {
        bool hasInstruction = operation.inlineBuiltin() == InlineBuiltin::MathAbs || operation.inlineBuiltin() == InlineBuiltin::MathSqrt;
        return hasInstruction && operation.operands().size() == 2 && isData(operation.operand(1)) && representationOf(operation.operand(1)) == Representation::Double;
    }
    if (!arithmetic.contains(operation.opcode))
        return false;
    bool computesNumber = operation.form() == OperatorForm::Numbers || operation.form() == OperatorForm::CheckedIntegers || operation.form() == OperatorForm::Integers;
    return computesNumber && isNumberOperand(operation.leftOperand()) && isNumberOperand(operation.rightOperand());
}

bool WideLoopFinder::findReduction(size_t indexOfParameter)
{
    ValueID parameter = m_parameters[indexOfParameter];
    if (m_found.lanes != CodeGenerator::WideLanes::Words || representationOf(parameter) != Representation::Int32)
        return false;
    ValueID result = passedInteger(m_code, m_passedOn[indexOfParameter]);
    if (isInvariant(result) || result == parameter)
        return false;
    const Operation& operation = at(result);
    bool isSum = operation.opcode == Opcode::Add && operation.form() == OperatorForm::Integers;
    static constexpr OpcodeSet bitwise { Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor };
    if (!isSum && !bitwise.contains(operation.opcode))
        return false;
    bool isCut = m_passedOn[indexOfParameter] != result && representationOf(m_passedOn[indexOfParameter]) == Representation::Int32;
    if (isSum && operation.representation == Representation::Int64 && !isCut)
        return false;
    bool leftIsParameter = passedInteger(m_code, operation.leftOperand()) == parameter;
    bool rightIsParameter = passedInteger(m_code, operation.rightOperand()) == parameter;
    if (leftIsParameter == rightIsParameter)
        return false;
    ValueID term = leftIsParameter ? operation.rightOperand() : operation.leftOperand();
    if (!isInteger(representationOf(term)) || !isOperand(term))
        return false;
    m_found.reductions.append({ indexOfParameter, result, term, operation.opcode, nullptr });
    return true;
}

bool WideLoopFinder::findTermsOfSums(bool hasStored)
{
    if (m_found.sums.isEmpty())
        return true;
    if (!m_found.numbersTested.isEmpty())
        return false;
    if (m_found.lanes == CodeGenerator::WideLanes::Words)
        return std::ranges::all_of(m_found.sums, [&](const CodeGenerator::WideSum& sum) { return sum.terms != CodeGenerator::WideSum::Terms::BoxedWords && isOperand(sum.term); });
    if (m_found.lanes != CodeGenerator::WideLanes::Numbers || hasStored || !m_reduced.isEmpty() || !m_loop.listsWhoseKindIsRead.isEmpty())
        return false;
    m_found.storageOfLists = ArrayStorage::Int32;
    return std::ranges::all_of(m_found.sums, [](const CodeGenerator::WideSum& sum) { return sum.terms == CodeGenerator::WideSum::Terms::BoxedWords; })
        && std::ranges::all_of(m_found.computed, [&](ValueID value) { return isBoxedTerm(value); })
        && std::ranges::all_of(m_loop.listsAskedForKind, [](const CodeGenerator::ListAskedForKind& asked) { return asked.kind == ArrayStorage::Int32 && !asked.isStoredInto; });
}

std::optional<CodeGenerator::WideLoop> WideLoopFinder::find()
{
    if (!findTests() || !findParameters())
        return std::nullopt;
    for (BlockID block : m_loop.turn)
        m_values.append(m_code.valuesOf(block));
    findLanes();
    Vector<ValueID, 16> ofReductions;
    bool hasStored = false;
    for (ValueID value : m_values) {
        const Operation& operation = at(value);
        if (operation.opcode == Opcode::LoopHint || operation.opcode == Opcode::GetOwnDoubleIndex || m_tests.contains(value) || isOfSum(value))
            continue;
        static constexpr OpcodeSet indexOperations { Opcode::Narrowed, Opcode::Increment, Opcode::Decrement, Opcode::Add, Opcode::Sub, Opcode::WidenInteger, Opcode::NarrowInteger };
        if (indexOperations.contains(operation.opcode) && isInteger(operation.representation)) {
            std::optional<int64_t> step = stepOfResult(operation);
            if (step) {
                m_found.scalars.append(value);
                m_found.stepsOfScalars.append(*step);
                continue;
            }
        }
        bool readsReduction = false;
        operation.forEachChild([&](ValueID operand) {
            readsReduction |= ofReductions.contains(operand) || std::ranges::any_of(m_reduced, [&](size_t index) { return m_parameters[index] == operand; });
        });
        if (readsReduction) {
            ofReductions.append(value);
            continue;
        }
        bool isStore = storesElement(operation);
        hasStored |= isStore;
        if (isAccessOfView(operation)) {
            if (!classifyAccess(value))
                return std::nullopt;
        } else if (isAccessOfList(operation)) {
            if (!classifyListAccess(value))
                return std::nullopt;
        } else if (!(m_found.lanes == CodeGenerator::WideLanes::Numbers ? classifyNumber(value) : classifyInteger(value)))
            return std::nullopt;
        m_found.computed.append(value);
    }
    if (m_found.accesses.isEmpty() || !m_presences.isEmpty())
        return std::nullopt;
    for (ValueID tested : m_found.numbersTested) {
        if (!isData(tested) || isWord(tested))
            return std::nullopt;
    }
    for (auto& [relation, step] : m_found.relationsHeldBeforeLoop) {
        std::optional<int64_t> stepOfPosition = stepOf(at(relation).leftOperand());
        if (!stepOfPosition || *stepOfPosition < 0)
            return std::nullopt;
        step = *stepOfPosition;
    }
    for (size_t index : m_reduced) {
        if (!findReduction(index))
            return std::nullopt;
    }
    for (size_t later = 0; !m_found.numbersTested.isEmpty() && later < m_found.accesses.size(); ++later) {
        for (size_t earlier = 0; earlier < later; ++earlier) {
            const Operation& stored = at(m_found.accesses[earlier]);
            const Operation& read = at(m_found.accesses[later]);
            if (storesElement(stored) && !storesElement(read) && m_generator.accessSameElement(stored, read))
                return std::nullopt;
        }
    }
    for (ValueID value : ofReductions) {
        bool isOfSomeReduction = std::ranges::any_of(m_found.reductions, [&](const CodeGenerator::WideReduction& reduction) {
            ValueID passed = m_passedOn[reduction.indexOfParameter];
            const Operation& operation = at(reduction.result);
            for (ValueID chain : { passed, operation.leftOperand(), operation.rightOperand() }) {
                for (ValueID link = chain;; link = at(link).valueOperand()) {
                    if (link == value)
                        return true;
                    if (link == passedInteger(m_code, link))
                        break;
                }
            }
            return false;
        });
        if (!isOfSomeReduction || m_code.useCountOf(value) != 1)
            return std::nullopt;
    }
    if (!findTermsOfSums(hasStored))
        return std::nullopt;
    if (!hasStored && m_found.sums.isEmpty() && (m_found.reductions.isEmpty() || m_found.lanes == CodeGenerator::WideLanes::Numbers))
        return std::nullopt;
    return WTF::move(m_found);
}

CodeGenerator::ElementIndex CodeGenerator::elementIndexOf(ValueID position) const
{
    static constexpr OpcodeSet passing { Opcode::Narrowed, Opcode::WidenInteger, Opcode::NarrowInteger };
    ElementIndex element { position, 0 };
    while (true) {
        const Operation& operation = code().at(element.base);
        if (passing.contains(operation.opcode)) {
            element.base = operation.valueOperand();
            continue;
        }
        if (operation.opcode == Opcode::Increment || operation.opcode == Opcode::Decrement) {
            if (operation.form() != OperatorForm::Integers)
                return element;
            element.offset += operation.opcode == Opcode::Increment ? 1 : -1;
            element.base = operation.valueOperand();
            continue;
        }
        bool isSum = operation.opcode == Opcode::Add || operation.opcode == Opcode::Sub;
        if (!isSum || operation.form() != OperatorForm::Integers || !code().isConstant(operation.rightOperand()) || code().at(operation.rightOperand()).opcode != Opcode::Integer)
            return element;
        int64_t added = code().at(operation.rightOperand()).integer();
        element.offset += operation.opcode == Opcode::Add ? added : -added;
        element.base = operation.leftOperand();
    }
}

bool CodeGenerator::accessSameElement(const Operation& first, const Operation& second) const
{
    ElementIndex indexOfFirst = elementIndexOf(first.positionOperand());
    ElementIndex indexOfSecond = elementIndexOf(second.positionOperand());
    if (indexOfFirst.base != indexOfSecond.base || indexOfFirst.offset != indexOfSecond.offset)
        return false;
    if (isAccessOfList(first) || isAccessOfList(second))
        return isAccessOfList(first) && isAccessOfList(second) && first.storageOperand() == second.storageOperand();
    return first.vectorOperand() && first.vectorOperand() == second.vectorOperand() && first.typedArrayType() == second.typedArrayType();
}

std::optional<CodeGenerator::WideLoop> CodeGenerator::findWideLoop(const CountedLoop& loop) const
{
    if (loop.testsAtEnd || loop.step != 1 || (loop.relation != Opcode::Less && loop.relation != Opcode::LessEq) || !loop.bound || m_keepsValuesInHeapFrame || !loop.loopCarriedValueStores.isEmpty() || loop.storesIntoSlots)
        return std::nullopt;
    if (!loop.flatStrings.isEmpty() || !loop.viewsAskedForKind.isEmpty() || !loop.boundsAskedInFront.isEmpty())
        return std::nullopt;
    if (checksLengthsOfViews(m_inputs.build.options().testing) || callsEndVectors())
        return std::nullopt;
    std::optional<WideLoop> wide = WideLoopFinder(*this, loop).find();
    if (!wide && loop.sumsOfIntegers.isEmpty() && loop.asksListsForAnotherKind()) {
        CountedLoop overIntegers = loop;
        for (ListAskedForKind& asked : overIntegers.listsAskedForKind)
            asked.kind = ArrayStorage::Int32;
        findSumsOfIntegers(overIntegers, code().at(code().at(loop.turn.last()).terminator.next()));
        if (!overIntegers.sumsOfIntegers.isEmpty())
            wide = WideLoopFinder(*this, overIntegers).find();
    }
    if (!wide)
        return std::nullopt;
    if (wide->lanes == WideLanes::Numbers && !m_followsNumbersOutOfBytes && std::ranges::any_of(wide->accesses, [&](ValueID access) { return opcodeOf(access) == Opcode::GetTypedArrayElement && representationOf(access) == Representation::Double; }))
        return std::nullopt;
    return wide;
}

unsigned CodeGenerator::lanesOfVector(WideLanes lanes)
{
    switch (lanes) {
    case WideLanes::Bytes:
        return 16;
    case WideLanes::Halfwords:
        return 8;
    case WideLanes::Words:
        return 4;
    case WideLanes::Numbers:
        return 2;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LValue CodeGenerator::wideOperand(const WideLoop& wide, ValueID value)
{
    for (size_t place = 0; place < wide.invariants.size(); ++place) {
        if (wide.invariants[place] == value)
            return wide.splats[place];
    }
    size_t place = std::ranges::find(wide.computed, value) - wide.computed.begin();
    RELEASE_ASSERT(place < wide.computed.size() && wide.vectors[place]);
    return wide.vectors[place];
}

std::optional<double> CodeGenerator::exactReciprocalOf(ValueID divisor) const
{
    if (!code().isConstant(divisor) || (opcodeOf(divisor) != Opcode::Integer && opcodeOf(divisor) != Opcode::Double))
        return std::nullopt;
    double number = opcodeOf(divisor) == Opcode::Integer ? static_cast<double>(code().at(divisor).integer()) : code().at(divisor).number();
    static constexpr int greatestExponent = 1000;
    int exponent = 0;
    if (std::abs(std::frexp(number, &exponent)) != 0.5 || std::abs(exponent) > greatestExponent)
        return std::nullopt;
    return 1 / number;
}

std::optional<int64_t> CodeGenerator::maskOfRemainder(const Operation& operation) const
{
    if (operation.opcode != Opcode::Mod || operation.form() != OperatorForm::Integers)
        return std::nullopt;
    ValueID divisor = operation.rightOperand();
    if (!code().isConstant(divisor) || opcodeOf(divisor) != Opcode::Integer)
        return std::nullopt;
    int64_t power = code().at(divisor).integer();
    Type divided = typeOf(operation.leftOperand());
    if (power <= 0 || power > std::numeric_limits<int32_t>::max() || !hasOneBitSet(static_cast<uint64_t>(power)) || !divided.isInteger() || divided.integers.minimum < 0)
        return std::nullopt;
    return power - 1;
}

LValue CodeGenerator::wideNumber(const WideLoop& wide, ValueID value, unsigned group)
{
    VectorOutput vectors(m_out);
    if (wide.words.contains(value))
        return vectors.toNumbers(wideOperand(wide, value), 0);
    auto index = std::ranges::find(wide.scalars, value);
    size_t place = index - wide.scalars.begin();
    int64_t step = 0;
    if (index != wide.scalars.end())
        step = wide.stepsOfScalars[place];
    else if (auto parameter = std::ranges::find(wide.indexParameters, value, &std::pair<ValueID, int64_t>::first); parameter != wide.indexParameters.end())
        step = parameter->second;
    else
        return wideOperand(wide, value);
    LValue first = vectors.splat(m_out.intToDouble(get(value).value));
    if (!step)
        return first;
    LValue added = vectors.insert(m_out.constDouble(static_cast<double>(step * 2 * group)), m_out.constDouble(static_cast<double>(step * (2 * group + 1))));
    return vectors.compute(VectorOutput::Arithmetic::Add, first, added);
}

LValue CodeGenerator::wideInteger(WideLoop& wide, ValueID value, unsigned group)
{
    size_t place = wide.steppedWords.findIf([&](const auto& stepped) { return stepped.first == value; });
    if (place == notFound)
        return wideOperand(wide, value);
    VectorOutput vectors(m_out);
    LValue& first = wide.splatsOfSteppedWords[place];
    if (!first)
        first = vectors.splatInteger(m_out.castToInt32(get(value).value), lanesOfVector(wide.lanes));
    int64_t step = wide.steppedWords[place].second;
    size_t steps = wide.stepsOfLanes.findIf([&](const auto& known) { return known.step == step && known.group == group; });
    return steps == notFound ? first : vectors.computeIntegers(Opcode::Add, lanesOfVector(wide.lanes), first, wide.stepsOfLanes[steps].vector);
}

TypedPointer CodeGenerator::elementOfWideLoop(const WideLoop& wide, const ValueContext& access)
{
    ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
    if (isAccessOfView(code().at(access.id())))
        return access->isInsideView() ? elements.elementInsideView(access) : elements.elementBelowNamedLength(access);
    LValue index = get(access->positionOperand()).value;
    if (index->type() == Machine::Int32)
        index = m_out.extendWordThatIsNotNegative(index);
    return elements.elementOfStorage(wide.storageOfLists, get(access->storageOperand()).value, index);
}

void CodeGenerator::requireWholeNumber(LValue number, ValueID parameter, LBasicBlock otherwise)
{
    LValue size = m_out.doubleAbs(number);
    LValue limit = m_out.constDouble(0x1p52);
    LBasicBlock isSmall = m_out.newBlock();
    m_out.branch(m_out.doubleLessThanOrEqual(size, limit), normalEdge(isSmall), unlikelyEdge(otherwise));
    m_out.appendTo(isSmall);
    if (typeOf(parameter).isNumberWithoutFraction())
        return;
    LBasicBlock isWhole = m_out.newBlock();
    m_out.branch(m_out.doubleEqual(m_out.doubleSub(m_out.doubleAdd(size, limit), limit), size), normalEdge(isWhole), unlikelyEdge(otherwise));
    m_out.appendTo(isWhole);
}

LValue CodeGenerator::loadWords(TypedPointer element, TypedArrayType type, unsigned lanes)
{
    VectorOutput vectors(m_out);
    switch (elementSize(type)) {
    case 1:
        return vectors.widenToWords(lanes == 4 ? m_out.load(element, Machine::Float) : m_out.load16ZeroExt32(element), type);
    case 2:
        return vectors.widenToWords(lanes == 4 ? m_out.loadDouble(element) : m_out.load32(element), type);
    case 4:
        return vectors.widenToWords(m_out.loadDouble(element), type);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void CodeGenerator::emitWideOperation(WideLoop& wide, size_t place, unsigned group)
{
    ValueID value = wide.computed[place];
    const Operation& operation = code().at(value);
    ValueContext context(operation.block(), value, operation, nullptr);
    SetForScope<const OperationContext*> whileEmitting(m_operation, &context);
    m_out.setOrigin(machineOriginOf(value));
    declareAccessesOf(context);
    VectorOutput vectors(m_out);
    LValue& result = wide.vectors[place];
    bool areNumbers = wide.lanes == WideLanes::Numbers;
    switch (operation.opcode) {
    case Opcode::GetArrayElement:
    case Opcode::PutDoubleIndex:
    case Opcode::PutContiguousIndex: {
        TypedPointer first = elementOfWideLoop(wide, context);
        TypedPointer element(*first.heap(), first.base(), first.offset() + group * bytesOfVector);
        if (operation.opcode == Opcode::GetArrayElement)
            result = m_out.load(element, Machine::Vector128);
        else
            m_out.store(wideNumber(wide, operation.valueOperand(), group), element);
        return;
    }
    case Opcode::IntegerToNumber:
        result = wideNumber(wide, operation.valueOperand(), group);
        return;
    case Opcode::GetTypedArrayElement:
    case Opcode::PutTypedArrayElement: {
        bool isSingle = operation.typedArrayType() == TypeFloat32;
        TypedPointer first = elementOfWideLoop(wide, context);
        bool areConverted = wide.words.contains(value);
        bool areWidened = wide.widenedElements.contains(value);
        TypedPointer element(*first.heap(), first.base(), first.offset() + group * (areConverted ? 2 * bytesOfElement(operation) : areWidened ? 4 * bytesOfElement(operation) : isSingle ? bytesOfVector / 2 : bytesOfVector));
        if (areConverted || areWidened) {
            result = loadWords(element, operation.typedArrayType(), areWidened ? 4 : 2);
            return;
        }
        if (operation.opcode == Opcode::GetTypedArrayElement) {
            result = isSingle ? vectors.singlesToNumbers(m_out.loadDouble(element)) : m_out.load(element, Machine::Vector128);
            return;
        }
        LValue stored = areNumbers ? wideNumber(wide, operation.valueOperand(), group) : wideInteger(wide, operation.valueOperand(), group);
        if (isSingle)
            m_out.storeDouble(vectors.numbersToSingles(stored), element);
        else
            m_out.store(stored, element);
        return;
    }
    case Opcode::Narrowed:
    case Opcode::WidenInteger:
    case Opcode::NarrowInteger:
    case Opcode::ToInt32:
        result = areNumbers && !wide.words.contains(value) ? wideNumber(wide, operation.valueOperand(), group) : areNumbers ? wideOperand(wide, operation.valueOperand()) : wideInteger(wide, operation.valueOperand(), group);
        return;
    case Opcode::Negate:
        result = vectors.computeIntegers(Opcode::BitXor, lanesOfVector(wide.lanes), wideNumber(wide, operation.valueOperand(), group), wide.signs);
        return;
    case Opcode::CallInline: {
        LValue numbers = wideNumber(wide, operation.operand(1), group);
        result = operation.inlineBuiltin() == InlineBuiltin::MathSqrt ? vectors.squareRoots(numbers) : vectors.computeIntegers(Opcode::BitAnd, lanesOfVector(wide.lanes), numbers, wide.magnitudes);
        return;
    }
    case Opcode::Mod: {
        auto mask = std::ranges::find(wide.masks, value, &std::pair<ValueID, LValue>::first);
        result = wideInteger(wide, operation.leftOperand(), group);
        if (mask != wide.masks.end())
            result = vectors.computeIntegers(Opcode::BitAnd, lanesOfVector(wide.lanes), result, mask->second);
        return;
    }
    case Opcode::LShift:
    case Opcode::URShift:
        result = vectors.shiftWords(operation.opcode == Opcode::LShift, wideInteger(wide, operation.leftOperand(), group), code().at(operation.rightOperand()).integer() & 31);
        return;
    default:
        break;
    }
    LValue left = areNumbers ? wideNumber(wide, operation.leftOperand(), group) : wideInteger(wide, operation.leftOperand(), group);
    if (areNumbers && operation.opcode == Opcode::Div) {
        auto reciprocal = std::ranges::find(wide.reciprocals, operation.rightOperand(), &std::pair<ValueID, LValue>::first);
        if (reciprocal != wide.reciprocals.end()) {
            result = vectors.compute(VectorOutput::Arithmetic::Mul, left, reciprocal->second);
            return;
        }
    }
    LValue right = areNumbers ? wideNumber(wide, operation.rightOperand(), group) : wideInteger(wide, operation.rightOperand(), group);
    if (areNumbers) {
        result = vectors.compute(operation.opcode == Opcode::Add ? VectorOutput::Arithmetic::Add : operation.opcode == Opcode::Sub ? VectorOutput::Arithmetic::Sub : operation.opcode == Opcode::Mul ? VectorOutput::Arithmetic::Mul : VectorOutput::Arithmetic::Div, left, right);
        return;
    }
    result = vectors.computeIntegers(operation.opcode, lanesOfVector(wide.lanes), left, right);
}

void CodeGenerator::layOutWide(const CountedLoop& loop, WideLoop& wide)
{
    build().statistics().count(StatisticID::LoopsInVectors);
    m_proc.noteVectors();
    BlockID head = m_order[loop.positionOfHead];
    std::span<const BlockID> turn = loop.turn.span();
    const Block& blockOfHead = code().at(head);
    std::span<const ValueID> parameters = blockOfHead.parameters();
    size_t indexOfCounter = std::ranges::find(parameters, loop.counter) - parameters.begin();
    using Values = Vector<LValue, 8>;
    unsigned lanes = lanesOfVector(wide.lanes);
    unsigned groups = wide.lanes == WideLanes::Bytes ? 1 : wide.lanes == WideLanes::Numbers ? 4 : 2;
    bool storesAtEnd = !wide.numbersTested.isEmpty();
    int64_t iterations = lanes * groups;
    LBasicBlock writtenHead = m_out.newBlock();
    LBasicBlock stretch = m_out.newBlock();
    LBasicBlock turns = m_out.newBlock();
    Values phisOfHead;
    Values phisOfWrittenHead;
    Values phisOfStretch;
    Values phisOfTurns;
    auto isReduced = [&](size_t index) {
        return std::ranges::contains(wide.reductions, index, &WideReduction::indexOfParameter) || std::ranges::contains(wide.sums, index, &WideSum::indexOfParameter);
    };
    for (size_t index = 0; index < parameters.size(); ++index) {
        LValue phi = m_values[parameters[index].index()];
        phisOfHead.append(phi);
        phisOfWrittenHead.append(phi ? m_out.phiIn(writtenHead, phi->type()) : nullptr);
        phisOfStretch.append(phi ? m_out.phiIn(stretch, phi->type()) : nullptr);
        phisOfTurns.append(phi && !isReduced(index) ? m_out.phiIn(turns, phi->type()) : nullptr);
    }
    for (WideReduction& reduction : wide.reductions)
        reduction.vector = m_out.phiIn(turns, Machine::Vector128);
    for (WideSum& sum : wide.sums)
        sum.vector = m_out.phiIn(turns, Machine::Vector128);
    LBasicBlock stretchIsWritten = wide.sums.isEmpty() ? nullptr : m_out.newBlock();
    auto wideValue = [&](LValue value) { return value->type() == Machine::Int32 ? m_out.signExt32To64(value) : value; };
    auto pass = [&](const Values& phis, LBasicBlock target, auto&& valueAt) {
        Values values;
        for (size_t index = 0; index < phis.size(); ++index)
            values.append(phis[index] ? valueAt(index) : nullptr);
        for (size_t index = 0; index < phis.size(); ++index) {
            if (phis[index])
                m_out.upsilon(values[index], phis[index]);
        }
        m_out.jump(normalEdge(target));
    };
    VectorOutput vectors(m_out);
    TerminatorContext context(head, blockOfHead.terminator, nullptr);
    LValue bound = nullptr;
    LValue lastStart = nullptr;
    auto forgetValuesOfTurn = [&] {
        for (BlockID block : turn) {
            for (ValueID value : code().valuesOf(block))
                m_values[value.index()] = nullptr;
        }
    };
    auto setIndexes = [&](const Values& phis) {
        for (size_t index = 0; index < parameters.size(); ++index)
            m_values[parameters[index].index()] = phis[index];
        forgetValuesOfTurn();
        SetForScope<BlockID> whileLayingOut(m_headOfTurnsLaidOut, head);
        for (ValueID value : wide.scalars)
            generateValue(code().blockOf(value), value, code().at(value), false, { });
    };

    m_out.appendTo(blockFor(head));
    m_out.setOrigin(Machine::Origin());
    {
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LBasicBlock written = m_out.newBlock();
        LBasicBlock here = m_out.m_block;
        m_out.appendTo(written);
        pass(phisOfWrittenHead, writtenHead, [&](size_t index) { return phisOfHead[index]; });
        m_out.appendTo(here);
        auto require = [&](LValue condition) {
            LBasicBlock holds = m_out.newBlock();
            m_out.branch(condition, normalEdge(holds), normalEdge(written));
            m_out.appendTo(holds);
        };
        LValue counter = phisOfHead[indexOfCounter];
        bound = get(loop.bound).value;
        RELEASE_ASSERT(bound->type() == counter->type());
        int64_t fewestIterations = 2 * iterations;
        if (bound->type() == Machine::Int32 && typeOf(loop.bound).integers.minimum < std::numeric_limits<int32_t>::min() + fewestIterations)
            require(m_out.greaterThanOrEqual(bound, m_out.constInt32(static_cast<int32_t>(std::numeric_limits<int32_t>::min() + fewestIterations))));
        require(askCounter(loop.relation, counter, m_out.sub(bound, m_out.constantOfType(bound->type(), fewestIterations - 1))));
        lastStart = m_out.sub(bound, m_out.constantOfType(bound->type(), iterations - 1));
        for (ValueID relation : loop.relationsAskedInFront)
            require(emitter<ControlEmitter>().hoistedResult(relation).value());
        for (ValueID length : loop.lengthsHeldAgainstBound)
            require(m_out.greaterThanOrEqual(m_out.zeroExt(get(length).value, Machine::Int64), wideValue(bound)));
        for (const SteppedIndex& held : loop.steppedIndexes) {
            LValue greatest = wideValue(phisOfHead[held.indexOfParameter]);
            if (held.step > 0)
                greatest = m_out.add(greatest, m_out.mul(m_out.sub(wideValue(bound), wideValue(counter)), m_out.constInt64(held.step)));
            int64_t added = held.offset - (held.step > 0 && loop.relation == Opcode::Less ? held.step : 0);
            require(m_out.lessThan(m_out.add(greatest, m_out.constInt64(added)), m_out.zeroExt(get(held.length).value, Machine::Int64)));
        }
        ElementAccessEmitter& elements = emitter<ElementAccessEmitter>();
        for (const ListAskedForKind& asked : loop.listsAskedForKind)
            require((asked.isStoredInto ? elements.listTakesStoresInArrayStorage(asked.list, ArrayStorage::Double) : elements.listHasArrayStorage(asked.list, wide.storageOfLists)).value());
        for (ValueID list : loop.listsWhoseKindIsRead)
            require(elements.listHasArrayStorage(list, ArrayStorage::Double).value());
        for (ValueID invariant : wide.invariantsTested) {
            LValue number = numberForLane(invariant);
            require(m_out.doubleEqual(number, number));
        }
        setIndexes(phisOfHead);
        struct Access {
            ValueID value;
            LValue address;
            int64_t size;
        };
        Vector<Access, 8> accesses;
        for (ValueID access : wide.accesses) {
            const Operation& operation = code().at(access);
            ValueContext contextOfAccess(code().blockOf(access), access, operation, nullptr);
            SetForScope<const OperationContext*> whileAddressing(m_operation, &contextOfAccess);
            declareAccessesOf(contextOfAccess);
            TypedPointer element = elementOfWideLoop(wide, contextOfAccess);
            accesses.append({ access, element.offset() ? m_out.add(element.base(), m_out.constInt64(element.offset())) : element.base(), bytesOfElement(operation) });
        }
        LValue iterationsLeft = m_out.add(m_out.sub(wideValue(bound), wideValue(counter)), m_out.constInt64(loop.relation == Opcode::LessEq ? 1 : 0));
        for (ValueID access : wide.accessesComparedWithLength) {
            const Operation& operation = code().at(access);
            ValueContext contextOfAccess(code().blockOf(access), access, operation, nullptr);
            SetForScope<const OperationContext*> whileComparing(m_operation, &contextOfAccess);
            LValue position = wideValue(get(operation.positionOperand()).value);
            LValue length = m_out.zeroExt(get(operation.lengthOperands()[0]).value, Machine::Int64);
            require(m_out.below(position, length));
            require(m_out.belowOrEqual(m_out.add(position, iterationsLeft), length));
        }
        for (auto [relation, step] : wide.relationsHeldBeforeLoop) {
            const Operation& operation = code().at(relation);
            ValueContext contextOfRelation(code().blockOf(relation), relation, operation, nullptr);
            SetForScope<const OperationContext*> whileComparing(m_operation, &contextOfRelation);
            LValue position = wideValue(get(operation.leftOperand()).value);
            LValue distance = m_out.mul(m_out.sub(iterationsLeft, m_out.constInt64(1)), m_out.constInt64(step));
            require(m_out.lessThan(m_out.add(position, distance), wideValue(get(operation.rightOperand()).value)));
        }
        unsigned iterationsInOrder = storesAtEnd ? static_cast<unsigned>(iterations) : lanes;
        for (size_t later = 0; later < accesses.size(); ++later) {
            for (size_t earlier = 0; earlier < later; ++earlier) {
                const Operation& first = code().at(accesses[earlier].value);
                const Operation& second = code().at(accesses[later].value);
                if (!storesElement(first) && !storesElement(second))
                    continue;
                if (accessSameElement(first, second) && (!storesAtEnd || !storesElement(first) || storesElement(second)))
                    continue;
                LValue distance = m_out.sub(accesses[later].address, accesses[earlier].address);
                if (storesAtEnd && storesElement(first) && !storesElement(second) && accesses[earlier].size == accesses[later].size) {
                    int64_t reach = iterations * accesses[later].size;
                    require(m_out.aboveOrEqual(m_out.add(distance, m_out.constInt64(reach - 1)), m_out.constInt64(reach + accesses[later].size - 1)));
                    continue;
                }
                if (accesses[earlier].size == accesses[later].size) {
                    require(m_out.aboveOrEqual(m_out.sub(distance, m_out.constInt64(1)), m_out.constInt64(iterationsInOrder * accesses[later].size - 1)));
                    continue;
                }
                LBasicBlock isApart = m_out.newBlock();
                LBasicBlock isNotBehind = m_out.newBlock();
                m_out.branch(m_out.greaterThanOrEqual(distance, m_out.mul(iterationsLeft, m_out.constInt64(accesses[earlier].size))), normalEdge(isApart), normalEdge(isNotBehind));
                m_out.appendTo(isNotBehind);
                m_out.branch(m_out.greaterThanOrEqual(m_out.neg(distance), m_out.mul(iterationsLeft, m_out.constInt64(accesses[later].size))), normalEdge(isApart), normalEdge(written));
                m_out.appendTo(isApart);
            }
        }
        pass(phisOfStretch, stretch, [&](size_t index) { return phisOfHead[index]; });
    }

    auto invariantNumber = [&](ValueID invariant) {
        if (code().isConstant(invariant) && opcodeOf(invariant) == Opcode::Integer)
            return m_out.constDouble(static_cast<double>(code().at(invariant).integer()));
        return representationOf(invariant) == Representation::Double ? numberForLane(invariant) : m_out.intToDouble(get(invariant).value);
    };
    constexpr int bitsOfWord = 32;
    constexpr int bitsOfSum = 48;
    constexpr uint64_t signOfWord = uint64_t { 1 } << (bitsOfWord - 1);
    LValue signsOfWords = nullptr;
    LValue endOfStretch = lastStart;
    m_out.appendTo(stretch);
    {
        SetForScope<const OperationContext*> whileBeginning(m_operation, &context);
        for (const WideSum& sum : wide.sums)
            requireWholeNumber(phisOfStretch[sum.indexOfParameter], parameters[sum.indexOfParameter], stretchIsWritten);
        wide.splats.shrink(0);
        for (ValueID invariant : wide.invariants)
            wide.splats.append(wide.lanes == WideLanes::Numbers ? vectors.splat(invariantNumber(invariant)) : vectors.splatInteger(m_out.castToInt32(get(invariant).value), lanes));
        wide.reciprocals.shrink(0);
        for (ValueID value : wide.computed) {
            const Operation& division = code().at(value);
            if (wide.lanes != WideLanes::Numbers || division.opcode != Opcode::Div)
                continue;
            if (std::optional<double> reciprocal = exactReciprocalOf(division.rightOperand()))
                wide.reciprocals.append({ division.rightOperand(), vectors.splat(m_out.constDouble(*reciprocal)) });
        }
        auto splatBits = [&](uint64_t bits) {
            LValue half = m_out.constInt64(static_cast<int64_t>(bits));
            return vectors.joinWords(half, half);
        };
        wide.signs = nullptr;
        wide.magnitudes = nullptr;
        for (ValueID value : wide.computed) {
            constexpr uint64_t signOfNumber = uint64_t { 1 } << 63;
            if (!wide.signs && opcodeOf(value) == Opcode::Negate)
                wide.signs = splatBits(signOfNumber);
            if (!wide.magnitudes && opcodeOf(value) == Opcode::CallInline && code().at(value).inlineBuiltin() == InlineBuiltin::MathAbs)
                wide.magnitudes = splatBits(~signOfNumber);
        }
        wide.masks.shrink(0);
        for (ValueID value : wide.computed) {
            std::optional<int64_t> mask = maskOfRemainder(code().at(value));
            if (mask && wide.lanes != WideLanes::Numbers && *mask < (int64_t { 1 } << (8 * bytesOfVector / lanes)) - 1)
                wide.masks.append({ value, vectors.splatInteger(m_out.constInt32(static_cast<int32_t>(*mask)), lanes) });
        }
        if (loop.poll || stretchIsWritten) {
            LValue counter = phisOfStretch[indexOfCounter];
            LValue behindStretch = m_out.add(counter, m_out.constantOfType(counter->type(), stretchIsWritten ? iterationsOfSums : iterationsBetweenPolls));
            endOfStretch = m_out.select(m_out.lessThan(behindStretch, lastStart), behindStretch, lastStart);
        }
        wide.stepsOfLanes.shrink(0);
        for (auto [stepped, step] : wide.steppedWords) {
            for (unsigned group = 0; step && group < groups; ++group) {
                if (wide.stepsOfLanes.containsIf([&](const auto& known) { return known.step == step && known.group == group; }))
                    continue;
                unsigned lanesOfHalf = lanes / 2;
                unsigned bitsOfLane = 64 / lanesOfHalf;
                auto halfAt = [&](unsigned firstLane) {
                    uint64_t half = 0;
                    for (unsigned lane = 0; lane < lanesOfHalf; ++lane)
                        half |= (static_cast<uint64_t>(step * (lanes * group + firstLane + lane)) & ((uint64_t { 1 } << bitsOfLane) - 1)) << lane * bitsOfLane;
                    return m_out.constInt64(static_cast<int64_t>(half));
                };
                wide.stepsOfLanes.append({ step, group, vectors.joinWords(halfAt(0), halfAt(lanesOfHalf)) });
            }
        }
        for (const WideReduction& reduction : wide.reductions)
            m_out.upsilon(vectors.splatInteger(m_out.constInt32(reduction.opcode == Opcode::BitAnd ? -1 : 0), lanes), reduction.vector);
        for (const WideSum& sum : wide.sums)
            m_out.upsilon(vectors.splatInteger(m_out.constInt32(0), lanesOfVector(WideLanes::Words)), sum.vector);
        if (wide.storageOfLists == ArrayStorage::Int32) {
            LValue sign = m_out.constInt64(static_cast<int64_t>(signOfWord));
            signsOfWords = vectors.joinWords(sign, sign);
        }
        pass(phisOfTurns, turns, [&](size_t index) { return phisOfStretch[index]; });
    }

    m_out.appendTo(turns);
    setIndexes(phisOfTurns);
    wide.vectors.fill(nullptr, wide.computed.size());
    wide.splatsOfSteppedWords.fill(nullptr, wide.steppedWords.size());
    Vector<LValue, 4> reduced;
    for (const WideReduction& reduction : wide.reductions)
        reduced.append(reduction.vector);
    Vector<LValue, 2> added;
    added.fill(nullptr, wide.sums.size());
    auto lanesOfSum = [&](const WideSum& sum) { return lanesOfVector(sum.terms == WideSum::Terms::NarrowWords ? WideLanes::Words : WideLanes::Numbers); };
    auto addTerms = [&] {
        for (size_t place = 0; place < wide.sums.size(); ++place) {
            const WideSum& sum = wide.sums[place];
            LValue terms = wideOperand(wide, sum.term);
            if (sum.terms == WideSum::Terms::Words)
                terms = vectors.sumsOfPairs(terms);
            else if (sum.terms == WideSum::Terms::BoxedWords)
                terms = vectors.computeIntegers(Opcode::BitXor, lanesOfSum(sum), terms, signsOfWords);
            added[place] = added[place] ? vectors.computeIntegers(Opcode::Add, lanesOfSum(sum), added[place], terms) : terms;
        }
    };
    auto emitOperations = [&](unsigned group, bool ofStores) {
        for (size_t place = 0; place < wide.computed.size(); ++place) {
            if (storesElement(code().at(wide.computed[place])) == ofStores)
                emitWideOperation(wide, place, group);
        }
    };
    if (storesAtEnd) {
        Vector<Vector<LValue, 16>, 4> vectorsOfGroups;
        LValue sum = nullptr;
        for (unsigned group = 0; group < groups; ++group) {
            emitOperations(group, false);
            for (ValueID tested : wide.numbersTested)
                sum = sum ? vectors.compute(VectorOutput::Arithmetic::Add, sum, wideOperand(wide, tested)) : wideOperand(wide, tested);
            vectorsOfGroups.append(wide.vectors);
        }
        SetForScope<const OperationContext*> whileTesting(m_operation, &context);
        LBasicBlock noneIsNaN = m_out.newBlock();
        LBasicBlock someIsNaN = m_out.newBlock();
        LValue sumOfLanes = m_out.doubleAdd(vectors.lane(sum, 0), vectors.lane(sum, 1));
        m_out.branch(m_out.doubleEqual(sumOfLanes, sumOfLanes), normalEdge(noneIsNaN), unlikelyEdge(someIsNaN));
        m_out.appendTo(someIsNaN);
        pass(phisOfWrittenHead, writtenHead, [&](size_t index) { return phisOfTurns[index]; });
        m_out.appendTo(noneIsNaN);
        for (unsigned group = 0; group < groups; ++group) {
            wide.vectors = vectorsOfGroups[group];
            emitOperations(group, true);
        }
    } else {
        for (unsigned group = 0; group < groups; ++group) {
            for (size_t place = 0; place < wide.computed.size(); ++place)
                emitWideOperation(wide, place, group);
            for (size_t place = 0; place < wide.reductions.size(); ++place)
                reduced[place] = vectors.computeIntegers(wide.reductions[place].opcode, lanes, reduced[place], wideOperand(wide, wide.reductions[place].term));
            addTerms();
        }
        for (size_t place = 0; place < wide.sums.size(); ++place)
            added[place] = vectors.computeIntegers(Opcode::Add, lanesOfSum(wide.sums[place]), wide.sums[place].vector, added[place]);
    }
    m_out.setOrigin(Machine::Origin());
    Values valuesBehind;
    for (size_t index = 0; index < parameters.size(); ++index) {
        auto stepped = std::ranges::find(wide.steps, index, &std::pair<size_t, int64_t>::first);
        valuesBehind.append(phisOfTurns[index] ? m_out.add(phisOfTurns[index], m_out.constantOfType(phisOfTurns[index]->type(), stepped->second * iterations)) : nullptr);
    }
    {
        SetForScope<const OperationContext*> whileEnding(m_operation, &context);
        LValue counterBehind = valuesBehind[indexOfCounter];
        LBasicBlock again = m_out.newBlock();
        LBasicBlock stretchHasEnded = m_out.newBlock();
        m_out.branch(askCounter(loop.relation, counterBehind, endOfStretch), normalEdge(again), unlikelyEdge(stretchHasEnded));
        m_out.appendTo(again);
        for (size_t place = 0; place < wide.reductions.size(); ++place)
            m_out.upsilon(reduced[place], wide.reductions[place].vector);
        for (size_t place = 0; place < wide.sums.size(); ++place)
            m_out.upsilon(added[place], wide.sums[place].vector);
        pass(phisOfTurns, turns, [&](size_t index) { return valuesBehind[index]; });

        m_out.appendTo(stretchHasEnded);
        Values values = valuesBehind;
        for (size_t place = 0; place < wide.reductions.size(); ++place) {
            size_t index = wide.reductions[place].indexOfParameter;
            LValue sum = vectors.reduceWords(wide.reductions[place].opcode, reduced[place]);
            LValue before = phisOfStretch[index];
            switch (wide.reductions[place].opcode) {
            case Opcode::BitAnd:
                values[index] = m_out.bitAnd(before, sum);
                break;
            case Opcode::BitOr:
                values[index] = m_out.bitOr(before, sum);
                break;
            case Opcode::BitXor:
                values[index] = m_out.bitXor(before, sum);
                break;
            default:
                values[index] = m_out.add(before, sum);
            }
        }
        for (size_t place = 0; place < wide.sums.size(); ++place) {
            const WideSum& sum = wide.sums[place];
            LValue total = nullptr;
            if (sum.terms == WideSum::Terms::NarrowWords)
                total = m_out.signExt32To64(vectors.reduceWords(Opcode::Add, added[place]));
            else {
                LValue first = m_out.bitCast(vectors.lane(added[place], 0), Machine::Int64);
                LValue second = m_out.bitCast(vectors.lane(added[place], 1), Machine::Int64);
                LValue iterationsOfStretch = m_out.sub(wideValue(counterBehind), wideValue(phisOfStretch[indexOfCounter]));
                if (sum.terms == WideSum::Terms::BoxedWords) {
                    static_assert(JSValue::NumberTag == static_cast<int64_t>(-(uint64_t { 2 } << bitsOfSum)));
                    LValue tags = m_out.shl(iterationsOfStretch, bitsOfSum);
                    first = m_out.add(first, tags);
                    second = m_out.add(second, tags);
                    LBasicBlock noneIsHole = m_out.newBlock();
                    m_out.branch(m_out.below(m_out.bitOr(first, second), m_out.constInt64(int64_t { 1 } << bitsOfSum)), normalEdge(noneIsHole), unlikelyEdge(stretchIsWritten));
                    m_out.appendTo(noneIsHole);
                }
                total = m_out.add(first, second);
                if (sum.terms == WideSum::Terms::BoxedWords)
                    total = m_out.sub(total, m_out.shl(iterationsOfStretch, bitsOfWord - 1));
            }
            values[sum.indexOfParameter] = m_out.doubleAdd(phisOfStretch[sum.indexOfParameter], m_out.intToDouble(total));
        }
        LBasicBlock ended = m_out.newBlock();
        if (loop.poll || stretchIsWritten) {
            LBasicBlock goesOn = m_out.newBlock();
            m_out.branch(askCounter(loop.relation, counterBehind, lastStart), normalEdge(goesOn), normalEdge(ended));
            m_out.appendTo(goesOn);
            if (loop.poll) {
                generateValue(code().blockOf(loop.poll), loop.poll, code().at(loop.poll), false, { });
                m_out.setOrigin(Machine::Origin());
            }
            pass(phisOfStretch, stretch, [&](size_t index) { return values[index]; });
        } else
            m_out.jump(normalEdge(ended));
        m_out.appendTo(ended);
        pass(phisOfWrittenHead, writtenHead, [&](size_t index) { return values[index]; });
        if (stretchIsWritten) {
            m_out.appendTo(stretchIsWritten);
            pass(phisOfWrittenHead, writtenHead, [&](size_t index) { return phisOfStretch[index]; });
        }
    }

    forgetValuesOfTurn();
    m_blocks[head.index()] = writtenHead;
    for (size_t index = 0; index < parameters.size(); ++index)
        m_values[parameters[index].index()] = phisOfWrittenHead[index];
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
