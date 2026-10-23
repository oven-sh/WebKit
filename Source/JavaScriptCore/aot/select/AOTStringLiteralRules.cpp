/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectionRules.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTInductionVariables.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTRegExpFlags.h"
#include "AOTRegExpSummary.h"
#include "AOTRepresentation.h"
#include "AOTSelectionContext.h"
#include "AOTStringArena.h"
#include "AOTStringRuleSupport.h"
#include "AOTVector.h"
#include "JSGlobalObjectFunctions.h"
#include "YarrFlags.h"
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class RootForm : uint8_t { Test, Index, Presence, Relation };

struct Match {
    SelectionGroup group;
    RootForm rootForm;
    Opcode row;
    ValueID subject;
    ValueID position { };
    int64_t defaultPosition { 0 };
    StringID literal { };
    StringView atom { };
    // The row has the text in its payload.
    ValueID droppedOperand { };
    uint32_t codeUnit { 0 };
    bool isMirrored { false };
    bool isOnlyBranchedOn { false };
};

enum class SubjectKind : uint8_t { String, Value };

struct ComparedLiteral {
    ValueID subject;
    ValueID operand;
    LiteralArgument argument;
    bool isOnLeft;
};

struct LiteralCall {
    ValueID string;
    ValueID operand;
    LiteralArgument argument;
    ValueID position;
};

} // anonymous namespace

static constexpr unsigned maxLengthOfSeparatorFoundInline = 16;

static constexpr std::array equalityOpcodes { Opcode::StrictEq, Opcode::Eq };
static constexpr std::array relationalOpcodes { Opcode::Less, Opcode::LessEq, Opcode::Greater, Opcode::GreaterEq };

static_assert([] {
    OpcodeSet opcodes;
    for (Opcode opcode : relationalOpcodes)
        opcodes.add(opcode);
    return opcodes == relationalOperators;
}());

static constexpr OptionSet<RegExpFlags> flagsPreventingStringSearch {
    RegExpFlags::Global, RegExpFlags::Sticky, RegExpFlags::IgnoreCase, RegExpFlags::Unicode, RegExpFlags::UnicodeSets
};

static bool isUndefined(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::Undefined);
}

static std::optional<ComparedLiteral> findComparedLiteral(SelectionContext& context, ValueID id, SubjectKind kind)
{
    const Value& comparison = context.function().at(id);
    BlockID block = context.analyses().useLists().blockOf(id);
    uint32_t leftIndex = *OperandPosition::left(comparison.opcode);
    uint32_t rightIndex = *OperandPosition::right(comparison.opcode);
    for (uint32_t subjectIndex : { leftIndex, rightIndex }) {
        ValueID subject = comparison.operand(subjectIndex);
        if (context.typeOf(subject).isAnyString() != (kind == SubjectKind::String))
            continue;
        Use useOfSubject { .kind = Use::Kind::Operand, .block = block, .user = id, .operandIndex = subjectIndex };
        if (std::optional<LiteralArgument> argument = comparedLiteralOf(context, useOfSubject))
            return ComparedLiteral { subject, comparison.operand(subjectIndex == leftIndex ? rightIndex : leftIndex), WTF::move(*argument), subjectIndex == rightIndex };
    }
    return std::nullopt;
}

// The rows take a cell or a boxed value. The operator compares two literal string words without decoding either.
static bool isLiteralStringWord(SelectionContext& context, ValueID id)
{
    return defaultRepresentationOf(context.typeOf(id)) == Representation::LiteralString;
}

static std::optional<Match> findEqualityOfValue(SelectionContext& context, ValueID id)
{
    std::optional<ComparedLiteral> compared = findComparedLiteral(context, id, SubjectKind::Value);
    if (!compared || isLiteralStringWord(context, compared->subject))
        return std::nullopt;

    bool isStrict = context.function().at(id).opcode == Opcode::StrictEq;
    const Type& subjectType = context.typeOf(compared->subject);
    if (isStrict && defaultRepresentationOf(subjectType) != Representation::JSValue)
        return std::nullopt;
    // StrictEq reads no string where one operand is never one, and the row reads string storage whatever it is given.
    if (isStrict && !(subjectType.primitives & Primitive::AnyString))
        return std::nullopt;
    if (!isStrict && !std::isnan(jsToNumber(compared->argument.text)))
        return std::nullopt;

    Match match { groupOfOne(id), RootForm::Test, isStrict ? Opcode::ValueEqualsLiteral : Opcode::ValueLooselyEqualsLiteral, compared->subject };
    match.literal = compared->argument.literal;
    match.droppedOperand = compared->operand;
    match.isOnlyBranchedOn = context.isOnlyBranchedOn(id);
    return match;
}

