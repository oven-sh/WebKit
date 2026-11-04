/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTEffectsDescription.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInlineBuiltinForms.h"
#include "AOTKnownValue.h"
#include "AOTObservableOperations.h"
#include "AOTSort.h"
#include "AOTSplitUnions.h"
#include "AOTStringArena.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <utility>

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

struct TestOfConcatenation {
    ValueSite call;
    ValueID string;
    BuiltinOverloadID overload;
    BuiltinOverloadID codeUnitRead;
    bool isAtEnd { false };
    bool mayBeTooLong { false };
    Vector<ValueID, 4> parts;
    Vector<ValueID, 4> readByCallAlone;
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

static bool collectPartsReadByCallAlone(FunctionPassContext& context, TestOfConcatenation& test, ValueID operand, ValueID user)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();

    size_t numberOfLinks = test.readByCallAlone.size();
    ValueID definition = operand;
    bool isReadByUserAlone = true;
    while (true) {
        std::optional<Use> use = useLists.singleUse(definition);
        isReadByUserAlone = use && use->kind == Use::Kind::Operand && use->user == user && use->block == test.call.block && useLists.blockOf(definition) == test.call.block;
        if (!isReadByUserAlone || !isCastWithoutTest(function.at(definition).opcode))
            break;
        test.readByCallAlone.append(definition);
        user = definition;
        definition = function.at(definition).operand(0);
    }

    const Value& value = function.at(definition);
    bool joinsTexts = isReadByUserAlone && isStringConcatenation(program, function, value) && std::ranges::all_of(value.operands(), [&](ValueID part) {
        return isConvertedWithoutCode(program.typeOf(function, part));
    });
    if (!joinsTexts) {
        test.readByCallAlone.shrink(numberOfLinks);
        test.parts.append(operand);
        return false;
    }

    test.readByCallAlone.append(definition);
    test.mayBeTooLong |= !context.analyses.isRemovableWhenUnused(definition);
    for (ValueID part : value.operands())
        collectPartsReadByCallAlone(context, test, part, definition);
    return true;
}

static std::optional<TestOfConcatenation> matchTestOfConcatenation(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Value& call = function.at(site.value);
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole)
        return std::nullopt;
    std::optional<InlineBuiltin> builtin = inlineImplementationOf(program, call);
    if (builtin != InlineBuiltin::StringStartsWith && builtin != InlineBuiltin::StringEndsWith)
        return std::nullopt;

    ValueID string = call.receiverOperand();
    std::span<const ValueID> arguments = call.argumentsOperands();
    if (!string || arguments.size() != 1 || !program.typeOf(function, string).isAnyString() || function.at(site.block).frequency.predicted() == BlockFrequency::Rare)
        return std::nullopt;

    TestOfConcatenation test { .call = site, .string = string, .isAtEnd = builtin == InlineBuiltin::StringEndsWith };
    if (!collectPartsReadByCallAlone(context, test, arguments[0], site.value))
        return std::nullopt;

    const BlockValues& values = function.at(site.block).values;
    uint32_t first = site.index;
    for (ValueID id : test.readByCallAlone)
        first = std::min(first, context.analyses.useLists().locationOf(id).index);
    for (uint32_t index = first; index < site.index; ++index) {
        if (!test.readByCallAlone.contains(values[index]) && !context.analyses.isRemovableWhenUnused(values[index]))
            return std::nullopt;
    }

    int64_t longest = static_cast<int64_t>(test.parts.size()) * Range::length().maximum;
    TypingContext typing { program, function, context.types };
    BuiltinOperandTypes operands { function.at(string).type, { TypeTable::string, context.types.intern(Type::integer(Range { -longest, longest })) } };
    OverloadMatch match = matchOverload({ typing, program.builtins().at(call.overload()).builtin, BuiltinCallMode::Call, operands, operands });
    if (match.state != BuiltinMatchState::Proven || !match.requirements.isEmpty() || !match.overload)
        return std::nullopt;
    test.overload = match.overload;

    BuiltinOperandTypes operandsOfRead { operands.receiver, { operands.arguments[1] } };
    OverloadMatch read = matchOverload({ typing, Builtins::StringPrototypeCharCodeAt, BuiltinCallMode::Call, operandsOfRead, operandsOfRead });
    if (read.state == BuiltinMatchState::Proven && read.requirements.isEmpty())
        test.codeUnitRead = read.overload;
    return test;
}

static std::optional<char16_t> findOnlyCodeUnit(FunctionPassContext& context, ValueID text)
{
    KnownValue known = knownValueOf(context.program, context.function, text);
    if (known.kind() != KnownValue::Kind::String)
        return std::nullopt;
    Vector<char16_t> buffer;
    StringView characters = context.program.strings.viewOf(known.asString(), buffer);
    return characters.length() == 1 ? std::optional(characters[0]) : std::nullopt;
}

