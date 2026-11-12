/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAccessEmitter.h"

#if ENABLE(AOT)

#include "AOTArrayCallbacks.h"
#include "AOTBodyIndex.h"
#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTCallEmitter.h"
#include "AOTConstantEmitter.h"
#include "AOTConversionEmitter.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTNameClass.h"
#include "AOTOpcodeSets.h"
#include "AOTStubsArrayCallbacks.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class CallbackReader {
public:
    CallbackReader(const Program& program, const Function& callback, const BodyIndex& index)
        : m_program(program)
        , m_callback(callback)
        , m_index(index)
    {
    }

    struct Test {
        ElementTest test { ElementTest::IsTrue };
        bool isNegated { false };
        bool returnsBoolean { true };
        ValueID read;
        ValueID compared;
        uint32_t types { 0 };
    };
    struct Fold {
        NumberFold fold { NumberFold::Sum };
        ValueID read;
    };
    struct FieldPair {
        FieldPairOperation operation { FieldPairOperation::Sum };
        ValueID first;
        ValueID second;
    };

    struct Map {
        NumberMap map { NumberMap::Sum };
        bool isNegated { false };
        ValueID read;
        ValueID operand;
    };

    std::optional<Test> findTest();
    std::optional<Fold> findFold();
    ValueID findFieldRead();
    std::optional<std::pair<FieldMap, ValueID>> findLength();
    std::optional<Map> findMap();
    std::optional<FieldPair> findMappedPair();
    std::optional<FieldPair> findFoldedPair();
    std::optional<FieldPair> findFieldOfField();
    struct TestedPair {
        FieldPairTest test { FieldPairTest::Equals };
        bool isNegated { false };
        FieldPair pair;
    };
    std::optional<TestedPair> findTestedPair();

private:
    static constexpr OpcodeSet constantsOfCallbacks { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };

    bool isOrdinaryFunction(unsigned numberOfParameters) const;
    BodyBlock entry() const { return m_index.block(entryBlock); }
    ValueID parameter(unsigned position) const { return entry().parameter(m_callback.firstParameterIndex() + position); }
    ValueID stripped(ValueID value) const { return value ? m_index.stripCasts(value) : value; }
    bool isOperation(ValueID value, Opcode opcode) const { return value && !m_index.isParameter(value) && m_index.opcodeOf(value) == opcode; }
    ValueID operandOf(ValueID value, unsigned operand) const { return stripped(m_index.value(value).operand(operand)); }
    bool isFieldOf(ValueID value, ValueID element) const;
    bool isConstantOrCapture(ValueID) const;
    bool isFieldOfCapture(ValueID) const;
    std::optional<FieldPair> fieldPairOf(ValueID, ValueID element) const;
    std::optional<FieldPair> fieldOfFieldOf(ValueID, ValueID element) const;
    ValueID resultOfOnlyBlock() const;
    std::optional<std::pair<ValueID, ValueID>> resultsOfArms(ValueID& condition) const;
    bool doesNothingElse(std::span<const ValueID> operations, bool mayNameBuiltins) const;

    const Program& m_program;
    const Function& m_callback;
    const BodyIndex& m_index;
};

} // anonymous namespace

bool CallbackReader::isOrdinaryFunction(unsigned numberOfParameters) const
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee, FunctionFlags::HasThis };
    return m_callback.hasBody() && !m_callback.hasAny(otherForms) && !isClassConstructorOf(m_callback.kind) && m_callback.numberOfPositionalParameters() >= numberOfParameters
        && !entry().hasHandler() && m_callback.firstParameterIndex() + numberOfParameters <= entry().numberOfParameters();
}

bool CallbackReader::isFieldOf(ValueID value, ValueID element) const
{
    if (!isOperation(value, Opcode::PropGet))
        return false;
    BodyValue read = m_index.value(value);
    if (read.isFreeName() || !read.key() || stripped(read.objectOperand()) != element)
        return false;
    return m_program.at(read.key()).kind == PropertyKeyKind::Text && m_program.nameClassOf(read.key()) == NameClass::Ordinary && !m_program.isIndexLike(read.key());
}

bool CallbackReader::isConstantOrCapture(ValueID value) const
{
    if (!value)
        return false;
    if (!m_index.isParameter(value))
        return constantsOfCallbacks.contains(m_index.opcodeOf(value));
    for (uint32_t place = 0; place < m_callback.numberOfEntryParameters() && place < entry().numberOfParameters(); ++place) {
        if (entry().parameter(place) == value)
            return m_callback.entryParameterKind(place) == EntryParameter::Capture;
    }
    return false;
}

bool CallbackReader::isFieldOfCapture(ValueID value) const
{
    if (!isOperation(value, Opcode::PropGet))
        return false;
    ValueID object = stripped(m_index.value(value).objectOperand());
    return object && m_index.isParameter(object) && isConstantOrCapture(object) && isFieldOf(value, object);
}

