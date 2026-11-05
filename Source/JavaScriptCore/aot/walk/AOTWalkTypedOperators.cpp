/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "JSGlobalObjectFunctions.h"
#include <cmath>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

bool buildOperatorOnNumbers(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool isTakenAsItIsByFormNumbers(const Type&);
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

static constexpr OpcodeSet arithmeticAmongTypedOperators = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise });
static constexpr OpcodeSet conversionsAmongTypedOperators { Opcode::ToNumber, Opcode::ToNumeric, Opcode::ToString, Opcode::ToBoolean };
static constexpr OpcodeSet opcodesOfTypedOperators = arithmeticAmongTypedOperators | allEqualities | allRelations | conversionsAmongTypedOperators;

static_assert(!OperandIndex::Eq::left && !OperandIndex::TypeTest::value && !OperandIndex::TypeOf::value);

namespace {

struct TypeAskedByTypeOfOperator {
    NamedBuiltinName answer;
    TypeID tested;
};

} // anonymous namespace

static constexpr TypeAskedByTypeOfOperator typesAskedByTypeOfOperator[] {
    { BuiltinNames::undefined, TypeTable::undefined },
    { BuiltinNames::bigint, TypeTable::bigInt },
    { BuiltinNames::boolean, TypeTable::boolean },
    { BuiltinNames::number, TypeTable::number },
    { BuiltinNames::string, TypeTable::string },
    { BuiltinNames::symbol, TypeTable::symbol },
    { BuiltinNames::function, TypeTable::function },
};

OperatorForm formAllowedForTypedOperator(Walker& walker, const Draft& draft)
{
    OperatorTypes operands { walker.typeOf(draft.operand(0)), { } };
    if (draft.numberOfNamedOperands() == 2)
        operands.right = walker.typeOf(draft.operand(1));
    OperatorForm form = chooseFormOfOperator(walker.program(), walker.types(), draft.opcode, operands);
    if (form == OperatorForm::Numbers && (!isTakenAsItIsByFormNumbers(walker.types().at(operands.left)) || (operands.right && !isTakenAsItIsByFormNumbers(walker.types().at(operands.right)))))
        return OperatorForm::Generic;
    constexpr OpcodeSet madeOnDoublesAsWell { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Increment, Opcode::Decrement, Opcode::Negate };
    if (form == OperatorForm::CheckedIntegers && madeOnDoublesAsWell.contains(draft.opcode)) {
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
            Reference operand = draft.operand(operandIndex);
            if (operand.isValue() && walker.representationOf(operand) == Representation::Double)
                return OperatorForm::Numbers;
        }
    }
    return form;
}

OperatorForm formAllowedForTypedOperator(Walker& walker, Opcode opcode, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (!hasOperatorForm(opcode))
        return OperatorForm::Generic;
    OperatorTypes operands { inputs.typeOf(value.operand(0)), { } };
    if (numberOfNamedOperandsOf(opcode) == 2)
        operands.right = inputs.typeOf(value.operand(1));
    return chooseFormOfOperator(walker.program(), walker.types(), opcode, operands);
}

static bool buildInFormOfTypedOperator(Walker& walker, Draft&& draft, OperatorForm form, ValueID input)
{
    walker.chooseConservativeFields(draft);
    draft.setForm(form);
    Origin origin = draft.origin();
    walker.bind(input, boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(draft)), origin));
    return true;
}

static std::optional<uint32_t> operandsNarrowedBehindTypedOperator(const Walker& walker, ValueID test)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isUnused(test))
        return 0;
    std::optional<BodyUser> user = index.onlyUser(test);
    if (!user)
        return std::nullopt;
    if (user->kind != BodyUseKind::TerminatorOperand)
        return 0;

    uint32_t operands = 0;
    for (BodyTarget target : index.terminator(user->block).targets()) {
        for (uint32_t operandIndex : target.proofs())
            operands |= 1u << (operandIndex & 31);
    }
    return operands;
}

static StringID onlyTextOfTypedOperator(const Type& type)
{
    return type.hasOnlyPrimitives(Primitive::LiteralString) ? singleStringLiteralOf(type) : StringID();
}

static void bindTypeTestAsTypedOperator(Walker& walker, const BodyValue& value, Reference tested, TypeID testedType)
{
    Draft test = walker.draftOf(value);
    test.become(Opcode::TypeTest);
    test.setValueOperand(tested);
    test.setTestedType(testedType);
    walker.bind(value.id(), walker.build(WTF::move(test)));
}

static TypeID typeAskedByTextOfTypeOfOperator(Walker& walker, StringID text, TypeID source)
{
    TypeArena& types = walker.types();
    for (const TypeAskedByTypeOfOperator& asked : typesAskedByTypeOfOperator) {
        if (types.spellSameText(text, walker.program().stringOf(asked.answer)))
            return asked.tested;
    }
    if (!types.spellSameText(text, walker.program().stringOf(BuiltinNames::object)) || types.at(source).mayBeUnspecified())
        return { };
    if (!types.overlap(source, TypeTable::function))
        return types.withPrimitives(TypeTable::object, Primitive::Null);
    if (types.isSubset(types.withOnlyObjects(source), TypeTable::function))
        return TypeTable::null;
    return { };
}

