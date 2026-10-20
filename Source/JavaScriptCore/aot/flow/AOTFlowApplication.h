/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTFlowVersionPlan.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct FlowPositionUpdate {
    TypeID type;
    TypeID predicted;

    bool isEmpty() const { return !type && !predicted; }
};

struct FlowFieldUpdate {
    FlowPositionUpdate position;
    FieldFlow flow;
};

struct FlowAppendedField {
    PropertyKeyID key;
    FlowPositionUpdate position;
    FieldFlow flow;
};

struct FlowLayoutUpdate {
    LayoutFlow flow;
    ClassFlow classFlow;
    Vector<FlowFieldUpdate> fields;
    Vector<FlowAppendedField> appendedFields;
    // More constructor orders. They list keys, because an appended field has no index before it is appended.
    Vector<Vector<PropertyKeyID>> literalKeyOrders;
};

struct FlowVariableUpdate {
    FlowPositionUpdate position;
    LocationFlow flow;
};

struct FlowSignatureUpdate {
    Vector<FlowPositionUpdate> entryParameters;
    // What the entry parameters of the generic version are expected to hold. Empty, or one for each.
    Vector<TypeID> predictionsOfGenericVersion;
    Vector<FlowPositionUpdate, 1> results;
    OptionSet<FunctionCapabilities> provenCapabilities;
    ResultCheck resultCheck { ResultCheck::ByCallee };
};

struct FlowValueProof {
    ValueID value;
    TypeID type;
    TypeID predicted;
};

struct FlowClaimUpdate {
    ValueID claim;
    CheckCause cause { CheckCause::Unknown };
    bool isRedundant { false };
};

struct FlowAccessStamp {
    ValueID value;
    LayoutSetID receivers;
};

struct FlowCallStamp {
    ValueID value;
    CalleeSetID callees;
    bool keepsShape { false };
};

struct FlowDirectCallUpdate {
    ValueID call;
    FunctionID function;
    ResultCheck resultCheck { ResultCheck::ByCallee };
    bool resultIsInsideWrittenType { false };
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
    LayoutID derivedLayout;
    bool keepsShape { false };
    bool createsDictionary { false };
    TypeID elements;
    // For a dictionary: findCopyFillingLiteral().
    ValueID fillingCopy;
};

struct FlowConstructionStamp {
    ValueID value;
    LayoutID derivedLayout;
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
    Vector<FlowClaimUpdate> claims;
    Vector<FlowAccessStamp> accesses;
    Vector<FlowCallStamp> calls;
    Vector<FlowDirectCallUpdate> directCalls;
    Vector<FlowTailCallUpdate> tailCalls;
    Vector<FlowAllocationStamp> allocations;
    Vector<FlowConstructionStamp> constructions;
    Vector<FlowSymbolConstant> symbolConstants;
    // The fields of the object under construction count as filled for every other read only because these reads do not look at them.
    Vector<ValueID> readsFromPrototype;
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

    uint32_t numberOfSites() const { return firstSiteOfFunction.isEmpty() ? 0 : firstSiteOfFunction.last(); }
    AccessSiteID siteOf(FunctionID functionAsRead, uint32_t ordinal) const { return AccessSiteID(firstSiteOfFunction[functionAsRead.index()] + ordinal); }
    uint32_t numberOfSitesOf(FunctionID functionAsRead) const { return firstSiteOfFunction[functionAsRead.index() + 1] - firstSiteOfFunction[functionAsRead.index()]; }
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

    FlowVersionPlan versions;
    FlowBoundFunctionPlan boundFunctions;
    FlowAccessSiteNumbering accessSites;
    Vector<FlowLayoutUpdate> layouts;
    Vector<FlowVariableUpdate> variables;
    Vector<FlowSignatureUpdate> signatures;
    Vector<FlowBodyUpdate> bodies;
};

void remapTypes(FlowPositionUpdate&, const TypeRemap&);
void remapTypes(FlowLayoutUpdate&, const TypeRemap&);
void remapTypes(FlowSignatureUpdate&, const TypeRemap&);
void remapTypes(FlowBodyUpdate&, const TypeRemap&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
