/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInt32Conversions.h"
#include "AOTKnownValue.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSplitUnions.h"
#include "AOTStringRanges.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <array>
#include <limits>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ValueRewrite {
    ValueID original;
    Value rewritten;
};

} // anonymous namespace

static constexpr OpcodeSet simplifiedIntrinsics {
    Opcode::IsPristineArray, Opcode::ToLength, Opcode::ToIntegerOrInfinity, Opcode::ToInt32, Opcode::ToUint32, Opcode::ToObject, Opcode::ToThis,
    Opcode::StepGenerator
};

static constexpr Range integersUnchangedByToLength { 0, maxExactInteger - 1 };

static constexpr std::array roundingFunctions { Builtins::MathFloor, Builtins::MathCeil, Builtins::MathRound, Builtins::MathTrunc };
static constexpr std::array numberConstructorTests { Builtins::NumberIsFinite, Builtins::NumberIsInteger, Builtins::NumberIsNaN, Builtins::NumberIsSafeInteger };

bool keepsTypeOf(FunctionPassContext& context, ValueID original, const Value& rewritten)
{
    TypeID rewrittenType = resultTypeOf(context.program, context.function, rewritten, context.types);
    return context.types.at(rewrittenType).isSubsetOf(context.program.typeOf(context.function, original), context.program);
}

static bool keepsEffectsOf(FunctionPassContext& context, const Value& original, const Value& rewritten)
{
    EffectContext effectContext { context.program, context.inputs.effects };
    ReplacedEffects replaced;
    replaced.add(effectContext, context.function, original);
    return effectsOf(effectContext, context.function, rewritten).isSubsetOf(replaced.effects());
}

static TypeID narrowPrediction(TypeArena& types, TypeID prediction, TypeID type)
{
    if (!prediction)
        return { };
    TypeID narrowed = types.intersectionOf(type, prediction);
    return narrowed == TypeTable::never || narrowed == type ? TypeID() : narrowed;
}

void replaceKeepingType(FunctionPassContext& context, ValueID original, Value&& rewritten)
{
    Function& function = context.function;
    BlockID block = context.analyses.useLists().blockOf(original);
    Value& value = function.at(original);

    ReplacedEffects replaced;
    replaced.add({ context.program, context.inputs.effects }, function, value);

    TypeID prediction = value.prediction();
    rewritten.setOrigin(value.origin());
    value = WTF::move(rewritten);
    value.type = resultTypeOf(context.program, function, value, context.types);
    value.setPrediction(narrowPrediction(context.types, prediction, value.type));
    checkRewrite(context, ValueSite { block, 0, original }, replaced);
}

std::optional<ValueID> findSingleUseDefinition(FunctionPassContext& context, ValueID operand, ValueID user)
{
    const Function& function = context.function;
    const UseLists& useLists = context.analyses.useLists();
    ValueID link = operand;
    while (isCastWithoutTest(function.at(link).opcode)) {
        std::optional<Use> use = useLists.singleUse(link);
        if (!use || use->kind != Use::Kind::Operand || use->user != user)
            return std::nullopt;
        user = link;
        link = function.at(link).operand(0);
    }
    return link;
}

static unsigned rewriteValues(FunctionPassContext& context, NOESCAPE const Invocable<std::optional<Value>(const Value&)> auto& simplify)
{
    const Function& function = context.function;
    Vector<ValueRewrite> rewrites;
    forEachValueSite(context.analyses, function, [&](const ValueSite& site) {
        std::optional<Value> rewritten = simplify(function.at(site.value));
        if (rewritten && keepsTypeOf(context, site.value, *rewritten))
            rewrites.append(ValueRewrite { site.value, WTF::move(*rewritten) });
    });

    for (ValueRewrite& rewrite : rewrites)
        replaceKeepingType(context, rewrite.original, WTF::move(rewrite.rewritten));
    if (!rewrites.isEmpty())
        context.analyses.didChangeValues();
    return static_cast<unsigned>(rewrites.size());
}

static const BuiltinOverload* findWholeCallOverload(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
        return nullptr;
    const BuiltinOverload& overload = program.builtins().at(value.overload());
    return overload.mode == BuiltinCallMode::Call ? &overload : nullptr;
}

static Value makeIdentityCast(const Function& function, ValueID value)
{
    Value upcast = IRBuilder::make(Opcode::Upcast, { value });
    upcast.setCastType(function.at(value).type);
    return upcast;
}

static Value makeBooleanConstant(bool boolean)
{
    Value constant = IRBuilder::make(Opcode::Boolean);
    constant.setBoolean(boolean);
    return constant;
}

static Value makeIntegerConstant(int64_t integer)
{
    Value constant = IRBuilder::make(Opcode::Integer);
    constant.setInteger(integer);
    return constant;
}

