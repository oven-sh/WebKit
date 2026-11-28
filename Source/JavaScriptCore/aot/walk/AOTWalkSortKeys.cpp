/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkSortKeys.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuiltinTable.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct KeyOfComparator {
    ValueID key;
    uint32_t parameter { 0 };

    bool isReversed() const { return parameter; }
};

class OperandComparison {
public:
    OperandComparison(const BodyIndex& index, uint32_t firstParameter)
        : m_index(index)
        , m_firstParameter(firstParameter)
    {
    }

    bool areAlike(ValueID left, ValueID right);
    std::optional<uint32_t> parameterOfLeft() const { return m_parameterOfLeft; }
    bool isOperationOfKey(ValueID value) const { return m_operationsOfKeys.contains(value); }

private:
    static constexpr unsigned mostOperations = 24;

    std::optional<uint32_t> positionOf(ValueID) const;
    bool haveAlikeFields(const BodyValue& left, const BodyValue& right) const;

    const BodyIndex& m_index;
    const uint32_t m_firstParameter;
    std::optional<uint32_t> m_parameterOfLeft;
    unsigned m_numberOfOperations { 0 };
    Vector<ValueID, 16> m_operationsOfKeys;
};

} // anonymous namespace

static constexpr OpcodeSet constantsOfSortKeys { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::Builtin };
static constexpr OpcodeSet operationsOfSortKeys = constantsOfSortKeys | OpcodeSet {
    Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::Mod, Opcode::BitAnd, Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift,
    Opcode::Negate, Opcode::ToNumeric, Opcode::BitNot, Opcode::ToNumber, Opcode::CheckInitialized, Opcode::RequireObjectCoercible,
    Opcode::PropGet, Opcode::ElemGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::Call,
};

std::optional<uint32_t> OperandComparison::positionOf(ValueID parameter) const
{
    BodyBlock entry = m_index.block(entryBlock);
    for (uint32_t position = 0; position < 2; ++position) {
        if (parameter == entry.parameter(m_firstParameter + position))
            return position;
    }
    return std::nullopt;
}

bool OperandComparison::haveAlikeFields(const BodyValue& left, const BodyValue& right) const
{
    switch (left.opcode()) {
    case Opcode::Boolean:
        return left.as<Opcode::Boolean>().boolean() == right.as<Opcode::Boolean>().boolean();
    case Opcode::Integer:
        return left.integer() == right.integer();
    case Opcode::Double:
        return std::bit_cast<uint64_t>(left.number()) == std::bit_cast<uint64_t>(right.number());
    case Opcode::String:
        return left.string() == right.string();
    case Opcode::Builtin:
        return left.as<Opcode::Builtin>().builtin() == right.as<Opcode::Builtin>().builtin();
    case Opcode::GetModuleVariable:
        return left.variable() == right.variable();
    case Opcode::GetEnvironmentField:
        return left.layout() == right.layout() && left.field() == right.field();
    case Opcode::PropGet:
        return left.key() == right.key() && !left.isFreeName() && !right.isFreeName();
    case Opcode::RequireObjectCoercible:
        return left.as<Opcode::RequireObjectCoercible>().nullishAccess() == right.as<Opcode::RequireObjectCoercible>().nullishAccess();
    case Opcode::Call: {
        auto leftCall = left.as<Opcode::Call>();
        auto rightCall = right.as<Opcode::Call>();
        return leftCall.argumentsOperands().size() == rightCall.argumentsOperands().size() && !leftCall.thisValueOperand() == !rightCall.thisValueOperand() && !left.isDoneInPlace() && !right.isDoneInPlace();
    }
    default:
        return true;
    }
}

