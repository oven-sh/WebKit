/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet opcodesOfNumberKeys { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };

static Origin originInFrontOfAccessInNumberKeys(Walker& walker, const BodyValue& value)
{
    Origin origin = walker.originOf(value);
    origin.isTailCall = false;
    return origin;
}

static TypedArrayType kindOfViewInNumberKeys(Walker& walker, TypeID base)
{
    if (!base || !walker.types().at(base).isObject())
        return NotTypedArray;
    TypedArrayType kind = typedArrayTypeOf(BuiltinTypeContext { walker.program(), walker.types() }, base);
    return isTypedView(kind) ? kind : NotTypedArray;
}

static bool isNeverIndexOfView(const Type& key)
{
    if (key.isNever() || !key.hasOnlyPrimitives(Primitive::Integer | Primitive::FractionalNumber))
        return false;
    return !(key.primitives & Primitive::Integer) || key.integers.maximum < 0;
}

static bool buildAccessOutsideEveryViewInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (inputs.areOnlyTypes() || opcode == Opcode::ElemHas || !walker.has(Mechanism::Folding))
        return false;
    TypeID key = inputs.typeOf(value.propertyOperand());
    if (!key || !isNeverIndexOfView(walker.types().at(key)) || kindOfViewInNumberKeys(walker, inputs.typeOf(value.objectOperand())) == NotTypedArray)
        return false;
    if (opcode == Opcode::ElemGet) {
        walker.bind(value.id(), constantInNumberForms(walker, PrimitiveKnownToFolding::undefined()));
        return true;
    }
    TypeID stored = inputs.typeOf(value.valueOperand());
    return stored && !isNeverType(stored) && walker.types().at(stored).isNumberLike() && !isBigInt(kindOfViewInNumberKeys(walker, inputs.typeOf(value.objectOperand())));
}

static bool buildStoreOfNumberLikeInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.opcode() != Opcode::ElemSet)
        return false;
    TypeID keyID = inputs.typeOf(value.propertyOperand());
    TypeID storedID = inputs.typeOf(value.valueOperand());
    if (!keyID || !storedID || isNeverType(storedID))
        return false;
    Type key = walker.types().at(keyID);
    Type stored = walker.types().at(storedID);
    if (!key.isNumber() || !stored.isNumberLike() || stored.isNumber())
        return false;
    TypedArrayType kind = kindOfViewInNumberKeys(walker, inputs.typeOf(value.objectOperand()));
    if (kind == NotTypedArray || isBigInt(kind))
        return false;
    if (inputs.areOnlyTypes()) {
        walker.noteFormAskedAhead(Opcode::PutTypedArrayElement, KnowledgeLevel::Proven);
        return true;
    }

    Reference number = buildOperatorInNumberForms(walker, Opcode::ToNumber, walker.referenceOf(value.valueOperand()), { }, originInFrontOfAccessInNumberKeys(walker, value));
    if (!number)
        return true;
    ECMAMode mode = value.mode();
    Draft put = walker.draftOf(value);
    ExpectedType expected = put.expected;
    ValueID inputValueGiven = put.inputValueGiven;
    put.become(Opcode::PutTypedArrayElement);
    put.expected = expected;
    put.inputValueGiven = inputValueGiven;
    put.setViewOperand(walker.referenceOf(value.objectOperand()));
    put.setPositionOperand(walker.referenceOf(value.propertyOperand()));
    put.setValueOperand(number);
    put.setTypedArrayType(kind);
    put.setMode(mode);
    put.setLevel(KnowledgeLevel::Proven);
    put.setKeyIsIntegral(key.isNumberWithoutFraction());
    walker.bind(value.id(), walker.build(WTF::move(put)));
    return true;
}

static bool hasTypedFormUnderKeyInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs, TypeID key)
{
    ValueID writtenKey = value.propertyOperand();
    auto typeUnderKey = [&](ValueID input) {
        return input == writtenKey ? key : inputs.typeOf(input);
    };
    TypedForm form = typedFormOf(walker, value, TypesOfInputs(typeUnderKey));
    walker.takeFormAskedAhead();
    return form == TypedForm::Built;
}

static void buildAccessUnderKeyInNumberKeys(Walker& walker, const BodyValue& value, Reference key)
{
    ValueID writtenKey = value.propertyOperand();
    Reference bound = walker.referenceBoundTo(writtenKey);
    walker.bind(writtenKey, key);
    if (!buildTypedElementAccess(walker, value))
        walker.copy(value);
    walker.bind(writtenKey, bound);
}

static bool keyIsBoundToRemainderInNumberKeys(Walker& walker, ValueID writtenKey)
{
    Reference key = walker.referenceOf(writtenKey);
    if (!key.isValue())
        return false;
    const Code& code = walker.code();
    ValueID record = key.id();
    while (opcodesThatGiveSameValue.contains(code.at(record).opcode))
        record = code.at(record).operand(0);
    return code.at(record).opcode == Opcode::Mod;
}

static WayInNumberForms endOfAccessInNumberKeys(Walker& walker, const BodyValue& value)
{
    if (walker.isUnreachable())
        return { };
    return { walker.currentBlock(), walker.referenceBoundTo(value.id()), { } };
}

static bool buildAccessUnderWholeRemainderInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    std::optional<WholeRemainder> remainder = findWholeRemainder(walker, value.propertyOperand(), inputs);
    if (!remainder || (remainder->mayBeNaN && !walker.opensGuesses()) || !keyIsBoundToRemainderInNumberKeys(walker, value.propertyOperand()))
        return false;
    TypeID whole = typeOfWholeRemainder(walker, inputs.typeOf(remainder->dividend), inputs.typeOf(remainder->divisor));
    if (!whole || !hasTypedFormUnderKeyInNumberKeys(walker, value, inputs, whole))
        return false;

    Origin origin = originInFrontOfAccessInNumberKeys(walker, value);
    Reference key = buildWholeRemainder(walker, origin, *remainder);
    if (!key)
        return true;
    if (!remainder->mayBeNaN) {
        buildAccessUnderKeyInNumberKeys(walker, value, key);
        return true;
    }

    Reference divisorIsNotZero = buildOperatorInNumberForms(walker, Opcode::ToBoolean, walker.referenceOf(remainder->divisor), { }, origin);
    if (!divisorIsNotZero)
        return true;
    BlockID whereDivisorIsZero = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    goOnWhereTestPassesInNumberForms(walker, divisorIsNotZero, whereDivisorIsZero);
    buildAccessUnderKeyInNumberKeys(walker, value, key);
    WayInNumberForms underInteger = endOfAccessInNumberKeys(walker, value);

    walker.continueIn(whereDivisorIsZero);
    buildAccessUnderKeyInNumberKeys(walker, value, constantInNumberForms(walker, PrimitiveKnownToFolding::number(PNaN)));
    std::array<WayInNumberForms, 2> ways { underInteger, endOfAccessInNumberKeys(walker, value) };
    if (Reference met = meetInNumberForms(walker, meeting, ways))
        walker.bind(value.id(), met);
    return true;
}

static bool buildAccessUnderCodeUnitInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Reference codeUnit = integerThatCharCodeIsKnownToBe(walker, value, value.propertyOperand());
    if (!codeUnit || !hasTypedFormUnderKeyInNumberKeys(walker, value, inputs, walker.typeOf(codeUnit)))
        return false;
    buildAccessUnderKeyInNumberKeys(walker, value, codeUnit);
    return true;
}

static bool buildViewAccessUnderNumberOrUndefinedInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.opcode() == Opcode::ElemHas || !walker.opensGuesses())
        return false;
    TypeID keyID = inputs.typeOf(value.propertyOperand());
    if (!keyID || kindOfViewInNumberKeys(walker, inputs.typeOf(value.objectOperand())) == NotTypedArray)
        return false;
    Type key = walker.types().at(keyID);
    if (!key.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) || !(key.primitives & Primitive::Number) || !(key.primitives & Primitive::Undefined))
        return false;
    TypeID number = walker.types().withoutNullish(keyID);
    if (!hasTypedFormUnderKeyInNumberKeys(walker, value, inputs, number))
        return false;

    Origin origin = originInFrontOfAccessInNumberKeys(walker, value);
    Reference writtenKey = walker.referenceOf(value.propertyOperand());
    Reference isNumber = buildTestOfNumber(walker, writtenKey, number, origin);
    if (!isNumber)
        return true;
    BlockID whereKeyIsUndefined = walker.addBlock(BlockFrequency::Rare);
    goOnWhereTestPassesInNumberForms(walker, isNumber, whereKeyIsUndefined);
    if (Reference proven = buildProofOfNumber(walker, writtenKey, isNumber, origin))
        buildAccessUnderKeyInNumberKeys(walker, value, proven);
    WayInNumberForms underNumber = endOfAccessInNumberKeys(walker, value);

    walker.continueIn(whereKeyIsUndefined);
    WayInNumberForms underUndefined;
    {
        Walker::ClosedArm closedArm(walker, true);
        if (!walker.isUnreachable()) {
            buildAccessUnderKeyInNumberKeys(walker, value, constantInNumberForms(walker, PrimitiveKnownToFolding::undefined()));
            underUndefined = endOfAccessInNumberKeys(walker, value);
        }
    }
    if (!underUndefined.end) {
        if (underNumber.end)
            walker.continueIn(underNumber.end);
        walker.bind(value.id(), underNumber.brought);
        return true;
    }
    std::array<WayInNumberForms, 2> ways { underNumber, underUndefined };
    if (Reference met = meetInNumberForms(walker, walker.addBlock(), ways))
        walker.bind(value.id(), met);
    return true;
}

bool buildElementAccessUnderNumber(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (!opcodesOfNumberKeys.contains(value.opcode()) || !hasMechanismsOfNumberForms(walker))
        return false;
    if (inputs.areOnlyTypes())
        return buildStoreOfNumberLikeInNumberKeys(walker, value, inputs);
    if (buildViewAccessUnderNumberOrUndefinedInNumberKeys(walker, value, inputs))
        return true;

    TypeID key = inputs.typeOf(value.propertyOperand());
    if (!key || !walker.types().at(key).isNumber() || !inputs.typeOf(value.objectOperand()))
        return false;
    if (buildAccessOutsideEveryViewInNumberKeys(walker, value, inputs))
        return true;
    if (buildAccessUnderWholeRemainderInNumberKeys(walker, value, inputs))
        return true;
    if (buildAccessUnderCodeUnitInNumberKeys(walker, value, inputs))
        return true;
    return buildStoreOfNumberLikeInNumberKeys(walker, value, inputs);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