static Value makeDoubleConstant(double number)
{
    Value constant = IRBuilder::make(Opcode::Double);
    constant.setNumber(number);
    return constant;
}

static std::optional<Value> simplifyIntegerConversion(FunctionPassContext& context, const Value& conversion, Range unchangedIntegers)
{
    ValueID operand = conversion.valueOperand();
    const Type& type = context.program.typeOf(context.function, operand);
    if (!type.isInteger() || !type.integers.isSubsetOf(unchangedIntegers))
        return std::nullopt;
    return makeIdentityCast(context.function, operand);
}

static std::optional<Value> simplifyObjectConversion(FunctionPassContext& context, const Value& conversion)
{
    ValueID operand = conversion.valueOperand();
    if (!context.program.typeOf(context.function, operand).isObject())
        return std::nullopt;
    return makeIdentityCast(context.function, operand);
}

static GeneratorStepCheck checkLeftToRun(GeneratorStepCheck check, const ClassPart& generator)
{
    switch (check) {
    case GeneratorStepCheck::IsUnmodifiedGenerator:
    case GeneratorStepCheck::IsUnmodified:
        return generator.provenPristine.contains(PristineAspects::Names) ? GeneratorStepCheck::None : GeneratorStepCheck::IsUnmodified;
    case GeneratorStepCheck::IsGenerator:
    case GeneratorStepCheck::None:
        return GeneratorStepCheck::None;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Value> simplifyGeneratorStep(FunctionPassContext& context, const Value& step)
{
    const Type& type = context.program.typeOf(context.function, step.generatorOperand());
    const ClassPart* generator = type.isObject() ? type.singleBuiltinClass() : nullptr;
    if (!generator || generator->builtinClass != BuiltinClasses::Generator)
        return std::nullopt;

    GeneratorStepCheck check = checkLeftToRun(step.check(), *generator);
    if (check == step.check())
        return std::nullopt;
    Value simplified = step;
    simplified.setCheck(check);
    return simplified;
}

static std::optional<Value> simplifyIntrinsic(FunctionPassContext& context, const Value& intrinsic)
{
    switch (intrinsic.opcode) {
    case Opcode::IsPristineArray:
        return makePristineTest(intrinsic.valueOperand(), BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names });
    case Opcode::ToLength:
        return simplifyIntegerConversion(context, intrinsic, integersUnchangedByToLength);
    case Opcode::ToIntegerOrInfinity:
        return simplifyIntegerConversion(context, intrinsic, Range::full());
    case Opcode::ToInt32:
        return simplifyIntegerConversion(context, intrinsic, Range::int32());
    case Opcode::ToUint32:
        return simplifyIntegerConversion(context, intrinsic, Range::uint32());
    case Opcode::ToObject:
    case Opcode::ToThis:
        return simplifyObjectConversion(context, intrinsic);
    case Opcode::StepGenerator:
        return simplifyGeneratorStep(context, intrinsic);
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned simplifyIntrinsics(FunctionPassContext& context)
{
    return rewriteValues(context, [&](const Value& value) -> std::optional<Value> {
        if (!simplifiedIntrinsics.contains(value.opcode))
            return std::nullopt;
        return simplifyIntrinsic(context, value);
    });
}

static std::optional<Value> simplifyOwnPropertyTest(FunctionPassContext& context, const Value& value)
{
    const Program& program = context.program;
    const BuiltinOverload* overload = findWholeCallOverload(program, value);
    if (!overload)
        return std::nullopt;

    BuiltinCall call = builtinCallOf(program, value);
    ValueID object;
    ValueID keyOperand;
    if (overload->builtin == Builtins::ObjectHasOwn) {
        object = call.operandAt(BuiltinOperand::argument(0));
        keyOperand = call.operandAt(BuiltinOperand::argument(1));
    } else if (overload->builtin == Builtins::ObjectPrototypeHasOwnProperty) {
        object = call.receiver;
        keyOperand = call.operandAt(BuiltinOperand::argument(0));
    }
    if (!object || !keyOperand || !program.typeOf(context.function, object).isObject())
        return std::nullopt;

    PropertyKeyID key = internKey(context.types, constantKeyOf(program, program.typeOf(context.function, keyOperand)));
    if (!key)
        return std::nullopt;

    Value test = IRBuilder::make(Opcode::PropHasOwn, { object });
    test.setKey(key);
    test.setAccessSite(value.accessSite());
    if (!keepsEffectsOf(context, value, test))
        return std::nullopt;
    return test;
}

unsigned simplifyOwnPropertyTests(FunctionPassContext& context)
{
    return rewriteValues(context, [&](const Value& value) {
        return simplifyOwnPropertyTest(context, value);
    });
}

static std::optional<Value> makeResultWithoutArguments(BuiltinID builtin)
{
    if (builtin == Builtins::Number || builtin == Builtins::MathHypot)
        return makeIntegerConstant(0);
    if (builtin == Builtins::MathMin)
        return makeDoubleConstant(std::numeric_limits<double>::infinity());
    if (builtin == Builtins::MathMax)
        return makeDoubleConstant(-std::numeric_limits<double>::infinity());
    return std::nullopt;
}

static KnownBoolean evaluateNumberTest(BuiltinID builtin, const Type& operand)
{
    if (std::ranges::contains(numberConstructorTests, builtin) && !(operand.primitives & Primitive::Number))
        return KnownBoolean::False;
    if (!operand.isInteger())
        return KnownBoolean::Unknown;
    if (builtin == Builtins::NumberIsInteger || builtin == Builtins::NumberIsFinite)
        return KnownBoolean::True;
    if (builtin == Builtins::NumberIsNaN || builtin == Builtins::IsNaN)
        return KnownBoolean::False;
    return KnownBoolean::Unknown;
}

static bool returnsOperandUnchanged(BuiltinID builtin, const Type& operand)
{
    if (std::ranges::contains(roundingFunctions, builtin))
        return operand.isInteger();
    if (builtin == Builtins::Boolean)
        return operand.isBoolean();
    return builtin == Builtins::Number && operand.isNumber();
}

static std::optional<Value> simplifyPrimitiveCall(FunctionPassContext& context, const Value& value)
{
    const BuiltinOverload* overload = findWholeCallOverload(context.program, value);
    if (!overload)
        return std::nullopt;

    std::span<const ValueID> arguments = value.argumentsOperands();
    if (arguments.empty())
        return makeResultWithoutArguments(overload->builtin);

    const Type& operand = context.program.typeOf(context.function, arguments[0]);
    if (returnsOperandUnchanged(overload->builtin, operand))
        return makeIdentityCast(context.function, arguments[0]);

    switch (evaluateNumberTest(overload->builtin, operand)) {
    case KnownBoolean::Unknown:
        return std::nullopt;
    case KnownBoolean::False:
        return makeBooleanConstant(false);
    case KnownBoolean::True:
        return makeBooleanConstant(true);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

unsigned simplifyPrimitiveCalls(FunctionPassContext& context)
{
    return rewriteValues(context, [&](const Value& value) {
        return simplifyPrimitiveCall(context, value);
    });
}

static ValueID findTruncatedNumber(FunctionPassContext& context, ValueID operand)
{
    const Function& function = context.function;
    const Value& definition = function.at(sameObjectThroughCasts(function, operand));

    ValueID number;
    if (definition.opcode == Opcode::ToIntegerOrInfinity)
        number = definition.valueOperand();
    else if (const BuiltinOverload* overload = findWholeCallOverload(context.program, definition); overload && overload->builtin == Builtins::MathTrunc && definition.argumentsOperands().size() == 1)
        number = definition.argumentsOperands()[0];
    if (!number || !context.program.typeOf(function, number).isNumber())
        return { };
    return number;
}

// ToInt32 drops the fraction itself and takes NaN, the infinities and both zeros to 0, as it does with what a truncation makes of them.
static std::optional<Value> simplifyInt32Conversion(FunctionPassContext& context, const Value& conversion)
{
    std::optional<Value> simplified;
    conversion.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        ValueID number = findTruncatedNumber(context, operand);
        if (!number)
            return;
        if (!simplified)
            simplified = conversion;
        simplified->setOperand(operandIndex, number);
    });
    return simplified;
}

unsigned simplifyInt32Conversions(FunctionPassContext& context)
{
    return rewriteValues(context, [&](const Value& value) -> std::optional<Value> {
        if (!int32ConvertingOperations.contains(value.opcode))
            return std::nullopt;
        return simplifyInt32Conversion(context, value);
    });
}

PassOutcome simplifyBuiltinCalls(FunctionPassContext& context)
{
    unsigned rewrites = simplifyIntrinsics(context);
    rewrites += simplifyOwnPropertyTests(context);
    rewrites += simplifyPrimitiveCalls(context);
    rewrites += simplifyInt32Conversions(context);
    rewrites += simplifyStringReads(context);
    rewrites += simplifyConcatenatingCalls(context);
    rewrites += simplifyMatches(context);
    rewrites += simplifyLastPiecesOfSplits(context);
    rewrites += simplifyPromiseAllOfLiterals(context);
    rewrites += simplifyPromiseResolve(context);

    PassOutcome outcome = rewrites ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= expandStringRanges(context);
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