bool OperandComparison::areAlike(ValueID left, ValueID right)
{
    if (!left || !right)
        return !left && !right;
    left = m_index.stripCasts(left);
    right = m_index.stripCasts(right);
    if (m_index.isParameter(left) || m_index.isParameter(right)) {
        if (!m_index.isParameter(left) || !m_index.isParameter(right) || !m_index.isInBlock(left, entryBlock) || !m_index.isInBlock(right, entryBlock))
            return false;
        std::optional<uint32_t> positionOfLeft = positionOf(left);
        std::optional<uint32_t> positionOfRight = positionOf(right);
        if (!positionOfLeft || !positionOfRight)
            return left == right && !positionOfLeft && !positionOfRight;
        if (m_parameterOfLeft && *m_parameterOfLeft != *positionOfLeft)
            return false;
        m_parameterOfLeft = positionOfLeft;
        return *positionOfRight == 1 - *positionOfLeft;
    }
    BodyValue leftValue = m_index.value(left);
    BodyValue rightValue = m_index.value(right);
    if (leftValue.opcode() != rightValue.opcode() || !operationsOfSortKeys.contains(leftValue.opcode()) || ++m_numberOfOperations > mostOperations || !haveAlikeFields(leftValue, rightValue))
        return false;
    m_operationsOfKeys.append(left);
    m_operationsOfKeys.append(right);
    Vector<ValueID, 8> operandsOfRight;
    rightValue.forEachOperand([&](ValueID operand, uint32_t) {
        operandsOfRight.append(operand);
    });
    size_t place = 0;
    bool areOperandsAlike = true;
    leftValue.forEachOperand([&](ValueID operand, uint32_t) {
        areOperandsAlike = areOperandsAlike && place < operandsOfRight.size() && areAlike(operand, operandsOfRight[place]);
        ++place;
    });
    return areOperandsAlike && place == operandsOfRight.size();
}

static std::optional<KeyOfComparator> findKeyOfComparator(const Walker& walker, FunctionID comparator)
{
    constexpr OptionSet<FunctionFlags> otherForms { FunctionFlags::IsAsync, FunctionFlags::IsGenerator, FunctionFlags::HasRestParameter, FunctionFlags::HasArgumentCount, FunctionFlags::HasHiddenRest, FunctionFlags::HasNewTarget, FunctionFlags::TakesCallee, FunctionFlags::HasThis };
    const Function& function = walker.program().at(comparator);
    const BodyIndex* index = walker.facts().bodies().find(comparator);
    if (!index || !function.hasBody() || function.numberOfPositionalParameters() != 2 || function.hasAny(otherForms) || isClassConstructorOf(function.kind) || index->numberOfBlocks() != 1)
        return std::nullopt;
    BodyBlock entry = index->block(entryBlock);
    uint32_t firstParameter = function.firstParameterIndex();
    if (entry.hasHandler() || entry.terminator().kind() != TerminatorKind::Return || entry.numberOfParameters() < firstParameter + 2)
        return std::nullopt;
    ValueID returned = entry.terminator().operand(0);
    ValueID difference = returned ? index->stripCasts(returned) : ValueID();
    if (!difference || index->isParameter(difference) || index->opcodeOf(difference) != Opcode::Sub)
        return std::nullopt;
    ValueID key = index->value(difference).operand(0);
    if (!key || index->isParameter(index->stripCasts(key)))
        return std::nullopt;
    OperandComparison operands(*index, firstParameter);
    if (!operands.areAlike(key, index->value(difference).operand(1)) || !operands.parameterOfLeft())
        return std::nullopt;
    for (BodyValue operation : entry.values()) {
        bool isPartOfDifference = operation.id() == difference || operands.isOperationOfKey(operation.id());
        if (!isPartOfDifference && !opcodesThatStateTypes.contains(operation.opcode()) && !constantsOfSortKeys.contains(operation.opcode()))
            return std::nullopt;
    }
    return KeyOfComparator { key, *operands.parameterOfLeft() };
}

static TypeID typeArrivingAtComparator(const Walker& walker, FunctionID comparator, uint32_t parameter)
{
    std::span<const LinkedParameter> parameters = walker.facts().linked().parametersOf(comparator);
    size_t place = walker.program().at(comparator).firstParameterIndex() + parameter;
    TypeID arriving = place < parameters.size() ? parameters[place].writtenOrSeenToArrive() : TypeID();
    return arriving && !isNeverType(arriving) && !walker.types().isUninformative(arriving) ? arriving : TypeID();
}

static FunctionID findSortKeyHelper(const Walker& walker, BuiltinID helper)
{
    const BuiltinTable& table = walker.program().builtins();
    if (!table.contains(helper))
        return { };
    for (const BuiltinOverload& row : table.overloadsOf(table.canonicalID(helper))) {
        const RuntimeFunctionEntry* held = row.mode == BuiltinCallMode::Call ? findRuntimeFunction(walker.program(), row.libraryFunction()) : nullptr;
        if (held && held->function && walker.program().at(held->function).hasBody())
            return held->function;
    }
    return { };
}

