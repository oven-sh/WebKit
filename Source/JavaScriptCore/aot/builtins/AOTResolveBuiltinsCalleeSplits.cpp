/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTExpectedTypesByUse.h"
#include "AOTTypeTestSite.h"
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
    case BuiltinImplementationKind::OwnSlot:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The table calls them host calls. The code for the call enters an operation that answers the usual operands itself.
static bool hasOperationInFrontOfHostCall(BuiltinID method)
{
    return method == Builtins::TextEncoderPrototypeEncode || method == Builtins::TextEncoderPrototypeEncodeInto || method == Builtins::TypedArrayPrototypeSubarray;
}

static bool isWorthAnArm(const Program& program, BuiltinID method)
{
    const BuiltinTable& table = program.builtins();
    if (hasOperationInFrontOfHostCall(method))
        return !!table.dispatchOf(method, BuiltinCallMode::Call);
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
    return type.anyObject && !function.at(id).expected();
}

// A method that is not among what the callee is expected to be gets no arm.
static bool isExpectedToBe(FunctionPassContext& context, ValueID callee, BuiltinID method)
{
    TypeID expected = context.function.at(callee).expected();
    return !expected || Type::forBuiltin(method).isSubsetOf(context.types.at(expected), context.program);
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

// What planCalleeSplit() asks first, of the call and of the type of its callee alone.
static bool mayHaveCalleeSplit(const Program& program, const Function& function, const Value& call)
{
    if (!call.calleeKey() || !call.thisValueOperand())
        return false;
    BuiltinMemberName name = builtinMemberNameOf(program, call.calleeKey());
    if (!name || name.isPrivateAlias)
        return false;
    return program.typeOf(function, call.calleeOperand()).anyObject && !hasCalleesToDispatchOver(program, call);
}

static std::optional<CalleeSplit> planCalleeSplit(FunctionPassContext& context, const ValueSite& site)
{
    const Program& program = context.program;
    const Function& function = context.function;
    const BuiltinTable& table = program.builtins();

    const Value& call = function.at(site.value);
    if (call.opcode != Opcode::Call)
        return std::nullopt;
    bool hasToStayTailCall = tailCallRequirementOf(function, site.block, site.value) == TailCallRequirement::Required;

    ValueID callee = call.calleeOperand();
    ValueID receiver = call.thisValueOperand();
    if (!call.calleeKey() || !receiver)
        return std::nullopt;
    BuiltinMemberName name = builtinMemberNameOf(program, call.calleeKey());
    if (!name || name.isPrivateAlias)
        return std::nullopt;

    if (!program.typeOf(function, callee).anyObject)
        return std::nullopt;
    if (hasCalleesToDispatchOver(program, call))
        return std::nullopt;
    // Which function the receiver is expected to be does not say what it inherits. The call that is passed on has that function as its callee, and the expectation with it.
    // The method that is read from a proven function is tested by the planner of tests, which leaves every tail call alone.
    const Type& receiverType = program.typeOf(function, receiver);
    bool knowsReceiver = !knowsNothingAbout(function, receiverType, receiver);
    if (!receiverType.anyObject && !tailCallRequirementOf(function, site.block, site.value))
        return std::nullopt;
    if (!knowsReceiver && !hasToStayTailCall && expectsReceiverOfCalledMethod(context, site.block, call))
        return std::nullopt;

    Vector<MemberOfClasses, 4> methods = methodsNamed(program, name.name);
    methods.removeAllMatching([&](const MemberOfClasses& method) {
        // What is expected of the receiver leads to such a method only where its prototype is locked. The function that was read says it anyway.
        bool isFoundByCalleeAlone = !hasToStayTailCall && receiverType.anyObject && hasOperationInFrontOfHostCall(method.member);
        if ((hasToStayTailCall || knowsReceiver) && !passesCallOn(table, method.member) && !isFoundByCalleeAlone)
            return true;
        // The arm of such a tail call is the call that the method passes on, whatever the method itself is made of.
        if (!hasToStayTailCall && !isWorthAnArm(program, method.member))
            return true;
        return !isExpectedToBe(context, callee, method.member);
    });
    if (methods.isEmpty())
        return std::nullopt;
    // Asked last of what has no effect of its own: few calls come this far, and the frequencies of a whole function are made for the first that does.
    if (context.analyses.blockFrequencies().classOf(site.block) != BlockFrequency::Normal)
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

Vector<CalleeSplit> planCalleeSplits(FunctionPassContext& context)
{
    Vector<CalleeSplit> splits;
    for (const ValueSite& site : context.sitesIn(SiteList::Calls)) {
        if (!mayHaveCalleeSplit(context.program, context.function, context.function.at(site.value)))
            continue;
        uint32_t state = RefusedSites::stateOf(context.function, site.value);
        if (context.refusedSites.contains(RefusalKind::CalleeSplit, site.value, state))
            continue;
        if (std::optional<CalleeSplit> split = planCalleeSplit(context, site))
            splits.append(WTF::move(*split));
        else
            context.refusedSites.add(RefusalKind::CalleeSplit, context.function, site.value, state);
    }
    return splits;
}

// Whoever finds the method under the name of the call is as a rule an instance of a class that has it. Nothing where no test finds the instances of one of these classes:
// they would take the arm of the receivers that are not expected.
static std::optional<Type> testedReceiverTypeOf(const Program& program, TypeArena& types, const MemberOfClasses& method)
{
    Type tested = Type::never();
    for (BuiltinClassID builtinClass : method.classes) {
        std::optional<Type> instances = testedTypeOfInstances(program, builtinClass, Type::anything());
        if (!instances)
            return std::nullopt;
        tested = tested.unionWith(*instances, types);
    }
    if (tested.isNever())
        return std::nullopt;
    return tested;
}

static void emitCalleeSplit(SiteExpansion& expansion, const CalleeSplit& split)
{
    FunctionPassContext& context = expansion.context();

    if (tailCallRequirementOf(context.function, split.site.block, split.site.value) == TailCallRequirement::Allowed)
        demoteTailCall(context, expansion.builder(), split.site.block);

    expansion.begin(split.site);
    IRBuilder& builder = expansion.builder();
    ValueID receiver = expansion.original().thisValueOperand();
    const Type& receiverType = context.program.typeOf(context.function, receiver);
    bool testsReceiver = knowsNothingAbout(context.function, receiverType, receiver) || (receiverType.anyObject && std::ranges::any_of(split.members, hasOperationInFrontOfHostCall, &MemberOfClasses::member));

    ValueSplit valueSplit { .value = split.callee, .arms = { }, .hasRest = true, .restFrequency = BlockFrequency::Unlikely };
    for (const MemberOfClasses& method : split.members)
        valueSplit.arms.append(SplitArm { calleeTypeOf(context.types, method), expansion.frequency() });

    emitSplit(expansion, valueSplit,
        [&](uint32_t armIndex, ValueID narrowedCallee) {
            auto finishArmWithCallOn = [&](ValueID narrowedReceiver) {
                ValueID call = expansion.appendOriginal({ OperandIndex::Call::callee, narrowedCallee, { }, { } });
                expansion.function().edit(call, ValueEdit::Definition).setThisValueOperand(narrowedReceiver);
                context.analyses.didChangeValues();
                expansion.finishArm(call);
            };

            std::optional<Type> testedReceiver = testsReceiver ? testedReceiverTypeOf(context.program, context.types, split.members[armIndex]) : std::nullopt;
            if (!testedReceiver) {
                finishArmWithCallOn(receiver);
                return;
            }

            // A passed test defines a value of the tested type, and the overloads of the method are matched against that.
            ValueID test = builder.appendTypeTest(receiver, *testedReceiver);
            IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(test, testedOperandIndex, expansion.frequency(), BlockFrequency::Unlikely);
            builder.moveToEndOf(branch.notTaken);
            finishArmWithCallOn(branch.provenOnNotTaken);
            builder.moveToEndOf(branch.taken);
            finishArmWithCallOn(branch.provenOnTaken);
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
