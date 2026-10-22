/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringRuleSupport.h"

#if ENABLE(AOT)

#include "AOTDecidedBranches.h"
#include "AOTEffectsOfOperands.h"
#include "AOTInductionVariables.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTPropertyAccessElements.h"
#include <algorithm>
#include <array>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct IndexedRead {
    InlineBuiltin builtin;
    Opcode read;
};

} // anonymous namespace

static constexpr std::array indexedReads {
    IndexedRead { InlineBuiltin::StringCharCodeAt, Opcode::StringRangeCodeUnitOrNaN },
    IndexedRead { InlineBuiltin::StringCodePointAt, Opcode::StringRangeCodePointOrUndefined },
    IndexedRead { InlineBuiltin::StringCharAt, Opcode::StringRangeCharacterOrEmpty },
};

static constexpr OpcodeSet comparisons = relationalOperators | OpcodeSet { Opcode::StrictEq, Opcode::Eq };

static CharacterRead createIndexedRead(ValueID user, Opcode rangeRead, ValueID position)
{
    return { .user = user, .rangeRead = rangeRead, .position = position };
}

static CharacterRead createReadOfWholeRange(ValueID user, Opcode rangeRead, StringID literal = { })
{
    return { .user = user, .rangeRead = rangeRead, .literal = literal, .needsBounds = true };
}

static std::optional<CharacterRead> readOfParseInt(SelectionContext& context, const Use& use, const Value& call)
{
    std::span<const ValueID> arguments = call.argumentsOperands();
    CharacterRead read = createReadOfWholeRange(use.user, Opcode::StringRangeParseInt);
    if (arguments.size() == 1)
        return read;
    if (arguments.size() != 2 || builtinOperandAt(call, use.operandIndex) != BuiltinOperand::argument(0) || !context.typeOf(arguments[1]).isInt32())
        return std::nullopt;
    read.radix = arguments[1];
    return read;
}

static std::optional<CharacterRead> readOfReceiver(SelectionContext& context, const Use& use, const Value& call, InlineBuiltin builtin)
{
    auto read = std::ranges::find(indexedReads, builtin, &IndexedRead::builtin);
    if (read == indexedReads.end() || !context.typeOf(call.receiverOperand()).isAnyString())
        return std::nullopt;

    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.empty())
        return createIndexedRead(use.user, read->read, { });
    if (arguments.size() > 1 || !context.typeOf(arguments[0]).isNumber())
        return std::nullopt;
    return createIndexedRead(use.user, read->read, arguments[0]);
}

static std::optional<CharacterRead> readOfBuiltinCall(SelectionContext& context, const Use& use, const Value& call)
{
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(context.program(), call);
    if (!builtin)
        return std::nullopt;
    if (builtinOperandAt(call, use.operandIndex).isReceiver())
        return readOfReceiver(context, use, call, *builtin);

    if (*builtin == InlineBuiltin::ParseInt)
        return readOfParseInt(context, use, call);
    if (*builtin == InlineBuiltin::CallNumberConstructor && !call.receiverOperand() && call.argumentsOperands().size() == 1)
        return createReadOfWholeRange(use.user, Opcode::StringRangeToNumber);
    return std::nullopt;
}

static std::optional<CharacterRead> readOfElement(SelectionContext& context, const Use& use, const Value& element)
{
    if (use.operandIndex != OperandIndex::ElemGet::object)
        return std::nullopt;
    std::optional<ElementAccessPlan> plan = planElementAccessOf(context.program(), context.function(), element);
    if (!plan || plan->form != ElementAccessForm::String)
        return std::nullopt;
    // A position that is known to lie inside the text finds a character, and the row that never answers undefined says so whatever becomes of its operands.
    bool mayBeUndefined = context.typeOf(use.user).primitives & Primitive::Undefined;
    return createIndexedRead(use.user, mayBeUndefined ? Opcode::StringRangeCharacterOrUndefined : Opcode::StringRangeCharacterOrEmpty, element.propertyOperand());
}

static std::optional<CharacterRead> readOfEquality(SelectionContext& context, const Use& use)
{
    std::optional<LiteralArgument> literal = comparedLiteralOf(context, use);
    if (!literal || context.pass().exceeds(LimitID::MaxInlineStringCompareLength, literal->text.length(), use.user))
        return std::nullopt;
    return createReadOfWholeRange(use.user, Opcode::StringRangeEqualsLiteral, literal->literal);
}

