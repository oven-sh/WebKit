/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypedArrayKinds.h"
#include "AOTWalkListReadsInLoops.h"
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
    Walker::StandIn keyInPlaceOfWritten(walker, value.propertyOperand(), key);
    if (!buildTypedElementAccess(walker, value))
        walker.copy(value);
}

static bool keyIsBoundToRemainderInNumberKeys(Walker& walker, ValueID writtenKey)
{
    Reference key = walker.referenceOf(writtenKey);
    if (!key.isValue())
        return false;
    const Code& code = walker.code();
    ValueID record = key.id();
    while (valuePreservingOpcodes.contains(code.at(record).opcode))
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
    if (value.opcode() == Opcode::ElemSet && value.valueOperand() == value.propertyOperand())
        return false;
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
    continueIfTrue(walker, divisorIsNotZero, whereDivisorIsZero);
    buildAccessUnderKeyInNumberKeys(walker, value, key);
    WayInNumberForms underInteger = endOfAccessInNumberKeys(walker, value);

    walker.continueIn(whereDivisorIsZero);
    buildAccessUnderKeyInNumberKeys(walker, value, walker.constant(PrimitiveKnownToFolding::number(PNaN)));
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

static TypeID numberToldFromUndefinedInKeyOfViewInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.opcode() == Opcode::ElemHas || !walker.opensGuesses())
        return { };
    TypeID keyID = inputs.typeOf(value.propertyOperand());
    if (!keyID || kindOfViewInNumberKeys(walker, inputs.typeOf(value.objectOperand())) == NotTypedArray)
        return { };
    Type key = walker.types().at(keyID);
    if (!key.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) || !(key.primitives & Primitive::Number) || !(key.primitives & Primitive::Undefined))
        return { };
    TypeID number = walker.types().withoutNullish(keyID);
    return hasTypedFormUnderKeyInNumberKeys(walker, value, inputs, number) ? number : TypeID();
}

bool tellsNumberFromUndefinedInKeyOfView(Walker& walker, const BodyValue& value, const TypesOfInputs& types)
{
    return opcodesOfNumberKeys.contains(value.opcode()) && hasMechanismsOfNumberForms(walker) && numberToldFromUndefinedInKeyOfViewInNumberKeys(walker, value, InputsOfTypedOperation(walker, &types));
}

static bool buildViewAccessUnderNumberOrUndefinedInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    TypeID number = numberToldFromUndefinedInKeyOfViewInNumberKeys(walker, value, inputs);
    if (!number)
        return false;

    Origin origin = originInFrontOfAccessInNumberKeys(walker, value);
    Reference writtenKey = walker.referenceOf(value.propertyOperand());
    Reference isNumber = buildTestOfNumber(walker, writtenKey, number, origin);
    if (!isNumber)
        return true;
    BlockID whereKeyIsUndefined = walker.addBlock(BlockFrequency::Rare);
    continueIfTrue(walker, isNumber, whereKeyIsUndefined);
    if (Reference proven = buildProofOfNumber(walker, writtenKey, isNumber, origin))
        buildAccessUnderKeyInNumberKeys(walker, value, proven);
    WayInNumberForms underNumber = endOfAccessInNumberKeys(walker, value);

    walker.continueIn(whereKeyIsUndefined);
    WayInNumberForms underUndefined;
    {
        Walker::ClosedArm closedArm(walker, true);
        if (!walker.isUnreachable()) {
            buildAccessUnderKeyInNumberKeys(walker, value, walker.constant(PrimitiveKnownToFolding::undefined()));
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

static bool buildViewAccessUnderUnknownKeyInNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.opcode() == Opcode::ElemHas || !walker.opensGuesses() || !walker.isInLoop())
        return false;
    TypeID keyID = inputs.typeOf(value.propertyOperand());
    if (!keyID || kindOfViewInNumberKeys(walker, inputs.typeOf(value.objectOperand())) == NotTypedArray)
        return false;
    TypeArena& types = walker.types();
    Type key = types.at(keyID);
    if (key.isNumber() || !key.isJavaScriptValue() || !(key.primitives & Primitive::Integer))
        return false;
    ExpectationOfValue expectation = findExpectation(walker, value.propertyOperand());
    if (expectation.type && !types.overlap(expectation.type, TypeTable::int32))
        return false;
    if (!hasTypedFormUnderKeyInNumberKeys(walker, value, inputs, TypeTable::int32))
        return false;

    Origin origin = originInFrontOfAccessInNumberKeys(walker, value);
    Reference writtenKey = walker.referenceOf(value.propertyOperand());
    Reference isWhole = writtenKey && writtenKey.isValue() ? buildTestOfNumber(walker, writtenKey, TypeTable::int32, origin) : Reference();
    if (!isWhole)
        return false;
    BlockID whereKeyIsAnother = walker.addBlock(BlockFrequency::Rare);
    continueIfTrue(walker, isWhole, whereKeyIsAnother);
    if (Reference proven = buildProofOfNumber(walker, writtenKey, isWhole, origin))
        buildAccessUnderKeyInNumberKeys(walker, value, proven);
    WayInNumberForms underWhole = endOfAccessInNumberKeys(walker, value);

    walker.continueIn(whereKeyIsAnother);
    WayInNumberForms underAnother;
    {
        Walker::ClosedArm closedArm(walker, true);
        if (!walker.isUnreachable()) {
            walker.copy(value);
            underAnother = endOfAccessInNumberKeys(walker, value);
        }
    }
    walker.recordSpeculation();
    if (!underAnother.end) {
        if (underWhole.end)
            walker.continueIn(underWhole.end);
        walker.bind(value.id(), underWhole.brought);
        return true;
    }
    std::array<WayInNumberForms, 2> ways { underWhole, underAnother };
    if (Reference met = meetInNumberForms(walker, walker.addBlock(), ways))
        walker.bind(value.id(), met);
    return true;
}

