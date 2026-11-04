/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `function Node(kind, pos, end) { this.kind = kind; .. }` above a hundred interfaces that a literal `kind` tells apart: which constructors store one of their parameters, as it came, under the key of the tag.
// The answer picks a layout and proves nothing: the maker's own layout is right for every object, and so is that of any member, which lists all that the maker lists.
// Withheld with TypedSlots: no maker is found, and every object has its maker's layout.

#include "config.h"
#include "AOTUnionTaggedMakers.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTThinSummary.h"
#include <algorithm>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// `this` of a construction is an object. An argument is anything.
enum class ValueOfTaggedMaker : bool { Anything, Object };

struct ObjectOfTaggedMaker {
    // In ThinSummary::nodes.
    uint32_t node { noSummaryNode };
    // As TaggedMaker::allocation.
    ValueID allocation;
};

} // anonymous namespace

// noSummaryNode: what lies at the Join comes from several nodes, or from none.
static uint32_t onlySourceOfJoinOfTaggedMaker(const ThinSummary& summary, uint32_t join)
{
    uint32_t source = noSummaryNode;
    for (const SummaryEdge& edge : summary.edges) {
        if (edge.to != join)
            continue;
        if (source != noSummaryNode && source != edge.from)
            return noSummaryNode;
        source = edge.from;
    }
    return source;
}

static const SummaryOperator* findOperatorGivingNodeOfTaggedMaker(const ThinSummary& summary, uint32_t node)
{
    for (const SummaryOperator& record : summary.operators) {
        if (record.result == node)
            return &record;
    }
    return nullptr;
}

// Where what lies at the node comes from whenever anything lies there. A copy, and a copy through a test, hand on the value that they are handed. ToThis hands on an object as it is, and a primitive comes out as a wrapper.
// noSummaryNode: several places. Copies that lead round to where they began bring nothing.
static uint32_t originOfNodeOfTaggedMaker(const ThinSummary& summary, uint32_t node, uint32_t object, ValueOfTaggedMaker kind)
{
    for (size_t steps = 0; steps <= summary.nodes.size(); ++steps) {
        if (node == noSummaryNode || node == object || summary.nodes[node].kind != SummaryNode::Kind::Join)
            return node;
        if (const SummaryOperator* record = findOperatorGivingNodeOfTaggedMaker(summary, node)) {
            if (record->opcode != Opcode::ToThis || kind != ValueOfTaggedMaker::Object)
                return noSummaryNode;
        }
        node = onlySourceOfJoinOfTaggedMaker(summary, node);
    }
    return noSummaryNode;
}

// The one positional parameter that the constructor stores under the key of the tag in the object.
// Nothing where it stores none, or where a record of its summary may leave something else under the key. What a callee does to an object that a call hands it is not looked at.
static std::optional<uint32_t> findParameterStoredAsTag(const Program& program, const Function& function, const ThinSummary& summary, uint32_t object, LayoutID maker, PropertyKeyID tagKey)
{
    // Evaluated text stores to `this` out of sight.
    if (summary.has(SummaryTrait::HasDirectEval))
        return std::nullopt;
    auto isObjectBeingMade = [&](uint32_t node) {
        return originOfNodeOfTaggedMaker(summary, node, object, ValueOfTaggedMaker::Object) == object;
    };

    std::optional<uint32_t> tagParameter;
    for (const SummaryAccess& access : summary.accesses) {
        if (access.isToContents() || (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define))
            continue;
        if (access.key && access.key != tagKey)
            continue;
        // [[Set]] ends in a definition on the receiver.
        if (!isObjectBeingMade(access.receiver != noSummaryNode ? access.receiver : access.subject))
            continue;
        if (!access.key)
            return std::nullopt;
        uint32_t stored = originOfNodeOfTaggedMaker(summary, access.value, object, ValueOfTaggedMaker::Anything);
        if (stored == noSummaryNode || summary.nodes[stored].kind != SummaryNode::Kind::Parameter)
            return std::nullopt;
        if (tagParameter && *tagParameter != summary.nodes[stored].first)
            return std::nullopt;
        tagParameter = summary.nodes[stored].first;
    }
    if (!tagParameter)
        return std::nullopt;

    for (const SummaryChange& change : summary.changes) {
        switch (change.kind) {
        case SummaryChange::Kind::DefinesByDescriptor:
            if (change.key && change.key != tagKey)
                continue;
            break;
        case SummaryChange::Kind::CopiesInto:
            break;
        case SummaryChange::Kind::Freezes:
        case SummaryChange::Kind::Deletes:
        case SummaryChange::Kind::ChangesPrototype:
        case SummaryChange::Kind::BecomesPrototype:
            continue;
        }
        if (isObjectBeingMade(change.subject))
            return std::nullopt;
    }

    // A store into an object that the body has made under exactly the maker's layout is an edge.
    const Layout& layout = program.at(maker);
    for (const SummaryEdge& edge : summary.edges) {
        const SummaryNode& target = summary.nodes[edge.to];
        if (target.kind == SummaryNode::Kind::Field && LayoutID(target.first) == maker && target.second < layout.fields.size() && layout.fields[target.second].key == tagKey)
            return std::nullopt;
    }

    uint32_t firstParameter = function.firstParameterIndex();
    if (*tagParameter < firstParameter || *tagParameter - firstParameter >= function.numberOfPositionalParameters())
        return std::nullopt;
    return tagParameter;
}

