/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTSplitUnions.h"
#include "AOTStringArena.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ConcatenatedPart {
    ValueID value;
    StringID text;
};

struct Concatenation {
    ValueSite site;
    Vector<ConcatenatedPart, 8> parts;
};

} // anonymous namespace

static constexpr uint16_t primitivesConvertedWithoutCode = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnyString | Primitive::BigInt;

static bool isConvertedWithoutCode(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(primitivesConvertedWithoutCode);
}

static const Value* findWholeCallOf(FunctionPassContext& context, ValueID id, BuiltinID builtin)
{
    const Value& call = context.function.at(id);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
        return nullptr;
    return context.program.builtins().at(call.overload()).builtin == builtin ? &call : nullptr;
}

static std::optional<ConcatenatedPart> separatorOf(FunctionPassContext& context, std::span<const ValueID> arguments)
{
    if (arguments.empty())
        return ConcatenatedPart { { }, context.strings.intern(StringView(","_s)) };
    const Type& type = context.program.typeOf(context.function, arguments[0]);
    if (type.isAnyString())
        return ConcatenatedPart { arguments[0], { } };
    if (!type.isNever() && type.hasOnlyPrimitives(Primitive::Undefined))
        return ConcatenatedPart { { }, context.strings.intern(StringView(","_s)) };
    return std::nullopt;
}

static std::optional<Concatenation> matchJoinOfConstants(FunctionPassContext& context, const ValueSite& site, std::span<const KnownValue> constants, std::span<const ValueID> arguments)
{
    KnownValue separator = arguments.empty() ? KnownValue::undefined() : knownValueOf(context.program, context.function, arguments[0]);
    if (separator.kind() == KnownValue::Kind::Undefined)
        separator = KnownValue::string(context.strings.intern(StringView(","_s)));
    if (separator.kind() != KnownValue::Kind::String)
        return std::nullopt;

    Vector<KnownValue> texts;
    for (size_t index = 0; index < constants.size(); ++index) {
        if (index)
            texts.append(separator);
        if (!constants[index].isNullish())
            texts.append(constants[index]);
    }
    if (texts.isEmpty())
        return Concatenation { site, { } };

    Vector<ValueID> positions(texts.size(), [](size_t index) {
        return ValueID(static_cast<uint32_t>(index));
    });
    Value concatenation = IRBuilder::make(Opcode::Strcat);
    concatenation.setVariadicOperands(positions.span());
    KnownValue joined = evaluateStringOperation(context.program, context.strings, context.function, concatenation, [&](ValueID position) {
        return texts[position.index()];
    });
    if (joined.kind() != KnownValue::Kind::String)
        return std::nullopt;
    return Concatenation { site, { ConcatenatedPart { { }, joined.asString() } } };
}

static std::optional<Concatenation> matchJoinOfLiteral(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const Value* call = findWholeCallOf(context, site.value, Builtins::ArrayPrototypeJoin);
    if (!call || !call->receiverOperand() || call->argumentsOperands().size() > 1)
        return std::nullopt;
    if (std::optional<Vector<KnownValue, 8>> constants = findConstantsOfListFor(context, call->receiverOperand(), site.value)) {
        if (std::optional<Concatenation> joined = matchJoinOfConstants(context, site, constants->span(), call->argumentsOperands()))
            return joined;
    }

    std::optional<ValueID> literalID = findSingleUseDefinition(context, call->receiverOperand(), site.value);
    if (!literalID || !context.analyses.useLists().singleUse(*literalID))
        return std::nullopt;
    const Value& literal = function.at(*literalID);
    if (literal.opcode != Opcode::NewArray || !literal.holes().empty())
        return std::nullopt;

    std::optional<ConcatenatedPart> separator = separatorOf(context, call->argumentsOperands());
    if (!separator)
        return std::nullopt;

    Concatenation concatenation { site, { } };
    std::span<const ValueID> elements = literal.elementsOperands();
    for (size_t index = 0; index < elements.size(); ++index) {
        if (index)
            concatenation.parts.append(*separator);
        const Type& type = context.program.typeOf(function, elements[index]);
        if (!isConvertedWithoutCode(type))
            return std::nullopt;
        if (type.hasOnlyPrimitives(Primitive::UndefinedOrNull))
            continue;
        if (type.mayBeUndefinedOrNull())
            return std::nullopt;
        concatenation.parts.append(ConcatenatedPart { elements[index], { } });
    }
    return concatenation;
}

static std::optional<Concatenation> matchRawTemplate(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const Value* call = findWholeCallOf(context, site.value, Builtins::StringRaw);
    if (!call || call->argumentsOperands().empty())
        return std::nullopt;

    std::span<const ValueID> arguments = call->argumentsOperands();
    const Value& templateObject = function.at(sameObjectThroughCasts(function, arguments[0]));
    if (templateObject.opcode != Opcode::NewTemplateObject)
        return std::nullopt;

    Concatenation concatenation { site, { } };
    std::span<const ValueID> substitutions = arguments.subspan(1);
    auto templateStrings = templateObject.templateStrings();
    for (size_t index = 0; index < templateStrings.size(); ++index) {
        concatenation.parts.append(ConcatenatedPart { { }, templateStrings[index].raw });
        if (index + 1 == templateStrings.size() || index >= substitutions.size())
            continue;
        if (!isConvertedWithoutCode(context.program.typeOf(function, substitutions[index])))
            return std::nullopt;
        concatenation.parts.append(ConcatenatedPart { substitutions[index], { } });
    }
    return concatenation;
}

