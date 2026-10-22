/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBuiltinMembers.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTIRBuilder.h"
#include "AOTPropertyAccessCache.h"
#include "AOTPropertyAccessInternal.h"
#include "AOTPropertyAccessMembers.h"
#include <algorithm>
#include <iterator>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PrimitiveClass {
    BuiltinClassID builtinClass;
    uint16_t primitives;
};

} // anonymous namespace

static constexpr PrimitiveClass primitiveClasses[] = {
    { BuiltinClasses::String, Primitive::AnyString },
    { BuiltinClasses::Symbol, Primitive::AnySymbol },
    { BuiltinClasses::BigInt, Primitive::BigInt },
    { BuiltinClasses::Number, Primitive::Number },
    { BuiltinClasses::Boolean, Primitive::Boolean },
};

static constexpr OpcodeSet callsByValue { Opcode::Call, Opcode::Construct, Opcode::CallList, Opcode::ConstructList };

static PropertyAccessContext accessContextOf(const BuiltinLookupContext& context)
{
    return { context.program, &context.flow };
}

BuiltinMemberName builtinMemberNameOf(const Program& program, PropertyKeyID id)
{
    const PropertyKey& key = program.at(id);
    switch (key.kind) {
    case PropertyKeyKind::EnginePrivateName:
        return { program.builtinName(key.text), true };
    case PropertyKeyKind::PrivateName:
    case PropertyKeyKind::CreatedSymbol:
        return { };
    case PropertyKeyKind::Text:
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return { program.builtinNameOf(id), false };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

BuiltinID findOwnMember(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    BuiltinMemberName name = builtinMemberNameOf(program, key);
    if (!name)
        return { };
    const BuiltinTable& table = program.builtins();
    BuiltinID member = table.findOwnMember(holder, name.name);
    if (name.isPrivateAlias && member && !table.at(member).flags.contains(BuiltinFlag::HasPrivateAlias))
        return { };
    return member;
}

bool listsEveryMemberUnder(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    constexpr OptionSet<BuiltinFlag> flagsOfCompleteLists { BuiltinFlag::ListsAllMembers, BuiltinFlag::IsGlobalObject };
    return program.builtins().at(holder).flags.containsAny(flagsOfCompleteLists) && program.at(key).kind != PropertyKeyKind::EnginePrivateName;
}

BuiltinClassID primitiveClassOf(uint16_t primitives)
{
    if (!primitives)
        return { };
    for (const PrimitiveClass& candidate : primitiveClasses) {
        if (!(primitives & ~candidate.primitives))
            return candidate.builtinClass;
    }
    return { };
}

static bool hasOnlyFunctions(const Type& type)
{
    if (type.primitives || type.anyObject || type.layouts || !type.builtins.isEmpty())
        return false;
    return std::ranges::all_of(type.classes, [](const ClassPart& part) {
        return part.builtinClass == BuiltinClasses::Function;
    });
}

BuiltinReceiver builtinReceiverOf(const Program& program, const Type& type)
{
    if (!type.isJavaScriptValue() || type.isNever())
        return { };
    if (!type.hasObjects()) {
        BuiltinClassID builtinClass = primitiveClassOf(type.primitives);
        if (!builtinClass)
            return { };
        return { BuiltinReceiverKind::Primitive, { }, builtinClass };
    }
    if (type.primitives)
        return { };
    if (BuiltinID object = type.singleBuiltin())
        return { BuiltinReceiverKind::BuiltinObject, program.builtins().canonicalID(object), { } };
    if (const ClassPart* part = type.singleBuiltinClass())
        return { BuiltinReceiverKind::Instance, { }, part->builtinClass };
    if (hasOnlyFunctions(type))
        return { BuiltinReceiverKind::Instance, { }, BuiltinClasses::Function };
    return { };
}

Type typeOfInstances(const BuiltinTable& table, BuiltinClassID builtinClass, const Type& receiver)
{
    if (const ClassPart* part = receiver.findClassPart(builtinClass)) {
        Type instances;
        instances.classes.append(*part);
        return instances.withoutClassProofs();
    }
    if (!table.at(builtinClass).isPrimitiveWrapper)
        return Type::builtinClass(builtinClass);

    const PrimitiveClass* primitiveClass = std::ranges::find(primitiveClasses, builtinClass, &PrimitiveClass::builtinClass);
    RELEASE_ASSERT(primitiveClass != std::end(primitiveClasses));
    uint16_t present = receiver.primitives & primitiveClass->primitives;
    Type instances = Type::fromPrimitives(present ? present : primitiveClass->primitives);
    instances.canonicalize();
    return instances;
}

std::optional<Type> testedTypeOfInstances(const Program& program, BuiltinClassID builtinClass, const Type& receiver)
{
    Type tested = typeOfInstances(program.builtins(), builtinClass, receiver).withoutClassProofs();
    if (builtinReceiverOf(program, tested).builtinClass != builtinClass)
        return std::nullopt;
    return tested;
}

bool isClosedKey(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    const BuiltinTable& table = program.builtins();
    if (table.at(holder).flags.contains(BuiltinFlag::IsLockedObject))
        return true;
    if (program.isIndexLike(key))
        return table.closesIndexKeys(holder);
    if (BuiltinName name = program.builtinNameOf(key))
        return table.isClosedKey(holder, name);
    return table.at(holder).flags.contains(BuiltinFlag::IsClosedToOtherNames) && program.at(key).kind == PropertyKeyKind::Text;
}

bool isLockedLocation(const Program& program, BuiltinID holder, PropertyKeyID key)
{
    const BuiltinTable& table = program.builtins();
    BuiltinID member = findOwnMember(program, holder, key);
    if (!member)
        return isClosedKey(program, holder, key);
    // Only library code can spell an engine private name, and it stores under none that is an alias.
    if (program.at(key).kind == PropertyKeyKind::EnginePrivateName)
        return true;
    if (table.at(member).mutability == BuiltinMutability::AlwaysLocked || table.isLocationOfLockedObject(member))
        return true;
    // What changes is the property of an instance. Start-up locks the one of the holder with the other members.
    return table.at(member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances) && table.at(holder).flags.contains(BuiltinFlag::HasLockedMembers);
}

LocationProof locationProofOf(const BuiltinLookupContext& context, BuiltinID member)
{
    switch (builtinLocationAnswerOf(accessContextOf(context), member)) {
    case BuiltinLocationAnswer::Unknown:
        return LocationProof::Unknown;
    case BuiltinLocationAnswer::Constant:
        return LocationProof::Constant;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LocationProof absenceProofOf(const BuiltinLookupContext& context, BuiltinID holder, PropertyKeyID key)
{
    switch (builtinHolderGainOf(accessContextOf(context), holder, key)) {
    case BuiltinHolderGain::Unknown:
        return LocationProof::Unknown;
    case BuiltinHolderGain::Impossible:
        return LocationProof::Constant;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static LocationProof proofOfLookup(const BuiltinTable& table, const MemberAnswer& proven, const BuiltinMemberAnswer& answer)
{
    if (!proven.needsNoTest())
        return LocationProof::Unknown;
    switch (proven.kind) {
    case MemberKind::Builtin:
        return answer.isFound() && table.canonicalID(proven.builtin) == answer.member ? LocationProof::Constant : LocationProof::Unknown;
    case MemberKind::Absent:
        return answer.outcome == BuiltinMemberLookup::Outcome::Absent ? LocationProof::Constant : LocationProof::Unknown;
    case MemberKind::Unknown:
    case MemberKind::Field:
    case MemberKind::Method:
    case MemberKind::Accessor:
    case MemberKind::Variable:
    case MemberKind::Namespace:
        return LocationProof::Unknown;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static BuiltinMemberAnswer answerFor(const BuiltinLookupContext& context, BuiltinID start, PropertyKeyID key, const BuiltinMemberLookup& found, BuiltinMemberName name)
{
    const BuiltinTable& table = context.program.builtins();
    BuiltinMemberAnswer answer { found.outcome, found.member ? table.canonicalID(found.member) : BuiltinID(), LocationProof::Unknown };
    if (name.isPrivateAlias) {
        if (!answer.isFound() || !table.at(found.member).flags.contains(BuiltinFlag::HasPrivateAlias))
            return { };
        answer.proof = LocationProof::Constant;
        return answer;
    }
    answer.proof = proofOfLookup(table, findBuiltinObjectMember(accessContextOf(context), start, key, Access::Read), answer);
    return answer;
}

BuiltinMemberAnswer findMemberFrom(const BuiltinLookupContext& context, BuiltinID holder, PropertyKeyID key)
{
    BuiltinMemberName name = builtinMemberNameOf(context.program, key);
    if (!name)
        return { };
    const BuiltinTable& table = context.program.builtins();
    return answerFor(context, table.canonicalID(holder), key, table.findMember(holder, name.name), name);
}

BuiltinMemberAnswer findMemberOfInstances(const BuiltinLookupContext& context, BuiltinClassID builtinClass, PropertyKeyID key)
{
    const BuiltinTable& table = context.program.builtins();
    BuiltinID prototype = table.at(builtinClass).prototype;
    BuiltinMemberName name = builtinMemberNameOf(context.program, key);
    if (!prototype || !name)
        return { };
    BuiltinMemberLookup found = table.findForInstance(builtinClass, name.name);
    bool isOwnProperty = found.member && table.at(found.member).flags.contains(BuiltinFlag::IsOwnPropertyOfInstances);
    if (isOwnProperty && !name.isPrivateAlias)
        return { BuiltinMemberLookup::Outcome::Found, table.canonicalID(found.member), LocationProof::Constant };
    // The instance answers first, and it may be a typed array.
    if (keyLookupOf(context.program, key) != KeyLookup::Ordinary)
        return { found.outcome, found.member ? table.canonicalID(found.member) : BuiltinID(), LocationProof::Unknown };
    return answerFor(context, table.canonicalID(prototype), key, found, name);
}

// Only a script adds a let, const or class binding to the realm: what eval and Function declare stays their own.
// A script may declare no name under which the global object holds a locked property, so such a name is never shadowed.
static bool canBeShadowedByScript(const BuiltinLookupContext& context, PropertyKeyID key)
{
    if (isLockedLocation(context.program, Builtins::GlobalThis, key))
        return false;
    return !context.flow.hasResults() || context.flow.unseenScriptsCanRun();
}

static bool canHaveGlobalBinding(const Program& program, PropertyKeyID key)
{
    return program.at(key).kind == PropertyKeyKind::Text && keyLookupOf(program, key) == KeyLookup::Ordinary;
}

FreeNameAnswer freeNameAnswerOf(const BuiltinLookupContext& context, PropertyKeyID key, bool seesScriptBindings)
{
    if (!seesScriptBindings || !canBeShadowedByScript(context, key)) {
        BuiltinMemberAnswer found = findMemberFrom(context, Builtins::GlobalThis, key);
        if (found.isConstant())
            return { FreeNameAnswerKind::Constant, found.member };
        MemberAnswer walked = findBuiltinObjectMember(accessContextOf(context), Builtins::GlobalThis, key, Access::Read);
        if (walked.kind == MemberKind::Absent && walked.needsNoTest())
            return { FreeNameAnswerKind::Absent, { } };
    }
    return { canHaveGlobalBinding(context.program, key) ? FreeNameAnswerKind::Bound : FreeNameAnswerKind::ByName, { } };
}

Vector<MemberOfClasses, 4> methodsNamed(const BuiltinTable& table, BuiltinName name)
{
    Vector<MemberOfClasses, 4> methods;
    for (BuiltinClassID builtinClass : table.classesWithInstanceMember(name)) {
        BuiltinMemberLookup found = table.findForInstance(builtinClass, name);
        if (found.outcome != BuiltinMemberLookup::Outcome::Found)
            continue;
        BuiltinID member = table.canonicalID(found.member);
        if (table.at(member).kind != BuiltinKind::Method)
            continue;
        auto existing = std::ranges::find(methods, member, &MemberOfClasses::member);
        if (existing == methods.end()) {
            methods.append(MemberOfClasses { member, { builtinClass } });
            continue;
        }
        existing->classes.append(builtinClass);
    }
    std::ranges::sort(methods, { }, &MemberOfClasses::member);
    return methods;
}

Vector<MemberOfClasses, 4> methodsNamed(const Program& program, BuiltinName name)
{
    PropertyAccessCache* cache = program.propertyAccessCache();
    if (!cache)
        return methodsNamed(program.builtins(), name);
    uint32_t hash = static_cast<uint32_t>(name.index() * 0x9e3779b1u);
    auto isWanted = [&](const PropertyAccessCache::MethodsOfName& entry) {
        return entry.name == name;
    };
    if (const PropertyAccessCache::MethodsOfName* known = cache->methodsByName.find(hash, isWanted))
        return known->methods;
    return cache->methodsByName.add(hash, { name, methodsNamed(program.builtins(), name) }, isWanted).methods;
}

BuiltinID singleBuiltinOf(const Type& type)
{
    if (type.mayBeUndefinedOrNull())
        return { };
    return type.singleBuiltin();
}

BuiltinID singleBuiltinCalleeOf(const Program& program, const Function& function, const Value& call)
{
    RELEASE_ASSERT(callsByValue.contains(call.opcode));
    BuiltinID callee = singleBuiltinOf(program.typeOf(function, call.calleeOperand()));
    return callee ? program.builtins().canonicalID(callee) : BuiltinID();
}

std::optional<Value> tryMakeBuiltinConstant(const BuiltinTable& table, BuiltinID member)
{
    const Builtin& builtin = table.at(member);
    if (builtin.scope == BuiltinScope::Module)
        return std::nullopt;
    if (isObjectKind(builtin.kind)) {
        Value constant = IRBuilder::make(Opcode::Builtin);
        constant.setBuiltin(table.canonicalID(member));
        return constant;
    }
    if (table.canonicalID(member) == Builtins::Undefined)
        return IRBuilder::make(Opcode::Undefined);
    std::optional<double> number = table.numberOf(member);
    if (!number)
        return std::nullopt;
    if (isTrackedInteger(*number)) {
        Value constant = IRBuilder::make(Opcode::Integer);
        constant.setInteger(static_cast<int64_t>(*number));
        return constant;
    }
    Value constant = IRBuilder::make(Opcode::Double);
    constant.setNumber(*number);
    return constant;
}

std::optional<Value> tryMakeBuiltinConstant(const Program& program, BuiltinID member)
{
    const BuiltinTable& table = program.builtins();
    if (std::optional<Value> constant = tryMakeBuiltinConstant(table, member))
        return constant;
    if (table.at(member).scope == BuiltinScope::Module)
        return std::nullopt;
    std::optional<StringView> text = table.stringOf(member);
    if (!text)
        return std::nullopt;
    // A pass adds no string to the program. The names of the table are in it from the start.
    StringID string = program.strings.find(encodeProgramString(*text).span());
    if (!string)
        return std::nullopt;
    Value constant = IRBuilder::make(Opcode::String);
    constant.setString(string);
    return constant;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
