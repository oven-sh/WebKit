/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTWalkNumberForms.h"
#include "AOTWalkStrings.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet accessFamiliesOfWalkTyped = opcodesOfFamily(OperationFamily::Access) | opcodesOfFamily(OperationFamily::PrivateAccess) | opcodesOfFamily(OperationFamily::Binding);
static constexpr OpcodeSet elementFamiliesOfWalkTyped = opcodesOfFamily(OperationFamily::Access) | opcodesOfFamily(OperationFamily::Allocation) | opcodesOfFamily(OperationFamily::ArrayIntrinsic)
    | opcodesOfFamily(OperationFamily::ForIn);
static constexpr OpcodeSet operatorFamiliesOfWalkTyped = opcodesOfFamily(OperationFamily::Operator) | opcodesOfFamily(OperationFamily::Cast) | opcodesOfFamily(OperationFamily::Guard);

TypedForm typedFormOfAccess(Walker&, const BodyValue&, const TypesOfInputs&);
bool buildTypedElementAccess(Walker&, const BodyValue&, const InputsOfTypedOperation&);
bool buildTypedOperator(Walker&, const BodyValue&, const InputsOfTypedOperation&);

static constexpr OpcodeSet accessesThatThrowOffNullishInWalkTyped { Opcode::PropGet, Opcode::PropSet, Opcode::ElemGet, Opcode::ElemSet };
static constexpr OpcodeSet accessesByNameInWalkTyped { Opcode::PropGet, Opcode::PropSet };
static constexpr OpcodeSet storesInWalkTyped { Opcode::PropSet, Opcode::ElemSet };

static TypedForm typedFormAsReceiverStands(Walker&, const BodyValue&, const TypesOfInputs&);

static TypeID typeOfReceiverBehindCheckInWalkTyped(Walker& walker, const BodyValue& access, TypeID typeOfReceiver)
{
    Opcode opcode = access.opcode();
    if (!typeOfReceiver || !accessesThatThrowOffNullishInWalkTyped.contains(opcode) || (accessesByNameInWalkTyped.contains(opcode) && access.isFreeName()))
        return { };
    Type type = walker.types().at(typeOfReceiver);
    if (!type.isJavaScriptValue() || !type.mayBeUndefinedOrNull() || type.hasUnboundedUnknown())
        return { };
    return walker.types().withoutNullish(typeOfReceiver);
}

static bool splitNullishOffReceiverInWalkTyped(Walker& walker, const BodyValue& access)
{
    ValueID input = access.objectOperand();
    Reference receiver = walker.referenceOf(input);
    TypeID rest = receiver ? typeOfReceiverBehindCheckInWalkTyped(walker, access, walker.typeOf(receiver)) : TypeID();
    if (!rest)
        return false;
    auto typeBehindCheck = [&](ValueID operand) {
        if (operand == input)
            return rest;
        Reference built = walker.referenceOf(operand);
        return built ? walker.typeOf(built) : TypeID(TypeTable::anything);
    };
    if (!isNeverType(rest) && (!receiver.isValue() || typedFormAsReceiverStands(walker, access, typeBehindCheck) == TypedForm::None))
        return false;

    Opcode opcode = access.opcode();
    if (isNeverType(rest) && !accessesByNameInWalkTyped.contains(opcode)) {
        Draft asWritten = walker.draftOf(access);
        walker.chooseConservativeFields(asWritten);
        Effects effectsOfCheck = declaredBoundInTable(Opcode::RequireObjectCoercible);
        Reference built = walker.build(WTF::move(asWritten), &effectsOfCheck);
        if (built && resultRuleOf(opcode) != ResultRule::None)
            walker.bind(access.id(), built);
        return true;
    }
    Draft check(Opcode::RequireObjectCoercible, walker.originOf(access));
    check.setValueOperand(receiver);
    check.setNullishAccess(storesInWalkTyped.contains(opcode) ? NullishAccess::Write : NullishAccess::Read);
    if (accessesByNameInWalkTyped.contains(opcode) && !liesInThread(access.key())) {
        if (const PropertyKey& key = walker.program().at(access.key()); key.kind == PropertyKeyKind::Text)
            check.setName(key.text);
    }
    if (Reference checked = walker.build(WTF::move(check)); checked && receiver.isValue())
        walker.state().noteCheckedName(receiver, checked);
    return walker.isUnreachable();
}