static bool buildListAccessUnderNumberAskedForInt32InNumberKeys(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (value.opcode() == Opcode::ElemHas || !walker.opensGuesses() || !walker.isInLoop() || !walker.canBuildIterationGuard())
        return false;
    if (!keyOfProvenListMayBeAskedForInt32(walker.types(), inputs.typeOf(value.objectOperand()), inputs.typeOf(value.propertyOperand())))
        return false;
    Reference writtenKey = walker.referenceOf(value.propertyOperand());
    if (!writtenKey || !writtenKey.isValue() || walker.state().checkedNameOf(writtenKey) || walker.code().at(writtenKey.id()).representation != Representation::Double)
        return false;
    std::optional<HoistedArrayGuard> tested = findHoistedArrayGuard(walker, value);
    if (!tested || !tested->turnsStoreIntoLists || !hasTypedFormUnderKeyInNumberKeys(walker, value, inputs, TypeTable::int32))
        return false;

    Origin origin = originInFrontOfAccessInNumberKeys(walker, value);
    Reference isInt32 = buildTestOfNumber(walker, writtenKey, TypeTable::int32, origin);
    if (!isInt32 || !isInt32.isValue())
        return false;
    BlockID whereKeyIsAnother = walker.addBlock(BlockFrequency::Rare);
    continueIfTrue(walker, isInt32, whereKeyIsAnother);
    BlockID whereKeyIsInt32 = walker.currentBlock();
    walker.continueIn(whereKeyIsAnother);
    if (!walker.isUnreachable())
        walker.restartIteration();
    walker.continueIn(whereKeyIsInt32);
    walker.recordSpeculation();
    if (walker.isUnreachable() || walker.hasFailed())
        return true;
    Reference proven = buildProofOfNumber(walker, writtenKey, isInt32, origin);
    if (!proven)
        return true;
    if (proven.isValue() && proven != writtenKey)
        walker.state().noteCheckedName(writtenKey, proven);
    walker.count(StatisticID::Int32KeysGuardedInIterations);
    buildAccessUnderKeyInNumberKeys(walker, value, proven);
    return true;
}

bool buildElementAccessUnderNumber(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    if (!opcodesOfNumberKeys.contains(value.opcode()) || !hasMechanismsOfNumberForms(walker))
        return false;
    if (inputs.areOnlyTypes())
        return false;
    if (buildViewAccessUnderNumberOrUndefinedInNumberKeys(walker, value, inputs))
        return true;

    if (buildViewAccessUnderUnknownKeyInNumberKeys(walker, value, inputs))
        return true;
    TypeID key = inputs.typeOf(value.propertyOperand());
    if (!key || !walker.types().at(key).isNumber() || !inputs.typeOf(value.objectOperand()))
        return false;
    if (buildAccessUnderWholeRemainderInNumberKeys(walker, value, inputs))
        return true;
    if (buildAccessUnderCodeUnitInNumberKeys(walker, value, inputs))
        return true;
    return buildListAccessUnderNumberAskedForInt32InNumberKeys(walker, value, inputs);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