static FunctionID functionOfComparator(Walker& walker, ValueID passed)
{
    Reference comparator = walker.referenceOf(passed);
    TypeID proven = comparator ? walker.typeOf(comparator) : TypeID();
    Type type = proven ? walker.types().at(proven) : Type();
    return proven && type.isObject() && type.isJavaScriptValue() ? singleFunctionOf(walker.types(), type) : FunctionID();
}

namespace {

class KeyCopyCheck {
public:
    KeyCopyCheck(const Walker& walker, FunctionID comparator, const KeyOfComparator& key)
        : m_walker(walker)
        , m_index(walker.facts().bodies().at(comparator))
        , m_comparator(comparator)
        , m_element(m_index.block(entryBlock).parameter(walker.program().at(comparator).firstParameterIndex() + key.parameter))
        , m_typeOfElement(typeArrivingAtComparator(walker, comparator, key.parameter))
    {
    }

    bool mayBeCopied(ValueID);

private:
    TypeID typeOf(ValueID);
    bool mayBeCopiedAsCallee(const BodyValue& call);
    bool isFieldOf(TypeID, PropertyKeyID);
    bool isString(TypeID);

    const Walker& m_walker;
    const BodyIndex& m_index;
    const FunctionID m_comparator;
    const ValueID m_element;
    const TypeID m_typeOfElement;
};

} // anonymous namespace

bool KeyCopyCheck::isString(TypeID type)
{
    TypeArena& types = m_walker.types();
    TypeID present = type ? types.withoutNullish(type) : TypeID();
    return present && !isNeverType(present) && types.at(present).hasOnlyPrimitives(Primitive::AnyString);
}

bool KeyCopyCheck::isFieldOf(TypeID type, PropertyKeyID key)
{
    TypeArena& types = m_walker.types();
    TypeID present = type ? types.withoutNullish(type) : TypeID();
    if (!present || isNeverType(present) || !types.at(present).hasOnlyLayouts())
        return false;
    return std::ranges::all_of(membersOf(types, types.at(present)), [&](LayoutID member) {
        return !!findField(m_walker.program(), member, key);
    });
}

TypeID KeyCopyCheck::typeOf(ValueID value)
{
    value = m_index.stripCasts(value);
    if (value == m_element)
        return m_typeOfElement;
    if (m_index.isParameter(value) || m_index.opcodeOf(value) != Opcode::PropGet)
        return { };
    TypeArena& types = m_walker.types();
    BodyValue read = m_index.value(value);
    TypeID object = typeOf(read.objectOperand());
    if (!isFieldOf(object, read.key()))
        return { };
    TypeID joined;
    Vector<LayoutID, 4> members(membersOf(types, types.at(types.withoutNullish(object))));
    for (LayoutID member : members) {
        TypeID proven = m_walker.facts().linked().provenOfField(*findField(m_walker.program(), member, read.key()));
        if (!proven)
            return { };
        joined = joined ? types.unionOf(joined, proven) : proven;
    }
    return joined;
}

bool KeyCopyCheck::mayBeCopiedAsCallee(const BodyValue& call)
{
    if (FunctionID callee = m_walker.facts().linked().calleeAt(m_comparator, call.id())) {
        const BodyIndex* body = m_walker.facts().bodies().find(callee);
        return body && !body->hasLoops() && copyOfCalleeCalledInLoopPays(m_walker, callee, call.argumentsOperands().size());
    }
    ValueID callee = m_index.stripCasts(call.calleeOperand());
    if (m_index.isParameter(callee))
        return false;
    BodyValue read = m_index.value(callee);
    return read.opcode() == Opcode::Builtin || (read.opcode() == Opcode::PropGet && isString(typeOf(read.objectOperand())));
}

bool KeyCopyCheck::mayBeCopied(ValueID value)
{
    value = m_index.stripCasts(value);
    if (m_index.isParameter(value))
        return true;
    BodyValue operation = m_index.value(value);
    switch (operation.opcode()) {
    case Opcode::Call:
        if (!mayBeCopiedAsCallee(operation))
            return false;
        return std::ranges::all_of(operation.argumentsOperands(), [&](ValueID argument) { return mayBeCopied(argument); });
    case Opcode::PropGet: {
        TypeID object = typeOf(operation.objectOperand());
        return (isString(object) || isFieldOf(object, operation.key())) && mayBeCopied(operation.objectOperand());
    }
    case Opcode::ElemGet:
        return false;
    default:
        break;
    }
    bool areOperandsCopied = true;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        areOperandsCopied = areOperandsCopied && (!operand || mayBeCopied(operand));
    });
    return areOperandsCopied;
}