static std::optional<uint32_t> elementTypesOf(TypeID tested)
{
    auto typesOfPrimitives = [](TypeID type) -> std::optional<uint32_t> {
        static constexpr std::pair<uint16_t, uint32_t> parts[] {
            { Primitive::Undefined, ElementTypes::encoded(JSValue::ValueUndefined) },
            { Primitive::Null, ElementTypes::encoded(JSValue::ValueNull) },
            { Primitive::False, ElementTypes::encoded(JSValue::ValueFalse) },
            { Primitive::True, ElementTypes::encoded(JSValue::ValueTrue) },
            { Primitive::Number, ElementTypes::number },
            { Primitive::String, ElementTypes::cell(StringType) },
            { Primitive::Symbol, ElementTypes::cell(SymbolType) },
            { Primitive::BigInt, ElementTypes::cell(HeapBigIntType) },
        };
        if (!holdsOnlyPrimitives(type))
            return std::nullopt;
        uint16_t primitives = 0;
        uint32_t types = 0;
        for (auto [primitive, bits] : parts) {
            if ((primitivesInID(type) & primitive) == primitive) {
                primitives |= primitive;
                types |= bits;
            }
        }
        return primitives && TypeArena::fromPrimitives(primitives) == type ? std::optional(types) : std::nullopt;
    };
    if (tested == TypeTable::object)
        return ElementTypes::object;
    for (TypeID rejected : { TypeTable::undefined, TypeTable::null, TypeTable::nullish, TypeTable::string, TypeTable::number, TypeTable::boolean }) {
        if (tested == TypeTable::anythingBut(rejected))
            return (*typesOfPrimitives(TypeArena::fromPrimitives(Primitive::All)) | ElementTypes::object) & ~*typesOfPrimitives(rejected);
    }
    return typesOfPrimitives(tested);
}

ValueID CallbackReader::resultOfOnlyBlock() const
{
    TerminatorKind end = entry().terminator().kind();
    if (m_index.numberOfBlocks() != 1 || (end != TerminatorKind::Return && end != TerminatorKind::ReturnCall))
        return { };
    return stripped(entry().terminator().operand(0));
}

std::optional<std::pair<ValueID, ValueID>> CallbackReader::resultsOfArms(ValueID& condition) const
{
    BodyTerminator branch = entry().terminator();
    if (m_index.numberOfBlocks() != 4 || branch.kind() != TerminatorKind::Branch)
        return std::nullopt;
    condition = stripped(branch.as<TerminatorKind::Branch>().conditionOperand());
    std::array<ValueID, 2> results;
    BlockID meeting;
    for (unsigned arm = 0; arm < 2; ++arm) {
        BodyTarget target = branch.targets()[arm];
        BodyBlock block = m_index.block(target.block());
        if (!target.arguments().empty() || block.hasHandler() || block.numberOfValues() || block.terminator().kind() != TerminatorKind::Jump)
            return std::nullopt;
        BodyTarget next = block.terminator().next();
        if (next.arguments().size() != 1 || (arm && next.block() != meeting))
            return std::nullopt;
        meeting = next.block();
        ValueID result = stripped(next.arguments()[0]);
        while (result && m_index.isParameter(result) && !m_index.isInBlock(result, entryBlock) && m_index.valueNarrowedInto(result))
            result = stripped(m_index.valueNarrowedInto(result));
        results[arm] = result;
    }
    BodyBlock last = m_index.block(meeting);
    if (last.hasHandler() || last.numberOfValues() || last.numberOfParameters() != 1 || last.terminator().kind() != TerminatorKind::Return || last.terminator().operand(0) != last.parameter(0))
        return std::nullopt;
    return std::pair { results[0], results[1] };
}

bool CallbackReader::doesNothingElse(std::span<const ValueID> operations, bool mayNameBuiltins) const
{
    for (BodyValue value : entry().values()) {
        bool isPart = std::ranges::find(operations, value.id()) != operations.end() || constantsOfCallbacks.contains(value.opcode()) || opcodesThatStateTypes.contains(value.opcode());
        if (!isPart && !(mayNameBuiltins && value.opcode() == Opcode::Builtin))
            return false;
    }
    return true;
}

std::optional<CallbackReader::Test> CallbackReader::findTest()
{
    ValueID tested = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!tested || (entry().terminator().kind() != TerminatorKind::Return && !isOperation(tested, Opcode::Call)))
        return std::nullopt;
    ValueID element = parameter(0);
    Vector<ValueID, 8> operations;
    Test test;
    while (isOperation(tested, Opcode::LogicalNot) || isOperation(tested, Opcode::ToBoolean)) {
        test.isNegated ^= m_index.opcodeOf(tested) == Opcode::LogicalNot;
        operations.append(tested);
        tested = operandOf(tested, 0);
    }
    if (!tested)
        return std::nullopt;
    auto isTested = [&](ValueID value) {
        return value == element || isFieldOf(value, element);
    };
    test.returnsBoolean = !operations.isEmpty();
    Opcode opcode = m_index.isParameter(tested) ? Opcode::Undefined : m_index.opcodeOf(tested);
    bool isRelation = opcode == Opcode::Less || opcode == Opcode::LessEq || opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
    if (opcode == Opcode::StrictEq || isRelation) {
        ValueID comparison = tested;
        operations.append(comparison);
        bool isOnLeft = isTested(operandOf(comparison, 0));
        tested = operandOf(comparison, isOnLeft ? 0 : 1);
        test.compared = operandOf(comparison, isOnLeft ? 1 : 0);
        if (isFieldOfCapture(test.compared))
            operations.append(test.compared);
        else if (!isConstantOrCapture(test.compared))
            return std::nullopt;
        if (isRelation) {
            bool isLess = isOnLeft ? opcode == Opcode::Less || opcode == Opcode::LessEq : opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
            bool takesEqual = opcode == Opcode::LessEq || opcode == Opcode::GreaterEq;
            test.test = isLess == takesEqual ? ElementTest::IsLessOrEqual : ElementTest::IsLess;
            test.isNegated ^= !isLess;
        } else
            test.test = ElementTest::Equals;
    } else if (opcode == Opcode::TypeTest) {
        auto typeTest = m_index.value(tested).as<Opcode::TypeTest>();
        std::optional<uint32_t> types = elementTypesOf(typeTest.testedType());
        if (!types)
            return std::nullopt;
        operations.append(tested);
        test.test = ElementTest::HasType;
        test.types = *types;
        tested = stripped(typeTest.valueOperand());
    } else if (opcode == Opcode::Call) {
        auto call = m_index.value(tested).as<Opcode::Call>();
        ValueID callee = stripped(call.calleeOperand());
        ValueID receiver = call.thisValueOperand() ? stripped(call.thisValueOperand()) : ValueID();
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (m_program.admitsReplacedBuiltins || !receiver || arguments.size() != 1 || !isOperation(callee, Opcode::PropGet) || m_index.value(callee).isFreeName())
            return std::nullopt;
        PropertyKeyID name = m_program.keyOf(BuiltinNames::startsWith);
        if (!name || m_index.value(callee).key() != name || stripped(m_index.value(callee).objectOperand()) != receiver)
            return std::nullopt;
        operations.appendList({ tested, callee });
        test.test = ElementTest::StartsWith;
        test.compared = stripped(arguments[0]);
        if (!isConstantOrCapture(test.compared))
            return std::nullopt;
        tested = receiver;
    }
    if (!isTested(tested))
        return std::nullopt;
    test.returnsBoolean |= test.test != ElementTest::IsTrue;
    if (tested != element) {
        test.read = tested;
        operations.append(tested);
    }
    return doesNothingElse(operations.span(), false) ? std::optional(test) : std::nullopt;
}

