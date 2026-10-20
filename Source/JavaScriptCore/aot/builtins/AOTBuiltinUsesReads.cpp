/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinUses.h"

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTValueSubstitution.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct MemberSite {
    ValueID receiver;
    PropertyKeyID key;
    TypeID testedReceiverType;
    BuiltinReceiver start;
    BuiltinMemberAnswer answer;
};

} // anonymous namespace

// What flow could not follow is in a prediction beside the objects that it did see. An operation that is about one class finds that class among them.
static BuiltinReceiver predictedReceiverOf(const Program& program, const Type& predicted, BuiltinClassID expectedClass)
{
    if (BuiltinReceiver receiver = builtinReceiverOf(program, predicted))
        return receiver;
    if (expectedClass && predicted.findClassPart(expectedClass))
        return { BuiltinReceiverKind::Instance, { }, expectedClass };
    return { };
}

static std::optional<MemberSite> findMember(const BuiltinUseInputs& inputs, ValueID receiver, PropertyKeyID key, BuiltinClassID expectedClass = { })
{
    const Program& program = inputs.program;
    Type present = program.typeOf(inputs.function, receiver).withoutNullish();
    if (present.isNever())
        return std::nullopt;

    MemberSite site { .receiver = receiver, .key = key };
    site.start = builtinReceiverOf(program, present);
    if (!site.start) {
        TypeID prediction = inputs.function.at(receiver).prediction();
        if (!prediction)
            return std::nullopt;
        Type predicted = inputs.types.at(prediction).intersectWith(present, inputs.types).withoutNullish();
        site.start = predictedReceiverOf(program, predicted, expectedClass);
        switch (site.start.kind) {
        case BuiltinReceiverKind::Unknown:
            return std::nullopt;
        case BuiltinReceiverKind::BuiltinObject: {
            Type testedReceiver = Type::forBuiltin(site.start.object);
            if (!testedReceiver.isSingleValue())
                return std::nullopt;
            site.testedReceiverType = inputs.types.intern(testedReceiver);
            break;
        }
        case BuiltinReceiverKind::Instance:
        case BuiltinReceiverKind::Primitive: {
            std::optional<Type> testedReceiver = testedTypeOfInstances(program, site.start.builtinClass, predicted);
            if (!testedReceiver)
                return std::nullopt;
            site.testedReceiverType = inputs.types.intern(*testedReceiver);
            break;
        }
        }
    }

    BuiltinLookupContext lookup = inputs.lookup();
    site.answer = site.start.kind == BuiltinReceiverKind::BuiltinObject
        ? findMemberFrom(lookup, site.start.object, key)
        : findMemberOfInstances(lookup, site.start.builtinClass, key);
    if (!site.answer.isFound())
        return std::nullopt;
    return site;
}

// No function is ever pristine. Flow says of the closures of a function whether one of them gets the key as a property of its own or another prototype.
static bool isClosureAsCreated(const BuiltinUseInputs& inputs, const MemberSite& site)
{
    if (site.start.builtinClass != BuiltinClasses::Function || site.testedReceiverType)
        return false;
    const Type& receiver = inputs.program.typeOf(inputs.function, site.receiver);
    if (receiver.anyFunction || !receiver.classes.isEmpty() || receiver.functions.isEmpty())
        return false;
    return std::ranges::all_of(receiver.functions, [&](FunctionID function) {
        // A class object is created with the static members of its definition, below the class object of its base.
        LayoutID constructedClass = inputs.program.at(function).constructedClass;
        if (constructedClass && !inputs.program.at(constructedClass).has(LayoutFlags::IsConstructorFunction))
            return false;
        // The closure of a coroutine inherits from the prototype of its own kind of function.
        if (inputs.program.at(function).hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }))
            return false;
        return inputs.flow.areClosuresAsCreated(function, singleElementSpan(site.key));
    });
}

