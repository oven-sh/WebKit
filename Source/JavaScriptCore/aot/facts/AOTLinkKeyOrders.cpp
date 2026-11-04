/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What Link says of the order of keys and of who fills a field how. All of it is written behind the flow, from the types that the flow ended with, so none of it hangs on the order of the worklist.
// Two proofs, in Facts::linked() alone: nobody lists the keys of the objects of a layout . the closures of a function stay as they are made.
// Three guesses: which functions literals lay as the accessors of a field . which fields makers give undefined alone . which one getter descriptors lay on the objects of a layout.

#include "config.h"
#include "AOTLinkKeyOrders.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

// A prototype object is no object of the layout, and has no order word of that layout. What is unseen beside the objects is of a layout that has escaped.
static void noteKeysListedInLinkKeyOrders(KeysListedInLink& listed, const LinkType& type)
{
    switch (type.objects) {
    case LinkType::Objects::None:
    case LinkType::Objects::Prototype:
    case LinkType::Objects::Prototypes:
        return;
    case LinkType::Objects::Layout:
        listed.layouts.set(type.layout.index());
        return;
    case LinkType::Objects::Family:
        listed.families.set(type.family.index());
        return;
    case LinkType::Objects::Lost:
        if (type.layout)
            listed.membersOfLostFamilies.set(type.layout.index());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void noteKeysListedOf(LinkFlow& flow, const LinkType& type)
{
    noteKeysListedInLinkKeyOrders(flow.keysListed, type);
}

static const LinkType& typeAtInLinkKeyOrders(const LinkFlow& flow, FunctionID function, uint32_t nodeInSummary)
{
    return flow.typeAt(flow.nodeOf(function, nodeInSummary));
}

// What a call or an allocation of its own has given the body, and `this` of a body that `new` or a constructor hands the object.
static bool isObjectOfMakerInLinkKeyOrders(const Program& program, FunctionID id, const SummaryNode& subject)
{
    switch (subject.kind) {
    case SummaryNode::Kind::Allocation:
    case SummaryNode::Kind::ResultOfCall:
        return true;
    case SummaryNode::Kind::Parameter: {
        const Function& function = program.at(id);
        std::optional<unsigned> thisParameter = function.thisParameterIndex();
        bool isHandedObjectBeingMade = function.kind == FunctionKind::Initializer || (function.takesThisFromConstruction() && function.constructedClass);
        return isHandedObjectBeingMade && thisParameter && subject.first == *thisParameter;
    }
    default:
        return false;
    }
}

static void noteWhatMakerGivesInLinkKeyOrders(GatheredForKeyOrders& gathered, LayoutID layout, PropertyKeyID key, const SummaryNode& given)
{
    bool isConstantUndefined = given.kind == SummaryNode::Kind::Constant && TypeID::fromBits(given.first) == TypeID(TypeTable::undefined);
    uint8_t& bits = gathered.whatMakersGive.add(static_cast<uint64_t>(layout.index()) << 32 | key.bits(), 0).iterator->value;
    bits |= isConstantUndefined ? 1 : 2;
}

// The class objects of a family are closures of constructors, of which closuresStayAsMade is never said. A lost closure has callers that nobody knows.
static void noteChangeOfClosureInLinkKeyOrders(GatheredForKeyOrders& gathered, const LinkType& subject)
{
    if (subject.functions == LinkType::Functions::Closure)
        gathered.functionsWhoseClosuresMayChange.set(subject.function.index());
}

void lookAtSummaryForKeyOrders(const LinkFlow& flow, TypeArena&, FunctionID id, const ThinSummary& summary, GatheredForKeyOrders& gathered)
{
    const Program& program = flow.program;
    for (uint32_t node : summary.nodesWhoseKeysAreListed)
        noteKeysListedInLinkKeyOrders(gathered.keysListed, typeAtInLinkKeyOrders(flow, id, node));

    for (const SummaryChange& change : summary.changes) {
        // A copy goes through the keys of its source in their order, and the target gets them so.
        if (change.kind == SummaryChange::Kind::CopiesInto && change.other != noSummaryNode)
            noteKeysListedInLinkKeyOrders(gathered.keysListed, typeAtInLinkKeyOrders(flow, id, change.other));
        if (change.kind != SummaryChange::Kind::BecomesPrototype)
            noteChangeOfClosureInLinkKeyOrders(gathered, typeAtInLinkKeyOrders(flow, id, change.subject));
    }

    for (const SummaryAccess& access : summary.accesses) {
        if (access.isToContents() || (access.kind != SummaryAccess::Kind::Store && access.kind != SummaryAccess::Kind::Define))
            continue;
        const LinkType& subject = typeAtInLinkKeyOrders(flow, id, access.subject);
        noteChangeOfClosureInLinkKeyOrders(gathered, subject);
        if (access.key && access.value != noSummaryNode && subject.objects == LinkType::Objects::Layout && isObjectOfMakerInLinkKeyOrders(program, id, summary.nodes[access.subject]))
            noteWhatMakerGivesInLinkKeyOrders(gathered, subject.layout, access.key, summary.nodes[access.value]);
    }

    // The scan lays a plain edge into a field only while the body that made the object has it alone.
    for (const SummaryEdge& edge : summary.edges) {
        const SummaryNode& to = summary.nodes[edge.to];
        if (to.kind != SummaryNode::Kind::Field)
            continue;
        const Layout& layout = program.at(LayoutID(to.first));
        if (!layout.isEnvironment() && to.second < layout.fields.size() && layout.fields[to.second].key)
            noteWhatMakerGivesInLinkKeyOrders(gathered, LayoutID(to.first), layout.fields[to.second].key, summary.nodes[edge.from]);
    }
}

static BitVector layoutsWhoseKeysAreListedInLinkKeyOrders(const LinkFlow& flow, const GatheredForKeyOrders& gathered)
{
    const Program& program = flow.program;
    BitVector listed = gathered.keysListed.layouts;
    listed.merge(flow.keysListed.layouts);
    BitVector lostFamilies;
    for (const KeysListedInLink* part : { &gathered.keysListed, &flow.keysListed }) {
        for (size_t family : part->families) {
            program.at(flow.families.layoutsOf(FamilyID(static_cast<uint32_t>(family)))).forEach([&](LayoutID member) {
                listed.set(member.index());
            });
        }
        for (size_t member : part->membersOfLostFamilies)
            lostFamilies.set(flow.unitedLayouts.representativeOf(static_cast<uint32_t>(member)));
    }
    // The families are as the flow left them. A layout that has met nobody is alone in its own.
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (lostFamilies.get(flow.unitedLayouts.representativeOf(index)))
            listed.set(index);
    }
    return listed;
}

static LinkedCell* findCellUnderKeyInLinkKeyOrders(const Program& program, LinkTables& tables, LayoutID layout, PropertyKeyID key)
{
    if (std::optional<uint32_t> listed = program.at(layout).findField(key))
        return &tables.fields[tables.firstFieldOfLayout[layout.index()] + *listed];
    std::span<const PropertyKeyID> appendedKeys = tables.appendedKeysOf(layout);
    size_t place = std::ranges::find(appendedKeys, key) - appendedKeys.begin();
    return place < appendedKeys.size() ? &tables.appendedFields[tables.firstAppendedFieldOfLayout[layout.index()] + place] : nullptr;
}

void writeAccessorsExpectedInFields(const LinkFlow& flow, LinkTables& tables)
{
    for (const Layout& layout : flow.program.layouts) {
        for (const StaticField& field : layout.staticFields) {
            if (field.key && !liesInThread(field.key))
                tables.keysOfStaticFields.set(field.key.index());
        }
    }
    Vector<AccessorLaidInField> laid = flow.accessorsLaidInFields;
    std::ranges::sort(laid);
    for (size_t first = 0; first < laid.size();) {
        LinkedAccessorsOfField expected { laid[first].field, { }, { } };
        bool hasSeveralGetters = false;
        bool hasSeveralSetters = false;
        size_t end = first;
        for (; end < laid.size() && laid[end].field == expected.field; ++end) {
            FunctionID& half = laid[end].isGetter ? expected.getter : expected.setter;
            bool& hasSeveral = laid[end].isGetter ? hasSeveralGetters : hasSeveralSetters;
            hasSeveral |= (half && half != laid[end].function) || !flow.tablesMayName(laid[end].function);
            half = laid[end].function;
        }
        if (hasSeveralGetters)
            expected.getter = { };
        if (hasSeveralSetters)
            expected.setter = { };
        uint32_t cell = tables.firstFieldOfLayout[expected.field.layout.index()] + expected.field.field;
        bool holdsAccessorFromBirth = cell < tables.firstFieldOfLayout[expected.field.layout.index() + 1] && tables.fields[cell].holdsAccessorFromBirth;
        expected.getterIsProven = expected.getter && holdsAccessorFromBirth && !flow.gettersThatMayBeAbsent.get(cell);
        expected.setterIsProven = expected.setter && holdsAccessorFromBirth && !flow.settersThatMayBeAbsent.get(cell);
        if (expected.getter || expected.setter)
            tables.accessorsExpectedInFields.append(expected);
        if (expected.getter || expected.setter) {
            // A literal lists what it lays.
            const Layout& layout = flow.program.at(expected.field.layout);
            if (expected.field.field < layout.fields.size() && !liesInThread(layout.fields[expected.field.field].key))
                tables.keysOfAccessorsExpectedInFields.set(layout.fields[expected.field.field].key.index());
        }
        first = end;
    }
}

void writeWhatLinkKnowsOfKeyOrders(const LinkFlow& flow, const GatheredForKeyOrders& gathered, LinkTables& tables)
{
    const Program& program = flow.program;
    // No test sees a listing, and none sees what was done to a function object: where unseen code is taken only to read and call, neither is said.
    if (flow.reading() == WhatUnseenCodeDoes::Anything) {
        BitVector listed = layoutsWhoseKeysAreListedInLinkKeyOrders(flow, gathered);
        for (uint32_t index = 0; index < tables.layouts.size(); ++index)
            tables.layouts[index].keysMayBeListed = listed.get(index);
        for (uint32_t index = 0; index < tables.functions.size(); ++index) {
            LinkedFunction& function = tables.functions[index];
            function.closuresStayAsMade = function.isReached && function.everyCallerIsKnown && !program.functions[index].constructedClass && !gathered.functionsWhoseClosuresMayChange.get(index);
        }
    }

    for (auto& [layoutAndKey, bits] : gathered.whatMakersGive) {
        if (bits != 1)
            continue;
        if (LinkedCell* cell = findCellUnderKeyInLinkKeyOrders(program, tables, LayoutID(static_cast<uint32_t>(layoutAndKey >> 32)), PropertyKeyID::fromBits(static_cast<uint32_t>(layoutAndKey))))
            cell->isOnlyGivenUndefinedByMakers = true;
    }
}

const LinkedAccessorsOfField* LinkTables::findAccessorsExpectedIn(FieldReference field) const
{
    auto found = std::ranges::lower_bound(accessorsExpectedInFields, field, { }, &LinkedAccessorsOfField::field);
    return found != accessorsExpectedInFields.end() && found->field == field ? &*found : nullptr;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
