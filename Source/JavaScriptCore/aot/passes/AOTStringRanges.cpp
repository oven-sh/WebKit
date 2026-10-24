/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringRanges.h"

#if ENABLE(AOT)

#include "AOTBuiltinCallPlan.h"
#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRuleSupport.h"
#include "AOTSplitStringWalk.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t startResult = 0;
static constexpr uint32_t endResult = 1;

static constexpr InlineBuiltin inlineBuiltinOf(StringRangeMethod method)
{
    switch (method) {
    case StringRangeMethod::Slice:
        return InlineBuiltin::StringSlice;
    case StringRangeMethod::Substring:
        return InlineBuiltin::StringSubstring;
    case StringRangeMethod::Substr:
        return InlineBuiltin::StringSubstr;
    case StringRangeMethod::SingleIndex:
        return InlineBuiltin::StringCharAt;
    case StringRangeMethod::SingleRelativeIndex:
        return InlineBuiltin::StringAt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static std::optional<StringRangeSource> sourceOfBuiltinCall(const Program& program, const Function& function, const Value& call)
{
    if (call.builtinCallForm() != BuiltinCallForm::Whole || overloadOf(program, call).result.substringOf != BuiltinOperand::receiver())
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, call);
    auto method = std::ranges::find_if(allMembersOf<StringRangeMethod>, [&](StringRangeMethod method) { return inlineBuiltinOf(method) == builtin; });
    ValueID receiver = call.receiverOperand();
    if (method == allMembersOf<StringRangeMethod>.end() || !receiver || !program.typeOf(function, receiver).isAnyString())
        return std::nullopt;

    StringRangeSource source { .string = receiver, .method = *method, .mayBeUndefined = *method == StringRangeMethod::SingleRelativeIndex };
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (arguments.size() > (source.isSingleUnit() ? 1u : 2u))
        return std::nullopt;
    std::array<ValueID*, 2> positions { &source.first, &source.second };
    for (size_t index = 0; index < arguments.size(); ++index) {
        const Type& type = program.typeOf(function, arguments[index]);
        if (type.isNumber())
            *positions[index] = arguments[index];
        else if (type.isNever() || !type.hasOnlyPrimitives(Primitive::Undefined))
            return std::nullopt;
    }
    return source;
}

std::optional<StringRangeSource> stringRangeSourceOf(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Substring || value.opcode == Opcode::SubstringOrUndefined)
        return StringRangeSource { .string = value.stringOperand(), .bounds = StringRangeBounds { value.startOperand(), value.endOperand() }, .mayBeUndefined = value.opcode == Opcode::SubstringOrUndefined };
    if (value.opcode == Opcode::CallBuiltin)
        return sourceOfBuiltinCall(program, function, value);
    if (value.opcode != Opcode::ElemGet)
        return std::nullopt;
    std::optional<ElementAccessPlan> plan = planElementAccessOf(program, function, value);
    // SingleIndex truncates the position like charAt, but s[1.5] and s[NaN] are undefined.
    if (!plan || plan->form != ElementAccessForm::String || !program.typeOf(function, value.propertyOperand()).isInteger())
        return std::nullopt;
    return StringRangeSource { .string = value.objectOperand(), .first = value.propertyOperand(), .method = StringRangeMethod::SingleIndex, .mayBeUndefined = true };
}

StringRangeBounds insertStringRange(IRBuilder& builder, ProgramPoint point, const StringRangeSource& source, std::optional<StringRangeBounds> enclosing)
{
    auto insert = [&](Value&& value) { return builder.insertBefore(point, WTF::move(value)); };
    StringRangeBounds bounds;
    if (source.bounds)
        bounds = *source.bounds;
    else {
        ValueID length = insert(enclosing ? IRBuilder::make(Opcode::Sub, { enclosing->end, enclosing->start }) : IRBuilder::make(Opcode::GetStringLength, { source.string }));
        ValueID first = source.first ? source.first : insert(makeInteger(0));
        Value range = IRBuilder::make(Opcode::StringRange, { length, first, source.second });
        range.setMethod(source.method);
        ValueID results = insert(WTF::move(range));
        bounds.start = insert(makeResultAt(results, startResult));
        bounds.end = insert(makeResultAt(results, endResult));
    }
    if (!enclosing)
        return bounds;
    ValueID start = insert(IRBuilder::make(Opcode::Add, { enclosing->start, bounds.start }));
    return { start, insert(IRBuilder::make(Opcode::Add, { enclosing->start, bounds.end })) };
}

Value makeStringRangeRead(Opcode read, ValueID string, std::optional<StringRangeBounds> bounds, ValueID position)
{
    RELEASE_ASSERT(familyOf(read) == OperationFamily::StringRangeRead);
    Value value = IRBuilder::make(read);
    value.setStringOperand(string);
    if (bounds) {
        value.setStartOperand(bounds->start);
        value.setEndOperand(bounds->end);
    }
    if (position)
        value.setPositionOperand(position);
    return value;
}

BuiltinCallSites collectBuiltinCallSites(FunctionPassContext& context)
{
    return BuiltinCallSites(context.sitesIn(SiteList::BuiltinCalls));
}

using RangeCalls = Vector<std::pair<ValueSite, StringRangeSource>>;

