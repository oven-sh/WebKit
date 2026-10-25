/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessReceivers.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTRemoveUnreadFields.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>
#include <wtf/EnumTraits.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct PrimitivePart {
    UnionPartKind kind;
    uint16_t primitives;
};

} // anonymous namespace

static constexpr PrimitivePart primitiveParts[] {
    { UnionPartKind::Nullish, Primitive::UndefinedOrNull },
    { UnionPartKind::Boolean, Primitive::Boolean },
    { UnionPartKind::Number, Primitive::Number },
    { UnionPartKind::String, Primitive::AnyString },
    { UnionPartKind::Symbol, Primitive::AnySymbol },
    { UnionPartKind::BigInt, Primitive::BigInt },
    { UnionPartKind::Absent, Primitive::Absent },
};

static constexpr uint16_t primitivesInParts()
{
    uint16_t primitives = 0;
    for (const PrimitivePart& part : primitiveParts)
        primitives |= part.primitives;
    return primitives;
}

static_assert(primitivesInParts() == Primitive::Defined);

static ReceiverState stateWithoutObjects()
{
    return { OptionSet<ObjectMutation>::all(), KeyEnumeration::NeverEnumerated, true };
}

ReceiverState receiverStateOf(const Program& program, LayoutID id)
{
    const LayoutFlow& flow = program.at(id).flow;
    if (flow.allocation == AllocationState::NeverAllocated)
        return stateWithoutObjects();
    return { flow.impossibleMutations, flow.keys, flow.addedKeysAreComplete };
}

ReceiverState receiverStateOf(const Program& program, LayoutSetID layouts)
{
    return program.familyTables().stateOf(layouts);
}

ReceiverState receiverStateOf(const Program& program, std::span<const LayoutID> layouts)
{
    if (layouts.empty())
        return { };
    ReceiverState state = stateWithoutObjects();
    for (LayoutID layout : layouts)
        state.meetWith(receiverStateOf(program, layout));
    return state;
}

OwnPropertyGain ownPropertyGainOf(const Program& program, LayoutID id, PropertyKeyID key)
{
    ReceiverState state = receiverStateOf(program, id);
    if (state.cannot(ObjectMutation::AddProperty))
        return OwnPropertyGain::Impossible;
    if (program.nameClassOf(key) == NameClass::Index)
        return OwnPropertyGain::Unknown;
    const LayoutFlow& flow = program.at(id).flow;
    if (!state.addedKeysAreComplete || std::ranges::binary_search(flow.addedKeys, key))
        return OwnPropertyGain::Unknown;
    if (flow.isReachedThroughUnknownReceivers && program.mayBeStoredThroughUnknownReceiver(key))
        return OwnPropertyGain::Unknown;
    return OwnPropertyGain::Impossible;
}

InstanceCount instanceCountOf(const PropertyAccessContext& context)
{
    if (context.flow && context.flow->programInstances() == ProgramInstances::AtMostOne)
        return InstanceCount::AtMostOne;
    return InstanceCount::Unknown;
}

void addCondition(MemberConditions& list, const MemberCondition& condition)
{
    auto position = std::ranges::lower_bound(list, condition);
    if (position != list.end() && *position == condition)
        return;
    list.insert(position - list.begin(), condition);
}

void addConditions(MemberConditions& list, std::span<const MemberCondition> conditions)
{
    for (const MemberCondition& condition : conditions)
        addCondition(list, condition);
}

ReceiverCandidates ReceiverCandidates::underGuard(std::span<const LayoutID> testedLayouts)
{
    return { .proof = ReceiverProof::Guard, .level = KnowledgeLevel::Expected, .proven = LayoutList(testedLayouts) };
}

bool hasMemberToFindUnder(const Program& program, LayoutID layout, PropertyKeyID name)
{
    if (!std::ranges::binary_search(program.layoutsWithName(name), layout))
        return false;
    std::optional<FieldReference> field = findField(program, layout, name, BaseChain::Proven);
    return !field || !isRemovedField(program, *field);
}

static bool mayFindFunctionUnder(const Program& program, LayoutID layout, PropertyKeyID name)
{
    std::optional<FieldReference> field = findField(program, layout, name, BaseChain::Proven);
    return !field || program.at(fieldAt(program, *field).declared.type).mayBeFunction();
}

LayoutList allocatedLayoutsWith(const Program& program, std::span<const PropertyKeyID> names, size_t stopsCountingAbove, std::span<const PropertyKeyID> calledNames)
{
    LayoutList layouts;
    if (names.empty())
        return layouts;

    PropertyKeyID rarestName = *std::ranges::min_element(names, { }, [&](PropertyKeyID name) {
        return program.layoutsWithName(name).size();
    });
    for (LayoutID layout : program.layoutsWithName(rarestName)) {
        bool hasEveryName = std::ranges::all_of(names, [&](PropertyKeyID name) {
            return hasMemberToFindUnder(program, layout, name);
        });
        if (!hasEveryName)
            continue;
        bool answersEveryCall = std::ranges::all_of(calledNames, [&](PropertyKeyID name) {
            return mayFindFunctionUnder(program, layout, name);
        });
        if (!answersEveryCall)
            continue;
        layouts.add(layout);
        if (layouts.size() > stopsCountingAbove)
            break;
    }
    return layouts;
}

