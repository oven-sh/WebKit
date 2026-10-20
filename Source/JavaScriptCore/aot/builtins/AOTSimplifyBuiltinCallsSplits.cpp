/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTEffectsCheck.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTRuleSupport.h"
#include "AOTSplitStringWalk.h"
#include "AOTSplitUnions.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <span>
#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct LastPiece {
    ValueSite read;
    ValueID split;
    SplitString splitString;
    StringID reverseSearchLiteral;
};

} // anonymous namespace

// split takes its matches from the left, each after the end of the one before: "a:::b" split at "::" ends in ":b". The last match is the last occurrence only where no two occurrences overlap,
// which is where no proper prefix of the separator is also its suffix.
static bool canOverlapItself(StringView separator)
{
    unsigned length = separator.length();
    Vector<unsigned, 16> longestBorders(FillWith { }, length, 0u);
    unsigned border = 0;
    for (unsigned index = 1; index < length; ++index) {
        while (border && separator[index] != separator[border])
            border = longestBorders[border - 1];
        if (separator[index] == separator[border])
            ++border;
        longestBorders[index] = border;
    }
    return border;
}

static StringID findReverseSearchLiteral(FunctionPassContext& context, ValueID separator)
{
    const Program& program = context.program;
    StringID literal = program.typeOf(context.function, separator).singleStringLiteral();
    if (!literal)
        return { };
    Vector<char16_t> buffer;
    return canOverlapItself(program.strings.viewOf(literal, buffer)) ? StringID() : literal;
}

static std::optional<ValueID> findSingleUseDefinitionUsedOnce(FunctionPassContext& context, ValueID operand, ValueID user)
{
    std::optional<ValueID> definition = findSingleUseDefinition(context, operand, user);
    if (!definition || !context.analyses.useLists().singleUse(*definition))
        return std::nullopt;
    return definition;
}

static bool readsLengthOf(FunctionPassContext& context, const Value& read, ValueID array)
{
    switch (read.opcode) {
    case Opcode::CallBuiltin:
        return read.builtinCallForm() == BuiltinCallForm::Whole && inlineImplementationOf(context.program, read) == InlineBuiltin::ArrayLength && read.receiverOperand() == array;
    case Opcode::PropGet:
        return read.key() == context.program.keyOf(BuiltinNames::length) && read.objectOperand() == array;
    case Opcode::GetArrayLength:
        return read.arrayOperand() == array;
    default:
        return false;
    }
}

// a.at(-1) and a.pop(), where nothing else uses a.
static std::optional<ValueID> findArrayReadByCall(FunctionPassContext& context, const ValueSite& site)
{
    const Value& call = context.function.at(site.value);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !call.receiverOperand())
        return std::nullopt;

    BuiltinID builtin = context.program.builtins().at(call.overload()).builtin;
    std::span<const ValueID> arguments = call.argumentsOperands();
    bool readsLastElement = builtin == Builtins::ArrayPrototypePop
        ? arguments.empty()
        : builtin == Builtins::ArrayPrototypeAt && arguments.size() == 1 && integerConstantThroughCasts(context.program, context.function, arguments[0]) == -1;
    if (!readsLastElement)
        return std::nullopt;
    return findSingleUseDefinitionUsedOnce(context, call.receiverOperand(), site.value);
}

// a[a.length - 1], where nothing else uses a, the length or the index.
static std::optional<ValueID> findArrayReadAtLastIndex(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const Value& read = function.at(site.value);
    if (read.opcode != Opcode::ElemGet)
        return std::nullopt;

    std::optional<ValueID> index = findSingleUseDefinitionUsedOnce(context, read.propertyOperand(), site.value);
    if (!index || function.at(*index).opcode != Opcode::Sub || integerConstantThroughCasts(context.program, function, function.at(*index).rightOperand()) != 1)
        return std::nullopt;
    std::optional<ValueID> length = findSingleUseDefinitionUsedOnce(context, function.at(*index).leftOperand(), *index);

    ValueID array = read.objectOperand();
    if (!length || !readsLengthOf(context, function.at(*length), array) || context.analyses.useLists().useCount(array) != 2)
        return std::nullopt;
    if (!isCastWithoutTest(function.at(array).opcode))
        return array;
    return findSingleUseDefinitionUsedOnce(context, function.at(array).operand(0), array);
}

static std::optional<LastPiece> matchLastPiece(FunctionPassContext& context, const ValueSite& site)
{
    std::optional<ValueID> array = findArrayReadByCall(context, site);
    if (!array)
        array = findArrayReadAtLastIndex(context, site);
    if (!array)
        return std::nullopt;
    // A pop inside a loop with its split outside takes another element on every run.
    if (!context.analyses.blockCycles().isInEveryCycleOf(context.analyses.useLists().blockOf(*array), site.block))
        return std::nullopt;
    std::optional<SplitString> splitString = splitStringOf(context, context.function.at(*array));
    if (!splitString)
        return std::nullopt;
    return LastPiece { site, *array, *splitString, findReverseSearchLiteral(context, splitString->separator) };
}

