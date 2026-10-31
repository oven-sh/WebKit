/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFormatPatternPlanner.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTValueIdentity.h"
#include "AOTWholeLiterals.h"
#include <algorithm>
#include <optional>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct FormatterSite {
    ValueID call;
    const WTF::Vector<uint32_t>* pattern;
    FormatPatternKind kind;
    bool makesFormatter;
    bool hasOptions;
};

} // anonymous namespace

// `new Intl.DateTimeFormat(..)`, `new Intl.NumberFormat(..)`, and the four methods that make a formatter, use it once and drop it. Locale and options are not filled in.
static std::optional<FormatterRequest> findFormatterMadeBy(const Program& program, const Value& call)
{
    using Required = IntlDateTimeFormat::RequiredComponent;
    using Defaults = IntlDateTimeFormat::Defaults;
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    const BuiltinOverload& overload = program.builtins().at(call.overload());
    BuiltinID builtin = program.builtins().canonicalID(overload.builtin);
    if (overload.mode == BuiltinCallMode::Construct) {
        if (call.receiverOperand())
            return std::nullopt;
        if (builtin == Builtins::IntlDateTimeFormat)
            return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { } };
        if (builtin == Builtins::IntlNumberFormat)
            return FormatterRequest { .kind = FormatPatternKind::Number, .locale = { } };
        return std::nullopt;
    }
    if (overload.mode != BuiltinCallMode::Call || !call.receiverOperand())
        return std::nullopt;
    if (builtin == Builtins::NumberPrototypeToLocaleString)
        return FormatterRequest { .kind = FormatPatternKind::Number, .locale = { } };
    if (builtin == Builtins::DatePrototypeToLocaleString)
        return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { }, .required = Required::Any, .defaults = Defaults::All };
    if (builtin == Builtins::DatePrototypeToLocaleDateString)
        return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { }, .required = Required::Date, .defaults = Defaults::Date };
    if (builtin == Builtins::DatePrototypeToLocaleTimeString)
        return FormatterRequest { .kind = FormatPatternKind::DateTime, .locale = { }, .required = Required::Time, .defaults = Defaults::Time };
    return std::nullopt;
}

static bool addOptions(const Program& program, std::span<const ConstantMember> members, FormatterRequest& request)
{
    for (const ConstantMember& member : members) {
        FormatterOption option { program.strings.toWTFString(program.at(member.key).text), FormatterOptionKind::Number, { }, 0 };
        switch (member.value.kind()) {
        case KnownValue::Kind::Undefined:
            // As if the name were not there.
            continue;
        case KnownValue::Kind::Boolean:
            option.kind = FormatterOptionKind::Boolean;
            option.number = member.value.asBoolean();
            break;
        case KnownValue::Kind::Integer:
        case KnownValue::Kind::Double:
            option.number = member.value.asNumber();
            break;
        case KnownValue::Kind::String:
            option.kind = FormatterOptionKind::String;
            option.string = program.strings.toWTFString(member.value.asString());
            break;
        case KnownValue::Kind::Unknown:
        case KnownValue::Kind::Null:
        case KnownValue::Kind::Object:
        case KnownValue::Kind::Function:
            return false;
        }
        request.options.append(WTF::move(option));
    }
    // Reading a constant runs nothing, so the order in which the literal lists them is not seen.
    std::ranges::sort(request.options, [](const FormatterOption& a, const FormatterOption& b) {
        return codePointCompareLessThan(a.name, b.name);
    });
    // `{ month: "long", month: "short" }`: the planner would see one of the two.
    for (size_t index = 1; index < request.options.size(); ++index) {
        if (request.options[index - 1].name == request.options[index].name)
            return false;
    }
    return true;
}

// The options are a literal of constants that only this call gets to see, or one in a variable that is stored to once, which nothing ever changes.
static bool findOptions(FunctionPassContext& context, ValueID call, ValueID operand, FormatterRequest& request)
{
    const Program& program = context.program;
    const Function& function = context.function;
    if (knownValueOf(program, function, operand).kind() == KnownValue::Kind::Undefined)
        return true;
    request.hasOptions = true;

    const Value& source = function.at(sameObjectThroughCasts(function, operand));
    if (source.opcode == Opcode::GetModuleVariable) {
        const Vector<ConstantMember>* members = context.inputs.flow.constantObjectIn(source.variable());
        return members && addOptions(program, members->span(), request);
    }
    std::optional<ValueID> literal = findSingleUseDefinition(context, operand, call);
    if (!literal || !context.analyses.useLists().singleUse(*literal))
        return false;
    std::optional<Vector<ConstantMember, 8>> members = findMembersOfObjectLiteral(program, function, function.at(*literal));
    return members && addOptions(program, members->span(), request);
}

