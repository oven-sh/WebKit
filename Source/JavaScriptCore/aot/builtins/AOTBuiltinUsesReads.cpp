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
#include "AOTIRBuilder.h"
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

enum class LocationState : uint8_t { Unknown, NeedsTest, Constant };

} // anonymous namespace

// An expected type may list other objects beside the instances of a class. An operation that is about one class finds that class among them.
static BuiltinReceiver expectedReceiverOf(const Program& program, const Type& expected, BuiltinClassID expectedClass)
{
    if (BuiltinReceiver receiver = builtinReceiverOf(program, expected))
        return receiver;
    if (expectedClass && expected.findClassPart(expectedClass))
        return { BuiltinReceiverKind::Instance, { }, expectedClass };
    return { };
}

std::optional<Type> testedTypeOfExpectedInstances(const Program& program, TypeArena& types, BuiltinClassID builtinClass, const Type& expected, const Type& present)
{
    if (builtinClass != BuiltinClasses::Function)
        return testedTypeOfInstances(program, builtinClass, expected);
    if (FunctionID function = expected.singleFunction())
        return Type::forFunction(function);
    // The test lets through the built-in functions that the receiver may be. Each of them has members of its own, so behind the test the receiver would still be no instance like any other.
    if (builtinReceiverOf(program, present.castTo(Type::function(), types)).builtinClass != BuiltinClasses::Function)
        return std::nullopt;
    return Type::function();
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
        TypeID expectedType = inputs.function.at(receiver).expected();
        if (!expectedType)
            return std::nullopt;
        Type expected = inputs.types.at(expectedType).intersectWith(present, inputs.types).withoutNullish();
        site.start = expectedReceiverOf(program, expected, expectedClass);
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
            std::optional<Type> testedReceiver = testedTypeOfExpectedInstances(program, inputs.types, site.start.builtinClass, expected, present);
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

// False only where builtinReceiverOf() knows nothing of the type without undefined and null.
static bool mayBeKnownReceiver(const Type& type)
{
    if (!type.isJavaScriptValue())
        return false;
    uint16_t primitives = type.primitives & ~Primitive::UndefinedOrNull;
    if (!type.hasObjects())
        return primitives;
    return !primitives && !type.anyObject && !type.layouts;
}

// A receiver that is only expected to be something is tested. What is left for a receiver of which nothing is known is planInheritedMember().
bool mayFindProvenAccess(const BuiltinUseInputs& inputs, const Value& access)
{
    const Program& program = inputs.program;
    const Type& receiverType = program.typeOf(inputs.function, access.objectOperand());
    if (access.opcode == Opcode::PropGet) {
        if (singleBuiltinOf(receiverType))
            return true;
        if (access.isForTypeOf())
            return false;
    }
    if (!builtinMemberNameOf(program, access.key()))
        return false;
    return mayBeKnownReceiver(receiverType) || receiverType.hasOnlyLayouts(program);
}

// What the receiver is where the test of the site has passed.
static const Type& receiverTypeAt(const BuiltinUseInputs& inputs, const MemberSite& site)
{
    if (site.testedReceiverType)
        return inputs.types.at(site.testedReceiverType);
    return inputs.program.typeOf(inputs.function, site.receiver);
}

// No function is ever pristine. Flow says of the closures of a function whether one of them gets the key as a property of its own or another prototype.
static bool isClosureAsCreated(const BuiltinUseInputs& inputs, const MemberSite& site)
{
    if (site.start.builtinClass != BuiltinClasses::Function)
        return false;
    const Type& receiver = receiverTypeAt(inputs, site);
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
        return inputs.flow.areClosuresAsCreated(function, singleElementSpan(site.key)) || inputs.flow.closuresInheritFromFunctionPrototype(function, site.key);
    });
}

// The read by name finds the member, and the test of what it has found costs less than a test of the closure that fails every time.
static bool isClosureChangedByProgram(const BuiltinUseInputs& inputs, const MemberSite& site)
{
    if (site.start.builtinClass != BuiltinClasses::Function)
        return false;
    const Type& receiver = receiverTypeAt(inputs, site);
    if (receiver.anyFunction || !receiver.classes.isEmpty() || receiver.functions.isEmpty())
        return false;
    return std::ranges::all_of(receiver.functions, [&](FunctionID function) {
        return inputs.flow.closuresAreChangedByProgram(function);
    });
}