std::optional<CallbackReader::Fold> CallbackReader::findFold()
{
    if (!isOrdinaryFunction(2))
        return std::nullopt;
    const BuiltinTable& table = m_program.builtins();
    ValueID accumulator = parameter(0);
    ValueID element = parameter(1);
    Fold fold;
    ValueID condition;
    if (auto results = resultsOfArms(condition)) {
        Opcode opcode = condition && !m_index.isParameter(condition) ? m_index.opcodeOf(condition) : Opcode::Undefined;
        bool isGreater = opcode == Opcode::Greater || opcode == Opcode::GreaterEq;
        if (!isGreater && opcode != Opcode::Less && opcode != Opcode::LessEq)
            return std::nullopt;
        std::pair operands { operandOf(condition, 0), operandOf(condition, 1) };
        std::pair reversed { operands.second, operands.first };
        bool takesLeft = *results == operands;
        if ((!takesLeft && *results != reversed) || (operands != std::pair { accumulator, element } && operands != std::pair { element, accumulator }))
            return std::nullopt;
        fold.fold = isGreater == takesLeft ? NumberFold::Greatest : NumberFold::Least;
        ValueID operations[] { condition };
        return doesNothingElse(operations, false) ? std::optional(fold) : std::nullopt;
    }
    ValueID result = resultOfOnlyBlock();
    if (!result || m_index.isParameter(result))
        return std::nullopt;
    BodyValue made = m_index.value(result);
    std::array<ValueID, 2> operands;
    if (made.opcode() == Opcode::Add)
        operands = { stripped(made.operand(0)), stripped(made.operand(1)) };
    else if (made.opcode() == Opcode::Call && !m_program.admitsReplacedBuiltins) {
        ValueID callee = stripped(made.as<Opcode::Call>().calleeOperand());
        std::span<const ValueID> arguments = made.as<Opcode::Call>().argumentsOperands();
        if (arguments.size() != 2 || !isOperation(callee, Opcode::Builtin))
            return std::nullopt;
        BuiltinID function = table.canonicalID(m_index.value(callee).as<Opcode::Builtin>().builtin());
        if (function != table.canonicalID(Builtins::MathMax) && function != table.canonicalID(Builtins::MathMin))
            return std::nullopt;
        fold.fold = function == table.canonicalID(Builtins::MathMax) ? NumberFold::Greatest : NumberFold::Least;
        operands = { stripped(arguments[0]), stripped(arguments[1]) };
    } else
        return std::nullopt;
    if (operands[0] != accumulator)
        std::swap(operands[0], operands[1]);
    if (operands[0] != accumulator || (operands[1] != element && !isFieldOf(operands[1], element)))
        return std::nullopt;
    if (operands[1] != element)
        fold.read = operands[1];
    ValueID operations[] { result, operands[1] };
    return doesNothingElse(operations, made.opcode() == Opcode::Call) ? std::optional(fold) : std::nullopt;
}

ValueID CallbackReader::findFieldRead()
{
    ValueID read = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!read || !isFieldOf(read, parameter(0)))
        return { };
    ValueID operations[] { read };
    return doesNothingElse(operations, false) ? read : ValueID();
}

std::optional<std::pair<FieldMap, ValueID>> CallbackReader::findLength()
{
    ValueID result = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!result || !isOperation(result, Opcode::PropGet))
        return std::nullopt;
    BodyValue length = m_index.value(result);
    PropertyKeyID key = m_program.keyOf(BuiltinNames::length);
    if (!key || length.isFreeName() || length.key() != key)
        return std::nullopt;
    ValueID element = parameter(0);
    ValueID measured = stripped(length.objectOperand());
    if (measured != element && !isFieldOf(measured, element))
        return std::nullopt;
    ValueID operations[] { result, measured };
    if (!doesNothingElse(operations, false))
        return std::nullopt;
    return measured == element ? std::pair { FieldMap::LengthOfElement, ValueID() } : std::pair { FieldMap::LengthOfField, measured };
}