static std::optional<FormatterSite> matchFormatterSite(FunctionPassContext& context, ValueID id)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Value& call = function.at(id);
    std::optional<FormatterRequest> request = findFormatterMadeBy(program, call);
    if (!request)
        return std::nullopt;
    // `new Number(5).toLocaleString("en-US")`: the function takes the number out of the object. A pattern is written from a number.
    if (request->kind == FormatPatternKind::Number && call.receiverOperand() && !program.typeOf(function, call.receiverOperand()).isSubsetOf(program.at(TypeTable::number), program))
        return std::nullopt;
    auto arguments = call.argumentsOperands();
    if (arguments.empty() || arguments.size() > 2)
        return std::nullopt;
    KnownValue locale = knownValueOf(program, function, arguments[0]);
    if (locale.kind() != KnownValue::Kind::String)
        return std::nullopt;
    const Terminator& terminator = function.at(context.analyses.useLists().blockOf(id)).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id && terminator.requirement() == TailCallRequirement::Required)
        return std::nullopt;
    request->locale = program.strings.toWTFString(locale.asString());
    if (arguments.size() == 2 && !findOptions(context, id, arguments[1], *request))
        return std::nullopt;
    const WTF::Vector<uint32_t>* pattern = context.build.formatPatternPlans().planFor(context.build.scratchRealms(), *request);
    if (!pattern)
        return std::nullopt;
    return FormatterSite { id, pattern, request->kind, !call.receiverOperand(), request->hasOptions };
}

static void demoteIfFormatterSiteIsReturned(FunctionPassContext& context, IRBuilder& builder, ValueID call)
{
    BlockID block = context.analyses.useLists().blockOf(call);
    const Terminator& terminator = context.function.at(block).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call)
        builder.demoteReturnCall(block);
}

static void makeFormatterWithPattern(FunctionPassContext& context, IRBuilder& builder, const FormatterSite& site)
{
    demoteIfFormatterSiteIsReturned(context, builder, site.call);
    Value formatter = IRBuilder::make(Opcode::NewFormatterWithPattern);
    formatter.setValueType(context.function.at(site.call).type);
    formatter.setFormatPattern(site.pattern->span());
    replaceKeepingType(context, site.call, WTF::move(formatter));
    if (!site.hasOptions)
        return;
    context.analyses.didChangeValues();
    ProgramPoint point = context.analyses.useLists().pointOf(site.call);
    builder.insertBefore({ point.block, point.index + 1 }, IRBuilder::make(Opcode::SetUpFormatterWithOptions, { site.call }));
    builder.applyInsertions();
}

// The function is still called for what the pattern does not cover: an invalid date, a year before 1, a number beyond 2^53.
static void formatWithPattern(FunctionPassContext& context, IRBuilder& builder, const FormatterSite& site)
{
    Value call = context.function.at(site.call);
    demoteIfFormatterSiteIsReturned(context, builder, site.call);
    IRBuilder::Expansion expansion = builder.beginExpansion(context.analyses.useLists().pointOf(site.call));
    Value format = IRBuilder::make(site.kind == FormatPatternKind::Number ? Opcode::FormatNumberWithPattern : Opcode::FormatDateWithPattern, { call.receiverOperand() });
    format.setFormatPattern(site.pattern->span());
    ValueID text = builder.append(WTF::move(format));
    IRBuilder::ProvenBranch isWritten = builder.emitBranchWithProof(builder.appendTypeTest(text, TypeTable::string), 0, BlockFrequency::Normal, BlockFrequency::Unlikely);
    builder.moveToEndOf(isWritten.taken);
    builder.endExpansion(expansion, isWritten.provenOnTaken);
    builder.moveToEndOf(isWritten.notTaken);
    builder.endExpansion(expansion, builder.append(WTF::move(call)));
}

unsigned formatFromPatterns(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<FormatterSite, 4> sites;
    for (const ValueSite& site : context.sitesIn(SiteList::BuiltinCalls)) {
        // The call that is left for what a pattern does not cover lies in an unlikely block.
        if (function.at(site.block).frequency.predicted() != BlockFrequency::Normal)
            continue;
        if (std::optional<FormatterSite> formatterSite = matchFormatterSite(context, site.value))
            sites.append(*formatterSite);
    }
    if (sites.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    bool changedControlFlow = false;
    for (const FormatterSite& site : sites) {
        if (site.makesFormatter)
            makeFormatterWithPattern(context, builder, site);
        else {
            formatWithPattern(context, builder, site);
            changedControlFlow = true;
        }
        context.analyses.didChangeValues();
    }
    if (changedControlFlow)
        context.analyses.didChangeControlFlow();
    return static_cast<unsigned>(sites.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