static void emitTestsOfParts(FunctionPassContext& context, IRBuilder& builder, const TestOfConcatenation& test)
{
    Function& function = context.function;
    ValueID call = test.call.value;
    BlockID block = context.analyses.useLists().blockOf(call);
    const Terminator& terminator = function.at(block).terminator;
    if (terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == call)
        builder.demoteReturnCall(block);

    Origin originOfConcatenation = function.at(test.readByCallAlone.last()).origin();
    for (ValueID id : test.readByCallAlone) {
        if (concatenationOpcodes.contains(function.at(id).opcode)) {
            originOfConcatenation = function.at(id).origin();
            break;
        }
    }

    IRBuilder::Expansion cut = builder.beginExpansion(context.analyses.useLists().pointOf(call));
    Origin originOfCall = builder.origin();

    const BlockValues& values = function.at(block).values;
    BitVector indexes(values.size());
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (!test.readByCallAlone.contains(values[index]))
            continue;
        explainOperationThatGoes(function, values[index], InPlaceOfOperation::ItsExpansion);
        indexes.quickSet(index);
    }
    builder.eraseValues(block, indexes);

    const Program& program = context.program;
    Vector<ValueID, 4> texts;
    for (ValueID part : test.parts) {
        const Type& type = program.typeOf(function, part);
        if (type.isAnyString())
            texts.append(part);
        else
            texts.append(type.isNumberLike() ? ValueID() : builder.append(Opcode::ToString, { part }));
    }
    Vector<ValueID, 4> lengths(texts.size());
    auto lengthOf = [&](size_t index) {
        if (!lengths[index])
            lengths[index] = builder.append(Opcode::GetStringLength, { texts[index] });
        return lengths[index];
    };

    ValueID total;
    uint64_t lengthOfShortTexts = 0;
    for (size_t index = 0; test.mayBeTooLong && index < texts.size(); ++index) {
        if (!texts[index])
            lengthOfShortTexts += longestTextOf(program, program.typeOf(function, test.parts[index]));
        else
            total = total ? builder.append(Opcode::Add, { total, lengthOf(index) }) : lengthOf(index);
    }
    if (total) {
        ValueID fits = builder.append(Opcode::LessEq, { total, builder.appendInteger(Range::length().maximum - static_cast<int64_t>(lengthOfShortTexts)) });
        IRBuilder::BranchBlocks blocks = builder.emitBranch(fits, BlockFrequency::Normal, BlockFrequency::Rare);

        builder.moveToEndOf(blocks.notTaken);
        builder.setOrigin(originOfConcatenation);
        Value concatenation = IRBuilder::make(Opcode::Strcat);
        concatenation.setVariadicOperands(test.parts.span());
        ValueID joined = builder.append(WTF::move(concatenation));
        builder.setOrigin(originOfCall);
        bool alwaysThrows = !texts.contains(ValueID());
        builder.endExpansion(cut, alwaysThrows ? builder.appendBoolean(false) : builder.append(makeBuiltinCall(test.overload, BuiltinCallForm::Whole, { test.string, { joined } }, { })));
        builder.moveToEndOf(blocks.taken);
    }

    ValueID position;
    for (size_t step = 0; step < texts.size(); ++step) {
        size_t index = test.isAtEnd ? texts.size() - 1 - step : step;
        if (!texts[index])
            texts[index] = builder.append(Opcode::ToString, { test.parts[index] });
        ValueID next;
        ValueID holds;
        if (std::optional<char16_t> unit = position && test.codeUnitRead ? findOnlyCodeUnit(context, texts[index]) : std::nullopt) {
            if (test.isAtEnd)
                next = builder.append(Opcode::Sub, { position, lengthOf(index) });
            ValueID read = builder.append(makeBuiltinCall(test.codeUnitRead, BuiltinCallForm::Whole, { test.string, { test.isAtEnd ? next : position } }, { }));
            holds = builder.append(Opcode::StrictEq, { read, builder.appendInteger(*unit) });
        } else if (position)
            holds = builder.append(makeBuiltinCall(test.overload, BuiltinCallForm::Whole, { test.string, { texts[index], position } }, { }));
        else
            holds = builder.append(makeBuiltinCall(test.overload, BuiltinCallForm::Whole, { test.string, { texts[index] } }, { }));
        if (step + 1 == texts.size()) {
            builder.endExpansion(cut, holds);
            break;
        }

        IRBuilder::BranchBlocks blocks = builder.emitBranch(holds);
        builder.moveToEndOf(blocks.notTaken);
        builder.endExpansion(cut, builder.appendBoolean(false));
        builder.moveToEndOf(blocks.taken);
        if (next)
            position = next;
        else if (test.isAtEnd)
            position = builder.append(Opcode::Sub, { position ? position : builder.append(Opcode::GetStringLength, { test.string }), lengthOf(index) });
        else
            position = position ? builder.append(Opcode::Add, { position, lengthOf(index) }) : lengthOf(index);
    }
}

// path.startsWith(directory + "/") asks about directory, then about "/" behind it, and makes no string.
unsigned testPartsOfConcatenations(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    Vector<TestOfConcatenation> tests;
    for (const ValueSite& site : calls) {
        std::optional<TestOfConcatenation> test = matchTestOfConcatenation(context, site);
        if (test && !context.exceeds(LimitID::MaxFusedConcatenationParts, test->parts.size(), site.value))
            tests.append(WTF::move(*test));
    }
    if (tests.isEmpty())
        return 0;

    AOT::sort(tests, std::ranges::greater { }, [](const TestOfConcatenation& test) {
        return std::pair { test.call.block, test.call.index };
    });
    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    for (const TestOfConcatenation& test : tests)
        emitTestsOfParts(context, builder, test);
    builder.setHandler(std::nullopt);
    return static_cast<unsigned>(tests.size());
}

static bool isReadByPositionAlone(FunctionPassContext& context, ValueID templateObject)
{
    const Function& function = context.function;
    return std::ranges::all_of(context.analyses.useLists().usesOf(templateObject), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand || use.operandIndex)
            return false;
        const Value& user = function.at(use.user);
        if (user.opcode == Opcode::ElemGet)
            return context.program.typeOf(function, user.propertyOperand()).isInteger();
        if (user.opcode == Opcode::PropGet)
            return user.key() == context.program.keyOf(BuiltinNames::length);
        return isCastWithoutTest(user.opcode) && isReadByPositionAlone(context, use.user);
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
