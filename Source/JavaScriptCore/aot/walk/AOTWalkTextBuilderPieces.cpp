/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include "AOTWalkTextBuilderRows.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t startOfRangeInTextBuilderPieces = 0;
static constexpr uint32_t endOfRangeInTextBuilderPieces = 1;
static constexpr int64_t decimalRadixInTextBuilderPieces = 10;
static constexpr int64_t hexadecimalRadixInTextBuilderPieces = 16;
static constexpr int64_t digitsOfByteInTextBuilderPieces = 2;

namespace {

struct RangeInTextBuilderPieces {
    Reference string;
    Reference start;
    Reference end;
};

} // anonymous namespace

static Reference referenceToRecordInTextBuilderPieces(const Code& code, ValueID record)
{
    return code.isConstant(record) ? Reference::constant(record) : Reference::value(record);
}

static Type typeOfRecordInTextBuilderPieces(const Walker& walker, ValueID record)
{
    TypeID type = walker.typeOf(referenceToRecordInTextBuilderPieces(walker.code(), record));
    return type ? walker.types().at(type) : Type();
}

static bool isUndefinedInTextBuilderPieces(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::Undefined);
}

static ValueID findRecordMadeToBeAppendedInTextBuilderPieces(const Walker& walker, Reference part)
{
    if (walker.isUnreachable() || !part.isValue())
        return { };
    const Code& code = walker.code();
    ValueID made = underlyingCellOf(code, part.id());
    if (code.isConstant(made) || code.blockOf(made) != walker.currentBlock() || !code.at(made).traits().isRemovableWhenUnused())
        return { };
    return made;
}

static std::optional<InlineBuiltin> codeInLineOfRowCalledInTextBuilderPieces(const Walker& walker, const Operation& call)
{
    if (call.opcode == Opcode::CallInline)
        return call.inlineBuiltin();
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    return walker.program().builtins().at(call.overload()).code.implementation.inlineBuiltin;
}

static ValueID rangeGivingBoundInTextBuilderPieces(const Code& code, ValueID bound, uint32_t index)
{
    const Operation& result = code.at(underlyingCellOf(code, bound));
    if (result.opcode != Opcode::ResultAt || result.index() != index || code.at(result.resultsOperand()).opcode != Opcode::StringRange)
        return { };
    return result.resultsOperand();
}

static bool endIsSumWithStartInTextBuilderPieces(const Walker& walker, ValueID start, ValueID end)
{
    const Code& code = walker.code();
    const Operation& sum = code.at(underlyingCellOf(code, end));
    if (sum.opcode != Opcode::Add)
        return false;
    ValueID rootOfStart = underlyingCellOf(code, start);
    for (uint32_t side : { OperandIndex::Add::left, OperandIndex::Add::right }) {
        Type added = typeOfRecordInTextBuilderPieces(walker, sum.operand(1 - side));
        if (underlyingCellOf(code, sum.operand(side)) == rootOfStart && added.isInteger() && added.integers.minimum >= 0)
            return true;
    }
    return false;
}

static bool isRangeInOrderInTextBuilderPieces(const Walker& walker, ValueID start, ValueID end)
{
    const Code& code = walker.code();
    if (ValueID range = rangeGivingBoundInTextBuilderPieces(code, start, startOfRangeInTextBuilderPieces)) {
        if (range == rangeGivingBoundInTextBuilderPieces(code, end, endOfRangeInTextBuilderPieces))
            return true;
        if (underlyingCellOf(code, code.at(range).lengthOperand()) == underlyingCellOf(code, end))
            return true;
    }
    if (endIsSumWithStartInTextBuilderPieces(walker, start, end))
        return true;
    Type typeOfStart = typeOfRecordInTextBuilderPieces(walker, start);
    Type typeOfEnd = typeOfRecordInTextBuilderPieces(walker, end);
    return typeOfStart.isInteger() && typeOfEnd.isInteger() && typeOfStart.integers.maximum <= typeOfEnd.integers.minimum;
}