ReceiverCandidates candidatesOf(const Program& program, const Type& receiver, const Type* expected, LayoutSetID stamp)
{
    bool mayHaveNoLayout = hasMembersWithoutLayout(program, receiver);

    ReceiverCandidates result;
    result.mayBeNullish = receiver.mayBeUndefinedOrNull();
    result.hasOtherParts = receiver.primitives & ~Primitive::UndefinedOrNull
        || receiver.anyObject
        || receiver.anyFunction
        || mayHaveNoLayout
        || !receiver.classes.isEmpty()
        || !receiver.functions.isEmpty()
        || !receiver.builtins.isEmpty()
        || !receiver.isJavaScriptValue();

    if (!receiver.layouts && !receiver.anyObject)
        return result;

    FamilyTables::Candidates members = program.familyTables().candidatesOf(receiver.layouts, expected ? expected->layouts : LayoutSetID(), stamp);
    if (receiver.layouts && !mayHaveNoLayout) {
        result.proof = ReceiverProof::Type;
        result.level = KnowledgeLevel::Proven;
        result.proven = WTF::move(members.proven);
        result.predicted = WTF::move(members.predicted);
        return result;
    }

    result.predicted = WTF::move(members.predicted.isEmpty() ? members.proven : members.predicted);
    result.ofExpectedType = WTF::move(members.ofExpectedType);
    if (!result.predicted.isEmpty())
        result.level = KnowledgeLevel::Expected;
    return result;
}

ReceiverCandidates receiverCandidates(const Program& program, const Function& function, ValueID receiver, LayoutSetID stamp)
{
    return candidatesOf(program, program.typeOf(function, receiver), &program.expectedOrTypeOf(function, receiver), stamp);
}

static void dumpLayouts(PrintStream& out, ASCIILiteral label, const LayoutList& layouts)
{
    if (layouts.isEmpty())
        return;
    out.print(" ", label, "=[");
    CommaPrinter comma;
    for (LayoutID layout : layouts)
        out.print(comma, "L", layout.index());
    out.print("]");
}

void ReceiverCandidates::dump(PrintStream& out) const
{
    out.print("proof=", enumName(proof), " level=", nameOf(level));
    dumpLayouts(out, "proven"_s, proven);
    dumpLayouts(out, "predicted"_s, predicted);
    if (hasOtherParts)
        out.print(" otherParts");
    if (mayBeNullish)
        out.print(" nullish");
}

static Type primitivePartOf(const Type& type, uint16_t primitives)
{
    Type part = Type::fromPrimitives(type.primitives & primitives);
    if (part.primitives & Primitive::Integer)
        part.integers = type.integers;
    if (part.primitives & Primitive::LiteralString)
        part.stringLiterals = type.stringLiterals;
    if (part.primitives & Primitive::LiteralSymbol)
        part.symbolLiterals = type.symbolLiterals;
    return part;
}

static void appendPart(UnionParts& parts, UnionPartKind kind, Type&& part)
{
    part.canonicalize();
    if (!part.isNever())
        parts.append(UnionPart { kind, WTF::move(part) });
}

UnionParts unionPartsOf(const Type& type)
{
    UnionParts parts;
    for (const PrimitivePart& primitivePart : primitiveParts) {
        if (type.primitives & primitivePart.primitives)
            appendPart(parts, primitivePart.kind, primitivePartOf(type, primitivePart.primitives));
    }

    if (type.layouts)
        appendPart(parts, UnionPartKind::Layouts, Type::forLayoutSet(type.layouts));
    for (const ClassPart& classPart : type.classes) {
        Type part;
        part.classes.append(classPart);
        if (classPart.builtinClass == BuiltinClasses::RegExp)
            part.regExpLiteralSet = type.regExpLiteralSet;
        appendPart(parts, UnionPartKind::BuiltinClass, WTF::move(part));
    }

    if (!type.functions.isEmpty()) {
        Type functions;
        functions.functions = type.functions;
        appendPart(parts, UnionPartKind::Functions, WTF::move(functions));
    }

    if (!type.builtins.isEmpty()) {
        Type builtins;
        builtins.builtins = type.builtins;
        appendPart(parts, UnionPartKind::Builtins, WTF::move(builtins));
    }

    if (!isClosedUnion(type)) {
        Type others;
        others.anyObject = type.anyObject;
        others.anyFunction = type.anyFunction;
        others.environment = type.environment;
        others.opaque = type.opaque;
        appendPart(parts, UnionPartKind::OtherObjects, WTF::move(others));
    }
    return parts;
}

size_t maxNumberOfUnionParts(const Type& type)
{
    size_t numberOfParts = type.classes.size();
    for (const PrimitivePart& primitivePart : primitiveParts)
        numberOfParts += !!(type.primitives & primitivePart.primitives);
    numberOfParts += !!type.layouts;
    numberOfParts += !type.functions.isEmpty();
    numberOfParts += !type.builtins.isEmpty();
    numberOfParts += !isClosedUnion(type);
    return numberOfParts;
}

bool isClosedUnion(const UnionParts& parts)
{
    return std::ranges::none_of(parts, [](const UnionPart& part) {
        return part.kind == UnionPartKind::OtherObjects;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
