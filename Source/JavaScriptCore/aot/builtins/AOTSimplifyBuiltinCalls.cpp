/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSimplifyBuiltinCallsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCallPlan.h"
#include "AOTCoroutines.h"
#include "AOTEffectsCheck.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfRules.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTInt32Conversions.h"
#include "AOTKnownValue.h"
#include "AOTPropertyAccessElements.h"
#include "AOTSplitUnions.h"
#include "AOTStringRanges.h"
#include "AOTThreshold.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <limits>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ValueRewrite {
    ValueID original;
    Value rewritten;
};

struct CallWithLiteralParts {
    ValueSite call;
    ValueID literal;
    ValueID literalOperand;
    BuiltinOverloadID form;
    Vector<ValueID, 4> arguments;
};

} // anonymous namespace

static constexpr Range integersUnchangedByToLength { 0, maxExactInteger - 1 };
static constexpr Range safeIntegers { 1 - maxExactInteger, maxExactInteger - 1 };

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

static TypeID narrowExpectedType(const Program& program, TypeArena& types, TypeID expected, TypeID type)
{
    if (!expected)
        return { };
    TypeID narrowed = types.intersectionOf(type, expected);
    if (narrowed == TypeTable::never || narrowed == type || !types.at(narrowed).isSubsetOf(types.at(type), program))
        return { };
    return narrowed;
}