static bool constructorOfTaggedFamilyMakesObjects(const Program& program, const TaggedFamily& family)
{
    const Layout& maker = program.at(family.maker);
    if (family.members.isEmpty() || !maker.constructor)
        return false;
    // Below a built-in class the run time makes the objects.
    if (maker.has(LayoutFlags::IsNeverAllocated) || maker.writtenBuiltinBase)
        return false;

    const Function& function = program.at(maker.constructor);
    bool isApplicationOrLibraryCode = function.role == FunctionRole::Source || function.role == FunctionRole::Library;
    return isApplicationOrLibraryCode && function.hasBody() && function.results.size() == 1 && function.constructedClass == family.maker;
}

// Whoever constructs makes `this`, and chooses the layout there.
static bool isTaggedFamilyOfConstructorFunction(const Program& program, const TaggedFamily& family)
{
    const Layout& maker = program.at(family.maker);
    const Function& function = program.at(maker.constructor);
    return maker.has(LayoutFlags::IsConstructorFunction) && function.takesThisFromConstruction() && function.has(FunctionFlags::HasThis);
}

// The constructor of a class allocates its own object, in an operation of its body. The walk chooses the layout there.
static bool isTaggedFamilyOfClassConstructor(const Program& program, const TaggedFamily& family)
{
    const Layout& maker = program.at(family.maker);
    const Function& function = program.at(maker.constructor);
    return !maker.has(LayoutFlags::IsConstructorFunction) && function.kind == FunctionKind::BaseConstructor && function.has(FunctionFlags::HasNewTarget) && !function.has(FunctionFlags::HasThis);
}

static uint32_t findNodeOfTaggedMaker(const ThinSummary& summary, SummaryNode::Kind kind, uint32_t first)
{
    for (uint32_t node = 0; node < summary.nodes.size(); ++node) {
        if (summary.nodes[node].kind == kind && summary.nodes[node].first == first)
            return node;
    }
    return noSummaryNode;
}

// Null where the body makes no object of the maker's layout, or makes them in several operations. An initializer defines fields of the object that it is handed as `this`, out of sight of the summary.
static ValueID findOwnAllocationOfTaggedMaker(const Program& program, const BodyIndex& index, LayoutID maker)
{
    ValueID allocation;
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (operation.opcode() == Opcode::CallDirect && program.at(operation.as<Opcode::CallDirect>().function()).kind == FunctionKind::Initializer)
                return { };
            if (operation.opcode() != Opcode::NewObject || operation.as<Opcode::NewObject>().layout() != maker)
                continue;
            if (allocation)
                return { };
            allocation = operation.id();
        }
    }
    return allocation;
}

// Under the prototype object of a class below the maker the run time answers with an instance of that class, whatever the allocation names.
// The walk tells by a test of new.target, so the prototype object has to be the one that the function's own new.target brings.
static bool allocatesUnderOwnNewTargetOfTaggedMaker(const Function& function, const BodyIndex& index, ValueID allocation, LayoutID maker)
{
    auto made = index.value(allocation).as<Opcode::NewObject>();
    ValueID prototype = made.prototypeOperand();
    if (!made.fields().empty() || !prototype || index.isParameter(prototype) || index.opcodeOf(prototype) != Opcode::GetConstructPrototype)
        return false;
    auto read = index.value(prototype).as<Opcode::GetConstructPrototype>();
    return read.layout() == maker && read.newTargetOperand() == index.block(entryBlock).parameter(*function.newTargetParameterIndex());
}