static PassOutcome expandRangeCalls(FunctionPassContext& context, const RangeCalls& calls)
{
    if (calls.isEmpty())
        return PassOutcome::Unchanged;

    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const auto& [site, source] : calls) {
        ReplacedEffects replaced;
        replaced.add({ context.program, context.inputs.effects }, function, function.at(site.value));

        builder.setOrigin(function.at(site.value).origin());
        StringRangeBounds bounds = insertStringRange(builder, site.point(), source, std::nullopt);

        Value& value = function.edit(site.value);
        value.become(Opcode::Substring);
        value.setStringOperand(source.string);
        value.setStartOperand(bounds.start);
        value.setEndOperand(bounds.end);
        checkRewrite(context, site, replaced);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

PassOutcome expandStringRanges(FunctionPassContext& context, const BuiltinCallSites& sites)
{
    RangeCalls calls;
    for (const ValueSite& site : sites) {
        std::optional<StringRangeSource> source = stringRangeSourceOf(context.program, context.function, context.function.at(site.value));
        // A Substring does not say that it holds at most one unit, and selection reads the code unit of charAt and at only from the call.
        if (source && !source->isSingleUnit())
            calls.append({ site, *source });
    }
    return expandRangeCalls(context, calls);
}

PassOutcome expandAppendedStringRanges(FunctionPassContext& context)
{
    const Function& function = context.function;
    RangeCalls calls;
    for (const ValueSite& site : collectBuiltinCallSites(context)) {
        const Value& value = function.at(site.value);
        const BuiltinForms* forms = context.program.builtins().formsOf(overloadOf(context.program, value).builtin);
        if (!forms || (forms->appendForm != InlineBuiltin::StringSliceAppend && forms->appendForm != InlineBuiltin::StringSubstringAppend))
            continue;
        std::optional<Use> use = context.analyses.useLists().singleUse(site.value);
        if (!use || use->kind != Use::Kind::Operand || use->block != site.block || function.at(use->user).opcode != Opcode::StringBuilderAppend)
            continue;
        if (std::optional<StringRangeSource> source = stringRangeSourceOf(context.program, function, value))
            calls.append({ site, *source });
    }
    return expandRangeCalls(context, calls);
}

std::optional<SplitString> splitStringOf(FunctionPassContext& pass, const Value& call)
{
    const Program& program = pass.program;
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || !call.receiverOperand() || call.argumentsOperands().size() != 1)
        return std::nullopt;
    const BuiltinImplementation& implementation = program.builtins().at(call.overload()).code.implementation;
    if (implementation.inlineBuiltin != InlineBuiltin::StringSplit || static_cast<SearchValueKind>(implementation.inlineParameters[0]) != SearchValueKind::String)
        return std::nullopt;

    ValueID string = call.receiverOperand();
    ValueID separator = call.argumentsOperands()[0];
    const Type& separatorType = program.typeOf(pass.function, separator);
    if (!program.typeOf(pass.function, string).isAnyString() || separatorType.isNever() || !separatorType.hasOnlyPrimitives(Primitive::LiteralString))
        return std::nullopt;
    if (std::ranges::any_of(separatorType.stringLiterals, [&](StringID text) { return program.strings[text].empty(); }))
        return std::nullopt;

    TypingContext typing { program, pass.function, pass.types };
    BuiltinOperandTypes operands { pass.function.at(string).type, { pass.function.at(separator).type, pass.types.intern(Type::integer(Range::length())) } };
    OverloadMatch match = matchOverload({ typing, Builtins::StringPrototypeIndexOf, BuiltinCallMode::Call, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty() || !match.overload)
        return std::nullopt;
    return SplitString { string, separator, match.overload };
}

Range positionRangeOfSplitString()
{
    // The end of a separator is the sum of an index and a length, and their types only say that each is at most the greatest length.
    return { 0, 2 * Range::length().maximum };
}

SplitStringStep appendSplitStringStep(IRBuilder& builder, const SplitString& splitString, ValueID position)
{
    TypeArena& types = builder.types();
    ValueID length = builder.append(Opcode::GetStringLength, { splitString.string });
    ValueID hasSubstring = builder.append(Opcode::LessEq, { position, length });
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(hasSubstring, 0, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID start = branch.provenOnTaken;

    ValueID found = builder.append(makeBuiltinCall(splitString.separatorSearch, BuiltinCallForm::Whole, { splitString.string, { splitString.separator, start } }, { }));
    ValueID isLast = builder.append(Opcode::Less, { found, builder.appendInteger(0) });

    BlockID merge = builder.addBlock(builder.function().at(builder.currentBlock()).frequency);
    ValueID end = builder.addParameter(merge, types.intern(Type::integer(Range::length())));
    ValueID next = builder.addParameter(merge, types.intern(Type::integer(positionRangeOfSplitString())));
    IRBuilder::ProvenBranch search = builder.emitBranchWithProof(isLast, 0, BlockFrequency::Normal, BlockFrequency::Normal);
    builder.emitJump(merge, { length, builder.append(Opcode::Add, { length, builder.appendInteger(1) }) });

    builder.moveToEndOf(search.notTaken);
    ValueID separatorLength = builder.append(Opcode::GetStringLength, { splitString.separator });
    builder.emitJump(merge, { search.provenOnNotTaken, builder.append(Opcode::Add, { search.provenOnNotTaken, separatorLength }) });

    builder.moveToEndOf(merge);
    return { branch.notTaken, start, end, next };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