std::optional<CallbackReader::Map> CallbackReader::findMap()
{
    ValueID result = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!result || m_index.isParameter(result) || entry().terminator().kind() != TerminatorKind::Return)
        return std::nullopt;
    Opcode opcode = m_index.opcodeOf(result);
    if (opcode != Opcode::Add && opcode != Opcode::Sub && opcode != Opcode::Mul && opcode != Opcode::Div)
        return std::nullopt;
    ValueID element = parameter(0);
    std::array operands { operandOf(result, 0), operandOf(result, 1) };
    Map map { opcode == Opcode::Div ? NumberMap::Quotient : opcode == Opcode::Mul ? NumberMap::Product : NumberMap::Sum, opcode == Opcode::Sub, { }, { } };
    if (!map.isNegated && opcode != Opcode::Div && isConstantOrCapture(operands[0]))
        std::swap(operands[0], operands[1]);
    if (!isConstantOrCapture(operands[1]) || (operands[0] != element && !isFieldOf(operands[0], element)))
        return std::nullopt;
    map.operand = operands[1];
    if (map.isNegated && (!isOperation(map.operand, Opcode::Integer) || !m_index.value(map.operand).integer()))
        return std::nullopt;
    if (operands[0] != element)
        map.read = operands[0];
    ValueID operations[] { result, operands[0] };
    return doesNothingElse(operations, false) ? std::optional(map) : std::nullopt;
}

std::optional<CallbackReader::FieldPair> CallbackReader::fieldPairOf(ValueID value, ValueID element) const
{
    if (!value || m_index.isParameter(value))
        return std::nullopt;
    FieldPair pair;
    switch (m_index.opcodeOf(value)) {
    case Opcode::Add:
        break;
    case Opcode::Sub:
        pair.operation = FieldPairOperation::Difference;
        break;
    case Opcode::Mul:
        pair.operation = FieldPairOperation::Product;
        break;
    default:
        return std::nullopt;
    }
    pair.first = operandOf(value, 0);
    pair.second = operandOf(value, 1);
    return pair.first != pair.second && isFieldOf(pair.first, element) && isFieldOf(pair.second, element) ? std::optional(pair) : std::nullopt;
}

std::optional<CallbackReader::FieldPair> CallbackReader::fieldOfFieldOf(ValueID value, ValueID element) const
{
    if (!isOperation(value, Opcode::PropGet) || m_index.value(value).isFreeName())
        return std::nullopt;
    ValueID first = stripped(m_index.value(value).objectOperand());
    return isFieldOf(first, element) && isFieldOf(value, first) ? std::optional(FieldPair { FieldPairOperation::FieldOfField, first, value }) : std::nullopt;
}

std::optional<CallbackReader::FieldPair> CallbackReader::findFieldOfField()
{
    ValueID result = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!result)
        return std::nullopt;
    std::optional<FieldPair> pair = fieldOfFieldOf(result, parameter(0));
    if (!pair)
        return std::nullopt;
    ValueID operations[] { pair->first, pair->second };
    return doesNothingElse(operations, false) ? pair : std::nullopt;
}

std::optional<CallbackReader::TestedPair> CallbackReader::findTestedPair()
{
    ValueID tested = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!tested || entry().terminator().kind() != TerminatorKind::Return)
        return std::nullopt;
    Vector<ValueID, 8> operations;
    TestedPair result;
    while (isOperation(tested, Opcode::LogicalNot) || isOperation(tested, Opcode::ToBoolean)) {
        result.isNegated ^= m_index.opcodeOf(tested) == Opcode::LogicalNot;
        operations.append(tested);
        tested = operandOf(tested, 0);
    }
    if (!tested || m_index.isParameter(tested))
        return std::nullopt;
    bool isReversed = false;
    switch (m_index.opcodeOf(tested)) {
    case Opcode::StrictEq:
        break;
    case Opcode::Greater:
        isReversed = true;
        [[fallthrough]];
    case Opcode::Less:
        result.test = FieldPairTest::IsLess;
        break;
    case Opcode::GreaterEq:
        isReversed = true;
        [[fallthrough]];
    case Opcode::LessEq:
        result.test = FieldPairTest::IsLessOrEqual;
        break;
    default:
        return std::nullopt;
    }
    ValueID element = parameter(0);
    result.pair.first = operandOf(tested, isReversed);
    result.pair.second = operandOf(tested, !isReversed);
    if (result.pair.first == result.pair.second || !isFieldOf(result.pair.first, element) || !isFieldOf(result.pair.second, element))
        return std::nullopt;
    operations.appendList({ tested, result.pair.first, result.pair.second });
    return doesNothingElse(operations.span(), false) ? std::optional(result) : std::nullopt;
}

std::optional<CallbackReader::FieldPair> CallbackReader::findMappedPair()
{
    ValueID result = isOrdinaryFunction(1) ? resultOfOnlyBlock() : ValueID();
    if (!result || entry().terminator().kind() != TerminatorKind::Return)
        return std::nullopt;
    std::optional<FieldPair> pair = fieldPairOf(result, parameter(0));
    if (!pair)
        return std::nullopt;
    ValueID operations[] { result, pair->first, pair->second };
    return doesNothingElse(operations, false) ? pair : std::nullopt;
}

