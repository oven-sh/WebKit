/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include "AOTWalkStrings.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesTakenInTextBuilderParts = Primitive::All | Primitive::LiteralString | Primitive::LiteralSymbol;

static Opcode narrowestFormInTextBuilderParts(const Type& type)
{
    if (type.isInteger())
        return Opcode::StringBuilderAppendInteger;
    if (type.isNumber())
        return Opcode::StringBuilderAppendNumber;
    if (type.isBoolean())
        return Opcode::StringBuilderAppendBoolean;
    if (type.isAnyString())
        return Opcode::StringBuilderAppendString;
    if (type.hasOnlyPrimitives(Primitive::AnyString | Primitive::UndefinedOrNull))
        return Opcode::StringBuilderAppendStringOrNullish;
    return Opcode::StringBuilderAppendValue;
}

bool partHasFormOfItsOwnInStringBuilder(const Type& type)
{
    return narrowestFormInTextBuilderParts(type) != Opcode::StringBuilderAppendValue || ((type.primitives & Primitive::Number) && !(type.primitives & Primitive::AnyString));
}

static Opcode formInFrameInTextBuilderParts(Opcode form)
{
    switch (form) {
    case Opcode::StringBuilderAppendConstant:
        return Opcode::StackStringBuilderAppendConstant;
    case Opcode::StringBuilderAppendInteger:
        return Opcode::StackStringBuilderAppendInteger;
    case Opcode::StringBuilderAppendNumber:
        return Opcode::StackStringBuilderAppendNumber;
    case Opcode::StringBuilderAppendBoolean:
        return Opcode::StackStringBuilderAppendBoolean;
    case Opcode::StringBuilderAppendCodeUnit:
        return Opcode::StackStringBuilderAppendCodeUnit;
    case Opcode::StringBuilderAppendString:
        return Opcode::StackStringBuilderAppendString;
    case Opcode::StringBuilderAppendStringOrNullish:
        return Opcode::StackStringBuilderAppendStringOrNullish;
    default:
        RELEASE_ASSERT(form == Opcode::StringBuilderAppendValue);
        return Opcode::StackStringBuilderAppendValue;
    }
}

StringID textOfKnownPartOfStringBuilder(Walker& walker, Reference part)
{
    if (!part || !part.isRecord())
        return { };
    const Operation& record = walker.code().at(part.id());
    if (record.opcode == Opcode::String)
        return record.string();

    TypeID id = walker.typeOf(part);
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };
    const StringTable& strings = walker.program().strings;
    switch (type.primitives) {
    case Primitive::Undefined:
        return strings.textOfUndefined();
    case Primitive::Null:
        return strings.textOfNull();
    case Primitive::False:
        return strings.textOf(false);
    case Primitive::True:
        return strings.textOf(true);
    case Primitive::LiteralString:
        return singleStringLiteralOf(type);
    default:
        return { };
    }
}

static void buildPartInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Opcode form, Reference value, StringID constant, Origin origin)
{
    Draft part(form, origin);
    walker.chooseConservativeFields(part);
    part.setBuilderOperand(name);
    becomeOperationOfStringBuilderInFrame(walker, text, name, part, formInFrameInTextBuilderParts(form));
    if (walker.isUnreachable())
        return;
    if (constant)
        part.setString(constant);
    else
        part.setValueOperand(value);

    text.stringOfState = { };
    text.lengthOfState = { };
    walker.build(WTF::move(part));
}

static Reference codeUnitThatTextIsMadeOfInTextBuilderParts(Walker& walker, Reference part, Origin origin)
{
    Reference root = rootForStrings(walker, part);
    if (!root.isValue())
        return { };
    const Operation& made = walker.code().at(root.id());
    if (made.opcode != Opcode::CallInline || made.inlineBuiltin() != InlineBuiltin::StringFromCharCode || made.argumentsOperands().size() != 1)
        return { };
    Reference number = referenceToRecordForStrings(walker, made.argumentsOperands()[0]);
    if (!walker.types().at(walker.typeOf(number)).isNumber())
        return { };
    return buildOperatorForStrings(walker, Opcode::BitAnd, number, integerForStrings(walker, UINT16_MAX), origin);
}

static bool appendCharacterByItsCodeUnitInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference part, const Type& type, Origin origin)
{
    if (!type.hasOnlyPrimitives(Primitive::AnyString | Primitive::Undefined))
        return false;
    Reference codeUnit = codePointOfCharacterInSight(walker, part);
    if (!codeUnit)
        codeUnit = codeUnitThatTextIsMadeOfInTextBuilderParts(walker, part, origin);
    if (!codeUnit)
        return false;
    Range units = walker.types().at(walker.typeOf(codeUnit)).integers;
    if (units.isSubsetOf(Range { 0, UINT16_MAX })) {
        buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendCodeUnit, codeUnit, { }, origin);
        return true;
    }
    if (!units.isSubsetOf(Range { -1, UINT16_MAX }) || handlerReadsStringInBuilder(walker, text))
        return false;
    bool isUndefinedOutside = walker.types().at(walker.code().at(rootForStrings(walker, part).id()).type).primitives & Primitive::Undefined;
    Reference isInside = buildOperatorForStrings(walker, Opcode::LessEq, integerForStrings(walker, 0), codeUnit, origin);
    return buildArmsForStrings(walker, isInside, BlockFrequency::Unlikely, [&](std::span<Reference>) {
        buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendCodeUnit, buildProofBehindTestForStrings(walker, codeUnit, isInside, OperandIndex::LessEq::right, origin), { }, origin);
    }, [&](std::span<Reference>) {
        if (isUndefinedOutside)
            buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendConstant, { }, walker.program().strings.textOfUndefined(), origin);
    }, { });
}

static void appendPartInTextBuilderParts(Walker& walker, StringInBuilder& text, Reference name, Reference part, Origin origin)
{
    if (!part || !part.isRecord()) {
        walker.fail("a part of a string in a builder is not a record of Code"_s);
        return;
    }
    if (StringID known = textOfKnownPartOfStringBuilder(walker, part)) {
        if (known != walker.program().strings.emptyString())
            buildPartInTextBuilderParts(walker, text, name, Opcode::StringBuilderAppendConstant, { }, known, origin);
        return;
    }

    TypeID proven = walker.typeOf(part);
    Type type = proven ? walker.types().at(proven) : Type();
    if (!proven || !type.hasOnlyPrimitives(primitivesTakenInTextBuilderParts)) {
        walker.fail("a part of a string in a builder may run code"_s);
        return;
    }
    if (const Operation& record = walker.code().at(part.id()); record.opcode == Opcode::Double && !record.number()) {
        part = walker.constant(Draft(Opcode::Integer, origin));
        type = walker.types().at(walker.typeOf(part));
    }

    if (appendCharacterByItsCodeUnitInTextBuilderParts(walker, text, name, part, type, origin))
        return;
    Opcode form = narrowestFormInTextBuilderParts(type);
    if (const Operation& record = walker.code().at(part.id()); form == Opcode::StringBuilderAppendNumber && record.opcode == Opcode::Unbox && part.isValue()) {
        Reference boxed = Reference::value(record.valueOperand());
        if (walker.representationOf(boxed) == Representation::JSValue && walker.types().at(walker.typeOf(boxed)).isNumber())
            part = boxed;
    }
    if (form == Opcode::StringBuilderAppendString && (appendPieceWithoutCutting(walker, text, name, part, origin) || appendResultOfBuiltinStraight(walker, text, name, part, origin))) {
        text.stringOfState = { };
        text.lengthOfState = { };
        return;
    }
    buildPartInTextBuilderParts(walker, text, name, form, part, { }, origin);
}

void appendPartsToStringBuilder(Walker& walker, StringInBuilder& text, Reference name, std::span<const Reference> parts, Origin origin)
{
    origin.isTailCall = false;
    for (Reference part : parts) {
        if (walker.isUnreachable())
            return;
        appendPartInTextBuilderParts(walker, text, name, part, origin);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
