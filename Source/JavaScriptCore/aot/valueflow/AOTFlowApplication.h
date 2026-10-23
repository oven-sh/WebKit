/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTTypeArena.h"
#include "AOTVector.h"
#include <algorithm>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// What flow says about one position: a parameter, a capture, a variable, a field, a result.
struct FlowPositionUpdate {
    // A part of the type of the position. None where that type stands as it is. Of a position that is not proven: that type without `absent`, where every read follows a store.
    TypeID type;
    // Whatever arrives lies inside the type of the position.
    bool isProven { false };
    // Of a position that something arrives at and that is not proven: what took the proof away, and where.
    UnprovenCause cause { UnprovenCause::Unknown };
    Witness witness { noWitness };

    bool isEmpty() const { return !type && !isProven; }
};

// Every definition that was seen under the name in the objects that hold the field makes an accessor of one form. It is what to expect: the run time keeps anything else out of the slot.
struct FlowAccessorField {
    // Contains IsAccessor, or is empty: the field holds a value.
    OptionSet<FieldForm> form;
    // The functions that the getter was seen to be a closure of, ascending. None where it may be something else.
    Vector<FunctionID, 1> getters;

    bool isAccessor() const { return form.contains(FieldForm::IsAccessor); }
};

struct FlowFieldUpdate {
    FlowPositionUpdate position;
    FieldFlow flow;
    FlowAccessorField accessor;
    // Of a field of an environment: see FlowLocationOrder. The allocations stop listing the field.
    bool undefinedAtStartIsNeverRead { false };
};

struct FlowAppendedField {
    PropertyKeyID key;
    FlowPositionUpdate position;
    FieldFlow flow;
    FlowAccessorField accessor;
};

struct FlowLayoutUpdate {
    LayoutFlow flow;
    ClassFlow classFlow;
    Vector<FlowFieldUpdate> fields;
    Vector<FlowAppendedField> appendedFields;
    // More constructor orders. They list keys, because an appended field has no index before it is appended.
    Vector<Vector<PropertyKeyID>> literalKeyOrders;
    // The literals with this layout make ordinary objects, and no type names the layout.
    bool objectsAreDictionaries { false };
};

struct FlowVariableUpdate {
    FlowPositionUpdate position;
    LocationFlow flow;
};

struct FlowSignatureUpdate {
    Vector<FlowPositionUpdate> entryParameters;
    Vector<FlowPositionUpdate, 1> results;
    OptionSet<FunctionCapabilities> provenCapabilities;
};

struct FlowValueProof {
    ValueID value;
    TypeID type;
};

struct FlowClaimUpdate {
    ValueID claim;
    bool isRedundant { false };
    // Where nothing that arrives can pass: the type of what arrives. See ValueFlowPositions::typeWhereNothingPasses().
    TypeID arriving;
};

// Written only where the set is complete.
struct FlowAccessStamp {
    ValueID value;
    LayoutSetID receivers;
};

struct FlowCallStamp {
    ValueID value;
    CalleeSetID callees;
};

struct FlowTailCallUpdate {
    ValueID call;
    TailCallRequirement requirement { TailCallRequirement::Required };
    bool claimsAreRedundant { false };
};

struct FlowAllocationStamp {
    ValueID value;
    AllocationSiteID site;
    AllocationClassID allocationClass;
    TypeID elements;
};

struct FlowSymbolConstant {
    ValueID call;
    PropertyKeyID key;
};

struct FlowBoundClosureCreation {
    ValueID call;
    FunctionID function;
};

struct FlowDefinedValue {
    ValueID definition;
    Value value;
};

struct FlowBodyUpdate {
    Vector<FlowValueProof> proofs;
    // Reads of locations that start absent, which these reads never find so. Ascending. Each gets a proof when the types of the locations are known.
    Vector<ValueID> readsAfterInitialization;
    Vector<FlowClaimUpdate> claims;
    Vector<FlowAccessStamp> accesses;
    Vector<FlowCallStamp> calls;
    Vector<FlowTailCallUpdate> tailCalls;
    Vector<FlowAllocationStamp> allocations;
    Vector<FlowSymbolConstant> symbolConstants;
    // Whatever holds the result of one of these calls of bind is typed as a closure of the function.
    Vector<FlowBoundClosureCreation> boundClosures;
    // Nobody sees what these field definitions store, so each stores a value that its field holds anyway.
    Vector<FlowDefinedValue> definedValues;
};

template<typename Entry, typename Member>
const Entry* findByValue(std::span<const Entry> ascendingEntries, ValueID value, Member Entry::* member)
{
    auto found = std::ranges::lower_bound(ascendingEntries, value, { }, member);
    if (found == ascendingEntries.end() || (*found).*member != value)
        return nullptr;
    return &*found;
}

struct FlowAccessSiteNumbering {
    Vector<uint32_t> firstSiteOfFunction;
    // By site. Only while the results are computed.
    Vector<ValueID> valueOfSite;

