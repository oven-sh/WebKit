/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTTypePropagation.h"
#include "AOTTypeTestSite.h"
#include "AOTValuePredictions.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static bool hasCompiledLibraryFunction(const Program& program, const BuiltinOverload& overload)
{
    const RuntimeFunctionEntry* entry = findRuntimeFunction(program, overload.libraryFunction());
    return entry && entry->function && program.at(entry->function).hasBody();
}

static bool avoidsHostCall(const Program& program, const BuiltinOverload& overload)
{
    if (overload.mode != BuiltinCallMode::Call || overload.isUndeclared())
        return false;

    switch (overload.code.implementation.kind) {
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::HostCall:
        return false;
    case BuiltinImplementationKind::Compiled:
        return hasCompiledLibraryFunction(program, overload);
    case BuiltinImplementationKind::Inline:
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
    case BuiltinImplementationKind::CachedGetter:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isWorthAnArm(const Program& program, BuiltinID method)
{
    const BuiltinTable& table = program.builtins();
    return table.dispatchOf(method, BuiltinCallMode::Call) && std::ranges::any_of(table.overloadsOf(method), [&](const BuiltinOverload& overload) {
        return avoidsHostCall(program, overload);
    });
}

// The call that such a method passes on is a tail call by the language, so the arm of the method stays one.
static bool passesCallOn(const BuiltinTable& table, BuiltinID method)
{
    return std::ranges::any_of(table.overloadsOf(method), [](const BuiltinOverload& overload) {
        return forwardsCall(overload);
    });
}

static bool knowsNothingAbout(const Function& function, const Type& type, ValueID id)
{
    const Value& value = function.at(id);
    return type.anyObject && !value.written() && !value.prediction();
}

// Flow predicts what it saw arrive at a read it cannot bound. A method that is not among it gets no arm.
static bool mayBeUnknownMethod(const Function& function, const Type& type, ValueID callee)
{
    return type.anyObject && !function.at(callee).written();
}

static bool isPredictedToBe(FunctionPassContext& context, ValueID callee, BuiltinID method)
{
    TypeID prediction = context.function.at(callee).prediction();
    return !prediction || Type::forBuiltin(method).isSubsetOf(context.types.at(prediction), context.program);
}

static bool hasCalleesToDispatchOver(const Program& program, const Value& call)
{
    CalleeSetID stamp = call.callees();
    if (!stamp)
        return false;
    const CalleeSet& callees = program.at(stamp);
    return callees.isComplete || !callees.functions.isEmpty();
}

static TypeID calleeTypeOf(TypeArena& types, const MemberOfClasses& method)
{
    return types.intern(Type::forBuiltin(method.member));
}

static std::optional<CalleeSplit> planCalleeSplit(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const BuiltinTable& table = program.builtins();

    const Value& call = function.at(site.value);
    if (call.opcode != Opcode::Call || context.analyses.blockFrequencies().classOf(site.block) != BlockFrequency::Normal)
        return std::nullopt;
    bool hasToStayTailCall = tailCallRequirementOf(function, site.block, site.value) == TailCallRequirement::Required;

    ValueID callee = call.calleeOperand();
    ValueID receiver = call.thisValueOperand();
    if (!call.calleeKey() || !receiver)
        return std::nullopt;
    BuiltinMemberName name = builtinMemberNameOf(program, call.calleeKey());
    if (!name || name.isPrivateAlias)
        return std::nullopt;

    if (!mayBeUnknownMethod(function, program.typeOf(function, callee), callee))
        return std::nullopt;
    if (hasCalleesToDispatchOver(program, call))
        return std::nullopt;
    // Which function the receiver is expected to be does not say what it inherits. The call that is passed on has that function as its callee, and the expectation with it.
    // The method that is read from a proven function is tested by the planner of tests, which leaves every tail call alone.
    const Type& receiverType = program.typeOf(function, receiver);
    bool knowsReceiver = !knowsNothingAbout(function, receiverType, receiver);
    if (!receiverType.anyObject && !tailCallRequirementOf(function, site.block, site.value))
        return std::nullopt;

    Vector<MemberOfClasses, 4> methods = methodsNamed(table, name.name);
    methods.removeAllMatching([&](const MemberOfClasses& method) {
        if ((hasToStayTailCall || knowsReceiver) && !passesCallOn(table, method.member))
            return true;
        // The arm of such a tail call is the call that the method passes on, whatever the method itself is made of.
        if (!hasToStayTailCall && !isWorthAnArm(program, method.member))
            return true;
        return !isPredictedToBe(context, callee, method.member);
    });
    if (methods.isEmpty())
        return std::nullopt;

    if (context.exceeds(LimitID::MaxDispatchCases, methods.size(), site.value))
        methods.shrink(static_cast<size_t>(context.limit(LimitID::MaxDispatchCases)));
    methods.removeAllMatching([&](const MemberOfClasses& method) {
        return context.analyses.findEarlierTest(makeTypeTest(callee, calleeTypeOf(context.types, method)), site.block) == EarlierTest::Failed;
    });
    if (methods.isEmpty())
        return std::nullopt;

    return CalleeSplit { site, callee, WTF::move(methods) };
}

static TypeID predictedReceiverTypeOf(const BuiltinTable&, TypeArena&, const MemberOfClasses&);

// Every object that flow saw arrive as the callee is a method of built-in classes under the name of the call: the receivers it saw were their instances.
static bool isMoreThanCallOfNativeCode(const BuiltinTable& table, BuiltinID method)
{
    return std::ranges::any_of(table.overloadsOf(method), [](const BuiltinOverload& overload) {
        BuiltinImplementationKind kind = overload.code.implementation.kind;
        return kind != BuiltinImplementationKind::Generic && kind != BuiltinImplementationKind::HostCall;
    });
}

// A value that the host may have made brings every method of the name that a class of the host has. The test goes to the receivers whose call becomes code of its own:
// the others call native code with the test or without it.
static TypeID receiverTypeSeenByFlow(FunctionPassContext& context, const Value& call)
{
    TypeID prediction = context.function.at(call.calleeOperand()).prediction();
    BuiltinMemberName name = builtinMemberNameOf(context.program, call.calleeKey());
    if (!prediction || !name || name.isPrivateAlias)
        return { };

    const BuiltinTable& table = context.program.builtins();
    Type seenMethods = Type::never();
    TypeID receivers = TypeTable::never;
    TypeID receiversWorthTest = TypeTable::never;
    for (const MemberOfClasses& method : methodsNamed(table, name.name)) {
        if (!isPredictedToBe(context, call.calleeOperand(), method.member))
            continue;
        seenMethods = seenMethods.unionWith(Type::forBuiltin(method.member), context.types);
        TypeID receiversOfMethod = predictedReceiverTypeOf(table, context.types, method);
        receivers = context.types.unionOf(receivers, receiversOfMethod);
        if (isMoreThanCallOfNativeCode(table, method.member))
            receiversWorthTest = context.types.unionOf(receiversWorthTest, receiversOfMethod);
    }
    Type seenObjects = context.types.at(prediction).withOnlyObjects();
    if (seenObjects.isNever() || !seenObjects.isSubsetOf(seenMethods, context.program))
        return { };
    return receiversWorthTest == TypeTable::never ? receivers : receiversWorthTest;
}

static Vector<ValuePrediction, 4> findReceiversSeenByFlow(FunctionPassContext& context)
{
    const Function& function = context.function;
    auto isUnknown = [&](ValueID value) {
        return knowsNothingAbout(function, context.program.typeOf(function, value), value);
    };

    Vector<ValuePrediction, 4> receivers;
    forEachValueSite(context.analyses, function, [&](const ValueSite& site) {
        const Value& call = function.at(site.value);
        if (call.opcode != Opcode::Call || !call.calleeKey() || !call.thisValueOperand())
            return;
        ValueID receiver = context.analyses.identityOf(call.thisValueOperand());
        if (!isUnknown(call.thisValueOperand()) || !isUnknown(receiver))
            return;
        TypeID seen = receiverTypeSeenByFlow(context, call);
        if (!seen)
            return;
        size_t index = receivers.findIf([&](const ValuePrediction& earlier) {
            return earlier.value == receiver;
        });
        if (index == notFound)
            receivers.append(ValuePrediction { receiver, seen });
        else
            receivers[index].predicted = context.types.unionOf(receivers[index].predicted, seen);
    });
    return receivers;
}

// One test in front of all the uses of the receiver, and the reads of the methods fold too.
bool predictReceiversFromSeenMethods(FunctionPassContext& context)
{
    return predictValues(context, findReceiversSeenByFlow(context).span());
}

Vector<CalleeSplit> planCalleeSplits(FunctionPassContext& context)
{
    Vector<CalleeSplit> splits;
    forEachValueSite(context.analyses, context.function, [&](const ValueSite& site) {
        if (std::optional<CalleeSplit> split = planCalleeSplit(context, site))
            splits.append(WTF::move(*split));
    });
    return splits;
}

static TypeID predictedReceiverTypeOf(const BuiltinTable& table, TypeArena& types, const MemberOfClasses& method)
{
    Type instances = Type::never();
    for (BuiltinClassID builtinClass : method.classes)
        instances = instances.unionWith(typeOfInstances(table, builtinClass, Type::anything()), types);
    return types.intern(instances);
}

static void emitCalleeSplit(SiteExpansion& expansion, const CalleeSplit& split)
{
    FunctionPassContext& context = expansion.context();
    const BuiltinTable& table = context.program.builtins();

    if (tailCallRequirementOf(context.function, split.site.block, split.site.value) == TailCallRequirement::Allowed)
        demoteTailCall(context, expansion.builder(), split.site.block);

    expansion.begin(split.site);
    ValueID receiver = expansion.original().thisValueOperand();

    ValueSplit valueSplit { .value = split.callee, .arms = { }, .hasRest = true, .restFrequency = BlockFrequency::Unlikely };
    for (const MemberOfClasses& method : split.members)
        valueSplit.arms.append(SplitArm { calleeTypeOf(context.types, method), expansion.frequency() });

    emitSplit(expansion, valueSplit,
        [&](uint32_t armIndex, ValueID narrowedCallee) {
            ValueID predictedReceiver = receiver;
            if (knowsNothingAbout(context.function, context.program.typeOf(context.function, receiver), receiver)) {
                Value predict = IRBuilder::make(Opcode::Predict, { receiver });
                predict.setPredicted(predictedReceiverTypeOf(table, context.types, split.members[armIndex]));
                predictedReceiver = expansion.append(WTF::move(predict));
            }

            ValueID call = expansion.appendOriginal({ OperandIndex::Call::callee, narrowedCallee, { }, { } });
            expansion.function().at(call).setThisValueOperand(predictedReceiver);
            expansion.finishArm(call);
        },
        [&](ValueID narrowedCallee) {
            expansion.finishArm(expansion.appendOriginal({ OperandIndex::Call::callee, narrowedCallee, { }, { } }));
        });
    expansion.end();
}

void emitCalleeSplits(FunctionPassContext& context, std::span<const CalleeSplit> splits)
{
    if (splits.empty())
        return;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const CalleeSplit& split : splits | std::views::reverse)
        emitCalleeSplit(expansion, split);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
