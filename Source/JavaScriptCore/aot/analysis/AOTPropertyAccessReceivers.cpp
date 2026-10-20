/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessReceivers.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <wtf/CommaPrinter.h>

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

void ReceiverState::meetWith(const ReceiverState& other)
{
    bool areNeverEnumerated = keys == KeyEnumeration::NeverEnumerated && other.keys == KeyEnumeration::NeverEnumerated;
    impossibleMutations = impossibleMutations & other.impossibleMutations;
    keys = areNeverEnumerated ? KeyEnumeration::NeverEnumerated : KeyEnumeration::Unknown;
    addedKeysAreComplete = addedKeysAreComplete && other.addedKeysAreComplete;
}

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
    // The list has names only.
    if (program.nameClassOf(key) == NameClass::Index)
        return OwnPropertyGain::Unknown;
    if (state.addedKeysAreComplete && !std::ranges::binary_search(program.at(id).flow.addedKeys, key))
        return OwnPropertyGain::Impossible;
    return OwnPropertyGain::Unknown;
}

// A namespace object has no shape: the test for its layout asks for its module, which never changes.
static bool alwaysPassesTestOfLayout(const Program& program, LayoutID layout)
{
    return program.at(layout).kind == LayoutKind::ModuleNamespace || receiverStateOf(program, layout).cannot(ObjectMutation::LoseShape);
}

LayoutTestKind layoutTestKindFor(const Program& program, std::span<const LayoutID> layouts)
{
    bool isPermanent = !layouts.empty() && std::ranges::all_of(layouts, [&](LayoutID layout) {
        return alwaysPassesTestOfLayout(program, layout);
    });
    return isPermanent ? LayoutTestKind::Type : LayoutTestKind::Shape;
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
    return { .proof = ReceiverProof::Guard, .level = KnowledgeLevel::Predicted, .proven = LayoutList(testedLayouts) };
}

// A set as written lists declared layouts. What is below one of them by a proven chain of bases is expected wherever it is.
static LayoutList expectedMembersOf(const Program& program, std::span<const LayoutID> members)
{
    LayoutList expected;
    for (LayoutID member : members) {
        if (program.at(member).flow.allocation != AllocationState::NeverAllocated)
            expected.add(member);
        for (LayoutID below : program.layoutsBelow(member))
            expected.add(below);
    }
    return expected;
}

static LayoutList allocatedMembersOf(const Program& program, std::span<const LayoutID> members)
{
    LayoutList allocated;
    for (LayoutID member : members) {
        if (program.at(member).flow.allocation != AllocationState::NeverAllocated)
            allocated.add(member);
    }
    return allocated;
}

LayoutList allocatedLayoutsWith(const Program& program, std::span<const PropertyKeyID> names)
{
    LayoutList layouts;
    if (names.empty())
        return layouts;

    PropertyKeyID rarestName = *std::ranges::min_element(names, { }, [&](PropertyKeyID name) {
        return program.layoutsWithName(name).size();
    });
    for (LayoutID layout : program.layoutsWithName(rarestName)) {
        bool hasEveryName = std::ranges::all_of(names, [&](PropertyKeyID name) {
            return std::ranges::binary_search(program.layoutsWithName(name), layout);
        });
        if (hasEveryName)
            layouts.add(layout);
    }
    return layouts;
}

ReceiverCandidates candidatesOf(const Program& program, const Type& receiver, const Type* prediction, LayoutSetID stamp)
{
    ReceiverCandidates result;
    result.mayBeNullish = receiver.mayBeUndefinedOrNull();
    result.hasOtherParts = receiver.primitives & ~Primitive::UndefinedOrNull
        || receiver.anyObject
        || receiver.anyFunction
        || !receiver.classes.isEmpty()
        || !receiver.functions.isEmpty()
        || !receiver.builtins.isEmpty()
        || !receiver.isJavaScriptValue();

    const LayoutSet* stamped = stamp ? &program.at(stamp) : nullptr;

    if (receiver.layouts) {
        result.proof = ReceiverProof::Type;
        result.level = KnowledgeLevel::Proven;
        result.proven = allocatedMembersOf(program, membersOf(program, receiver));

        // A stamp proves nothing alone. Like a prediction, it says which members of the type to expect.
        LayoutList expected = result.proven;
        if (prediction && prediction->layouts)
            expected.intersect(expectedMembersOf(program, membersOf(program, *prediction)));
        if (stamped && stamped->isComplete)
            expected.intersect(LayoutList(stamped->members.span()));
        if (!expected.isEmpty() && expected.size() < result.proven.size())
            result.predicted = WTF::move(expected);
        return result;
    }

    if (!receiver.anyObject)
        return result;

    if (prediction && prediction->layouts)
        result.predicted = expectedMembersOf(program, membersOf(program, *prediction));
    else if (stamped)
        result.predicted = LayoutList(stamped->members.span());
    if (!result.predicted.isEmpty())
        result.level = KnowledgeLevel::Predicted;
    return result;
}

static bool hasLayoutsFromClaim(const Program& program, const Function& function, ValueID receiver)
{
    for (ValueID id : CastChain(function, receiver)) {
        const Value& value = function.at(id);
        if (value.opcode == Opcode::Claim && program.at(value.type).layouts && !program.typeOf(function, value.valueOperand()).layouts)
            return true;
    }
    return false;
}

ReceiverCandidates receiverCandidates(const Program& program, const Function& function, ValueID receiver, LayoutSetID stamp)
{
    // A written type is no proof of a shape: it predicts one where the checker predicts nothing narrower.
    ReceiverCandidates result = candidatesOf(program, program.typeOf(function, receiver), &program.predictionOrTypeOf(function, receiver), stamp);
    if (result.level == KnowledgeLevel::Proven && hasLayoutsFromClaim(program, function, receiver))
        result.level = KnowledgeLevel::Written;
    return result;
}

static ASCIILiteral nameOf(ReceiverProof proof)
{
    switch (proof) {
    case ReceiverProof::Unknown:
        return "Unknown"_s;
    case ReceiverProof::Guard:
        return "Guard"_s;
    case ReceiverProof::Type:
        return "Type"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
    out.print("proof=", nameOf(proof), " level=", nameOf(level));
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

static Type builtinClassPartOf(const Type& type, const ClassPart& classPart)
{
    Type part;
    part.classes.append(classPart);
    if (classPart.builtinClass == BuiltinClasses::RegExp)
        part.regExpLiteralSet = type.regExpLiteralSet;
    return part;
}

static Type otherObjectsPartOf(const Type& type)
{
    Type part;
    part.anyObject = type.anyObject;
    part.anyFunction = type.anyFunction;
    part.environment = type.environment;
    part.opaque = type.opaque;
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

    appendPart(parts, UnionPartKind::Layouts, Type::forLayoutSet(type.layouts));
    for (const ClassPart& classPart : type.classes)
        appendPart(parts, UnionPartKind::BuiltinClass, builtinClassPartOf(type, classPart));

    Type functions;
    functions.functions = type.functions;
    appendPart(parts, UnionPartKind::Functions, WTF::move(functions));

    Type builtins;
    builtins.builtins = type.builtins;
    appendPart(parts, UnionPartKind::Builtins, WTF::move(builtins));

    appendPart(parts, UnionPartKind::OtherObjects, otherObjectsPartOf(type));
    return parts;
}

bool isClosedUnion(const UnionParts& parts)
{
    return std::ranges::none_of(parts, [](const UnionPart& part) {
        return part.kind == UnionPartKind::OtherObjects;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