    uint32_t numberOfSites() const { return firstSiteOfFunction.isEmpty() ? 0 : firstSiteOfFunction.last(); }
    AccessSiteID siteOf(FunctionID functionAsRead, uint32_t ordinal) const { return AccessSiteID(firstSiteOfFunction[functionAsRead.index()] + ordinal); }
    uint32_t numberOfSitesOf(FunctionID functionAsRead) const { return firstSiteOfFunction[functionAsRead.index() + 1] - firstSiteOfFunction[functionAsRead.index()]; }
    // None for a value without a site.
    AccessSiteID siteOfValue(FunctionID functionAsRead, ValueID value) const
    {
        uint32_t first = firstSiteOfFunction[functionAsRead.index()];
        std::span<const ValueID> values = valueOfSite.subspan(first, numberOfSitesOf(functionAsRead));
        auto found = std::ranges::lower_bound(values, value);
        return found != values.end() && *found == value ? AccessSiteID(first + static_cast<uint32_t>(found - values.begin())) : AccessSiteID();
    }
};

constexpr bool hasAccessSite(Opcode opcode) { return PayloadWord::accessSite(opcode).has_value(); }

void forEachAccessSiteValue(const Function& function, NOESCAPE const Invocable<void(ValueID, uint32_t ordinal)> auto& functor)
{
    uint32_t ordinal = 0;
    for (uint32_t index = 0; index < function.values.size(); ++index) {
        if (hasAccessSite(function.values[index].opcode))
            functor(ValueID(index), ordinal++);
    }
}

struct FlowBoundClosureSite {
    FunctionID functionAsRead;
    ValueID call;
    FunctionID boundFunction;
};

// The bound functions that types name, so they exist from the application on. Each binds this and no argument.
struct FlowBoundFunctionPlan {
    Vector<FunctionID> targets;
    uint32_t firstFunction { 0 };
    Vector<FlowBoundClosureSite> sites;

    FunctionID functionAt(size_t index) const { return FunctionID(firstFunction + static_cast<uint32_t>(index)); }

    FunctionID functionBoundTo(FunctionID target) const
    {
        auto found = std::ranges::lower_bound(targets, target);
        RELEASE_ASSERT(found != targets.end() && *found == target);
        return functionAt(found - targets.begin());
    }

    std::span<const FlowBoundClosureSite> sitesIn(FunctionID functionAsRead) const LIFETIME_BOUND
    {
        auto found = std::ranges::equal_range(sites, functionAsRead, { }, &FlowBoundClosureSite::functionAsRead);
        return sites.subspan(found.begin() - sites.begin(), found.size());
    }
};

struct FlowApplication {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(FlowApplication);

    uint32_t functionCountAsRead { 0 };
    FlowBoundFunctionPlan boundFunctions;
    FlowAccessSiteNumbering accessSites;
    Vector<FlowLayoutUpdate> layouts;
    Vector<FlowVariableUpdate> variables;
    Vector<FlowSignatureUpdate> signatures;
    Vector<FlowBodyUpdate> bodies;
    bool hasDictionaries { false };
    // Ascending by the first: a type as read that names a layout whose objects are dictionaries. The second names Object in its place.
    Vector<std::pair<TypeID, TypeID>> typesBehindDictionaries;
    // The same types. The second names nothing in its place: what a test looks for, which can find a layout and nothing of an ordinary object.
    Vector<std::pair<TypeID, TypeID>> typesTestedBehindDictionaries;
    // By type: whether it comes first in a pair of the lists above.
    BitVector typesNamingDictionaries;
    // By set of layouts, of those that the table had when the list above was made.
    BitVector layoutSetsNamingDictionaries;
};

TypeID typeBehindDictionaries(std::span<const std::pair<TypeID, TypeID>> ascendingTypes, TypeID);

struct DictionaryTypeRewriter {
    explicit DictionaryTypeRewriter(const FlowApplication& application)
        : ascendingTypes(application.typesBehindDictionaries.span())
        , ascendingTestedTypes(application.typesTestedBehindDictionaries.span())
        , typesNamingDictionaries(application.typesNamingDictionaries)
    {
    }

    TypeID typeBehind(TypeID id) const { return typesNamingDictionaries.get(id.index()) ? typeBehindDictionaries(ascendingTypes, id) : id; }
    TypeID typeTestedBehind(TypeID id) const { return typesNamingDictionaries.get(id.index()) ? typeBehindDictionaries(ascendingTestedTypes, id) : id; }
    void operator()(TypeID& id) const { id = typeBehind(id); }
    void operator()(auto&) const { }

    std::span<const std::pair<TypeID, TypeID>> ascendingTypes;
    std::span<const std::pair<TypeID, TypeID>> ascendingTestedTypes;
    const BitVector& typesNamingDictionaries;
};

// Of an operation that makes an object without a member below %Object.prototype%: `{}`.
LayoutID layoutOfEmptyLiteral(const Program&, const Value&);

void remapTypes(FlowPositionUpdate&, const TypeRemap&);
void remapTypes(FlowLayoutUpdate&, const TypeRemap&);
void remapTypes(FlowSignatureUpdate&, const TypeRemap&);
void remapTypes(FlowBodyUpdate&, const TypeRemap&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