static Reference stringCutFromInTextBuilderPieces(const Code& code, ValueID string)
{
    const Operation& root = code.at(underlyingCellOf(code, string));
    return referenceToRecordInTextBuilderPieces(code, root.opcode == Opcode::FlattenString ? root.stringOperand() : string);
}

static std::optional<RangeInTextBuilderPieces> rangeOfSubstringInTextBuilderPieces(const Walker& walker, ValueID made)
{
    const Code& code = walker.code();
    const Operation& cut = code.at(made);
    if (!isRangeInOrderInTextBuilderPieces(walker, cut.startOperand(), cut.endOperand()))
        return std::nullopt;
    Reference string = stringCutFromInTextBuilderPieces(code, cut.stringOperand());
    if (!typeOfRecordInTextBuilderPieces(walker, string.id()).isAnyString() || !isReadByNoRecordSoFar(walker, Reference::value(made)))
        return std::nullopt;
    return RangeInTextBuilderPieces { string, referenceToRecordInTextBuilderPieces(code, cut.startOperand()), referenceToRecordInTextBuilderPieces(code, cut.endOperand()) };
}

bool appendPieceWithoutCutting(Walker& walker, StringInBuilder& text, Reference name, Reference part, Origin origin)
{
    if (opcodesWithoutEmitter().contains(Opcode::StringBuilderAppendStringRange))
        return false;
    ValueID made = findRecordMadeToBeAppendedInTextBuilderPieces(walker, part);
    if (!made || walker.code().at(made).opcode != Opcode::Substring)
        return false;
    origin.isTailCall = false;
    std::optional<RangeInTextBuilderPieces> range = rangeOfSubstringInTextBuilderPieces(walker, made);
    if (!range)
        return false;
    if (underlyingCellOf(walker.code(), range->start.id()) == underlyingCellOf(walker.code(), range->end.id()))
        return true;

    Draft append(Opcode::StringBuilderAppendStringRange, origin);
    walker.chooseConservativeFields(append);
    append.setBuilderOperand(name);
    append.setStringOperand(range->string);
    append.setStartOperand(range->start);
    append.setEndOperand(range->end);
    becomeOperationOfStringBuilderInFrame(walker, text, name, append, Opcode::StackStringBuilderAppendStringRange);
    if (walker.isUnreachable())
        return true;
    text.stringOfState = { };
    text.lengthOfState = { };
    walker.build(WTF::move(append));
    return true;
}

static bool isDecimalRadixIfPassedInTextBuilderPieces(const Walker& walker, std::span<const ValueID> arguments)
{
    if (arguments.empty())
        return true;
    if (arguments.size() != 1)
        return false;
    Type radix = typeOfRecordInTextBuilderPieces(walker, arguments[0]);
    return isUndefinedInTextBuilderPieces(radix) || (radix.isInteger() && radix.integers == Range::one(decimalRadixInTextBuilderPieces));
}

static ValueID valueWhoseTextWasMadeInTextBuilderPieces(const Walker& walker, const Operation& made)
{
    ValueID value;
    if (made.opcode == Opcode::ToString)
        value = made.valueOperand();
    else if (std::optional<InlineBuiltin> codeInLine = codeInLineOfRowCalledInTextBuilderPieces(walker, made)) {
        std::span<const ValueID> arguments = made.argumentsOperands();
        if (*codeInLine == InlineBuiltin::NumberToString && made.receiverOperand() && isDecimalRadixIfPassedInTextBuilderPieces(walker, arguments))
            return typeOfRecordInTextBuilderPieces(walker, made.receiverOperand()).isNumber() ? made.receiverOperand() : ValueID();
        if (*codeInLine == InlineBuiltin::CallStringConstructor && arguments.size() == 1)
            value = arguments[0];
    }
    if (!value)
        return { };
    Type type = typeOfRecordInTextBuilderPieces(walker, value);
    return !type.isNever() && hasTextByItself(type) ? value : ValueID();
}

static bool isIntegerInTextBuilderPieces(const Walker& walker, ValueID value, int64_t integer)
{
    Type type = typeOfRecordInTextBuilderPieces(walker, value);
    return type.isInteger() && type.integers == Range::one(integer);
}