std::optional<CallbackReader::FieldPair> CallbackReader::findFoldedPair()
{
    ValueID result = isOrdinaryFunction(2) ? resultOfOnlyBlock() : ValueID();
    if (!isOperation(result, Opcode::Add) || operandOf(result, 0) != parameter(0))
        return std::nullopt;
    ValueID addend = operandOf(result, 1);
    std::optional<FieldPair> pair = fieldPairOf(addend, parameter(1));
    if (!pair)
        pair = fieldOfFieldOf(addend, parameter(1));
    if (!pair)
        return std::nullopt;
    ValueID operations[] { result, addend, pair->first, pair->second };
    return doesNothingElse(operations, false) ? pair : std::nullopt;
}

bool AccessEmitter::canUseCallbackStubs(const ValueContext& call) const
{
    bool isRare = m_code.at(call.block()).frequency == BlockFrequency::Rare && call->opcode != Opcode::CallDirect;
    return !isRare && !m_program.admitsInspector && (m_generator.isInLoop(call.block()) || m_facts.linked().functionsCalledInLoopsOfOwnFile.get(m_unit.function.index()));
}

const Operation* AccessEmitter::findClosurePassed(ValueID argument) const
{
    while (operationAt(argument).opcode == Opcode::Box || operationAt(argument).opcode == Opcode::Upcast)
        argument = operationAt(argument).operand(0);
    const Operation& made = operationAt(argument);
    return made.opcode == Opcode::NewClosure ? &made : nullptr;
}

std::optional<uint32_t> AccessEmitter::siteOfCallback(FunctionID callback, const BodyIndex& index, ValueID readByCallback)
{
    BodyValue read = index.value(readByCallback);
    PropertyKeyID key = read.key();
    Wire::SourceLocation place = m_program.at(callback).locationAt(read.position());
    if (!hasModule(place) || liesInThread(key) || namesSizeOfTables(m_program, m_facts, key))
        return std::nullopt;
    Vector<uint32_t, 12> places { place.module, place.position };
    return m_mentions.siteCache(WTF::move(places), key, MentionList::firstUseOfSiteAnswers + static_cast<uint32_t>(SiteUse::Read));
}

LValue AccessEmitter::distanceOfCallbackSite(FunctionID callback, const BodyIndex& index, ValueID readByCallback, CallbackSource& source)
{
    source = CallbackSource::Element;
    if (!readByCallback)
        return m_out.int64Zero;
    std::optional<uint32_t> site = siteOfCallback(callback, index, readByCallback);
    if (!site)
        return nullptr;
    source = CallbackSource::Field;
    return zeroExtended(numberOf(*site)).value();
}

LValue AccessEmitter::valueUsedByCallback(const ValueContext& call, const Operation& closure, const Function& callback, const BodyIndex& index, ValueID compared)
{
    if (index.isParameter(compared)) {
        std::span<const ValueID> captures = closure.capturesOperands();
        BodyBlock entry = index.block(entryBlock);
        uint32_t firstCapture = callback.numberOfEntryParametersBefore(EntryParameter::Capture);
        for (uint32_t capture = 0; capture < captures.size(); ++capture) {
            if (entry.parameter(firstCapture + capture) == compared)
                return emitter<ConversionEmitter>().box(call, get(captures[capture]), typeOf(captures[capture])).value();
        }
        return nullptr;
    }
    BodyValue constant = index.value(compared);
    switch (constant.opcode()) {
    case Opcode::PropGet: {
        std::optional<uint32_t> site = siteOfCallback(closure.function(), index, compared);
        LValue object = site ? valueUsedByCallback(call, closure, callback, index, index.stripCasts(constant.objectOperand())) : nullptr;
        return object ? callStub(call, readAtSiteRunningNoCode(ReceiverAtSite::IsAnyValue, true), { object }, { .numberedImmediate = NumberedImmediate { *site } }).first : nullptr;
    }
    case Opcode::Undefined:
        return boxedConstant(jsUndefined()).value();
    case Opcode::Null:
        return boxedConstant(jsNull()).value();
    case Opcode::Boolean:
        return boxedConstant(jsBoolean(constant.as<Opcode::Boolean>().boolean())).value();
    case Opcode::Integer:
        return boxedConstant(jsNumber(static_cast<double>(constant.integer()))).value();
    case Opcode::Double:
        return boxedConstant(jsNumber(purifyNaN(constant.number()))).value();
    case Opcode::String:
        return emitter<ConstantEmitter>().stringConstant(call, constant.string()).value();
    default:
        return nullptr;
    }
}