// ScanOfBody::addAllocation(): such an object has a Join of its own, at which the layout that the allocation names and all that are written below it lie.
static std::optional<ObjectOfTaggedMaker> findObjectMadeByTaggedClassConstructor(const Program& program, const BodyIndexTable& bodies, const TaggedFamily& family, const ThinSummary& summary)
{
    FunctionID constructor = program.at(family.maker).constructor;
    const BodyIndex* index = bodies.find(constructor);
    ValueID allocation = index ? findOwnAllocationOfTaggedMaker(program, *index, family.maker) : ValueID();
    if (!allocation || !allocatesUnderOwnNewTargetOfTaggedMaker(program.at(constructor), *index, allocation, family.maker))
        return std::nullopt;

    uint32_t objectsOfMaker = findNodeOfTaggedMaker(summary, SummaryNode::Kind::Allocation, family.maker.index());
    if (objectsOfMaker == noSummaryNode)
        return std::nullopt;
    uint32_t object = noSummaryNode;
    for (const SummaryEdge& edge : summary.edges) {
        if (edge.from != objectsOfMaker)
            continue;
        if (object != noSummaryNode && object != edge.to)
            return std::nullopt;
        object = edge.to;
    }
    if (object == noSummaryNode || summary.nodes[object].kind != SummaryNode::Kind::Join)
        return std::nullopt;
    return ObjectOfTaggedMaker { object, allocation };
}

static std::optional<ObjectOfTaggedMaker> findObjectOfTaggedConstructor(const Program& program, const BodyIndexTable& bodies, const TaggedFamily& family, const ThinSummary& summary)
{
    if (isTaggedFamilyOfConstructorFunction(program, family)) {
        const Function& function = program.at(program.at(family.maker).constructor);
        uint32_t thisValue = findNodeOfTaggedMaker(summary, SummaryNode::Kind::Parameter, *function.thisParameterIndex());
        return thisValue == noSummaryNode ? std::nullopt : std::optional(ObjectOfTaggedMaker { thisValue, { } });
    }
    if (isTaggedFamilyOfClassConstructor(program, family))
        return findObjectMadeByTaggedClassConstructor(program, bodies, family, summary);
    return std::nullopt;
}

Vector<TaggedMaker> findTaggedMakers(const Program& program, const ThinSummaries& summaries, const BodyIndexTable& bodies, Mechanisms mechanisms)
{
    Vector<TaggedMaker> makers;
    if (!withoutMechanismsMissingDependencies(mechanisms).contains(Mechanism::TypedSlots))
        return makers;

    for (uint32_t index = 0; index < program.taggedFamilies.size(); ++index) {
        const TaggedFamily& family = program.taggedFamilies[index];
        if (!constructorOfTaggedFamilyMakesObjects(program, family))
            continue;
        FunctionID constructor = program.at(family.maker).constructor;
        const ThinSummary& summary = summaries.at(constructor);
        std::optional<ObjectOfTaggedMaker> object = findObjectOfTaggedConstructor(program, bodies, family, summary);
        if (!object)
            continue;
        if (std::optional<uint32_t> tagParameter = findParameterStoredAsTag(program, program.at(constructor), summary, object->node, family.maker, family.tagKey))
            makers.append(TaggedMaker { index, constructor, *tagParameter, object->allocation });
    }

    // A function constructs one class. Of two families with one maker the first stands.
    std::ranges::stable_sort(makers, { }, &TaggedMaker::constructor);
    makers.shrink(makers.size() - std::ranges::unique(makers, { }, &TaggedMaker::constructor).size());
    return makers;
}

const TaggedMaker* findTaggedMaker(std::span<const TaggedMaker> ascendingByConstructor, FunctionID constructor)
{
    auto found = std::ranges::lower_bound(ascendingByConstructor, constructor, { }, &TaggedMaker::constructor);
    return found != ascendingByConstructor.end() && found->constructor == constructor ? &*found : nullptr;
}

LayoutID layoutMadeWithTag(const TaggedFamily& family, const LayoutTag& tag)
{
    for (const TaggedLayout& member : family.members) {
        if (member.tags.contains(tag))
            return member.layout;
    }
    return { };
}

// A member has its maker as its written base, and no body names a member, so nothing is written below one.
bool onlyTaggedMembersLieBelow(const Program& program, LayoutID layout)
{
    size_t numberOfMembers = 0;
    for (const TaggedFamily& family : std::ranges::equal_range(program.taggedFamilies, layout, { }, &TaggedFamily::maker))
        numberOfMembers += family.members.size();
    std::span<const LayoutID> layoutsBelow = program.layoutsExpectedBelow(layout);
    return layoutsBelow.size() == numberOfMembers && std::ranges::all_of(layoutsBelow, [&](LayoutID below) {
        return program.at(below).isDerived() && writtenBaseOf(program.at(below)) == layout;
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