std::optional<CharacterRead> characterReadOf(SelectionContext& context, const Use& use)
{
    if (use.kind != Use::Kind::Operand || context.isClaimed(use.user))
        return std::nullopt;

    const Value& user = context.function().at(use.user);
    if (user.opcode == Opcode::CallBuiltin)
        return readOfBuiltinCall(context, use, user);
    if (user.opcode == Opcode::ElemGet)
        return readOfElement(context, use, user);
    if (user.opcode == Opcode::StrictEq || user.opcode == Opcode::Eq)
        return readOfEquality(context, use);
    if (user.opcode == Opcode::ToNumber || user.opcode == Opcode::ToNumeric)
        return createReadOfWholeRange(use.user, Opcode::StringRangeToNumber);
    return std::nullopt;
}

Value makeCharacterRead(const CharacterRead& read, ValueID string, std::optional<StringRangeBounds> bounds)
{
    RELEASE_ASSERT(bounds || !read.needsBounds);
    RELEASE_ASSERT(!!read.position == OperandPosition::position(read.rangeRead).has_value());

    Value value = makeStringRangeRead(read.rangeRead, string, bounds, read.position);
    if (read.rangeRead == Opcode::StringRangeEqualsLiteral)
        value.setLiteral(read.literal);
    if (read.radix)
        value.setRadixOperand(read.radix);
    return value;
}

std::optional<LiteralArgument> literalArgumentOf(SelectionContext& context, ValueID id)
{
    const Program& program = context.program();
    std::optional<StringID> literal = stringConstantOf(program, context.function(), id);
    if (!literal)
        return std::nullopt;
    return LiteralArgument { *literal, program.strings.toWTFString(*literal) };
}

std::optional<LiteralArgument> comparedLiteralOf(SelectionContext& context, const Use& use)
{
    if (use.kind != Use::Kind::Operand)
        return std::nullopt;
    const Function& function = context.function();
    const Value& comparison = function.at(use.user);
    if (!comparisons.contains(comparison.opcode) || isConditionOfBranchWithProofs(function, context.analyses().useLists(), use.user))
        return std::nullopt;

    bool isLeftOperand = OperandPosition::left(comparison.opcode) == use.operandIndex;
    return literalArgumentOf(context, isLeftOperand ? comparison.rightOperand() : comparison.leftOperand());
}

static bool isConstantPart(SelectionContext& context, ValueID part, const Type& type)
{
    if (std::optional<LiteralArgument> literal = literalArgumentOf(context, part))
        return literal->text.containsOnlyLatin1() && !context.pass().exceeds(LimitID::MaxLengthCopiedInline, literal->text.length(), part);
    return type.isSingleValue() && type.hasOnlyPrimitives(Primitive::UndefinedOrNull | Primitive::Boolean);
}

static bool isRangeWithSingleUserInBlock(SelectionContext& context, ValueID part)
{
    if (context.isClaimed(part))
        return false;
    std::optional<StringRangeSource> source = stringRangeSourceOf(context.program(), context.function(), context.function().at(part));
    if (!source || source->mayBeUndefined)
        return false;
    std::optional<ValueID> user = context.singleUser(part);
    if (!user)
        return false;

    const UseLists& useLists = context.analyses().useLists();
    ValueLocation definition = useLists.locationOf(part);
    return definition.placement == ValuePlacement::InBlock && definition.block == useLists.blockOf(*user);
}

StringBuilderPartForm partFormOf(SelectionContext& context, ValueID part, CodeShape shape)
{
    const Type& type = context.typeOf(part);
    if (isConstantPart(context, part, type))
        return StringBuilderPartForm::Constant;
    // In compact code a part is one call. The call for a string takes the string itself: it has no list of values to fill and does not ask what it was given.
    if (shape == CodeShape::Compact)
        return type.isAnyString() ? StringBuilderPartForm::String : StringBuilderPartForm::Value;
    if (type.isInteger())
        return StringBuilderPartForm::Integer;
    if (type.isNumber())
        return StringBuilderPartForm::Number;
    if (type.isBoolean())
        return StringBuilderPartForm::Boolean;
    if (type.isAnyString())
        return isRangeWithSingleUserInBlock(context, part) ? StringBuilderPartForm::StringRange : StringBuilderPartForm::String;
    if (type.isAnyStringOrNullish())
        return StringBuilderPartForm::StringOrNullish;
    return StringBuilderPartForm::Value;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