std::optional<StubKey> AccessEmitter::chooseCallbackStub(const ValueContext& call, const Operation& closure, CompiledMethodOfArray method, uint32_t flags, Vector<LValue, 8>& arguments)
{
    bool searches = method == CompiledMethodOfArray::Some || method == CompiledMethodOfArray::Every || method == CompiledMethodOfArray::Find || method == CompiledMethodOfArray::FindIndex;
    bool folds = method == CompiledMethodOfArray::Reduce;
    bool filters = method == CompiledMethodOfArray::Filter;
    bool maps = method == CompiledMethodOfArray::Map;
    const BodyIndex* index = m_facts.bodies().find(closure.function());
    if (!index || (!searches && !folds && !filters && !maps) || arguments.size() > (folds ? 2 : 1))
        return std::nullopt;
    const Function& callback = m_program.at(closure.function());
    CallbackReader reader(m_program, callback, *index);
    CallbackSource source;
    LValue site = nullptr;
    LValue operand = nullptr;
    StubKey stub;
    auto takeOperandOfTest = [&](const CallbackReader::Test& test) {
        if (test.compared)
            operand = valueUsedByCallback(call, closure, callback, *index, test.compared);
        else if (test.test == ElementTest::HasType)
            operand = boxedConstant(jsNumber(static_cast<int32_t>(test.types))).value();
        return operand || !test.compared;
    };
    auto distanceOfSites = [&](const CallbackReader::FieldPair& pair) -> LValue {
        LValue first = distanceOfCallbackSite(closure.function(), *index, pair.first, source);
        LValue second = first ? distanceOfCallbackSite(closure.function(), *index, pair.second, source) : nullptr;
        return second ? m_out.bitOr(first, m_out.shl(second, 32)) : nullptr;
    };
    if (auto pair = folds && arguments.size() == 2 ? reader.findFoldedPair() : std::nullopt) {
        site = distanceOfSites(*pair);
        operand = arguments.takeLast();
        stub = foldFieldPairs(pair->operation);
    } else if (folds) {
        std::optional<CallbackReader::Fold> fold = reader.findFold();
        if (!fold || (fold->read && arguments.size() == 1))
            return std::nullopt;
        site = distanceOfCallbackSite(closure.function(), *index, fold->read, source);
        if (arguments.size() == 2)
            operand = arguments.takeLast();
        else
            flags |= ArrayCallbackFlags::hasNoInitialValue;
        stub = foldArray(source, fold->fold);
    } else if (maps) {
        if (auto length = reader.findLength()) {
            site = distanceOfCallbackSite(closure.function(), *index, length->second, source);
            stub = mapArrayToField(length->first);
        } else if (ValueID read = reader.findFieldRead()) {
            site = distanceOfCallbackSite(closure.function(), *index, read, source);
            stub = mapArrayToField(FieldMap::Field);
        } else if (auto pair = reader.findMappedPair()) {
            site = distanceOfSites(*pair);
            stub = mapFieldPairs(pair->operation);
        } else if (auto pair = reader.findFieldOfField()) {
            site = distanceOfSites(*pair);
            stub = mapArrayToField(FieldMap::FieldOfField);
        } else if (auto test = reader.findTest(); test && test->returnsBoolean) {
            site = distanceOfCallbackSite(closure.function(), *index, test->read, source);
            if (!takeOperandOfTest(*test))
                return std::nullopt;
            if (test->isNegated)
                flags |= ArrayCallbackFlags::testIsNegated;
            stub = mapArrayToTest(source, test->test);
        } else {
            std::optional<CallbackReader::Map> map = reader.findMap();
            if (!map)
                return std::nullopt;
            site = distanceOfCallbackSite(closure.function(), *index, map->read, source);
            if (map->isNegated)
                operand = boxedConstant(jsNumber(-static_cast<double>(index->value(map->operand).integer()))).value();
            else
                operand = valueUsedByCallback(call, closure, callback, *index, map->operand);
            if (!operand)
                return std::nullopt;
            stub = mapArrayToNumber(source, map->map);
        }
    } else if (auto tested = reader.findTestedPair()) {
        site = distanceOfSites(tested->pair);
        if (tested->isNegated != (method == CompiledMethodOfArray::Every))
            flags |= ArrayCallbackFlags::testIsNegated;
        stub = filters ? filterFieldPairs(tested->test) : searchFieldPairs(tested->test);
    } else {
        std::optional<CallbackReader::Test> test = reader.findTest();
        if (!test)
            return std::nullopt;
        site = distanceOfCallbackSite(closure.function(), *index, test->read, source);
        if (!takeOperandOfTest(*test))
            return std::nullopt;
        if (test->isNegated != (method == CompiledMethodOfArray::Every))
            flags |= ArrayCallbackFlags::testIsNegated;
        stub = filters ? filterArray(source, test->test) : searchArray(source, test->test);
    }
    if (!site) {
        if (folds && operand)
            arguments.append(operand);
        return std::nullopt;
    }
    arguments.append(site);
    arguments.append(m_out.constInt64(ArrayCallbackFlags::encode(method, flags)));
    if (operand)
        arguments.append(operand);
    build().statistics().count(StatisticID::ArrayCallbacksRunByStubs);
    return stub;
}

std::optional<StubKey> AccessEmitter::tryCallbackStub(const ValueContext& call, CompiledMethodOfArray method, uint8_t lentArguments, Vector<LValue, 8>& arguments)
{
    if (arguments.isEmpty() || (lentArguments && lentArguments != onlyFirstArgumentIsLent) || !canUseCallbackStubs(call))
        return std::nullopt;
    const Operation* closure = findClosurePassed(call->argumentsOperands()[0]);
    return closure ? chooseCallbackStub(call, *closure, method, lentArguments ? ArrayCallbackFlags::callbackIsLent : 0, arguments) : std::nullopt;
}

std::optional<CompiledMethodOfArray> AccessEmitter::methodOfArrayNamed(PropertyKeyID key) const
{
    const BuiltinTable& table = m_program.builtins();
    BuiltinName name = isSymbolOfRealm(key) || liesInThread(key) ? BuiltinName() : m_program.builtinNameOf(key);
    BuiltinID member = name ? table.findOwnMember(table.canonicalID(Builtins::ArrayPrototype), name) : BuiltinID();
    return member ? compiledMethodOfArray(table.canonicalID(member)) : std::nullopt;
}