static void addReceiverRequirements(const BuiltinUseInputs& inputs, const MemberSite& site, ProgramPoint point, BuiltinUsePlan& plan, bool& isPossible)
{
    const BuiltinTable& table = inputs.program.builtins();
    UseRequirement requirement { site.receiver, site.testedReceiverType, { } };

    bool isInherited = site.start.kind == BuiltinReceiverKind::Instance && !table.at(site.answer.member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
    if (isInherited && !isClosureAsCreated(inputs, site)) {
        PristineRequirement names { site.start.builtinClass, PristineAspects::Names };
        if (pristineProofOf(inputs.pristine(), site.receiver, names, point) != PristineProof::Proven) {
            if (pristineTestOf(table, names.builtinClass) == PristineTest::NeverPasses) {
                isPossible = false;
                return;
            }
            requirement.pristine = names;
        }
    }
    if (!requirement.requiredType && !requirement.pristine)
        return;
    if (!prependRequirement(inputs.types, plan, WTF::move(requirement)))
        isPossible = false;
}

static NullishCheck nullishCheckFor(const Program& program, NullishAccess access, BuiltinName name)
{
    return { access, program.builtins().isSymbolName(name) ? StringID() : program.stringOf(name) };
}

static bool hasAccessorFunction(const BuiltinTable& table, BuiltinID member, BuiltinCallMode mode)
{
    switch (mode) {
    case BuiltinCallMode::Get:
        return BuiltinCellKey::getter(member).hasCell(table);
    case BuiltinCallMode::Set:
        return BuiltinCellKey::setter(member).hasCell(table);
    case BuiltinCallMode::Call:
    case BuiltinCallMode::Construct:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool mayCallAccessorFunction(const BuiltinTable& table, const BuiltinUsePlan& plan)
{
    if (plan.kind != BuiltinUseKind::Call)
        return true;

    const BuiltinImplementation& implementation = table.at(plan.call.overload).code.implementation;
    switch (implementation.kind) {
    case BuiltinImplementationKind::Generic:
    case BuiltinImplementationKind::HostCall:
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::CachedGetter:
        return true;
    case BuiltinImplementationKind::Inline:
        return implementation.otherwise.has_value();
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinUsePlan planMemberAccess(const BuiltinUseInputs& inputs, ValueID use, BuiltinID member, BuiltinCallMode mode, ProgramPoint point)
{
    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    if (!table.dispatchOf(member, mode))
        return { };

    const Value& access = inputs.function.at(use);
    bool isStore = mode == BuiltinCallMode::Set;

    KnownBuiltinCall known;
    known.callee = member;
    known.mode = mode;
    known.operands.receiver = access.objectOperand();
    if (isStore)
        known.operands.arguments.append(access.valueOperand());
    known.stamps.accessSite = access.accessSite();
    if (known.stamps.accessSite)
        known.stamps.allocationSite = inputs.flow.resultSiteOf(known.stamps.accessSite);
    known.nullishReceiver = NullishReceiver::ThrowsAtRead;

    BuiltinUsePlan plan = planKnownBuiltinCall(inputs, known, point);
    if (!plan)
        return { };
    if (isStore && access.mode() == ECMAMode::Sloppy && table.at(plan.call.overload).throws())
        return { };
    if (mayCallAccessorFunction(table, plan) && !hasAccessorFunction(table, member, mode))
        return { };
    if (plan.kind == BuiltinUseKind::UnmatchedCall && program.typeOf(inputs.function, known.operands.receiver).mayBeUndefinedOrNull())
        return { };
    plan.nullishCheck = nullishCheckFor(program, isStore ? NullishAccess::Write : NullishAccess::Read, builtinMemberNameOf(program, access.key()).name);
    return plan;
}

static BuiltinUsePlan planFoundMemberAccess(const BuiltinUseInputs& inputs, const MemberSite& site, ValueID use, BuiltinCallMode mode, ProgramPoint point)
{
    const Builtin& member = inputs.program.builtins().at(site.answer.member);
    bool isOwnProperty = member.kind == BuiltinKind::DataProperty && member.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
    if (!isOwnProperty && !site.answer.isConstant())
        return { };

    BuiltinUsePlan plan = planMemberAccess(inputs, use, site.answer.member, mode, point);
    if (!plan)
        return { };

    bool isPossible = true;
    addReceiverRequirements(inputs, site, point, plan, isPossible);
    if (!isPossible)
        return { };
    if (plan.kind == BuiltinUseKind::UnmatchedCall && !plan.requirements.isEmpty())
        return { };
    return plan;
}

static BuiltinUsePlan planConstantRead(const BuiltinUseInputs& inputs, const MemberSite& site, ProgramPoint point)
{
    if (!site.answer.isConstant())
        return { };

    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    BuiltinUsePlan plan;
    plan.kind = BuiltinUseKind::Constant;
    plan.member = site.answer.member;
    plan.nullishCheck = nullishCheckFor(program, NullishAccess::Read, builtinMemberNameOf(program, site.key).name);
    if (BuiltinName symbol = table.nameOfSymbol(plan.member))
        plan.key = program.keyOf(symbol);
    else if (!tryMakeBuiltinConstant(table, plan.member))
        return { };

    bool isPossible = true;
    addReceiverRequirements(inputs, site, point, plan, isPossible);
    if (!isPossible)
        return { };
    return plan;
}

// An array that finds Array under `constructor` gets a new array: the Symbol.species of Array answers its receiver.
BuiltinUsePlan planArraySpeciesCreate(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    const Value& creation = inputs.function.at(use);
    if (!program.typeOf(inputs.function, creation.lengthOperand()).isInteger())
        return { };

    std::optional<MemberSite> site = findMember(inputs, creation.arrayOperand(), program.keyOf(BuiltinNames::constructor), BuiltinClasses::Array);
    if (!site || site->start.kind != BuiltinReceiverKind::Instance || site->start.builtinClass != BuiltinClasses::Array)
        return { };
    if (!site->testedReceiverType && program.typeOf(inputs.function, site->receiver).mayBeUndefinedOrNull())
        return { };
    if (!site->answer.isConstant() || table.canonicalID(site->answer.member) != Builtins::Array)
        return { };
    BuiltinMemberAnswer species = findMemberFrom(inputs.lookup(), Builtins::Array, program.keyOf(BuiltinNames::speciesSymbol));
    if (!species.isConstant() || !isLockedLocation(program, Builtins::Array, program.keyOf(BuiltinNames::speciesSymbol)))
        return { };

    BuiltinUsePlan plan;
    plan.kind = BuiltinUseKind::NewArray;
    bool isPossible = true;
    addReceiverRequirements(inputs, *site, point, plan, isPossible);
    if (!isPossible)
        return { };
    return plan;
}

static BuiltinUsePlan planInheritedMember(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point, BuiltinCallMode mode)
{
    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    const Value& access = inputs.function.at(use);
    const Type& receiverType = program.typeOf(inputs.function, access.objectOperand());
    if (!receiverType.hasOnlyLayouts() || hasOnlyNeverAllocatedLayouts(program, receiverType))
        return { };

    Access lookup = mode == BuiltinCallMode::Get ? Access::Read : Access::Store;
    MemberPartition partition = partitionMembers({ program, &inputs.flow }, membersOf(program, receiverType), access.key(), lookup);
    if (!partition.hasOneArm())
        return { };
    const MemberAnswer& answer = partition.arms[0].answer;
    if (answer.kind != MemberKind::Builtin || !answer.needsNoTest() || !answer.conditions.isEmpty())
        return { };
    if (isObjectKind(table.at(answer.builtin).kind))
        return { };
    return planMemberAccess(inputs, use, table.canonicalID(answer.builtin), mode, point);
}

BuiltinUsePlan planBuiltinRead(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const Value& read = inputs.function.at(use);
    if (read.isForTypeOf() || !builtinMemberNameOf(inputs.program, read.key()))
        return { };

    std::optional<MemberSite> site = findMember(inputs, read.objectOperand(), read.key());
    if (!site)
        return planInheritedMember(inputs, use, point, BuiltinCallMode::Get);

    switch (inputs.program.builtins().at(site->answer.member).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
        if (inputs.analyses && isClosureAsCreated(inputs, *site) && isTestedByValidation(inputs.function, inputs.analyses->useLists(), use))
            return { };
        return planConstantRead(inputs, *site, point);
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        return planFoundMemberAccess(inputs, *site, use, BuiltinCallMode::Get, point);
    case BuiltinKind::NativeBinding:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinUsePlan planBuiltinStore(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const Value& store = inputs.function.at(use);
    if (!builtinMemberNameOf(inputs.program, store.key()))
        return { };

    std::optional<MemberSite> site = findMember(inputs, store.objectOperand(), store.key());
    if (!site)
        return planInheritedMember(inputs, use, point, BuiltinCallMode::Set);

    switch (inputs.program.builtins().at(site->answer.member).kind) {
    case BuiltinKind::Accessor:
    case BuiltinKind::DataProperty:
        return planFoundMemberAccess(inputs, *site, use, BuiltinCallMode::Set, point);
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
    case BuiltinKind::NativeBinding:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