static bool isDigitZeroInTextBuilderPieces(const Walker& walker, ValueID value)
{
    const Operation& record = walker.code().at(underlyingCellOf(walker.code(), value));
    if (record.opcode != Opcode::String)
        return false;
    std::span<const uint8_t> characters = walker.program().strings[record.string()];
    return characters.size() == 1 && characters[0] == '0';
}

std::optional<HexadecimalText> findHexadecimalText(const Walker& walker, Reference part)
{
    ValueID made = findRecordMadeToBeAppendedInTextBuilderPieces(walker, part);
    if (!made || !isReadByNoRecordSoFar(walker, Reference::value(made)))
        return std::nullopt;
    const Code& code = walker.code();
    ValueID padding;
    if (const Operation& record = code.at(made); record.opcode == Opcode::CallInline && record.inlineBuiltin() == InlineBuiltin::StringPadStart) {
        std::span<const ValueID> arguments = record.argumentsOperands();
        if (!record.receiverOperand() || arguments.size() != 2 || !isIntegerInTextBuilderPieces(walker, arguments[0], digitsOfByteInTextBuilderPieces) || !isDigitZeroInTextBuilderPieces(walker, arguments[1]))
            return std::nullopt;
        padding = made;
        made = underlyingCellOf(code, record.receiverOperand());
    }
    const Operation& record = code.at(made);
    if (code.isConstant(made) || record.opcode != Opcode::CallInline || record.inlineBuiltin() != InlineBuiltin::NumberToString || !record.receiverOperand())
        return std::nullopt;
    std::span<const ValueID> arguments = record.argumentsOperands();
    if (arguments.size() != 1 || !isIntegerInTextBuilderPieces(walker, arguments[0], hexadecimalRadixInTextBuilderPieces) || !typeOfRecordInTextBuilderPieces(walker, record.receiverOperand()).isNumber())
        return std::nullopt;
    static constexpr OpcodeSet numberViews { Opcode::Unbox, Opcode::IntegerToNumber };
    Reference receiver = referenceToRecordInTextBuilderPieces(code, record.receiverOperand());
    Reference number = receiver;
    while (number.isValue() && numberViews.contains(code.at(number.id()).opcode))
        number = referenceToRecordInTextBuilderPieces(code, code.at(number.id()).operand(0));
    return HexadecimalText { number, receiver, made, padding };
}

static Reference buildCallAgainInTextBuilderPieces(Walker& walker, ValueID made, Reference receiver, Origin origin)
{
    if (walker.isUnreachable() || !receiver)
        return { };
    const Code& code = walker.code();
    const Operation& call = code.at(made);
    Vector<Reference, 2> arguments;
    for (ValueID argument : call.argumentsOperands())
        arguments.append(referenceToRecordInTextBuilderPieces(code, argument));
    Draft again(Opcode::CallInline, origin);
    walker.chooseConservativeFields(again);
    again.setInlineBuiltin(call.inlineBuiltin());
    again.setInlineParameters(call.inlineParameters());
    again.setOverload(call.overload());
    again.setReceiverOperand(receiver);
    again.setArgumentsOperands(arguments.span());
    return walker.build(WTF::move(again));
}

Reference buildHexadecimalTextAgain(Walker& walker, const HexadecimalText& text, Origin origin)
{
    Reference made = buildCallAgainInTextBuilderPieces(walker, text.conversion, text.receiver, origin);
    return text.padding ? buildCallAgainInTextBuilderPieces(walker, text.padding, made, origin) : made;
}

bool appendResultOfBuiltinStraight(Walker& walker, StringInBuilder& text, Reference name, Reference part, Origin origin)
{
    ValueID made = findRecordMadeToBeAppendedInTextBuilderPieces(walker, part);
    ValueID value = made ? valueWhoseTextWasMadeInTextBuilderPieces(walker, walker.code().at(made)) : ValueID();
    if (!value || !isReadByNoRecordSoFar(walker, Reference::value(made)))
        return false;
    Reference converted = referenceToRecordInTextBuilderPieces(walker.code(), value);
    appendPartsToStringBuilder(walker, text, name, singleElementSpan(converted), origin);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