static std::optional<Concatenation> matchConcat(FunctionPassContext& context, const ValueSite& site)
{
    const Function& function = context.function;
    const Value* call = findWholeCallOf(context, site.value, Builtins::StringPrototypeConcat);
    if (!call || !call->receiverOperand() || !context.program.typeOf(function, call->receiverOperand()).isAnyString())
        return std::nullopt;

    Concatenation concatenation { site, { ConcatenatedPart { call->receiverOperand(), { } } } };
    for (ValueID argument : call->argumentsOperands()) {
        if (!isConvertedWithoutCode(context.program.typeOf(function, argument)))
            return std::nullopt;
        concatenation.parts.append(ConcatenatedPart { argument, { } });
    }
    return concatenation;
}

static Value makeStringConstant(StringID text)
{
    Value constant = IRBuilder::make(Opcode::String);
    constant.setString(text);
    return constant;
}

static Value makeConcatenation(FunctionPassContext& context, IRBuilder& builder, const Concatenation& concatenation)
{
    if (concatenation.parts.isEmpty())
        return makeStringConstant(context.program.strings.emptyString());

    builder.setOrigin(context.function.at(concatenation.site.value).origin());
    Vector<ValueID, 8> operands;
    for (const ConcatenatedPart& part : concatenation.parts)
        operands.append(part.value ? part.value : builder.insertBefore(concatenation.site.point(), makeStringConstant(part.text)));

    if (operands.size() == 1)
        return IRBuilder::make(Opcode::ToString, { operands[0] });
    Value result = IRBuilder::make(Opcode::Strcat);
    result.setVariadicOperands(operands.span());
    return result;
}

unsigned simplifyConcatenatingCalls(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    Vector<Concatenation> concatenations;
    for (const ValueSite& site : calls) {
        std::optional<Concatenation> concatenation = matchJoinOfLiteral(context, site);
        if (!concatenation)
            concatenation = matchRawTemplate(context, site);
        if (!concatenation)
            concatenation = matchConcat(context, site);
        if (concatenation && !context.exceeds(LimitID::MaxFusedConcatenationParts, concatenation->parts.size(), site.value))
            concatenations.append(WTF::move(*concatenation));
    }
    if (concatenations.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    Vector<Value> rewritten;
    rewritten.reserveInitialCapacity(concatenations.size());
    for (const Concatenation& concatenation : concatenations)
        rewritten.append(makeConcatenation(context, builder, concatenation));
    builder.applyInsertions();

    for (size_t index = 0; index < concatenations.size(); ++index)
        replaceKeepingType(context, concatenations[index].site.value, WTF::move(rewritten[index]));
    context.analyses.didChangeValues();
    return static_cast<unsigned>(concatenations.size());
}

static bool isReadByPositionAlone(FunctionPassContext& context, ValueID templateObject)
{
    const Function& function = context.function;
    return std::ranges::all_of(context.analyses.useLists().usesOf(templateObject), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand || use.operandIndex)
            return false;
        const Value& user = function.at(use.user);
        switch (user.opcode) {
        case Opcode::ElemGet:
            return context.program.typeOf(function, user.propertyOperand()).isInteger();
        case Opcode::PropGet:
            return user.key() == context.program.keyOf(BuiltinNames::length);
        default:
            return isCastWithoutTest(user.opcode) && isReadByPositionAlone(context, use.user);
        }
    });
}

// Nobody can tell such an object from an array literal of its strings, which need not be built.
unsigned replaceTemplateObjectsReadByPosition(FunctionPassContext& context)
{
    Function& function = context.function;
    Vector<ValueSite, 4> sites;
    context.forEachSiteIn(SiteList::TemplateObjects, [&](const ValueSite& site) {
        if (function.at(site.value).templateStrings().size() <= context.build.limit(LimitID::MaxVirtualArrayLength) && isReadByPositionAlone(context, site.value))
            sites.append(site);
    });
    if (sites.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    Vector<Value> literals;
    literals.reserveInitialCapacity(sites.size());
    for (const ValueSite& site : sites) {
        builder.setOrigin(function.at(site.value).origin());
        TypeID contents = TypeTable::string;
        Vector<ValueID, 8> elements;
        for (const TemplateString& part : function.at(site.value).templateStrings()) {
            if (!part.cooked)
                contents = context.types.unionOf(contents, TypeTable::undefined);
            elements.append(builder.insertBefore(site.point(), part.cooked ? makeStringConstant(part.cooked) : IRBuilder::make(Opcode::Undefined)));
        }
        Value literal = IRBuilder::make(Opcode::NewArray);
        literal.setContents(contents);
        literal.setElementsOperands(elements.span());
        literals.append(WTF::move(literal));
    }
    builder.applyInsertions();

    for (size_t index = 0; index < sites.size(); ++index)
        replaceKeepingType(context, sites[index].value, WTF::move(literals[index]));
    context.analyses.didChangeValues();
    return static_cast<unsigned>(sites.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