std::optional<StubKey> AccessEmitter::tryCallbackStubOfAnyReceiver(const ValueContext& call, PropertyKeyID key, uint8_t lentArguments, Vector<LValue, 8>& arguments)
{
    std::span<const ValueID> passed = call->argumentsOperands();
    if (call->methodOfReceiverAsMade() || lentArguments != onlyFirstArgumentIsLent || passed.size() > maxArgumentCountOfCompiledMethodOfArray || !canUseCallbackStubs(call))
        return std::nullopt;
    const Operation* closure = findClosurePassed(passed[0]);
    std::optional<CompiledMethodOfArray> method = closure ? methodOfArrayNamed(key) : std::nullopt;
    if (!method)
        return std::nullopt;
    arguments.append(undefined().value());
    for (ValueID argument : passed.subspan(1))
        arguments.append(in<Boxed>(argument).value());
    return chooseCallbackStub(call, *closure, *method, ArrayCallbackFlags::onlyTries, arguments);
}

namespace {

struct LibraryMethodOfArray {
    BuiltinID builtin;
    CompiledMethodOfArray method;
};

} // anonymous namespace

static std::optional<LibraryMethodOfArray> findLibraryMethod(const Program& program, FunctionID function)
{
    static constexpr std::array methods { Builtins::ArrayPrototypeSome, Builtins::ArrayPrototypeEvery, Builtins::ArrayPrototypeFind, Builtins::ArrayPrototypeFindIndex, Builtins::ArrayPrototypeFilter, Builtins::ArrayPrototypeMap, Builtins::ArrayPrototypeReduce };
    RuntimeFunctionID enclosing = program.at(function).enclosingRuntimeFunction;
    if (program.at(function).role != FunctionRole::Library || !enclosing)
        return std::nullopt;
    const BuiltinTable& table = program.builtins();
    for (auto method : methods) {
        BuiltinID builtin = table.canonicalID(method);
        for (const BuiltinOverload& row : table.overloadsOf(builtin)) {
            if (row.mode != BuiltinCallMode::Call || row.libraryFunction() != enclosing)
                continue;
            const RuntimeFunctionEntry* held = findRuntimeFunction(program, enclosing);
            std::optional<CompiledMethodOfArray> compiled = held && held->function == function ? compiledMethodOfArray(builtin) : std::nullopt;
            return compiled ? std::optional(LibraryMethodOfArray { builtin, *compiled }) : std::nullopt;
        }
    }
    return std::nullopt;
}

static bool stubCanRunCallback(const Program& program, const Facts& facts, CompiledMethodOfArray method, FunctionID callback, bool hasInitialValue)
{
    const BodyIndex* index = facts.bodies().find(callback);
    if (!index || program.admitsInspector)
        return false;
    CallbackReader reader(program, program.at(callback), *index);
    if (method == CompiledMethodOfArray::Reduce) {
        if (hasInitialValue && reader.findFoldedPair())
            return true;
        std::optional<CallbackReader::Fold> fold = reader.findFold();
        return fold && (hasInitialValue || !fold->read);
    }
    if (method == CompiledMethodOfArray::Map) {
        std::optional<CallbackReader::Test> test = reader.findTest();
        return (test && test->returnsBoolean) || reader.findLength() || reader.findFieldRead() || reader.findMappedPair() || reader.findFieldOfField() || reader.findMap();
    }
    return reader.findTest() || reader.findTestedPair();
}

bool stubCanRunCallback(const Program& program, const Facts& facts, FunctionID method, FunctionID callback, bool hasInitialValue)
{
    std::optional<LibraryMethodOfArray> called = findLibraryMethod(program, method);
    return called && stubCanRunCallback(program, facts, called->method, callback, hasInitialValue);
}

unsigned elementParameterOfCallback(const Program& program, FunctionID method)
{
    std::optional<LibraryMethodOfArray> called = findLibraryMethod(program, method);
    return called && called->method == CompiledMethodOfArray::Reduce;
}

bool stubCanRunCallback(const Program& program, const Facts& facts, BuiltinID method, FunctionID callback, bool hasInitialValue)
{
    static constexpr std::array methods { CompiledMethodOfArray::Some, CompiledMethodOfArray::Every, CompiledMethodOfArray::Find, CompiledMethodOfArray::FindIndex, CompiledMethodOfArray::Filter, CompiledMethodOfArray::Map, CompiledMethodOfArray::Reduce };
    std::optional<CompiledMethodOfArray> called = compiledMethodOfArray(program.builtins().canonicalID(method));
    return called && std::ranges::contains(methods, *called) && stubCanRunCallback(program, facts, *called, callback, hasInitialValue);
}