template<Opcode opcode> static bool buildTypedOperationOf(Walker& walker, const BodyValue& walked)
{
    BodyValueOf<opcode> value(walked);
    if (buildTypedForIn(walker, value) || buildAccessUnderIndexOfForIn(walker, value))
        return true;
    if (accessesThatThrowOffNullishInWalkTyped.contains(opcode)) {
        if (splitNullishOffReceiverInWalkTyped(walker, value))
            return true;
    }
    if (buildTypedOperationOnStrings(walker, value, InputsOfTypedOperation(walker, nullptr)))
        return true;
    if (accessFamiliesOfWalkTyped.contains(opcode) && buildTypedAccess(walker, value))
        return true;
    if (elementFamiliesOfWalkTyped.contains(opcode) && buildTypedElementAccess(walker, value))
        return true;
    if (buildFormOnShapes(walker, value, InputsOfTypedOperation(walker, nullptr)))
        return true;
    if (buildTypedIteration(walker, value) || buildTypedConstruction(walker, value) || buildInstanceOfBuiltin(walker, value, InputsOfTypedOperation(walker, nullptr)))
        return true;
    if (buildTestAgainstJoinOfTexts(walker, value))
        return true;
    if (buildTypedJoinOfTexts(walker, value))
        return true;
    if (buildNumberForm(walker, value))
        return true;
    return operatorFamiliesOfWalkTyped.contains(opcode) && buildTypedOperator(walker, value);
}

bool buildTypedOperation(Walker& walker, const BodyValue& value)
{
    using Links = bool (*)(Walker&, const BodyValue&);
    static constexpr auto links = []<size_t... opcodes>(std::index_sequence<opcodes...>) {
        return std::array<Links, sizeof...(opcodes)> { &buildTypedOperationOf<static_cast<Opcode>(opcodes)>... };
    }(std::make_index_sequence<numberOfEnumerators<Opcode>>());
    return links[static_cast<size_t>(value.opcode())](walker, value);
}

TypedForm typedFormOf(Walker& walker, const BodyValue& value, const TypesOfInputs& types)
{
    if (accessesThatThrowOffNullishInWalkTyped.contains(value.opcode())) {
        ValueID receiver = value.objectOperand();
        if (TypeID rest = typeOfReceiverBehindCheckInWalkTyped(walker, value, types(receiver))) {
            if (isNeverType(rest))
                return TypedForm::Built;
            return typedFormAsReceiverStands(walker, value, [&](ValueID input) {
                return input == receiver ? rest : types(input);
            });
        }
    }
    return typedFormAsReceiverStands(walker, value, types);
}

static TypedForm typedFormAsReceiverStands(Walker& walker, const BodyValue& value, const TypesOfInputs& types)
{
    Opcode opcode = value.opcode();
    if (buildTypedOperationOnStrings(walker, value, InputsOfTypedOperation(walker, &types)))
        return TypedForm::Built;
    if (accessFamiliesOfWalkTyped.contains(opcode)) {
        if (TypedForm form = typedFormOfAccess(walker, value, types); form != TypedForm::None)
            return form;
    }
    InputsOfTypedOperation inputs(walker, &types);
    if (elementFamiliesOfWalkTyped.contains(opcode) && buildTypedElementAccess(walker, value, inputs))
        return TypedForm::Built;
    if (buildFormOnShapes(walker, value, inputs))
        return TypedForm::Built;
    if (buildInstanceOfBuiltin(walker, value, inputs))
        return TypedForm::Built;
    return operatorFamiliesOfWalkTyped.contains(opcode) && buildTypedOperator(walker, value, inputs) ? TypedForm::Built : TypedForm::None;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