static ValueID insertStartOfLastPiece(FunctionPassContext& context, IRBuilder& builder, const LastPiece& piece, ValueID string)
{
    ProgramPoint point = piece.read.point();
    auto insert = [&](Value&& value) {
        return builder.insertBefore(point, WTF::move(value));
    };

    Value search = IRBuilder::make(Opcode::StringLastIndexOfLiteral, { string, insert(makeInteger(INT32_MAX)) });
    search.setLiteral(piece.reverseSearchLiteral);
    ValueID found = insert(WTF::move(search));

    Vector<char16_t> buffer;
    unsigned separatorLength = context.program.strings.viewOf(piece.reverseSearchLiteral, buffer).length();
    ValueID afterSeparator = insert(IRBuilder::make(Opcode::Add, { found, insert(makeInteger(separatorLength)) }));
    if (separatorLength == 1)
        return afterSeparator;

    // Without a separator the piece starts at 0: the sign of -1 fills a word, whose complement masks the sum away.
    ValueID signs = insert(IRBuilder::make(Opcode::RShift, { found, insert(makeInteger(31)) }));
    return insert(IRBuilder::make(Opcode::BitAnd, { afterSeparator, insert(IRBuilder::make(Opcode::BitNot, { signs })) }));
}

static void emitReverseSearch(FunctionPassContext& context, IRBuilder& builder, const LastPiece& piece)
{
    Function& function = context.function;
    ReplacedEffects replaced;
    replaced.add({ context.program, context.inputs.effects }, function, function.at(piece.split));
    replaced.add({ context.program, context.inputs.effects }, function, function.at(piece.read.value));

    builder.setOrigin(function.at(piece.read.value).origin());
    ValueID string = piece.splitString.string;
    ValueID start = insertStartOfLastPiece(context, builder, piece, string);
    ValueID end = builder.insertBefore(piece.read.point(), IRBuilder::make(Opcode::GetStringLength, { string }));

    Value& read = function.at(piece.read.value);
    read.become(Opcode::Substring);
    read.setStringOperand(string);
    read.setStartOperand(start);
    read.setEndOperand(end);
    read.type = resultTypeOf(context.program, function, read, context.types);
    checkRewrite(context, piece.read, replaced);
}

// The walk cuts nothing on its way: it remembers where the substring it has reached begins.
static void emitWalkToLastPiece(FunctionPassContext& context, IRBuilder& builder, const SplitString& splitString, ProgramPoint read)
{
    IRBuilder::Expansion cut = builder.beginExpansion(read);

    ValueID zero = builder.appendInteger(0);
    BlockID header = builder.addBlock(context.function.at(builder.currentBlock()).frequency);
    ValueID position = builder.addParameter(header, context.types.intern(Type::integer(positionRangeOfSplitString())));
    ValueID startOfLast = builder.addParameter(header, context.types.intern(Type::integer(Range::length())));
    builder.emitJump(header, { zero, zero });

    builder.moveToEndOf(header);
    builder.append(Opcode::LoopHint, { });
    SplitStringStep step = appendSplitStringStep(builder, splitString, position);
    builder.emitJump(header, { step.next, step.start });

    builder.moveToEndOf(step.done);
    ValueID length = builder.append(Opcode::GetStringLength, { splitString.string });
    builder.endExpansion(cut, builder.append(Opcode::Substring, { splitString.string, startOfLast, length }));
}

static void emitForwardWalks(FunctionPassContext& context, IRBuilder& builder, std::span<const LastPiece> pieces)
{
    Vector<std::pair<ProgramPoint, const LastPiece*>, 2> walks;
    for (const LastPiece& piece : pieces) {
        if (!piece.reverseSearchLiteral)
            walks.append({ context.analyses.useLists().pointOf(piece.read.value), &piece });
    }
    std::ranges::sort(walks, std::ranges::greater { }, [](const auto& walk) {
        return std::pair { walk.first.block, walk.first.index };
    });
    for (const auto& [read, piece] : walks)
        emitWalkToLastPiece(context, builder, piece->splitString, read);
    builder.setHandler(std::nullopt);
}

unsigned simplifyLastPiecesOfSplits(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<LastPiece> pieces;
    forEachValueSite(context.analyses, function, [&](const ValueSite& site) {
        if (std::optional<LastPiece> piece = matchLastPiece(context, site))
            pieces.append(*piece);
    });
    if (pieces.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const LastPiece& piece : pieces) {
        if (piece.reverseSearchLiteral)
            emitReverseSearch(context, builder, piece);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();

    emitForwardWalks(context, builder, pieces.span());
    return static_cast<unsigned>(pieces.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