std::optional<Boxed> AccessEmitter::tryStubOfDirectCall(const ValueContext& call, FunctionID callee)
{
    std::span<const ValueID> operands = call->argumentsOperands();
    auto isClosure = [&](ValueID operand) {
        return operand && operationAt(operand).opcode == Opcode::Box && findClosurePassed(operand);
    };
    if (operands.size() < 2 || !std::ranges::any_of(operands, isClosure) || !canUseCallbackStubs(call))
        return std::nullopt;
    const Function& function = m_program.at(callee);
    if (operands.size() != function.numberOfEntryParameters())
        return std::nullopt;
    ValueID receiver;
    Vector<ValueID, 3> passed;
    std::optional<int64_t> count;
    for (uint32_t place = 0; place < operands.size(); ++place) {
        switch (function.entryParameterKind(place)) {
        case EntryParameter::This:
            receiver = operands[place];
            break;
        case EntryParameter::Parameter:
            passed.append(operands[place]);
            break;
        case EntryParameter::ArgumentCount:
            if (operationAt(operands[place]).opcode != Opcode::Integer)
                return std::nullopt;
            count = operationAt(operands[place]).integer();
            break;
        default:
            return std::nullopt;
        }
    }
    const Operation* closure = receiver && !passed.isEmpty() ? findClosurePassed(passed[0]) : nullptr;
    std::optional<LibraryMethodOfArray> method = closure ? findLibraryMethod(m_program, callee) : std::nullopt;
    if (!method || (method->method == CompiledMethodOfArray::Reduce && (!count || *count < 1 || *count > 2)))
        return std::nullopt;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    Vector<LValue, 8> arguments;
    unsigned numberOfArguments = method->method == CompiledMethodOfArray::Reduce ? static_cast<unsigned>(*count) : 1;
    for (unsigned place = 0; place < numberOfArguments; ++place)
        arguments.append(conversions.box(call, get(passed[place]), typeOf(passed[place])).value());
    uint32_t flags = (closure->loan() != LoanOfClosure::None ? ArrayCallbackFlags::callbackIsLent : 0) | (call.returnCall() ? ArrayCallbackFlags::onlyTries : 0);
    std::optional<StubKey> stub = chooseCallbackStub(call, *closure, method->method, flags, arguments);
    if (!stub)
        return std::nullopt;
    ConventionCall conventionCall { .stub = *stub, .thisValue = conversions.box(call, get(receiver), typeOf(receiver)).value(), .arguments = arguments.span() };
    return emitter<CallEmitter>().callByConvention(call, conventionCall, { .numberedImmediate = NumberedImmediate { mentionOfName(m_program.keyOf(m_program.builtins().at(method->builtin).name)) } });
}

bool AccessEmitter::returnsOneBoxedValue() const
{
    ArgumentLocations own = m_plan.conventions().locationsOf(m_program, m_unit);
    return own.results.size() == 1 && own.results[0].representation == Representation::JSValue && !m_plan.conventions().spillsArgumentRegisters(m_unit);
}

void AccessEmitter::returnUnlessEmpty(Boxed answer)
{
    LBasicBlock returns = newBlock();
    LBasicBlock continues = newBlock();
    branch(isNotEmpty(answer), { normalEdge(returns), normalEdge(continues) });
    m_out.appendTo(returns);
    Operand operand = answer;
    emitter<CallEmitter>().returnValues(singleElementSpan(operand));
    m_out.appendTo(continues);
}

void AccessEmitter::returnStubResultInTail(const ValueContext& read)
{
    const Terminator& terminator = m_code.at(read.block()).terminator;
    if (terminator.kind != TerminatorKind::ReturnCall || terminator.callOperand() != m_code.onlyUserOf(read.id()) || isSymbolOfRealm(read->key()) || !canUseCallbackStubs(read))
        return;
    const Operation& call = operationAt(terminator.callOperand());
    auto unboxed = [&](ValueID id) {
        return operationAt(id).opcode == Opcode::Box ? operationAt(id).operand(0) : id;
    };
    if (call.opcode != Opcode::Call || call.methodIsNamedByKeyAlone() || call.calleeOperand() != read.id() || !call.thisValueOperand() || unboxed(call.thisValueOperand()) != unboxed(read->objectOperand()))
        return;
    std::span<const ValueID> passed = call.argumentsOperands();
    std::optional<CompiledMethodOfArray> method = call.methodOfReceiverAsMade() ? compiledMethodOfArray(call.methodOfReceiverAsMade()) : methodOfArrayNamed(read->key());
    const Operation* closure = method && !passed.empty() && passed.size() <= 2 ? findClosurePassed(passed[0]) : nullptr;
    if (!closure || !returnsOneBoxedValue())
        return;
    std::span<const ValueID> following = read.operationsBehind();
    for (ValueID id : following) {
        const Operation& operation = operationAt(id);
        if (operation.opcode != Opcode::Box && operation.opcode != Opcode::NewClosure && !operation.traits().isPure())
            return;
    }
    auto isAvailable = [&](ValueID id) {
        ValueID made = unboxed(id);
        return made != read.id() && !std::ranges::contains(following, made) && (!operationAt(made).block() || generator().wordOf(made));
    };
    auto isMade = [&](ValueID id) {
        return !std::ranges::contains(following, id) && (!operationAt(id).block() || generator().wordOf(id));
    };
    if (!isAvailable(read->objectOperand()) || !std::ranges::all_of(closure->capturesOperands(), isMade) || (passed.size() == 2 && !isAvailable(passed[1])))
        return;
    ConversionEmitter& conversions = emitter<ConversionEmitter>();
    auto boxed = [&](ValueID id) {
        ValueID made = unboxed(id);
        if (std::optional<Boxed> word = generator().findBoxed(made))
            return word->value();
        return conversions.box(read, get(made), typeOf(made)).value();
    };
    Vector<LValue, 8> arguments { undefined().value() };
    if (passed.size() == 2)
        arguments.append(boxed(passed[1]));
    std::optional<StubKey> stub = chooseCallbackStub(read, *closure, *method, ArrayCallbackFlags::onlyTries, arguments);
    if (!stub)
        return;
    ConventionCall conventionCall { .stub = *stub, .thisValue = boxed(read->objectOperand()), .arguments = arguments.span() };
    returnUnlessEmpty(emitter<CallEmitter>().callByConvention(read, conventionCall, { .numberedImmediate = NumberedImmediate { mentionOfName(read->key()) }, .origin = call.origin() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