bool sortByKeysOfComparator(Walker& walker, const BodyValue& call)
{
    if (!walker.opensGuesses() || !walker.has(Mechanism::Inlining) || !walker.has(Mechanism::Guesses) || !walker.has(Mechanism::EscapeAnalysis) || walker.program().admitsReplacedBuiltins || walker.standsUnderHandler() || walker.hasStringsInBuilders()
        || walker.isInFusedChain() || walker.program().at(walker.unit().function).isCoroutine())
        return false;
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() != 1 || !call.thisValueOperand())
        return false;
    FunctionID comparator = functionOfComparator(walker, arguments[0]);
    std::optional<KeyOfComparator> key = comparator ? findKeyOfComparator(walker, comparator) : std::nullopt;
    if (!key || !typeArrivingAtComparator(walker, comparator, key->parameter) || !KeyCopyCheck(walker, comparator, *key).mayBeCopied(key->key))
        return false;
    FunctionID helper = findSortKeyHelper(walker, Builtins::CompilerHelpersSortByKeys);
    if (!mayCopyFusedChain(walker, helper))
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(call.id()));
    uint32_t place = call.id().index() - block.firstValue().index();
    Walker::StretchInArms stretch = walker.stretchInArms(block.id(), place, place + 1);
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Draft direction(Opcode::Integer, origin);
    direction.setInteger(key->isReversed() ? -1 : 1);
    Vector<Reference, 4> operands { walker.referenceOf(call.thisValueOperand()), walker.referenceOf(arguments[0]), walker.constant(WTF::move(direction)) };
    Walker::ArmsThatMeet arms;
    BlockID asWritten;
    bool isCopied = !operands.contains(Reference()) && copyFusedChain(walker, call, helper, operands.span(), asWritten);
    if (walker.hasFailed())
        return true;
    if (isCopied) {
        walker.count(StatisticID::SortComparatorCallsElided);
        walker.listArm(stretch, arms);
    }
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), call.id()))
        walker.leaveAnythingAt(callee);
    if (asWritten || !isCopied) {
        std::optional<Walker::ClosedArm> closedArm;
        if (asWritten) {
            walker.continueIn(asWritten);
            closedArm.emplace(walker, true);
        }
        SetForScope fusesNoChain(walker.fusesNoChain(), true);
        walker.walkStretch(block.id(), place, place + 1);
        walker.listArm(stretch, arms);
    }
    if (!walker.hasFailed() && walker.joinArms(stretch, arms, walker.addBlock()))
        walker.walkStretch(block.id(), place + 1, place + 1);
    return true;
}

bool copyKeyOfComparator(Walker& walker, const BodyValue& call, FunctionID comparator)
{
    if (!walker.isInCopyOfFusedChain() || call.opcode() != Opcode::Call || !walker.currentBodyIndex().isParameter(walker.currentBodyIndex().stripCasts(call.calleeOperand())) || walker.currentFunction() != findSortKeyHelper(walker, Builtins::CompilerHelpersSortByKeys))
        return false;
    std::span<const ValueID> arguments = call.argumentsOperands();
    std::optional<KeyOfComparator> key = arguments.size() == 1 ? findKeyOfComparator(walker, comparator) : std::nullopt;
    if (!key)
        return false;
    Origin origin = walker.originOf(call);
    origin.isTailCall = false;
    Reference element = walker.narrowInFusedChain(walker.referenceOf(arguments[0]), typeArrivingAtComparator(walker, comparator, key->parameter), origin);
    if (!element || walker.isUnreachable() || walker.hasFailed())
        return true;
    Reference passed[] { element, element };
    Draft site(Opcode::Call, origin);
    walker.chooseConservativeFields(site);
    site.setCalleeOperand(walker.referenceOf(call.calleeOperand()));
    site.setArgumentsOperands(passed);
    Vector<Reference, 8> operands = collectPassedOperands(site, walker.program().at(comparator));
    return walker.walkInlineKey(call, comparator, operands.span(), site, key->key);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