static bool buildTypeTestInPlaceOfTypeOfOperator(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    for (uint32_t side = 0; side < 2; ++side) {
        ValueID answer = value.operand(side);
        if (index.opcodeOf(answer) != Opcode::TypeOf)
            continue;
        StringID text = onlyTextOfTypedOperator(types.at(inputs.typeOf(value.operand(1 - side))));
        if (!text)
            return false;
        ValueID asked = index.value(answer).operand(OperandIndex::TypeOf::value);
        TypeID tested = typeAskedByTextOfTypeOfOperator(walker, text, inputs.typeOf(asked));
        if (!tested || types.testablePartOf(tested) != tested)
            return false;
        if (!inputs.areOnlyTypes())
            bindTypeTestAsTypedOperator(walker, value, walker.referenceOf(asked), tested);
        return true;
    }
    return false;
}

static bool equalsTextAloneInTypedOperator(Walker& walker, const Type& text, const Type& other)
{
    StringID literal = onlyTextOfTypedOperator(text);
    if (!literal || other.hasObjects() || !other.isJavaScriptValue() || other.mayBeUnspecified())
        return false;
    Vector<char16_t> buffer;
    return std::isnan(jsToNumber(walker.types().viewOf(literal, buffer)));
}

static bool buildEqualityAsTypedOperator(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    std::optional<uint32_t> narrowed = operandsNarrowedBehindTypedOperator(walker, value.id());
    if (narrowed && !*narrowed && buildTypeTestInPlaceOfTypeOfOperator(walker, value, inputs))
        return true;

    static_assert(!OperandIndex::StrictEq::left && OperandIndex::Eq::right == 1 && OperandIndex::StrictEq::right == 1);
    Type leftType = walker.types().at(inputs.typeOf(value.operand(0)));
    Type rightType = walker.types().at(inputs.typeOf(value.operand(1)));
    if (leftType.isNever() || rightType.isNever())
        return false;

    Opcode opcode = value.opcode();
    if (opcode == Opcode::Eq) {
        if (rightType.isUndefinedOrNull() && leftType.isJavaScriptValue() && narrowed && !(*narrowed & ~1u)) {
            if (!inputs.areOnlyTypes())
                bindTypeTestAsTypedOperator(walker, value, walker.referenceOf(value.operand(0)), TypeTable::nullish);
            return true;
        }
        if (leftType.isUndefinedOrNull() && rightType.isJavaScriptValue() && narrowed && !*narrowed) {
            if (!inputs.areOnlyTypes())
                bindTypeTestAsTypedOperator(walker, value, walker.referenceOf(value.operand(1)), TypeTable::nullish);
            return true;
        }
        if (equalsTextAloneInTypedOperator(walker, leftType, rightType) || equalsTextAloneInTypedOperator(walker, rightType, leftType))
            opcode = Opcode::StrictEq;
    }
    OperatorForm form = formAllowedForTypedOperator(walker, opcode, value, inputs);
    if (form == OperatorForm::Generic || inputs.areOnlyTypes())
        return form != OperatorForm::Generic;

    Draft draft = walker.draftOf(value);
    if (opcode != draft.opcode) {
        Reference left = draft.leftOperand();
        Reference right = draft.rightOperand();
        draft.become(opcode);
        draft.setLeftOperand(left);
        draft.setRightOperand(right);
    }
    if (opcode == Opcode::StrictEq && form == OperatorForm::NumberAndValue && narrowed && !*narrowed && walker.has(Mechanism::UnboxedNumbers)) {
        Draft readAsNumber(Opcode::NumberOrNaN, draft.origin());
        readAsNumber.setValueOperand(leftType.isNumber() ? draft.rightOperand() : draft.leftOperand());
        Reference number = walker.build(WTF::move(readAsNumber));
        if (leftType.isNumber())
            draft.setRightOperand(number);
        else
            draft.setLeftOperand(number);
        form = OperatorForm::Numbers;
    }
    return buildInFormOfTypedOperator(walker, WTF::move(draft), form, value.id());
}

static bool buildRelationAsTypedOperator(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    OperatorForm form = formAllowedForTypedOperator(walker, value.opcode(), value, inputs);
    if (form == OperatorForm::Numbers) {
        bool isEachTakenAsItIs = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            isEachTakenAsItIs &= isTakenAsItIsByFormNumbers(walker.types().at(inputs.typeOf(operand)));
        });
        if (!isEachTakenAsItIs)
            return false;
    }
    if (form == OperatorForm::Generic || inputs.areOnlyTypes())
        return form != OperatorForm::Generic;
    return buildInFormOfTypedOperator(walker, walker.draftOf(value), form, value.id());
}

bool buildTypedOperator(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (!opcodesOfTypedOperators.contains(opcode))
        return false;

    if (arithmeticAmongTypedOperators.contains(opcode))
        return buildOperatorOnNumbers(walker, value, inputs);
    if (allEqualities.contains(opcode))
        return buildEqualityAsTypedOperator(walker, value, inputs);
    if (allRelations.contains(opcode))
        return buildRelationAsTypedOperator(walker, value, inputs);

    OperatorForm form = formAllowedForTypedOperator(walker, opcode, value, inputs);
    if (form == OperatorForm::Generic || inputs.areOnlyTypes())
        return form != OperatorForm::Generic;
    return buildInFormOfTypedOperator(walker, walker.draftOf(value), form, value.id());
}

bool buildTypedOperator(Walker& walker, const BodyValue& value)
{
    return buildTypedOperator(walker, value, InputsOfTypedOperation(walker, nullptr));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
