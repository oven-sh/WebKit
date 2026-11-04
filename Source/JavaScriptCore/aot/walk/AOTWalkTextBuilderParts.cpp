/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations with LoopInvariants, what is appended to a text in a builder: `s += ", "` writes bytes, `s += i` digits, `s += name` goes through a stub, and what may be any primitive is told apart at run time.
// Every part is a primitive by its proven type, so no append runs code of the program. At a suspension: no text lies in a builder across one. At a throw: an append completes or leaves the builder as it was.
// Withheld: no text lies in a builder, and nobody asks.

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// A symbol throws where it is appended, as it does where it is joined.
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
    // Its code takes every cell for a string, and whatever is no cell for undefined or null.
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
    // No type names a text that the walk has made.
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

// `constant`: the text of an AppendConstant. Null: the form takes `value`.
static void buildPartInTextBuilderParts(Walker& walker, TextInStringBuilder& text, Reference name, Opcode form, Reference value, StringID constant, Origin origin)
{
    Draft part(form, origin);
    walker.chooseConservativeFields(part);
    part.setBuilderOperand(name);
    // A draft that becomes another operation loses what it held but for what the question sets.
    bool liesInFrame = becomeOperationOfStringBuilderInFrame(walker, text, name, part, formInFrameInTextBuilderParts(form));
    if (walker.isUnreachable())
        return;
    if (constant)
        part.setString(constant);
    else
        part.setValueOperand(value);

    text.stringOfState = { };
    text.lengthOfState = { };
    Reference cursor = walker.build(WTF::move(part));
    if (liesInFrame && cursor)
        noteCursorBehind(walker, text, cursor);
}

static void appendPartInTextBuilderParts(Walker& walker, TextInStringBuilder& text, Reference name, Reference part, Origin origin)
{
    if (!part || !part.isRecord()) {
        walker.fail("a part of a text in a builder names no record of Code"_s);
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
        walker.fail("a part of a text in a builder may run code"_s);
        return;
    }
    // The text of -0 is "0".
    if (const Operation& record = walker.code().at(part.id()); record.opcode == Opcode::Double && !record.number()) {
        part = walker.constant(Draft(Opcode::Integer, origin));
        type = walker.types().at(walker.typeOf(part));
    }

    Opcode form = narrowestFormInTextBuilderParts(type);
    // A number that somebody has taken out of its box is at hand in the box too, and the code of the part asks the tag of that: a small integer goes straight to its digits, without the way to a double and back.
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

void appendPartsToStringBuilder(Walker& walker, TextInStringBuilder& text, Reference name, std::span<const Reference> parts, Origin origin)
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