static std::optional<Match> findEquality(SelectionContext& context, ValueID id)
{
    std::optional<ComparedLiteral> compared = findComparedLiteral(context, id, SubjectKind::String);
    if (!compared)
        return findEqualityOfValue(context, id);
    if (isLiteralStringWord(context, compared->subject))
        return std::nullopt;

    Match match { groupOfOne(id), RootForm::Test, Opcode::StringEqualsLiteral, compared->subject };
    match.literal = compared->argument.literal;
    match.droppedOperand = compared->operand;
    match.isOnlyBranchedOn = context.isOnlyBranchedOn(id);
    return match;
}

static std::optional<Match> findRelation(SelectionContext& context, ValueID id)
{
    std::optional<ComparedLiteral> compared = findComparedLiteral(context, id, SubjectKind::String);
    if (!compared || compared->argument.text.length() != 1)
        return std::nullopt;

    Match match { groupOfOne(id), RootForm::Relation, Opcode::StringOrderToCharacter, compared->subject };
    match.codeUnit = compared->argument.text[0];
    match.droppedOperand = compared->operand;
    match.isMirrored = compared->isOnLeft;
    return match;
}

static std::optional<LiteralCall> findLiteralCall(SelectionContext& context, const Value& call)
{
    ValueID receiver = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!receiver || arguments.empty() || arguments.size() > 2 || !context.typeOf(receiver).isAnyString())
        return std::nullopt;
    std::optional<LiteralArgument> argument = literalArgumentOf(context, arguments[0]);
    if (!argument)
        return std::nullopt;

    bool hasPosition = arguments.size() > 1 && !isUndefined(context.typeOf(arguments[1]));
    return LiteralCall { receiver, arguments[0], WTF::move(*argument), hasPosition ? arguments[1] : ValueID() };
}

static std::optional<Match> findEndTest(SelectionContext& context, ValueID id, Opcode row)
{
    std::optional<LiteralCall> call = findLiteralCall(context, context.function().at(id));
    if (!call || call->position)
        return std::nullopt;
    if (context.pass().exceeds(LimitID::MaxInlineStringCompareLength, call->argument.text.length(), id))
        return std::nullopt;

    Match match { groupOfOne(id), RootForm::Test, row, call->string };
    match.literal = call->argument.literal;
    match.droppedOperand = call->operand;
    match.isOnlyBranchedOn = context.isOnlyBranchedOn(id);
    return match;
}

static Match createForwardSearch(ValueID id, RootForm rootForm, ValueID string, StringView sought)
{
    Match match { groupOfOne(id), rootForm, Opcode::StringIndexOfLiteral, string };
    if (sought.length() == 1) {
        match.row = Opcode::StringIndexOfCharacter;
        match.codeUnit = sought[0];
    }
    return match;
}

static std::optional<Match> findForwardSearch(SelectionContext& context, ValueID id, RootForm rootForm)
{
    std::optional<LiteralCall> call = findLiteralCall(context, context.function().at(id));
    if (!call || call->argument.text.isEmpty())
        return std::nullopt;
    if (call->position && !context.typeOf(call->position).isNumber())
        return std::nullopt;

    Match match = createForwardSearch(id, rootForm, call->string, call->argument.text);
    match.literal = call->argument.literal;
    match.droppedOperand = call->operand;
    match.position = call->position;
    return match;
}

static std::optional<Match> findBackwardSearch(SelectionContext& context, ValueID id)
{
    std::optional<LiteralCall> call = findLiteralCall(context, context.function().at(id));
    if (!call || call->argument.text.isEmpty())
        return std::nullopt;
    if (call->position && !context.typeOf(call->position).isInteger())
        return std::nullopt;

    Match match { groupOfOne(id), RootForm::Index, Opcode::StringLastIndexOfLiteral, call->string };
    match.literal = call->argument.literal;
    match.droppedOperand = call->operand;
    match.position = call->position;
    match.defaultPosition = INT32_MAX;
    return match;
}