static void addReceiverRequirements(const BuiltinUseInputs& inputs, const MemberSite& site, ProgramPoint point, BuiltinUsePlan& plan, bool& isPossible)
{
    const BuiltinTable& table = inputs.program.builtins();
    UseRequirement requirement { site.receiver, site.testedReceiverType, { } };
    if (inputs.asksOnlyForProvenPlans && (requirement.requiredType || !plan.requirements.isEmpty())) {
        isPossible = false;
        return;
    }

    bool isInherited = site.start.kind == BuiltinReceiverKind::Instance && !table.at(site.answer.member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
    if (isInherited && !isClosureAsCreated(inputs, site)) {
        PristineRequirement names { site.start.builtinClass, aspectThatFindsMember(table, site.start.builtinClass, site.answer.member) };
        if (pristineProofOf(inputs.pristine(), site.receiver, names, point) != PristineProof::Proven) {
            if (pristineTestOf(table, names.builtinClass) == PristineTest::NeverPasses || isClosureChangedByProgram(inputs, site)) {
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

static BuiltinID firstHolderOf(const BuiltinTable& table, const BuiltinReceiver& start)
{
    switch (start.kind) {
    case BuiltinReceiverKind::Unknown:
        break;
    case BuiltinReceiverKind::BuiltinObject:
        return start.object;
    case BuiltinReceiverKind::Instance:
    case BuiltinReceiverKind::Primitive:
        return table.at(start.builtinClass).prototype;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool passesOnlyHoldersThatNeverGainKey(const BuiltinLookupContext& lookup, BuiltinID firstHolder, PropertyKeyID key, BuiltinID member)
{
    const BuiltinTable& table = lookup.program.builtins();
    BuiltinID holder = firstHolder;
    while (holder) {
        holder = table.canonicalID(holder);
        // One function object under two names lies at two locations. The test names the original's, which says nothing of the other.
        if (BuiltinID found = findOwnMember(lookup.program, holder, key))
            return found == member;
        if (absenceProofOf(lookup, holder, key) != LocationProof::Constant)
            return false;
        const Builtin& object = table.at(holder);
        if (object.prototypeLink != BuiltinPrototypeLink::Builtin)
            return false;
        holder = object.inheritsFrom;
    }
    return false;
}

static bool hasPassedTestOfLocation(const BuiltinUseInputs& inputs, BuiltinID member)
{
    return std::ranges::any_of(inputs.passedTests, [&](const Value& test) {
        return test.opcode == Opcode::IsBuiltinLocationUnchanged && test.member() == member;
    });
}

// The host makes the value at the first read, and after that only a write replaces it.
static bool isComputedOnceByHost(const Builtin& member)
{
    return member.kind == BuiltinKind::DataProperty
        && member.scope == BuiltinScope::Realm
        && member.mutability == BuiltinMutability::ConstantUnlessWritten
        && member.flags.contains(BuiltinFlag::IsLazilyCreated)
        && !member.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
}

// The engine reports the object that the host has made, and the watch compares the location with that.
static bool isObjectMadeByHost(const BuiltinTable& table, BuiltinID id)
{
    if (!isComputedOnceByHost(table.at(id)))
        return false;
    return std::ranges::any_of(table.overloadsOf(id), [&](const BuiltinOverload& overload) {
        if (overload.mode != BuiltinCallMode::Get)
            return false;
        const BuiltinType& type = table.at(overload.result.type);
        return !type.isAnything() && type.primitives.isEmpty() && type.stringLiterals.empty() && type.mayBeObject();
    });
}

// BuiltinLocationWatch arms no other location, so the test of another one fails every time it runs.
static bool canBeWatchedAtRunTime(const BuiltinTable& table, BuiltinID id)
{
    const Builtin& member = table.at(id);
    if (!member.name || !member.holder || table.at(member.holder).flags.contains(BuiltinFlag::IsGlobalObject))
        return false;

    switch (member.kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
        return true;
    case BuiltinKind::Accessor:
        // What the holder has is compared with the cells of the two functions. A getter of native code has no function, and only native code makes such a property.
        return BuiltinCellKey::getter(id).hasCell(table) || BuiltinCellKey::setter(id).hasCell(table) || std::ranges::any_of(table.overloadsOf(id), [](const BuiltinOverload& overload) {
            return overload.code.implementation.callee == BuiltinCallee::CustomGetter;
        });
    case BuiltinKind::Constant:
        return table.nameOfSymbol(id) || table.numberOf(id);
    case BuiltinKind::DataProperty:
        return isObjectMadeByHost(table, id);
    case BuiltinKind::NativeBinding:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Where flow has lost sight of a holder, a member that only a write replaces is tested at run time.
static LocationState locationStateOf(const BuiltinUseInputs& inputs, const MemberSite& site)
{
    if (site.answer.isConstant())
        return LocationState::Constant;
    if (!site.answer.isFound())
        return LocationState::Unknown;

    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    const Builtin& member = table.at(site.answer.member);
    if (member.mutability != BuiltinMutability::ConstantUnlessWritten || member.scope != BuiltinScope::Realm)
        return LocationState::Unknown;
    if (!canBeWatchedAtRunTime(table, site.answer.member))
        return LocationState::Unknown;
    if (keyLookupOf(program, site.key) != KeyLookup::Ordinary)
        return LocationState::Unknown;
    if (!passesOnlyHoldersThatNeverGainKey(inputs.lookup(), firstHolderOf(table, site.start), site.key, site.answer.member))
        return LocationState::Unknown;
    return hasPassedTestOfLocation(inputs, site.answer.member) ? LocationState::Constant : LocationState::NeedsTest;
}

static bool addLocationRequirement(const BuiltinUseInputs& inputs, const MemberSite& site, LocationState location, BuiltinUsePlan& plan)
{
    RELEASE_ASSERT(location != LocationState::Unknown);
    if (location == LocationState::Constant)
        return true;
    UseRequirement requirement;
    requirement.unchangedLocation = site.answer.member;
    return appendRequirement(inputs.types, plan, requirement);
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
    case BuiltinImplementationKind::Compiled:
    case BuiltinImplementationKind::CachedGetter:
        return true;
    case BuiltinImplementationKind::HostCall:
        return implementation.callee != BuiltinCallee::CustomGetter;
    case BuiltinImplementationKind::Inline:
        return implementation.otherwise.has_value();
    case BuiltinImplementationKind::Number:
    case BuiltinImplementationKind::Cell:
    case BuiltinImplementationKind::OwnSlot:
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

static std::optional<Value> tryMakeOwnPropertyOfClosures(const BuiltinUseInputs& inputs, FunctionID id, BuiltinID member)
{
    const Function& function = inputs.program.at(id);
    if (function.role != FunctionRole::Source)
        return std::nullopt;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
        break;
    // The name of an accessor function begins with "get " or "set ", and a class may define either name as a static member.
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return std::nullopt;
    }

    const BuiltinTable& table = inputs.program.builtins();
    if (member == table.canonicalID(Builtins::FunctionPrototypeLength)) {
        Value length = IRBuilder::make(Opcode::Integer);
        length.setInteger(function.length);
        return length;
    }
    // A function without a name of its own may be given one where it is evaluated, and an image that hides the names of functions answers the empty string.
    if (member != table.canonicalID(Builtins::FunctionPrototypeName) || !function.name || !inputs.keepsFunctionNames)
        return std::nullopt;
    Value name = IRBuilder::make(Opcode::String);
    name.setString(function.name);
    return name;
}

// The closures of a function are created with the length and the name of its definition.
static BuiltinUsePlan planReadOfClosureProperty(const BuiltinUseInputs& inputs, const MemberSite& site, ValueID use)
{
    if (site.start.kind != BuiltinReceiverKind::Instance || site.start.builtinClass != BuiltinClasses::Function)
        return { };
    FunctionID function = receiverTypeAt(inputs, site).singleFunction();
    if (!function || !inputs.flow.areClosuresAsCreated(function, singleElementSpan(site.key)))
        return { };
    if (inputs.analyses && isTestedByValidation(inputs.function, inputs.useLists(), use))
        return { };
    std::optional<Value> constant = tryMakeOwnPropertyOfClosures(inputs, function, site.answer.member);
    if (!constant)
        return { };

    const Program& program = inputs.program;
    BuiltinUsePlan plan;
    plan.kind = BuiltinUseKind::Constant;
    plan.member = site.answer.member;
    plan.constant = WTF::move(constant);
    plan.nullishCheck = nullishCheckFor(program, NullishAccess::Read, builtinMemberNameOf(program, site.key).name);
    if (site.testedReceiverType && !prependRequirement(inputs.types, plan, { site.receiver, site.testedReceiverType, { } }))
        return { };
    return plan;
}

static BuiltinOverloadID findOverload(const BuiltinTable& table, BuiltinID member, BuiltinCallMode mode)
{
    for (BuiltinOverloadID id : table.overloadIDsOf(member)) {
        if (table.at(id).mode == mode)
            return id;
    }
    return { };
}

// The instance keeps what the first read gave in a cell of its own. The overload says what type the value has.
static BuiltinUsePlan planHostValueRead(const BuiltinUseInputs& inputs, const MemberSite& site)
{
    const Program& program = inputs.program;
    BuiltinUsePlan plan;
    plan.call.overload = findOverload(program.builtins(), site.answer.member, BuiltinCallMode::Get);
    if (!plan.call.overload)
        return { };
    plan.kind = BuiltinUseKind::HostValue;
    plan.member = site.answer.member;
    plan.nullishCheck = nullishCheckFor(program, NullishAccess::Read, builtinMemberNameOf(program, site.key).name);
    return plan;
}

static BuiltinUsePlan planFoundMemberAccess(const BuiltinUseInputs& inputs, const MemberSite& site, ValueID use, BuiltinCallMode mode, ProgramPoint point)
{
    bool isRead = mode == BuiltinCallMode::Get;
    if (isRead) {
        if (BuiltinUsePlan plan = planReadOfClosureProperty(inputs, site, use))
            return plan;
    }

    const Builtin& member = inputs.program.builtins().at(site.answer.member);
    bool isOwnProperty = member.kind == BuiltinKind::DataProperty && member.flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
    LocationState location = isOwnProperty ? LocationState::Constant : locationStateOf(inputs, site);
    if (location == LocationState::Unknown)
        return { };
    // A store to a data property is itself a write to the location.
    if (location == LocationState::NeedsTest && !isRead && member.kind != BuiltinKind::Accessor)
        return { };

    // A cell that is filled once holds the value for good. The test of a location that the host fills passes only while the location holds what the host has made.
    bool readsHostValue = isRead && isComputedOnceByHost(member);
    BuiltinUsePlan plan = readsHostValue ? planHostValueRead(inputs, site) : planMemberAccess(inputs, use, site.answer.member, mode, point);
    if (!plan)
        return { };

    bool isPossible = addLocationRequirement(inputs, site, location, plan);
    addReceiverRequirements(inputs, site, point, plan, isPossible);
    if (!isPossible)
        return { };
    if (plan.kind == BuiltinUseKind::UnmatchedCall && !plan.requirements.isEmpty())
        return { };
    return plan;
}

static BuiltinUsePlan planConstantRead(const BuiltinUseInputs& inputs, const MemberSite& site, ProgramPoint point)
{
    LocationState location = locationStateOf(inputs, site);
    if (location == LocationState::Unknown)
        return { };

    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    BuiltinUsePlan plan;
    plan.kind = BuiltinUseKind::Constant;
    plan.member = site.answer.member;
    plan.nullishCheck = nullishCheckFor(program, NullishAccess::Read, builtinMemberNameOf(program, site.key).name);
    if (BuiltinName symbol = table.nameOfSymbol(plan.member))
        plan.key = program.keyOf(symbol);
    else if (!tryMakeBuiltinConstant(program, plan.member))
        return { };

    bool isPossible = addLocationRequirement(inputs, site, location, plan);
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
    if (!receiverType.hasOnlyLayouts(program) || hasOnlyNeverAllocatedLayouts(program, receiverType))
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

// A free name that nothing is bound to throws, and the message has the name. `typeof` of it and a read of the global object answer undefined.
static BuiltinUsePlan planReadOfAbsentGlobal(const Value& read)
{
    if (read.isFreeName() && !read.isForTypeOf())
        return { };
    BuiltinUsePlan plan;
    plan.kind = BuiltinUseKind::Undefined;
    return plan;
}

BuiltinUsePlan planBuiltinRead(const BuiltinUseInputs& inputs, ValueID use, ProgramPoint point)
{
    const Program& program = inputs.program;
    const BuiltinTable& table = program.builtins();
    const Value& read = inputs.function.at(use);

    BuiltinID receiver = singleBuiltinOf(program.typeOf(inputs.function, read.objectOperand()));
    bool readsGlobal = receiver && table.canonicalID(receiver) == table.canonicalID(Builtins::GlobalThis);
    if (readsGlobal) {
        switch (freeNameAnswerOf(inputs.lookup(), read.key(), read.isFreeName()).kind) {
        case FreeNameAnswerKind::ByName:
        case FreeNameAnswerKind::Bound:
            return { };
        case FreeNameAnswerKind::Absent:
            return planReadOfAbsentGlobal(read);
        case FreeNameAnswerKind::Constant:
            break;
        }
    } else if (read.isForTypeOf() || !builtinMemberNameOf(program, read.key()))
        return { };

    std::optional<MemberSite> site = findMember(inputs, read.objectOperand(), read.key());
    if (!site)
        return planInheritedMember(inputs, use, point, BuiltinCallMode::Get);

    // Behind its test a receiver that is expected to be the global object is the global object, and the read is planned again there.
    bool readsGlobalBehindTest = site->testedReceiverType && site->start.kind == BuiltinReceiverKind::BuiltinObject
        && table.canonicalID(site->start.object) == table.canonicalID(Builtins::GlobalThis);
    if (readsGlobalBehindTest && freeNameAnswerOf(inputs.lookup(), read.key(), read.isFreeName()).kind != FreeNameAnswerKind::Constant)
        return { };

    switch (table.at(site->answer.member).kind) {
    case BuiltinKind::Namespace:
    case BuiltinKind::Constructor:
    case BuiltinKind::Function:
    case BuiltinKind::Prototype:
    case BuiltinKind::Method:
    case BuiltinKind::Constant:
        ASSERT(!read.isFreeName() || readsGlobal);
        if (inputs.analyses && isClosureAsCreated(inputs, *site) && isTestedByValidation(inputs.function, inputs.useLists(), use))
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
