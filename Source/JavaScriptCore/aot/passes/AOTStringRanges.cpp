/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStringRanges.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTPropertyAccessElements.h"
#include "AOTRuleSupport.h"
#include "AOTSplitUnions.h"
#include "AOTVector.h"
#include <array>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct RangeCall {
    ValueSite site;
    StringRangeSource source;
};

} // anonymous namespace

static constexpr uint32_t startResult = 0;
static constexpr uint32_t endResult = 1;

std::optional<StringRangeMethod> rangeMethodOf(InlineBuiltin builtin)
{
    for (StringRangeMethod method : allMembersOf<StringRangeMethod>) {
        if (inlineBuiltinOf(method) == builtin)
            return method;
    }
    return std::nullopt;
}

static std::optional<StringRangeSource> sourceOfBuiltinCall(const Program& program, const Function& function, const Value& call)
{
    if (call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    if (overloadOf(program, call).result.substringOf != BuiltinOperand::receiver())
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, call);
    if (!builtin)
        return std::nullopt;
    std::optional<StringRangeMethod> method = rangeMethodOf(*builtin);
    if (!method)
        return std::nullopt;
    ValueID receiver = call.receiverOperand();
    if (!receiver || !program.typeOf(function, receiver).isAnyString())
        return std::nullopt;

    StringRangeSource source { .string = receiver, .method = *method, .mayBeUndefined = *method == StringRangeMethod::SingleRelativeIndex };
    std::span<const ValueID> arguments = call.argumentsOperands();
    size_t maximumArgumentCount = source.isSingleUnit() ? 1 : 2;
    if (arguments.size() > maximumArgumentCount)
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

static std::optional<StringRangeSource> sourceOfElementRead(const Program& program, const Function& function, const Value& read)
{
    std::optional<ElementAccessPlan> plan = planElementAccessOf(program, function, read);
    if (!plan || plan->form != ElementAccessForm::String)
        return std::nullopt;
    // SingleIndex truncates the position like charAt, but s[1.5] and s[NaN] are undefined.
    ValueID key = read.propertyOperand();
    if (!program.typeOf(function, key).isInteger())
        return std::nullopt;
    return StringRangeSource { .string = read.objectOperand(), .first = key, .method = StringRangeMethod::SingleIndex, .mayBeUndefined = true };
}

std::optional<StringRangeSource> stringRangeSourceOf(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode == Opcode::Substring)
        return StringRangeSource { .string = value.stringOperand(), .bounds = StringRangeBounds { value.startOperand(), value.endOperand() } };
    if (value.opcode == Opcode::SubstringOrUndefined)
        return StringRangeSource { .string = value.stringOperand(), .bounds = StringRangeBounds { value.startOperand(), value.endOperand() }, .mayBeUndefined = true };
    if (value.opcode == Opcode::CallBuiltin)
        return sourceOfBuiltinCall(program, function, value);
    if (value.opcode == Opcode::ElemGet)
        return sourceOfElementRead(program, function, value);
    return std::nullopt;
}

static StringRangeBounds insertClampedBounds(IRBuilder& builder, ProgramPoint point, const StringRangeSource& source, ValueID length)
{
    ValueID first = source.first ? source.first : builder.insertBefore(point, makeInteger(0));

    Value range = IRBuilder::make(Opcode::StringRange, { length, first, source.second });
    range.setMethod(source.method);
    ValueID results = builder.insertBefore(point, WTF::move(range));
    ValueID start = builder.insertBefore(point, makeResultAt(results, startResult));
    ValueID end = builder.insertBefore(point, makeResultAt(results, endResult));
    return { start, end };
}

StringRangeBounds insertStringRange(IRBuilder& builder, ProgramPoint point, const StringRangeSource& source, std::optional<StringRangeBounds> enclosing)
{
    StringRangeBounds bounds;
    if (source.bounds)
        bounds = *source.bounds;
    else {
        Value length = enclosing ? IRBuilder::make(Opcode::Sub, { enclosing->end, enclosing->start }) : IRBuilder::make(Opcode::GetStringLength, { source.string });
        bounds = insertClampedBounds(builder, point, source, builder.insertBefore(point, WTF::move(length)));
    }
    if (!enclosing)
        return bounds;

    ValueID start = builder.insertBefore(point, IRBuilder::make(Opcode::Add, { enclosing->start, bounds.start }));
    ValueID end = builder.insertBefore(point, IRBuilder::make(Opcode::Add, { enclosing->start, bounds.end }));
    return { start, end };
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

static Vector<RangeCall> collectRangeCalls(FunctionPassContext& context, const BuiltinCallSites& sites)
{
    const Function& function = context.function;
    Vector<RangeCall> calls;
    for (const ValueSite& site : sites) {
        std::optional<StringRangeSource> source = stringRangeSourceOf(context.program, function, function.at(site.value));
        // A Substring does not say that it holds at most one unit, and selection reads the code unit of charAt and at only from the call.
        if (source && !source->isSingleUnit())
            calls.append(RangeCall { site, *source });
    }
    return calls;
}

static bool hasRangeAppendForm(const Program& program, const Value& call)
{
    const BuiltinForms* forms = program.builtins().formsOf(overloadOf(program, call).builtin);
    return forms && (forms->appendForm == InlineBuiltin::StringSliceAppend || forms->appendForm == InlineBuiltin::StringSubstringAppend);
}

static Vector<RangeCall> collectAppendedRangeCalls(FunctionPassContext& context, const BuiltinCallSites& sites)
{
    const Function& function = context.function;
    Vector<RangeCall> calls;
    for (const ValueSite& site : sites) {
        const Value& value = function.at(site.value);
        if (!hasRangeAppendForm(context.program, value))
            continue;
        std::optional<Use> use = context.analyses.useLists().singleUse(site.value);
        if (!use || use->kind != Use::Kind::Operand || use->block != site.block || function.at(use->user).opcode != Opcode::StringBuilderAppend)
            continue;
        if (std::optional<StringRangeSource> source = stringRangeSourceOf(context.program, function, value))
            calls.append(RangeCall { site, *source });
    }
    return calls;
}

static PassOutcome expandRangeCalls(FunctionPassContext& context, const Vector<RangeCall>& calls)
{
    if (calls.isEmpty())
        return PassOutcome::Unchanged;

    Function& function = context.function;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (const RangeCall& call : calls) {
        ReplacedEffects replaced;
        replaced.add({ context.program, context.inputs.effects }, function, function.at(call.site.value));

        builder.setOrigin(function.at(call.site.value).origin());
        StringRangeBounds bounds = insertStringRange(builder, call.site.point(), call.source, std::nullopt);

        Value& value = function.edit(call.site.value);
        value.become(Opcode::Substring);
        value.setStringOperand(call.source.string);
        value.setStartOperand(bounds.start);
        value.setEndOperand(bounds.end);
        checkRewrite(context, call.site, replaced);
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

PassOutcome expandStringRanges(FunctionPassContext& context, const BuiltinCallSites& sites)
{
    return expandRangeCalls(context, collectRangeCalls(context, sites));
}

PassOutcome expandAppendedStringRanges(FunctionPassContext& context)
{
    return expandRangeCalls(context, collectAppendedRangeCalls(context, collectBuiltinCallSites(context)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