static std::optional<Match> findAtomSearch(SelectionContext& context, ValueID id, InlineBuiltin builtin)
{
    const Value& call = context.function().at(id);
    ValueID receiver = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!receiver || arguments.size() != 1)
        return std::nullopt;

    bool stringIsReceiver = builtin == InlineBuiltin::StringSearch;
    ValueID string = stringIsReceiver ? receiver : arguments[0];
    ValueID regExp = stringIsReceiver ? arguments[0] : receiver;
    if (!context.typeOf(string).isAnyString())
        return std::nullopt;

    std::optional<RegExpLiteralInType> literal = singleRegExpLiteralAsCreatedOf(context.typeOf(regExp));
    if (!literal || literal->flags().containsAny(flagsPreventingStringSearch) || context.flow().mayReadLegacyRegExpStatics())
        return std::nullopt;

    Vector<char16_t> patternBuffer;
    StringView pattern = context.program().strings.viewOf(literal->pattern, patternBuffer);
    const RegExpSummary& summary = context.build().regExpSummaries().summaryOf(literal->pattern, toYarrFlags(literal->flags()), pattern);
    if (summary.metadata != RegExpMetadata::Known || !summary.isValid() || summary.atom.isEmpty())
        return std::nullopt;

    Match match = createForwardSearch(id, stringIsReceiver ? RootForm::Index : RootForm::Presence, string, summary.atom);
    match.atom = summary.atom;
    match.droppedOperand = regExp;
    return match;
}

static std::optional<Match> findBuiltinCall(SelectionContext& context, ValueID id)
{
    const Value& call = context.function().at(id);
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(context.program(), call);
    if (!builtin)
        return std::nullopt;

    switch (*builtin) {
    case InlineBuiltin::StringStartsWith:
        return findEndTest(context, id, Opcode::StringStartsWithLiteral);
    case InlineBuiltin::StringEndsWith:
        return findEndTest(context, id, Opcode::StringEndsWithLiteral);
    case InlineBuiltin::StringIndexOf:
        return findForwardSearch(context, id, RootForm::Index);
    case InlineBuiltin::StringIncludes:
        return findForwardSearch(context, id, RootForm::Presence);
    case InlineBuiltin::StringLastIndexOf:
        return findBackwardSearch(context, id);
    case InlineBuiltin::StringSearch:
    case InlineBuiltin::RegExpTest:
        return findAtomSearch(context, id, *builtin);
    default:
        return std::nullopt;
    }
}

static void rewriteTest(GroupRewriter& rewriter, const Match& match)
{
    Value& test = rewriter.replaceRoot(match.row);
    bool takesString = OperandPosition::string(match.row).has_value();
    if (takesString)
        test.setStringOperand(match.subject);
    else
        test.setValueOperand(match.subject);
    test.setLiteral(match.literal);
    if (match.isOnlyBranchedOn)
        rewriter.emitTestAndBranch();
    else if (takesString)
        rewriter.leaveUnclaimed(match.group.root);
}

static StringID literalOf(SelectionContext& context, const Match& match)
{
    if (match.literal || !Value::opcodesWithLiteral.contains(match.row))
        return match.literal;
    return context.pass().strings.intern(match.atom);
}

static ValueID insertPosition(SelectionContext& context, GroupRewriter& rewriter, const Match& match)
{
    if (!match.position)
        return rewriter.insert(makeInteger(match.defaultPosition));

    // Position 0 lies in every string.
    if (const Type& type = context.typeOf(match.position); type.isInteger() && type.integers == Range::one(0))
        return match.position;

    ValueID length = rewriter.insert(IRBuilder::make(Opcode::GetStringLength, { match.subject }));
    Value range = IRBuilder::make(Opcode::StringRange, { length, match.position });
    range.setMethod(StringRangeMethod::Substring);
    ValueID bounds = rewriter.insert(WTF::move(range));
    return rewriter.insert(makeResultAt(bounds, 0));
}

static void fillSearch(Value& search, const Match& match, ValueID position, StringID literal)
{
    search.setStringOperand(match.subject);
    search.setPositionOperand(position);
    if (Value::opcodesWithLiteral.contains(search.opcode))
        search.setLiteral(literal);
    else
        search.setCodeUnit(match.codeUnit);
}