void replaceKeepingType(FunctionPassContext& context, ValueID original, Value&& rewritten)
{
    Function& function = context.function;
    BlockID block = context.analyses.useLists().blockOf(original);
    Value& value = function.edit(original);

    ReplacedEffects replaced;
    if (context.build.options().verifiesProgram)
        replaced.add({ context.program, context.inputs.effects }, function, value);

    TypeID expected = value.expected();
    rewritten.setOrigin(value.origin());
    value = WTF::move(rewritten);
    value.type = resultTypeOf(context.program, function, value, context.types);
    value.setExpected(narrowExpectedType(context.program, context.types, expected, value.type));
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

static unsigned rewriteValues(FunctionPassContext& context, SiteList list, NOESCAPE const Invocable<std::optional<Value>(const Value&)> auto& simplify)
{
    const Function& function = context.function;
    Vector<ValueRewrite> rewrites;
    for (const ValueSite& site : context.sitesIn(list)) {
        std::optional<Value> rewritten = simplify(function.at(site.value));
        if (rewritten && keepsTypeOf(context, site.value, *rewritten))
            rewrites.append(ValueRewrite { site.value, WTF::move(*rewritten) });
    }

    for (ValueRewrite& rewrite : rewrites)
        replaceKeepingType(context, rewrite.original, WTF::move(rewrite.rewritten));
    if (!rewrites.isEmpty())
        context.analyses.didChangeValues();
    return static_cast<unsigned>(rewrites.size());
}

static unsigned rewriteBuiltinCalls(FunctionPassContext& context, const BuiltinCallSites& calls, NOESCAPE const Invocable<std::optional<Value>(const Value&)> auto& simplify)
{
    const Function& function = context.function;
    Vector<ValueRewrite> rewrites;
    for (const ValueSite& site : calls) {
        std::optional<Value> rewritten = simplify(function.at(site.value));
        if (rewritten && keepsTypeOf(context, site.value, *rewritten))
            rewrites.append(ValueRewrite { site.value, WTF::move(*rewritten) });
    }

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

static bool mayBeMadeByBuiltinConstructor(const Program& program, LayoutID layout)
{
    for (size_t steps = 0; steps < program.layouts.size(); ++steps) {
        const LayoutFlow& flow = program.at(layout).flow;
        if (flow.baseState == BaseState::None)
            return false;
        if (flow.baseState != BaseState::Layout)
            return true;
        layout = flow.base;
    }
    return true;
}

static bool mayBeMadeByBuiltinConstructor(const Program& program, const Type& type)
{
    if (!type.layouts)
        return false;
    return std::ranges::any_of(program.at(type.layouts).members, [&](LayoutID layout) {
        return mayBeMadeByBuiltinConstructor(program, layout);
    });
}

static Type typeOfArraysAmong(FunctionPassContext& context, const Type& operand)
{
    Type arrays = Type::builtinClass(BuiltinClasses::Array);
    if (operand.builtins.contains(Builtins::ArrayPrototype))
        arrays = arrays.unionWith(Type::forBuiltin(Builtins::ArrayPrototype), context.types);
    return arrays;
}

static std::optional<Value> simplifyTestOfCellKind(FunctionPassContext& context, const Value& test)
{
    const BuiltinTable& table = context.program.builtins();
    const Type& operand = context.program.typeOf(context.function, test.valueOperand());
    JSType first = toJSType(test.first());
    JSType last = toJSType(test.last());
    if (first > last || mayBeMadeByBuiltinConstructor(context.program, operand))
        return std::nullopt;

    Type tested;
    BitVector kindsWithClass;
    for (uint32_t index = 0; index < table.classCount(); ++index) {
        const BuiltinClass& candidate = table.at(BuiltinClassID(index));
        if (candidate.origin != BuiltinOrigin::Engine || candidate.instanceTest != BuiltinInstanceTest::JSTypeEquals || candidate.type < first || candidate.type > last)
            continue;
        if (candidate.id == BuiltinClasses::Array)
            return std::nullopt;
        tested = tested.unionWith(Type::builtinClass(candidate.id), context.types);
        kindsWithClass.set(candidate.type - first);
    }
    if (kindsWithClass.bitCount() != static_cast<size_t>(last - first) + 1)
        return std::nullopt;
    return makeTypeTest(test.valueOperand(), context.types.intern(WTF::move(tested)));
}

static std::optional<Value> simplifyIntrinsic(FunctionPassContext& context, const Value& intrinsic)
{
    switch (intrinsic.opcode) {
    case Opcode::HasJSType:
        return simplifyTestOfCellKind(context, intrinsic);
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
    return rewriteValues(context, SiteList::SimplifiedIntrinsics, [&](const Value& value) {
        return simplifyIntrinsic(context, value);
    });
}

namespace {

struct BuiltinOwnPropertyTest {
    ValueID object;
    PropertyKeyID key;
};

} // anonymous namespace

static std::optional<BuiltinOwnPropertyTest> findOwnPropertyTest(FunctionPassContext& context, const Value& value)
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
    if (!object || !keyOperand)
        return std::nullopt;

    PropertyKeyID key = internKey(context.types, constantKeyOf(program, program.typeOf(context.function, keyOperand)));
    if (!key)
        return std::nullopt;
    return BuiltinOwnPropertyTest { object, key };
}

static Value makeOwnPropertyTest(const Value& call, ValueID object, PropertyKeyID key)
{
    Value test = IRBuilder::make(Opcode::PropHasOwn, { object });
    test.setKey(key);
    test.setAccessSite(call.accessSite());
    return test;
}

static std::optional<Value> simplifyOwnPropertyTest(FunctionPassContext& context, const Value& value)
{
    std::optional<BuiltinOwnPropertyTest> found = findOwnPropertyTest(context, value);
    if (!found || !context.program.typeOf(context.function, found->object).isObject())
        return std::nullopt;
    Value test = makeOwnPropertyTest(value, found->object, found->key);
    if (!keepsEffectsOf(context, value, test))
        return std::nullopt;
    return test;
}

static unsigned simplifyOwnPropertyTestsOfWhatMayBeMissing(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    const Program& program = context.program;
    Function& function = context.function;
    Vector<std::pair<ValueSite, BuiltinOwnPropertyTest>> tests;
    for (const ValueSite& site : calls) {
        std::optional<BuiltinOwnPropertyTest> found = findOwnPropertyTest(context, function.at(site.value));
        if (!found)
            continue;
        const Type& object = program.typeOf(function, found->object);
        if (!object.isObject() && object.hasObjects() && object.withoutNullish().isObject())
            tests.append({ site, *found });
    }
    if (tests.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (auto& [site, test] : tests) {
        builder.setOrigin(function.at(site.value).origin());
        ValueID object = builder.insertBefore(site.point(), IRBuilder::make(Opcode::ToObject, { test.object }));
        replaceKeepingType(context, site.value, makeOwnPropertyTest(function.at(site.value), object, test.key));
    }
    builder.applyInsertions();
    context.analyses.didChangeValues();
    return static_cast<unsigned>(tests.size());
}

unsigned simplifyOwnPropertyTests(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    unsigned rewrites = rewriteBuiltinCalls(context, calls, [&](const Value& value) {
        return simplifyOwnPropertyTest(context, value);
    });
    return rewrites + simplifyOwnPropertyTestsOfWhatMayBeMissing(context, calls);
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
    if (builtin == Builtins::NumberIsInteger || builtin == Builtins::NumberIsFinite || builtin == Builtins::IsFinite)
        return KnownBoolean::True;
    if (builtin == Builtins::NumberIsNaN || builtin == Builtins::IsNaN)
        return KnownBoolean::False;
    if (builtin != Builtins::NumberIsSafeInteger)
        return KnownBoolean::Unknown;
    if (operand.integers.isSubsetOf(safeIntegers))
        return KnownBoolean::True;
    return operand.integers.intersectWith(safeIntegers).isEmpty() ? KnownBoolean::False : KnownBoolean::Unknown;
}

static KnownBoolean evaluateQuestionAboutClass(const Program& program, BuiltinID builtin, const Type& operand)
{
    const BuiltinTable& table = program.builtins();
    auto isOfKind = [&](BuiltinClassID builtinClass) {
        for (BuiltinClassID current = builtinClass; current; current = table.at(current).parent) {
            if (builtin == Builtins::ErrorIsError ? current == BuiltinClasses::Error : current == BuiltinClasses::TypedArray || current == BuiltinClasses::DataView)
                return true;
        }
        return false;
    };
    if (builtin != Builtins::ErrorIsError && builtin != Builtins::ArrayBufferIsView)
        return KnownBoolean::Unknown;
    if (!operand.hasObjects())
        return KnownBoolean::False;
    if (operand.anyObject)
        return KnownBoolean::Unknown;
    if (operand.layouts) {
        bool areRecords = std::ranges::all_of(program.at(operand.layouts).members, [&](LayoutID layout) {
            return program.at(layout).kind == LayoutKind::Record;
        });
        if (!areRecords)
            return KnownBoolean::Unknown;
    }
    bool hasOthers = operand.primitives || operand.layouts || operand.anyFunction || !operand.functions.isEmpty() || !operand.builtins.isEmpty();
    bool hasSome = false;
    for (const ClassPart& part : operand.classes) {
        if (isOfKind(part.builtinClass))
            hasSome = true;
        else if (table.at(part.builtinClass).origin == BuiltinOrigin::Engine)
            hasOthers = true;
        else
            return KnownBoolean::Unknown;
    }
    if (hasSome && hasOthers)
        return KnownBoolean::Unknown;
    return hasSome ? KnownBoolean::True : KnownBoolean::False;
}

static bool isOfNoClassOfHost(const Program& program, const Type& operand)
{
    if (operand.anyObject || operand.anyFunction || !operand.builtins.isEmpty())
        return false;
    if (operand.layouts) {
        bool areMadeByProgram = std::ranges::all_of(program.at(operand.layouts).members, [&](LayoutID layout) {
            return program.at(layout).kind == LayoutKind::Record || !mayBeMadeByBuiltinConstructor(program, layout);
        });
        if (!areMadeByProgram)
            return false;
    }
    return std::ranges::all_of(operand.classes, [&](const ClassPart& part) {
        return program.builtins().at(part.builtinClass).origin == BuiltinOrigin::Engine;
    });
}

static bool returnsOperandUnchanged(BuiltinID builtin, const Type& operand)
{
    if (std::ranges::contains(roundingFunctions, builtin))
        return operand.isInteger();
    if (builtin == Builtins::Boolean)
        return operand.isBoolean();
    if (builtin == Builtins::String)
        return operand.isAnyString();
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

    if (overload->builtin == Builtins::PrivateGlobalsInherits && arguments.size() == 2 && isOfNoClassOfHost(context.program, context.program.typeOf(context.function, arguments[1])))
        return makeBooleanConstant(false);

    const Type& operand = context.program.typeOf(context.function, arguments[0]);
    if (returnsOperandUnchanged(overload->builtin, operand))
        return makeIdentityCast(context.function, arguments[0]);
    // Number(value) and ToNumber differ only for a BigInt.
    if (overload->builtin == Builtins::Number && !operand.isNever() && operand.isNumberLike())
        return IRBuilder::make(Opcode::ToNumber, { arguments[0] });
    if (overload->builtin == Builtins::ArrayIsArray && !operand.anyObject && !operand.findClassPart(BuiltinClasses::Proxy) && !mayBeMadeByBuiltinConstructor(context.program, operand)) {
        return makeTypeTest(arguments[0], context.types.intern(typeOfArraysAmong(context, operand)));
    }

    KnownBoolean answer = evaluateNumberTest(overload->builtin, operand);
    if (answer == KnownBoolean::Unknown)
        answer = evaluateQuestionAboutClass(context.program, overload->builtin, operand);
    switch (answer) {
    case KnownBoolean::Unknown:
        break;
    case KnownBoolean::False:
        return makeBooleanConstant(false);
    case KnownBoolean::True:
        return makeBooleanConstant(true);
    }

    if (mayBeMadeByBuiltinConstructor(context.program, operand))
        return std::nullopt;
    if (overload->builtin == Builtins::ErrorIsError)
        return makeTypeTest(arguments[0], context.types.intern(Type::builtinClass(BuiltinClasses::Error)));
    if (overload->builtin == Builtins::ArrayBufferIsView) {
        const BuiltinTable& table = context.program.builtins();
        Type views = Type::builtinClass(BuiltinClasses::DataView);
        for (uint32_t index = 0; index < table.classCount(); ++index) {
            BuiltinClassID builtinClass(index);
            if (table.at(builtinClass).origin == BuiltinOrigin::Engine && table.at(builtinClass).typedArrayType != NotTypedArray)
                views = views.unionWith(Type::builtinClass(builtinClass), context.types);
        }
        return makeTypeTest(arguments[0], context.types.intern(WTF::move(views)));
    }
    return std::nullopt;
}

unsigned simplifyPrimitiveCalls(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    return rewriteBuiltinCalls(context, calls, [&](const Value& value) {
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
    return rewriteValues(context, SiteList::Int32Conversions, [&](const Value& value) {
        return simplifyInt32Conversion(context, value);
    });
}

static bool isLiteralWithAllMembers(const Program& program, const Value& literal)
{
    if (literal.opcode != Opcode::NewObject || literal.prototypeOperand())
        return false;
    return program.at(literal.layout()).kind == LayoutKind::Record && literal.fields().size() == literal.valuesOperands().size();
}

static bool handsObjectOn(Opcode opcode)
{
    return isCastWithoutTest(opcode) || opcode == Opcode::FinishObject;
}

static ValueID findObjectHandedOn(const Function& function, ValueID operand)
{
    ValueID link = operand;
    while (handsObjectOn(function.at(link).opcode))
        link = function.at(link).operand(0);
    return link;
}

static bool isSeenOnlyAsLiteralOf(FunctionPassContext& context, std::span<const CallWithLiteralParts> calls, ValueID value)
{
    const Function& function = context.function;
    return std::ranges::all_of(context.analyses.useLists().usesOf(value), [&](const Use& use) {
        if (use.kind != Use::Kind::Operand)
            return false;
        if (handsObjectOn(function.at(use.user).opcode))
            return isSeenOnlyAsLiteralOf(context, calls, use.user);
        bool isLiteralOfCall = std::ranges::any_of(calls, [&](const CallWithLiteralParts& call) {
            return call.call.value == use.user && call.literalOperand == value;
        });
        return isLiteralOfCall && std::ranges::count(function.at(use.user).argumentsOperands(), value) == 1;
    });
}

static bool collectLiteralParts(const Program& program, const Value& literal, std::span<const BuiltinName> names, std::span<ValueID> parts)
{
    const Layout& layout = program.at(literal.layout());
    std::span<const ValueID> values = literal.valuesOperands();
    for (size_t index = 0; index < values.size(); ++index) {
        PropertyKeyID key = layout.fields[literal.fields()[index]].key;
        BuiltinName name = key ? program.builtinNameOf(key) : BuiltinName();
        auto position = name ? std::ranges::find(names, name) : names.end();
        if (position == names.end())
            return false;
        parts[position - names.begin()] = values[index];
    }
    return true;
}

static bool takesOperandsAsTheyAre(const TypingContext& typing, BuiltinOverloadID id, std::span<const TypeID> arguments)
{
    const BuiltinOverload& overload = typing.program.builtins().at(id);
    if (overload.mode != BuiltinCallMode::Call || overload.receiver || overload.parameters.size() != arguments.size())
        return false;
    for (unsigned index = 0; index < arguments.size(); ++index) {
        if (overload.parameters[index].conversion != BuiltinConversion::None)
            return false;
        if (!typing.types.at(arguments[index]).isSubsetOf(parameterTypeOfOverload(typing.program, id, BuiltinOperand::argument(index)), typing.program))
            return false;
    }
    return true;
}

static BuiltinOverloadID findFormTakingParts(FunctionPassContext& context, const Value& call, BuiltinID form, std::span<const ValueID> arguments)
{
    const BuiltinTable& builtins = context.program.builtins();
    TypingContext typing { context.program, context.function, context.types };
    EffectContext effectContext { context.program, context.inputs.effects };
    BuiltinOperandTypes operands;
    for (ValueID argument : arguments)
        operands.arguments.append(argument ? context.function.at(argument).type : TypeTable::undefined);

    for (BuiltinOverloadID overload : builtins.overloadIDsOf(form)) {
        if (!takesOperandsAsTheyAre(typing, overload, operands.arguments.span()))
            continue;
        if (!resultTypeOfBuiltinCall(typing, { .overload = overload, .operands = operands }, 0).isSubsetOf(context.types.at(call.type), context.program))
            return { };
        if (!effectsOfOverload(effectContext, overload, nullptr, operands.arguments.span()).isSubsetOf(effectsOf(effectContext, context.function, call)))
            return { };
        return overload;
    }
    return { };
}

static std::optional<CallWithLiteralParts> matchCallWithLiteralParts(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const Value& call = function.at(site.value);
    if (call.opcode != Opcode::CallBuiltin || (call.builtinCallForm() != BuiltinCallForm::Whole && call.builtinCallForm() != BuiltinCallForm::AnyReceiver) || call.receiverOperand())
        return std::nullopt;

    const BuiltinTable& builtins = program.builtins();
    BuiltinID called = builtins.canonicalID(builtins.at(call.overload()).builtin);
    const BuiltinForms* forms = builtins.formsOf(called);
    if (!forms || !forms->literalPartsForm)
        return std::nullopt;
    if (builtins.at(call.overload()).mode != (builtins.at(called).kind == BuiltinKind::Constructor ? BuiltinCallMode::Construct : BuiltinCallMode::Call))
        return std::nullopt;

    std::span<const ValueID> arguments = call.argumentsOperands();
    size_t literalArgument = forms->literalPartsArgument->argumentIndex();
    if (arguments.size() != literalArgument + 1)
        return std::nullopt;

    ValueID literal = findObjectHandedOn(function, arguments[literalArgument]);
    if (!isLiteralWithAllMembers(program, function.at(literal)))
        return std::nullopt;

    std::span<const BuiltinName> names = std::span(forms->literalPartNames).first(std::ranges::count_if(forms->literalPartNames, [](BuiltinName name) {
        return !!name;
    }));
    CallWithLiteralParts match { site, literal, arguments[literalArgument], { }, Vector<ValueID, 4>(arguments.first(literalArgument)) };
    match.arguments.grow(literalArgument + names.size());
    if (!collectLiteralParts(program, function.at(literal), names, match.arguments.mutableSpan().subspan(literalArgument)))
        return std::nullopt;

    match.form = findFormTakingParts(context, call, forms->literalPartsForm, match.arguments.span());
    if (!match.form)
        return std::nullopt;
    return match;
}

static unsigned takeLiteralArgumentsApart(FunctionPassContext& context, const BuiltinCallSites& calls)
{
    Function& function = context.function;
    Vector<CallWithLiteralParts> candidates;
    for (const ValueSite& site : calls) {
        if (std::optional<CallWithLiteralParts> match = matchCallWithLiteralParts(context, site))
            candidates.append(WTF::move(*match));
    }
    Vector<CallWithLiteralParts> matches;
    for (const CallWithLiteralParts& candidate : candidates) {
        if (isSeenOnlyAsLiteralOf(context, candidates.span(), candidate.literal))
            matches.append(candidate);
    }
    if (matches.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (CallWithLiteralParts& match : matches) {
        builder.setOrigin(function.at(match.call.value).origin());
        for (ValueID& argument : match.arguments) {
            if (!argument)
                argument = builder.insertBefore(match.call.point(), IRBuilder::make(Opcode::Undefined));
        }
    }
    builder.applyInsertions();

    for (const CallWithLiteralParts& match : matches) {
        const Value& call = function.at(match.call.value);
        Value rewritten = makeBuiltinCall(match.form, BuiltinCallForm::Whole, { { }, match.arguments }, { call.expectedResult(), call.accessSite(), call.allocationSite() });
        replaceKeepingType(context, match.call.value, WTF::move(rewritten));
    }
    context.analyses.didChangeValues();
    context.build.statistics().count(StatisticID::LiteralArgumentsTakenApart, matches.size());
    return static_cast<unsigned>(matches.size());
}

PassOutcome simplifyBuiltinCalls(FunctionPassContext& context)
{
    unsigned rewrites = 0;
    auto hasSitesIn = [&](SiteList list) {
        return !context.sitesIn(list).empty();
    };
    if (hasSitesIn(SiteList::SimplifiedIntrinsics))
        rewrites += simplifyIntrinsics(context);
    if (hasSitesIn(SiteList::TemplateObjects))
        rewrites += replaceTemplateObjectsReadByPosition(context);
    if (context.hasConstantListSource() && hasSitesIn(SiteList::ConstantListSpreads))
        rewrites += takeConstantListsApart(context);
    if (context.hasConstantListSource() && hasSitesIn(SiteList::ConstantListCalls)) {
        rewrites += simplifyConcatenationsOfLiterals(context);
        rewrites += simplifySearchesOfLiterals(context);
    }
    if (!hasSitesIn(SiteList::BuiltinCalls)) {
        bool hasTruncation = std::ranges::any_of(context.sitesIn(SiteList::SimplifiedIntrinsics), [&](const ValueSite& site) {
            return context.function.at(site.value).opcode == Opcode::ToIntegerOrInfinity;
        });
        if (hasTruncation && hasSitesIn(SiteList::Int32Conversions))
            rewrites += simplifyInt32Conversions(context);
        if (!rewrites)
            return PassOutcome::Unchanged;
        context.keptAnalyses = allAnalyses;
        return PassOutcome::Changed;
    }

    BuiltinCallSites calls = collectBuiltinCallSites(context);
    auto didRewrite = [&](unsigned number) {
        if (!number)
            return;
        rewrites += number;
        calls = collectBuiltinCallSites(context);
    };
    didRewrite(simplifyOwnPropertyTests(context, calls));
    didRewrite(simplifyPrimitiveCalls(context, calls));
    if (hasSitesIn(SiteList::Int32Conversions))
        didRewrite(simplifyInt32Conversions(context));
    didRewrite(simplifyStringReads(context, calls));
    didRewrite(simplifyConcatenatingCalls(context, calls));
    didRewrite(simplifyMatches(context, calls));
    didRewrite(simplifyLastPiecesOfSplits(context, calls));
    didRewrite(simplifySplitsOfConstants(context, calls));
    didRewrite(simplifyPromiseAllOfLiterals(context, calls));
    didRewrite(simplifyPromiseResolve(context, calls));
    didRewrite(leaveURLsUnbuilt(context));
    didRewrite(expandRangesOfViews(context, calls));
    didRewrite(takeLiteralArgumentsApart(context, calls));

    PassOutcome outcome = rewrites ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= expandStringRanges(context, calls);
    if (stepAwaitedAsyncGenerators(context))
        return PassOutcome::Changed;
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