static void rewriteSearch(SelectionContext& context, GroupRewriter& rewriter, const Match& match)
{
    StringID literal = literalOf(context, match);
    ValueID position = insertPosition(context, rewriter, match);
    if (match.rootForm == RootForm::Index) {
        fillSearch(rewriter.replaceRoot(match.row), match, position, literal);
        rewriter.leaveUnclaimed(match.group.root);
        return;
    }

    Value search = IRBuilder::make(match.row);
    fillSearch(search, match, position, literal);
    ValueID found = rewriter.insert(WTF::move(search));
    rewriter.leaveUnclaimed(found);

    ValueID notFound = rewriter.insert(makeInteger(-1));
    ValueID isAbsent = rewriter.insert(IRBuilder::make(Opcode::StrictEq, { found, notFound }));
    rewriter.replaceRoot(Opcode::LogicalNot).setValueOperand(isAbsent);
}

static void rewriteRelation(GroupRewriter& rewriter, const Match& match)
{
    Opcode relation = rewriter.original(match.group.root).opcode;
    Value row = IRBuilder::make(match.row, { match.subject });
    row.setCodeUnit(match.codeUnit);
    ValueID order = rewriter.insert(WTF::move(row));
    rewriter.leaveUnclaimed(order);

    ValueID zero = rewriter.insert(makeInteger(0));
    Value& root = rewriter.replaceRoot(match.isMirrored ? mirroredComparison(relation) : relation);
    root.setLeftOperand(order);
    root.setRightOperand(zero);
}

static void rewriteMatch(SelectionContext& context, GroupRewriter& rewriter, const Match& match)
{
    switch (match.rootForm) {
    case RootForm::Test:
        rewriteTest(rewriter, match);
        return;
    case RootForm::Index:
    case RootForm::Presence:
        rewriteSearch(context, rewriter, match);
        return;
    case RootForm::Relation:
        rewriteRelation(rewriter, match);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static StringID findLiteralSeparator(SelectionContext& context, const Value& call)
{
    if (call.builtinCallForm() != BuiltinCallForm::Prefix || inlineImplementationOf(context.program(), call) != InlineBuiltin::StringSplitPrefix)
        return { };
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() != 1)
        return { };
    std::optional<LiteralArgument> separator = literalArgumentOf(context, arguments[0]);
    if (!separator || separator->text.isEmpty() || separator->text.length() > maxLengthOfSeparatorFoundInline || !separator->text.containsOnlyASCII())
        return { };
    return separator->literal;
}

static void recordLiteralSeparators(SelectionContext& context)
{
    Function& function = context.pass().function;
    uint32_t numberOfSeparators = 0;
    for (ValueID id : context.candidates(Opcode::CallBuiltin)) {
        if (context.isOnSlowPath(id))
            continue;
        StringID separator = findLiteralSeparator(context, function.at(id));
        if (!separator)
            continue;
        function.edit(id, ValueEdit::Definition).setLiteralSeparator(separator);
        ++numberOfSeparators;
    }
    context.didSelectForms(SelectionRuleID::StringLiteralForms, numberOfSeparators);
}

void selectStringLiteralForms(SelectionContext& context)
{
    recordLiteralSeparators(context);

    Vector<Match> matches;
    auto addMatches = [&](Opcode opcode, std::optional<Match> (*find)(SelectionContext&, ValueID)) {
        for (ValueID id : context.candidates(opcode)) {
            if (context.isOnSlowPath(id))
                continue;
            std::optional<Match> match = find(context, id);
            if (match && tryClaimWithoutOperand(context, SelectionRuleID::StringLiteralForms, match->group, match->droppedOperand) == ClaimResult::Claimed)
                matches.append(WTF::move(*match));
        }
    };

    for (Opcode opcode : equalityOpcodes)
        addMatches(opcode, findEquality);
    for (Opcode opcode : relationalOpcodes)
        addMatches(opcode, findRelation);
    addMatches(Opcode::CallBuiltin, findBuiltinCall);

    context.rewriteGroups(SelectionRuleID::StringLiteralForms, matches, [&](GroupRewriter& rewriter, const Match& match) {
        rewriteMatch(context, rewriter, match);
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
